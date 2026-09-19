/*
 * Description: Producer process. Connects to one mpv instance over the JSON IPC
 *   socket, turns vo-passes updates into telemetry records and pushes them into
 *   that instance's ring buffer.
 * Author: Alex Wu
 * Dependencies: framewire core library
 * Usage: framewire-producer --socket /tmp/mpv-a.sock --shm /framewire-a --label espcn
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "framewire/ipc_client.h"
#include "framewire/json.h"
#include "framewire/spsc_ring.h"
#include "framewire/telemetry.h"
#include "framewire/term.h"

namespace {

using namespace framewire;

// observe ids, echoed back by mpv on every property change event.
//
// vo-passes is deliberately not in this list. mpv accepts an observe request
// for the property and then only ever sends the value once, so per frame pass
// timings have to be pulled with get_property instead. playback-time is the
// clock that drives those pulls, because the property ticks exactly once per
// decoded frame
constexpr int64_t kObserveFrameTick = 1;
constexpr int64_t kObserveDropCount = 2;
constexpr int64_t kObserveDelayedCount = 3;

// request ids for the vo-passes pulls start above the observe ids so a reply
// can be told apart from an observe acknowledgement by id alone
constexpr int64_t kPollIdBase = 1000;

// cap on vo-passes requests in flight. mpv answers in order, and letting the
// queue grow without limit would turn a slow reply into unbounded memory and
// telemetry that lags further behind every frame
constexpr int kMaxOutstandingPolls = 4;

constexpr uint64_t kHeartbeatIntervalNs = 250ull * 1000 * 1000;

// a playback position that jumps back by more than this counts as a loop
// restart rather than a small seek correction
constexpr int64_t kLoopBackstepNs = 1000000000;

struct Options {
  std::string socket_path;
  std::string shm_name;
  std::string label = "stream";
  uint32_t capacity = 65536;
  int connect_timeout_ms = 10000;
  bool verbose = false;
};

void PrintUsage() {
  std::fprintf(stderr,
               "usage: framewire-producer --socket PATH --shm NAME [options]\n"
               "\n"
               "  --socket PATH     mpv --input-ipc-server socket to read from\n"
               "  --shm NAME        shared memory ring to create, leading slash\n"
               "  --label TEXT      name shown in the dashboard\n"
               "  --capacity N      ring slots, power of two (default 65536)\n"
               "  --timeout MS      how long to wait for mpv (default 10000)\n"
               "  --verbose         log every parse problem\n");
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

    if (arg == "--socket") {
      const char* v = next("--socket");
      if (v == nullptr) return false;
      opt->socket_path = v;
    } else if (arg == "--shm") {
      const char* v = next("--shm");
      if (v == nullptr) return false;
      opt->shm_name = v;
    } else if (arg == "--label") {
      const char* v = next("--label");
      if (v == nullptr) return false;
      opt->label = v;
    } else if (arg == "--capacity") {
      const char* v = next("--capacity");
      if (v == nullptr) return false;
      opt->capacity = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (arg == "--timeout") {
      const char* v = next("--timeout");
      if (v == nullptr) return false;
      opt->connect_timeout_ms = std::atoi(v);
    } else if (arg == "--verbose") {
      opt->verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return false;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return false;
    }
  }

  if (opt->socket_path.empty() || opt->shm_name.empty()) {
    PrintUsage();
    return false;
  }
  return true;
}

/*
 * Holds the pass layout seen so far so a change can be detected cheaply.
 *
 * mpv resends the full pass list on every frame. Hashing the names once per
 * frame is far cheaper than comparing strings, and the hash only has to catch
 * a real shader chain change.
 */
struct LayoutTracker {
  uint32_t hash = 0;
  unsigned count = 0;
  uint8_t version = 0;
};

/*
 * Reads one pass list out of a vo-passes payload into a record.
 *
 * Args:
 *   passes: The fresh or redraw array from vo-passes.
 *   rec: Record to fill, pass_ns and gpu_total_ns are written.
 *   names: Receives a pointer to each pass description.
 * Returns:
 *   Number of passes read.
 */
