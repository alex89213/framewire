/*
 * Description: Consumer process. Reads both telemetry rings, correlates frames
 *   between the two mpv instances and draws the live comparison dashboard.
 * Author: Alex Wu
 * Dependencies: framewire core library
 * Usage: framewire --shm-a /framewire-a --shm-b /framewire-b
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include "framewire/dashboard.h"
#include "framewire/spsc_ring.h"
#include "framewire/stats.h"
#include "framewire/term.h"

namespace {

using namespace framewire;

// records pulled from one ring per pump, high enough to drain a burst in one
// batch and low enough that one busy stream cannot starve the other
constexpr size_t kPumpBatch = 4096;

struct Options {
  std::string shm_a = "/framewire-a";
  std::string shm_b = "/framewire-b";
  std::string label_a;
  std::string label_b;
  std::string report_path;
  std::string json_path;
  double refresh_hz = 10.0;
  double tolerance_ms = 8.0;
  double duration_s = 0.0;  // zero runs until interrupted
  int wait_ms = 15000;
  bool color = true;
  bool force_plain = false;
};

void PrintUsage() {
  std::fprintf(stderr,
               "usage: framewire [options]\n"
               "\n"
               "  --shm-a NAME      first ring name (default /framewire-a)\n"
               "  --shm-b NAME      second ring name (default /framewire-b)\n"
               "  --label-a TEXT    override the label stored by the producer\n"
               "  --label-b TEXT    override the label stored by the producer\n"
               "  --refresh HZ      dashboard refresh rate (default 10)\n"
               "  --tolerance MS    frame pairing window (default 8)\n"
               "  --duration S      stop after S seconds, then print the report\n"
               "  --wait MS         how long to wait for the producers (default 15000)\n"
               "  --report PATH     also write the final report to a file\n"
               "  --json PATH       write the final report as JSON, for joining with other runs\n"
               "  --no-color        plain output with no escape codes\n"
               "  --plain           skip the live view, print the report at exit\n");
}

bool ParseArgs(int argc, char** argv, Options* opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--shm-a") {
      const char* v = next("--shm-a");
      if (!v) return false;
      opt->shm_a = v;
    } else if (arg == "--shm-b") {
      const char* v = next("--shm-b");
      if (!v) return false;
      opt->shm_b = v;
    } else if (arg == "--label-a") {
      const char* v = next("--label-a");
      if (!v) return false;
      opt->label_a = v;
    } else if (arg == "--label-b") {
      const char* v = next("--label-b");
      if (!v) return false;
      opt->label_b = v;
    } else if (arg == "--refresh") {
      const char* v = next("--refresh");
      if (!v) return false;
      opt->refresh_hz = std::atof(v);
    } else if (arg == "--tolerance") {
      const char* v = next("--tolerance");
      if (!v) return false;
      opt->tolerance_ms = std::atof(v);
    } else if (arg == "--duration") {
      const char* v = next("--duration");
      if (!v) return false;
      opt->duration_s = std::atof(v);
    } else if (arg == "--wait") {
      const char* v = next("--wait");
      if (!v) return false;
      opt->wait_ms = std::atoi(v);
    } else if (arg == "--report") {
      const char* v = next("--report");
      if (!v) return false;
      opt->report_path = v;
    } else if (arg == "--json") {
      const char* v = next("--json");
      if (!v) return false;
      opt->json_path = v;
    } else if (arg == "--no-color") {
      opt->color = false;
    } else if (arg == "--plain") {
      opt->force_plain = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return false;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return false;
    }
  }
  return true;
}

/*
 * Attaches to a ring, retrying until the producer has created the segment.
 *
 * Args:
 *   name: Shared memory name.
 *   timeout_ms: How long to keep retrying.
 * Returns:
 *   The attached mapping.
 */
RingMapping OpenWithRetry(const std::string& name, int timeout_ms) {
  const uint64_t deadline = MonotonicNanos() + MillisToNanos(timeout_ms);
  std::string last_error;

  for (;;) {
    try {
      return RingMapping::Open(name);
    } catch (const std::exception& e) {
      last_error = e.what();
    }

    if (MonotonicNanos() >= deadline || Terminal::ShutdownRequested()) {
      throw std::runtime_error("gave up waiting for ring '" + name + "': " + last_error);
    }
    const timespec nap{0, 50 * 1000 * 1000};
    nanosleep(&nap, nullptr);
  }
}

std::string LabelFor(const RingMapping& m, const std::string& override_label,
                     const char* fallback) {
  if (!override_label.empty()) return override_label;
  const std::string stored = m.header()->label;
  return stored.empty() ? fallback : stored;
}

