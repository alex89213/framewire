/*
 * Description: Implementation of the mpv JSON IPC socket client.
 * Author: Alex Wu
 * Dependencies: framewire/ipc_client.h, framewire/json.h
 * Usage:
 */

#include "framewire/ipc_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "framewire/json.h"
#include "framewire/telemetry.h"

namespace framewire {
namespace {

// cap on how much unparsed data may pile up. mpv messages are well under a
// kilobyte, so anything past this means the stream is not what is expected
constexpr size_t kMaxBufferBytes = 4u * 1024 * 1024;

constexpr size_t kReadChunk = 16 * 1024;

}  // namespace

MpvIpcClient::~MpvIpcClient() { Close(); }

void MpvIpcClient::Close() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  read_buffer_.clear();
  scan_from_ = 0;
}

bool MpvIpcClient::Connect(const std::string& socket_path, int timeout_ms) {
  Close();

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    last_error_ = "socket path is too long for sockaddr_un: " + socket_path;
    return false;
  }
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

  const uint64_t deadline = MonotonicNanos() + MillisToNanos(timeout_ms);

  for (;;) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      last_error_ = std::string("socket failed: ") + std::strerror(errno);
      return false;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      // non blocking only after connect succeeds, which keeps the connect
      // itself simple and still leaves reads under poll control
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        last_error_ = std::string("fcntl failed: ") + std::strerror(errno);
        close(fd);
        return false;
      }
      fd_ = fd;
      last_error_.clear();
      return true;
    }

    const int connect_errno = errno;
    close(fd);

    if (MonotonicNanos() >= deadline) {
      last_error_ = "could not connect to '" + socket_path + "': " + std::strerror(connect_errno);
      return false;
    }

    // mpv creates the socket shortly after launch, so a miss early on is
    // normal and worth a short sleep rather than a hard failure
    const timespec nap{0, 20 * 1000 * 1000};
    nanosleep(&nap, nullptr);
  }
}

bool MpvIpcClient::SendCommand(const std::vector<std::string>& args, int64_t request_id) {
  if (fd_ < 0) {
    last_error_ = "not connected";
    return false;
  }

  const std::string line = BuildMpvCommand(args, request_id);
  size_t sent = 0;
  while (sent < line.size()) {
    // MSG_NOSIGNAL keeps a closed socket from killing the process with SIGPIPE
    const ssize_t n =
        send(fd_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd pfd{fd_, POLLOUT, 0};
      if (poll(&pfd, 1, 1000) <= 0) {
        last_error_ = "timed out writing a command to mpv";
        return false;
      }
      continue;
    }
    last_error_ = std::string("send failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool MpvIpcClient::ObserveProperty(int64_t observe_id, const std::string& property) {
  return SendCommand({"observe_property", std::to_string(observe_id), property}, observe_id);
}

bool MpvIpcClient::TakeBufferedLine(std::string* out_line) {
  const size_t newline = read_buffer_.find('\n', scan_from_);
  if (newline == std::string::npos) {
    // remember how far the search got, so a message arriving in many chunks
    // does not rescan the same bytes on every wake up
    scan_from_ = read_buffer_.size();
    return false;
  }

  out_line->assign(read_buffer_, 0, newline);
  read_buffer_.erase(0, newline + 1);
  scan_from_ = 0;
  return true;
}

MpvIpcClient::PollResult MpvIpcClient::PollLine(std::string* out_line, int timeout_ms) {
  if (fd_ < 0) {
    last_error_ = "not connected";
    return PollResult::Error;
  }

  // drain whatever a previous read already pulled in before touching the
  // socket, otherwise a batch of messages would need one poll each
  if (TakeBufferedLine(out_line)) return PollResult::Line;

  const uint64_t deadline = MonotonicNanos() + MillisToNanos(timeout_ms);

  for (;;) {
    const uint64_t now = MonotonicNanos();
    int wait_ms = 0;
    if (deadline > now) wait_ms = static_cast<int>((deadline - now) / 1000000ull);

    pollfd pfd{fd_, POLLIN, 0};
    const int ready = poll(&pfd, 1, wait_ms);

    if (ready < 0) {
      if (errno == EINTR) {
        // a signal woke the poll. returning lets the caller check a shutdown
        // flag instead of getting stuck in an uninterruptible wait
        return PollResult::Timeout;
      }
      last_error_ = std::string("poll failed: ") + std::strerror(errno);
      return PollResult::Error;
    }
    if (ready == 0) return PollResult::Timeout;

    char chunk[kReadChunk];
    const ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);

    if (n == 0) return PollResult::Closed;
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (MonotonicNanos() >= deadline) return PollResult::Timeout;
        continue;
      }
      if (errno == EINTR) continue;
      last_error_ = std::string("recv failed: ") + std::strerror(errno);
      return PollResult::Error;
    }

    read_buffer_.append(chunk, static_cast<size_t>(n));
    if (read_buffer_.size() > kMaxBufferBytes) {
      last_error_ = "mpv sent more unframed data than expected, dropping the connection";
      return PollResult::Error;
    }

    if (TakeBufferedLine(out_line)) return PollResult::Line;
    if (MonotonicNanos() >= deadline) return PollResult::Timeout;
  }
}

}  // namespace framewire