unsigned ReadPasses(const JsonValue& passes, TelemetryRecord* rec,
                    std::vector<std::string>* names) {
  names->clear();
  unsigned count = 0;
  uint64_t total = 0;

  unsigned seen = 0;
  for (JsonValue p = passes.FirstChild(); p.valid(); p = p.NextSibling()) {
    // mpv reports pass times in nanoseconds already, so no unit conversion
    const int64_t last = p["last"].AsInt(0);
    const uint64_t value = last > 0 ? static_cast<uint64_t>(last) : 0;

    // the total keeps accumulating past the storage cap on purpose. a chain
    // longer than pass_ns can hold still has to report an honest gpu total,
    // otherwise the longer chain looks cheaper than it really is
    total += value;
    ++seen;

    if (count < kMaxPasses) {
      rec->pass_ns[count] = static_cast<uint32_t>(value > UINT32_MAX ? UINT32_MAX : value);
      names->emplace_back(p["desc"].AsString(""));
      ++count;
    }
  }

  for (unsigned i = count; i < kMaxPasses; ++i) rec->pass_ns[i] = 0;
  rec->pass_count = static_cast<uint8_t>(count);
  rec->gpu_total_ns = total;
  if (seen > count) rec->flags |= kFlagPassOverflow;
  return seen;
}

