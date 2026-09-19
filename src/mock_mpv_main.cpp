/*
 * Description: Stand in for mpv that speaks enough of the JSON IPC protocol to
 *   drive the producer, used for testing and benchmarking without a GPU.
 * Author: Alex Wu
 * Dependencies: framewire core library
 * Usage: framewire-mock-mpv --socket /tmp/mpv-a.sock --profile espcn --fps 60
 */

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "framewire/json.h"
#include "framewire/telemetry.h"
#include "framewire/term.h"

namespace {

using namespace framewire;

struct PassProfile {
  const char* desc;
  double mean_ns;
  double sigma;  // spread of the lognormal jitter
};

struct Options {
  std::string socket_path;
  std::string profile = "baseline";
  double fps = 60.0;
  double duration_s = 0.0;
  double drop_rate = 0.0;  // share of frames reported as dropped
  unsigned seed = 1;
};

void PrintUsage() {
  std::fprintf(stderr,
               "usage: framewire-mock-mpv --socket PATH [options]\n"
               "\n"
               "  --socket PATH     unix socket to create\n"
               "  --profile NAME    espcn, espcn-heavy or baseline (default baseline)\n"
               "  --fps N           frames to emit per second (default 60)\n"
               "  --duration S      stop after S seconds\n"
               "  --drop-rate R     share of frames marked dropped, 0 to 1\n"
               "  --seed N          random seed (default 1)\n");
}

/*
 * Returns the pass list for a named profile.
 *
 * The numbers are shaped to look like a real gpu shader chain, with an upscale
 * step that dominates and small fixed cost steps around the edges.
 *
 * Args:
 *   name: Profile name.
 * Returns:
 *   The passes that profile renders.
 */
std::vector<PassProfile> ProfileFor(const std::string& name) {
  if (name == "espcn") {
    return {
        {"vo-passes/fresh: upload", 90000, 0.18},
        {"espcn conv1 relu", 640000, 0.22},
        {"espcn conv2 relu", 410000, 0.22},
        {"espcn conv3 depth-to-space", 300000, 0.25},
        {"output blit", 120000, 0.15},
    };
  }
  if (name == "espcn-heavy") {
    return {
        {"vo-passes/fresh: upload", 90000, 0.18},
        {"espcn conv1 relu", 1180000, 0.26},
        {"espcn conv2 relu", 860000, 0.26},
        {"espcn conv3 relu", 640000, 0.24},
        {"espcn conv4 depth-to-space", 420000, 0.28},
        {"output blit", 120000, 0.15},
    };
  }
  // the baseline chain is mpv's built in scaler, fewer passes and cheaper
  return {
      {"vo-passes/fresh: upload", 90000, 0.18},
      {"ewa_lanczos scale", 1420000, 0.30},
      {"output blit", 120000, 0.15},
  };
}

/*
 * Creates and binds the listening socket.
 *
 * Args:
 *   path: Filesystem path for the socket.
 * Returns:
 *   The listening descriptor, or negative one on failure.
 */
int Listen(const std::string& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "framewire-mock-mpv: socket path is too long\n");
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  // a leftover socket file from a previous run would make bind fail with
  // EADDRINUSE even though nothing is listening
  unlink(path.c_str());

  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::fprintf(stderr, "framewire-mock-mpv: socket failed: %s\n", std::strerror(errno));
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::fprintf(stderr, "framewire-mock-mpv: bind failed: %s\n", std::strerror(errno));
    close(fd);
    return -1;
  }
  if (listen(fd, 4) != 0) {
    std::fprintf(stderr, "framewire-mock-mpv: listen failed: %s\n", std::strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

bool SendLine(int fd, const std::string& line) {
  size_t sent = 0;
  while (sent < line.size()) {
    const ssize_t n = send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    return false;
  }
  return true;
}

/*
 * Answers a command the way real mpv does.
 *
 * The strictness here is the point. An earlier version of this mock answered
 * every command with success, which hid a real bug: the producer was sending
 * the observe id as a quoted string, and mpv rejects that with "invalid
 * parameter" and then never sends the property at all. A mock that accepts
 * anything is worse than no mock, because the mock reports a green run while
 * the real thing captures nothing.
 *
 * Args:
 *   fd: Client socket.
 *   line: One received command.
 *   passes_json: Current vo-passes payload to answer a get_property with.
 * Returns:
 *   True when the command was understood and answered with success.
 */
bool HandleCommand(int fd, const std::string& line, const std::string& passes_json) {
  JsonDoc doc;
  if (!doc.Parse(line)) return false;

  const JsonValue root = doc.root();
  const int64_t request_id = root["request_id"].AsInt(0);
  const JsonValue command = root["command"];
  const std::string_view name = command[0].AsString();

  auto fail = [&](const char* error) {
    std::string reply = "{\"error\":\"";
    reply += error;
    reply += "\",\"data\":null,\"request_id\":";
    reply += std::to_string(request_id);
    reply += "}\n";
    SendLine(fd, reply);
    return false;
  };

  if (name == "observe_property") {
    // mpv wants the observe id as a JSON number and refuses a quoted one
    if (!command[1].is_number()) return fail("invalid parameter");
    if (!command[2].is_string()) return fail("invalid parameter");
  } else if (name == "get_property") {
    if (!command[1].is_string()) return fail("invalid parameter");

    if (command[1].AsString() == "vo-passes") {
      if (passes_json.empty()) return fail("property unavailable");
      std::string reply = "{\"error\":\"success\",\"data\":";
      reply += passes_json;
      reply += ",\"request_id\":" + std::to_string(request_id) + "}\n";
      SendLine(fd, reply);
      return true;
    }
  } else if (name.empty()) {
    return fail("invalid parameter");
  }

  std::string reply = "{\"error\":\"success\",\"data\":null,\"request_id\":";
  reply += std::to_string(request_id);
  reply += "}\n";
  SendLine(fd, reply);
  return true;
}

int Run(const Options& opt) {
  Terminal::InstallSignalHandlers();

  const int listen_fd = Listen(opt.socket_path);
  if (listen_fd < 0) return 1;
  std::fprintf(stderr, "framewire-mock-mpv: listening on %s (profile %s, %.1f fps)\n",
               opt.socket_path.c_str(), opt.profile.c_str(), opt.fps);

  int client = -1;
  while (client < 0 && !Terminal::ShutdownRequested()) {
    pollfd pfd{listen_fd, POLLIN, 0};
    const int ready = poll(&pfd, 1, 200);
    if (ready > 0) client = accept(listen_fd, nullptr, nullptr);
  }
  if (client < 0) {
    close(listen_fd);
    unlink(opt.socket_path.c_str());
    return 0;
  }
  std::fprintf(stderr, "framewire-mock-mpv: producer connected\n");

  const auto passes = ProfileFor(opt.profile);
  std::mt19937_64 rng(opt.seed);
  std::lognormal_distribution<double> jitter(0.0, 1.0);
  std::uniform_real_distribution<double> uniform(0.0, 1.0);

  const auto frame_interval_ns = static_cast<uint64_t>(1e9 / (opt.fps > 0.1 ? opt.fps : 0.1));
  const uint64_t start = MonotonicNanos();
  uint64_t next_frame = start;
  uint64_t frames = 0;
  uint64_t drops = 0;
  uint64_t delayed = 0;

  std::string payload;
  std::string read_buffer;

  while (!Terminal::ShutdownRequested()) {
    if (opt.duration_s > 0.0 &&
        MonotonicNanos() - start >= static_cast<uint64_t>(opt.duration_s * 1e9)) {
      break;
    }

    // drain any command the producer sent, so observe_property gets answered
    pollfd pfd{client, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0) {
      char chunk[4096];
      const ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n == 0) break;
      if (n > 0) {
        read_buffer.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = read_buffer.find('\n')) != std::string::npos) {
          HandleCommand(client, read_buffer.substr(0, nl), payload);
          read_buffer.erase(0, nl + 1);
        }
      }
    }

    const uint64_t now = MonotonicNanos();
    if (now < next_frame) {
      const uint64_t wait = next_frame - now;
      const timespec nap{static_cast<time_t>(wait / 1000000000ull),
                         static_cast<long>(wait % 1000000000ull)};
      nanosleep(&nap, nullptr);
      continue;
    }
    next_frame += frame_interval_ns;
    ++frames;

    // a lognormal multiplier gives a right leaning tail, which is what real
    // gpu pass timings look like. a plain uniform spread would make the p99
    // and p999 columns uninteresting
    payload = "{\"fresh\":[";
    for (size_t i = 0; i < passes.size(); ++i) {
      std::lognormal_distribution<double>::param_type p(0.0, passes[i].sigma);
      jitter.param(p);
      const double value = passes[i].mean_ns * jitter(rng);
      const auto ns = static_cast<uint64_t>(value);

      if (i != 0) payload.push_back(',');
      payload += "{\"desc\":";
      JsonEscapeTo(payload, passes[i].desc);
      payload += ",\"last\":" + std::to_string(ns);
      payload += ",\"avg\":" + std::to_string(static_cast<uint64_t>(passes[i].mean_ns));
      payload += ",\"peak\":" + std::to_string(ns * 2);
      payload += "}";
    }
    payload += "],\"redraw\":[]}";

    // real mpv only sends vo-passes when asked, so the mock keeps the payload
    // ready and announces the frame through playback-time, matching the
    // protocol the producer actually has to speak
    const double playback_time =
        static_cast<double>(frames) / (opt.fps > 0.1 ? opt.fps : 0.1);
    char tick[160];
    std::snprintf(tick, sizeof(tick),
                  "{\"event\":\"property-change\",\"id\":1,\"name\":\"playback-time\","
                  "\"data\":%.6f}\n",
                  playback_time);
    if (!SendLine(client, tick)) break;

    if (opt.drop_rate > 0.0 && uniform(rng) < opt.drop_rate) {
      ++drops;
      const std::string drop_event =
          "{\"event\":\"property-change\",\"id\":2,\"name\":\"frame-drop-count\",\"data\":" +
          std::to_string(drops) + "}\n";
      if (!SendLine(client, drop_event)) break;
    }
    if (opt.drop_rate > 0.0 && uniform(rng) < opt.drop_rate * 0.5) {
      ++delayed;
      const std::string delayed_event =
          "{\"event\":\"property-change\",\"id\":3,\"name\":\"vo-delayed-frame-count\",\"data\":" +
          std::to_string(delayed) + "}\n";
      if (!SendLine(client, delayed_event)) break;
    }
  }

  std::fprintf(stderr, "framewire-mock-mpv: sent %llu frames, %llu drops\n",
               static_cast<unsigned long long>(frames), static_cast<unsigned long long>(drops));

  close(client);
  close(listen_fd);
  unlink(opt.socket_path.c_str());
  return 0;
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
      if (!v) return false;
      opt->socket_path = v;
    } else if (arg == "--profile") {
      const char* v = next("--profile");
      if (!v) return false;
      opt->profile = v;
    } else if (arg == "--fps") {
      const char* v = next("--fps");
      if (!v) return false;
      opt->fps = std::atof(v);
    } else if (arg == "--duration") {
      const char* v = next("--duration");
      if (!v) return false;
      opt->duration_s = std::atof(v);
    } else if (arg == "--drop-rate") {
      const char* v = next("--drop-rate");
      if (!v) return false;
      opt->drop_rate = std::atof(v);
    } else if (arg == "--seed") {
      const char* v = next("--seed");
      if (!v) return false;
      opt->seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return false;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return false;
    }
  }
  if (opt->socket_path.empty()) {
    PrintUsage();
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;
  return Run(opt);
}