int Run(const Options& opt) {
  Terminal::InstallSignalHandlers();

  // two consumers on one ring would each take a share of the records and
  // neither would see the full stream, which breaks the single consumer rule
  // the whole queue is built on
  if (opt.shm_a == opt.shm_b) {
    throw std::runtime_error("--shm-a and --shm-b must name different rings, both are '" +
                             opt.shm_a + "'");
  }

  std::fprintf(stderr, "framewire: waiting for rings %s and %s\n", opt.shm_a.c_str(),
               opt.shm_b.c_str());

  RingMapping map_a = OpenWithRetry(opt.shm_a, opt.wait_ms);
  RingMapping map_b = OpenWithRetry(opt.shm_b, opt.wait_ms);

  const std::string label_a = LabelFor(map_a, opt.label_a, "stream a");
  const std::string label_b = LabelFor(map_b, opt.label_b, "stream b");

  StreamAggregator agg_a(label_a, RingConsumer(std::move(map_a)));
  StreamAggregator agg_b(label_b, RingConsumer(std::move(map_b)));

  Correlator correlator(static_cast<uint64_t>(opt.tolerance_ms * 1e6));

  Terminal terminal;
  Screen screen;
  const bool interactive = !opt.force_plain && terminal.Enter();

  DashboardState state;
  state.color = opt.color && interactive;

  const uint64_t start = MonotonicNanos();
  const double refresh_hz = opt.refresh_hz > 0.1 ? opt.refresh_hz : 0.1;
  const auto refresh_ns = static_cast<uint64_t>(1e9 / refresh_hz);
  uint64_t next_refresh = start;
  bool both_gone_reported = false;

  std::vector<TelemetryRecord> batch_a;
  std::vector<TelemetryRecord> batch_b;

  StreamSnapshot snap_a;
  StreamSnapshot snap_b;
  ComparisonSnapshot snap_cmp;

  while (!Terminal::ShutdownRequested()) {
    batch_a.clear();
    batch_b.clear();

    const size_t read_a = agg_a.Pump(kPumpBatch, &batch_a);
    const size_t read_b = agg_b.Pump(kPumpBatch, &batch_b);

    if (!state.paused) {
      correlator.PushA(batch_a);
      correlator.PushB(batch_b);
      correlator.Process();
    }

    const uint64_t now = MonotonicNanos();
    state.elapsed_ns = now - start;

    if (opt.duration_s > 0.0 &&
        state.elapsed_ns >= static_cast<uint64_t>(opt.duration_s * 1e9)) {
      break;
    }

    if (now >= next_refresh) {
      next_refresh = now + refresh_ns;
      ++state.refreshes;

      snap_a = agg_a.Snapshot();
      snap_b = agg_b.Snapshot();
      snap_cmp = correlator.Snapshot(snap_a, snap_b);

      // once both producers are gone there is nothing left to draw, so drain
      // whatever is queued and stop rather than spinning on a dead ring
      if (!snap_a.producer_alive && !snap_b.producer_alive) {
        if (both_gone_reported) break;
        both_gone_reported = true;
        state.status = "both producers finished, press q to exit";
      }

      if (interactive) {
        int width = 0;
        int height = 0;
        terminal.GetSize(&width, &height);
        if (Terminal::TakeResizeFlag()) screen.Invalidate();

        screen.BeginFrame(width, height);
        RenderDashboard(screen, snap_a, snap_b, snap_cmp, state);
        screen.EndFrame();
      }
    }

    if (interactive) {
      const int key = terminal.ReadKey();
      if (key == 'q' || key == 'Q' || key == 3) break;
      if (key == 'p' || key == 'P') {
        state.paused = !state.paused;
        state.status = state.paused ? "paused, p resumes" : "";
      }
      if (key == 'r' || key == 'R') {
        agg_a.ResetStats();
        agg_b.ResetStats();
        correlator.Reset();
        state.status = "stats reset";
      }
    }

    // nothing arrived from either ring, so give the cpu back instead of
    // spinning. a busy poll would burn a core to no benefit at 60 frames a
    // second, where a frame is 16 milliseconds apart
    if (read_a == 0 && read_b == 0) {
      const timespec nap{0, 2 * 1000 * 1000};
      nanosleep(&nap, nullptr);
    }
  }

  // one last drain so records already in the rings are counted
  batch_a.clear();
  batch_b.clear();
  agg_a.Pump(kPumpBatch, &batch_a);
  agg_b.Pump(kPumpBatch, &batch_b);
  correlator.PushA(batch_a);
  correlator.PushB(batch_b);
  correlator.Process(true);

  snap_a = agg_a.Snapshot();
  snap_b = agg_b.Snapshot();
  snap_cmp = correlator.Snapshot(snap_a, snap_b);

  terminal.Leave();

  const std::string report =
      BuildTextReport(snap_a, snap_b, snap_cmp, MonotonicNanos() - start);
  std::fputs(report.c_str(), stdout);

  if (!opt.json_path.empty()) {
    const std::string json = BuildJsonReport(snap_a, snap_b, snap_cmp, MonotonicNanos() - start);
    FILE* f = std::fopen(opt.json_path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "framewire: could not write %s\n", opt.json_path.c_str());
    } else {
      std::fputs(json.c_str(), f);
      std::fclose(f);
    }
  }

  if (!opt.report_path.empty()) {
    FILE* f = std::fopen(opt.report_path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "framewire: could not write %s\n", opt.report_path.c_str());
    } else {
      std::fputs(report.c_str(), f);
      std::fclose(f);
      std::fprintf(stderr, "framewire: report written to %s\n", opt.report_path.c_str());
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;

  try {
    return Run(opt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "framewire: %s\n", e.what());
    return 1;
  }
}
