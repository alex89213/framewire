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
  // baseline first, every other stream is reported against it
  std::vector<std::string> shm_names;
  std::vector<std::string> labels;
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
               "  --shm NAME        ring to read, repeat for each stream, first is\n"
               "                    the baseline every other stream is compared to\n"
               "  --label TEXT      override the label for the matching --shm\n"
               "  --shm-a, --shm-b  aliases for the first two --shm arguments\n"
               "  --label-a, --label-b  aliases for the first two --label arguments\n"
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

    auto place = [](std::vector<std::string>* into, size_t index, const char* value) {
      if (into->size() <= index) into->resize(index + 1);
      (*into)[index] = value;
    };

    if (arg == "--shm") {
      const char* v = next("--shm");
      if (!v) return false;
      opt->shm_names.emplace_back(v);
    } else if (arg == "--label") {
      const char* v = next("--label");
      if (!v) return false;
      opt->labels.emplace_back(v);
    } else if (arg == "--shm-a" || arg == "--shm-b") {
      const char* v = next(arg.c_str());
      if (!v) return false;
      place(&opt->shm_names, arg == "--shm-a" ? 0 : 1, v);
    } else if (arg == "--label-a" || arg == "--label-b") {
      const char* v = next(arg.c_str());
      if (!v) return false;
      place(&opt->labels, arg == "--label-a" ? 0 : 1, v);
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

  if (opt->shm_names.empty()) {
    opt->shm_names = {"/framewire-a", "/framewire-b"};
  }
  if (opt->shm_names.size() < 2) {
    std::fprintf(stderr, "at least two rings are needed to compare anything\n");
    return false;
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

int Run(const Options& opt) {
  Terminal::InstallSignalHandlers();

  // two consumers on one ring would each take a share of the records and
  // neither would see the full stream, which breaks the single consumer rule
  // the whole queue is built on
  for (size_t i = 0; i < opt.shm_names.size(); ++i) {
    for (size_t j = i + 1; j < opt.shm_names.size(); ++j) {
      if (opt.shm_names[i] == opt.shm_names[j]) {
        throw std::runtime_error("--shm was given '" + opt.shm_names[i] +
                                 "' more than once, each stream needs its own ring");
      }
    }
  }

  std::fprintf(stderr, "framewire: waiting for %zu rings\n", opt.shm_names.size());

  std::vector<StreamAggregator> aggregators;
  aggregators.reserve(opt.shm_names.size());

  for (size_t i = 0; i < opt.shm_names.size(); ++i) {
    RingMapping mapping = OpenWithRetry(opt.shm_names[i], opt.wait_ms);
    std::string label = i < opt.labels.size() ? opt.labels[i] : std::string();
    if (label.empty()) {
      const std::string stored = mapping.header()->label;
      label = stored.empty() ? ("stream " + std::to_string(i)) : stored;
    }
    aggregators.emplace_back(label, RingConsumer(std::move(mapping)));
  }

  Correlator correlator(aggregators.size(), static_cast<uint64_t>(opt.tolerance_ms * 1e6));

  Terminal terminal;
  Screen screen;
  const bool interactive = !opt.force_plain && terminal.Enter();

  DashboardState state;
  state.color = opt.color && interactive;

  const uint64_t start = MonotonicNanos();
  const double refresh_hz = opt.refresh_hz > 0.1 ? opt.refresh_hz : 0.1;
  const auto refresh_ns = static_cast<uint64_t>(1e9 / refresh_hz);
  uint64_t next_refresh = start;
  bool all_gone_reported = false;

  std::vector<std::vector<TelemetryRecord>> batches(aggregators.size());
  std::vector<StreamSnapshot> snaps(aggregators.size());
  ComparisonSnapshot snap_cmp;

  while (!Terminal::ShutdownRequested()) {
    size_t read_total = 0;
    for (size_t i = 0; i < aggregators.size(); ++i) {
      batches[i].clear();
      read_total += aggregators[i].Pump(kPumpBatch, &batches[i]);
    }

    if (!state.paused) {
      for (size_t i = 0; i < aggregators.size(); ++i) correlator.Push(i, batches[i]);
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

      for (size_t i = 0; i < aggregators.size(); ++i) snaps[i] = aggregators[i].Snapshot();
      snap_cmp = correlator.Snapshot(snaps);

      // once every producer is gone there is nothing left to draw, so drain
      // whatever is queued and stop rather than spinning on dead rings
      bool any_alive = false;
      for (const auto& s : snaps) {
        if (s.producer_alive) any_alive = true;
      }
      if (!any_alive) {
        if (all_gone_reported) break;
        all_gone_reported = true;
        state.status = "every producer finished, press q to exit";
      }

      if (interactive) {
        int width = 0;
        int height = 0;
        terminal.GetSize(&width, &height);
        if (Terminal::TakeResizeFlag()) screen.Invalidate();

        screen.BeginFrame(width, height);
        RenderDashboard(screen, snaps, snap_cmp, state);
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
        for (auto& a : aggregators) a.ResetStats();
        correlator.Reset();
        state.status = "stats reset";
      }
    }

    // nothing arrived from any ring, so give the cpu back instead of spinning
    if (read_total == 0) {
      const timespec nap{0, 2 * 1000 * 1000};
      nanosleep(&nap, nullptr);
    }
  }

  // one last drain so records already in the rings are counted
  for (size_t i = 0; i < aggregators.size(); ++i) {
    batches[i].clear();
    aggregators[i].Pump(kPumpBatch, &batches[i]);
    correlator.Push(i, batches[i]);
  }
  correlator.Process(true);

  for (size_t i = 0; i < aggregators.size(); ++i) snaps[i] = aggregators[i].Snapshot();
  snap_cmp = correlator.Snapshot(snaps);

  terminal.Leave();

  const uint64_t elapsed = MonotonicNanos() - start;
  const std::string report = BuildTextReport(snaps, snap_cmp, elapsed);
  std::fputs(report.c_str(), stdout);

  if (!opt.json_path.empty()) {
    const std::string json = BuildJsonReport(snaps, snap_cmp, elapsed);
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