int Run(const Options& opt) {
  Terminal::InstallSignalHandlers();

  MpvIpcClient client;
  if (!client.Connect(opt.socket_path, opt.connect_timeout_ms)) {
    std::fprintf(stderr, "framewire-producer: %s\n", client.last_error().c_str());
    return 1;
  }
  std::fprintf(stderr, "framewire-producer: connected to %s\n", opt.socket_path.c_str());

  RingProducer producer(RingMapping::Create(opt.shm_name, opt.capacity, opt.label));
  std::fprintf(stderr, "framewire-producer: ring %s ready, %u slots\n", opt.shm_name.c_str(),
               opt.capacity);

  if (!client.ObserveProperty(kObserveFrameTick, "playback-time") ||
      !client.ObserveProperty(kObserveDropCount, "frame-drop-count") ||
      !client.ObserveProperty(kObserveDelayedCount, "vo-delayed-frame-count")) {
    std::fprintf(stderr, "framewire-producer: %s\n", client.last_error().c_str());
    return 1;
  }

  JsonDoc doc;
  std::vector<std::string> names;
  std::vector<const char*> name_ptrs;
  LayoutTracker layout;

  uint64_t seq = 0;
  uint64_t last_frame_ns = 0;
  uint64_t last_heartbeat = 0;
  uint32_t drop_total = 0;
  uint32_t delayed_total = 0;
  uint32_t last_drop_total = 0;
  uint32_t last_delayed_total = 0;
  uint64_t parse_errors = 0;
  uint64_t poll_errors = 0;
  uint64_t frame_ticks = 0;
  int64_t next_poll_id = 0;
  int outstanding_polls = 0;
  int64_t media_time_ns = 0;
  int64_t last_position_ns = 0;
  int64_t media_epoch_ns = 0;
  uint64_t loops = 0;
  int overflow_warned = 0;

  std::string line;
  while (!Terminal::ShutdownRequested()) {
    const uint64_t now = MonotonicNanos();
    if (now - last_heartbeat > kHeartbeatIntervalNs) {
      producer.Heartbeat();
      last_heartbeat = now;
    }

    const auto result = client.PollLine(&line, 200);
    if (result == MpvIpcClient::PollResult::Closed) {
      std::fprintf(stderr, "framewire-producer: mpv closed the socket\n");
      break;
    }
    if (result == MpvIpcClient::PollResult::Error) {
      std::fprintf(stderr, "framewire-producer: %s\n", client.last_error().c_str());
      break;
    }
    if (result != MpvIpcClient::PollResult::Line) continue;

    if (!doc.Parse(line)) {
      ++parse_errors;
      if (opt.verbose) {
        std::fprintf(stderr, "framewire-producer: bad json: %s\n", doc.error().c_str());
      }
      continue;
    }

    const JsonValue root = doc.root();
    const JsonValue event = root["event"];

    if (event.valid()) {
      if (event.AsString() != "property-change") continue;

      const std::string_view prop = root["name"].AsString();

      if (prop == "frame-drop-count") {
        drop_total = static_cast<uint32_t>(root["data"].AsInt(0));
        continue;
      }
      if (prop == "vo-delayed-frame-count") {
        delayed_total = static_cast<uint32_t>(root["data"].AsInt(0));
        continue;
      }
      if (prop != "playback-time") continue;

      const double position = root["data"].AsDouble(-1.0);
      if (position >= 0.0) {
        auto position_ns = static_cast<int64_t>(position * 1e9);

        // looping restarts the clock at zero. without an epoch offset the key
        // would jump backwards, and the correlator relies on a key that only
        // ever increases
        if (position_ns + kLoopBackstepNs < last_position_ns) {
          media_epoch_ns += last_position_ns;
          ++loops;
        }
        last_position_ns = position_ns;
        media_time_ns = media_epoch_ns + position_ns;
      }

      // a new frame was presented, so ask for the pass timings behind it. the
      // reply is handled below when the answer comes back
      ++frame_ticks;
      if (outstanding_polls < kMaxOutstandingPolls &&
          client.GetProperty("vo-passes", kPollIdBase + next_poll_id)) {
        ++next_poll_id;
        ++outstanding_polls;
      }
      continue;
    }

    // not an event, so this is a reply to a command. only the vo-passes pulls
    // carry an id at or above the base, everything lower is an observe ack
    const int64_t request_id = root["request_id"].AsInt(-1);
    if (request_id < kPollIdBase) continue;

    if (outstanding_polls > 0) --outstanding_polls;
    if (root["error"].AsString() != "success") {
      ++poll_errors;
      continue;
    }

    const JsonValue data = root["data"];
    // mpv splits passes into work done for a new frame and work done to redraw
    // the same frame. fresh is the interesting one, redraw is only used when a
    // paused player is still presenting
    JsonValue passes = data["fresh"];
    uint16_t flags = kFlagNone;
    if (!passes.valid() || passes.size() == 0) {
      passes = data["redraw"];
      flags |= kFlagRedraw;
    }
    if (!passes.valid() || passes.size() == 0) continue;

    TelemetryRecord rec{};
    rec.t_mono_ns = MonotonicNanos();
    const unsigned seen_passes = ReadPasses(passes, &rec, &names);
    if (rec.pass_count == 0) continue;
    if (seen_passes > rec.pass_count && overflow_warned == 0) {
      std::fprintf(stderr,
                   "framewire-producer: chain has %u passes, only the first %u are listed "
                   "individually. the gpu total still covers all of them\n",
                   seen_passes, static_cast<unsigned>(kMaxPasses));
      overflow_warned = 1;
    }

    // republish the directory only when the chain actually changes, so the
    // consumer is not rereading names on every single frame
    uint32_t name_hash = 0;
    for (const auto& n : names) name_hash = name_hash * 31u + Fnv1a(n.data(), n.size());
    if (name_hash != layout.hash || names.size() != layout.count) {
      name_ptrs.clear();
      for (const auto& n : names) name_ptrs.push_back(n.c_str());
      layout.version = producer.PublishLayout(name_ptrs.data(), static_cast<unsigned>(names.size()));
      layout.hash = name_hash;
      layout.count = static_cast<unsigned>(names.size());
      if (seq > 0) flags |= kFlagLayoutChange;
    }

    rec.seq = ++seq;
    rec.media_time_ns = media_time_ns;
    rec.layout_version = layout.version;
    rec.frame_time_ns = last_frame_ns == 0 ? 0 : rec.t_mono_ns - last_frame_ns;
    last_frame_ns = rec.t_mono_ns;

    if (drop_total > last_drop_total) flags |= kFlagDroppedFrame;
    if (delayed_total > last_delayed_total) flags |= kFlagDelayedFrame;
    last_drop_total = drop_total;
    last_delayed_total = delayed_total;

    rec.dropped_total = drop_total;
    rec.delayed_total = delayed_total;
    rec.flags |= flags;
    StampChecksum(rec);

    producer.TryPush(rec);
  }

  producer.MarkDone();
  std::fprintf(stderr,
               "framewire-producer: stopped after %llu records from %llu frame ticks, "
               "%llu ring drops, %llu parse errors, %llu property errors\n",
               static_cast<unsigned long long>(seq),
               static_cast<unsigned long long>(frame_ticks),
               static_cast<unsigned long long>(producer.dropped()),
               static_cast<unsigned long long>(parse_errors),
               static_cast<unsigned long long>(poll_errors));
  if (loops > 0) {
    std::fprintf(stderr, "framewire-producer: video looped %llu times\n",
                 static_cast<unsigned long long>(loops));
  }

  if (seq == 0) {
    std::fprintf(stderr,
                 "framewire-producer: no pass timings arrived. mpv only fills in vo-passes for "
                 "--vo=gpu or --vo=gpu-next\n");
  }

  // the ring is deliberately left mapped until this process exits, so a
  // consumer still draining the tail keeps a valid segment underneath
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;

  try {
    return Run(opt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "framewire-producer: %s\n", e.what());
    return 1;
  }
}
