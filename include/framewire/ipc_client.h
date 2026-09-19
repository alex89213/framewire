/*
 * Description: Client for the mpv JSON IPC socket, handling connect, command
 *   writes and newline framed reads without blocking the caller.
 * Author: Alex Wu
 * Dependencies: framewire/json.h
 * Usage:
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace framewire {

/*
 * Talks to one mpv instance over the unix socket from --input-ipc-server.
 *
 * mpv frames messages with newlines and may hand back several messages in one
 * read, or half of one. The client holds a byte buffer and only surfaces
 * complete lines, so callers never see a partial JSON document.
 *
 * The socket is kept non blocking and every wait goes through poll, because
 * the producer also has to send heartbeats on a timer and a blocking read
 * would stall that.
 */
class MpvIpcClient {
 public:
  enum class PollResult {
    Line,     // a complete message is available in the out parameter
    Timeout,  // nothing arrived before the deadline
    Closed,   // mpv closed the socket
    Error,    // the socket failed
  };

  MpvIpcClient() = default;
  MpvIpcClient(const MpvIpcClient&) = delete;
  MpvIpcClient& operator=(const MpvIpcClient&) = delete;
  ~MpvIpcClient();

  /*
   * Connects to an mpv IPC socket, retrying until a deadline.
   *
   * Retrying matters because mpv creates the socket a moment after launch, so
   * a producer started at the same time as mpv would otherwise lose the race.
   *
   * Args:
   *   socket_path: Path passed to mpv as --input-ipc-server.
   *   timeout_ms: How long to keep retrying before giving up.
   * Returns:
   *   True once connected.
   */
  bool Connect(const std::string& socket_path, int timeout_ms);

  /*
   * Sends one command to mpv.
   *
   * Args:
   *   args: Command words, sent as the command array.
   *   request_id: Value mpv echoes back in the reply.
   * Returns:
   *   True when the whole line reached the socket.
   */
  bool SendCommand(const std::vector<std::string>& args, int64_t request_id);

  /*
   * Asks mpv to push updates for a property.
   *
   * Args:
   *   observe_id: Identifier mpv puts on every change event for the property.
   *   property: Property name, for example vo-passes.
   * Returns:
   *   True when the request was sent.
   */
  bool ObserveProperty(int64_t observe_id, const std::string& property);

  /*
   * Asks mpv for the current value of a property.
   *
   * Needed because mpv accepts an observe request for vo-passes but then only
   * ever sends the value once. Per frame pass timings have to be pulled.
   *
   * Args:
   *   property: Property name to read.
   *   request_id: Value mpv echoes back, used to match the reply.
   * Returns:
   *   True when the request was sent.
   */
  bool GetProperty(const std::string& property, int64_t request_id);

  /*
   * Waits for the next complete message.
   *
   * Args:
   *   out_line: Receives one message with the newline stripped.
   *   timeout_ms: How long to wait, zero returns immediately.
   * Returns:
   *   What happened while waiting.
   */
  PollResult PollLine(std::string* out_line, int timeout_ms);

  void Close();
  bool connected() const { return fd_ >= 0; }
  const std::string& last_error() const { return last_error_; }

 private:
  // Pulls one already buffered line out of the read buffer.
  bool TakeBufferedLine(std::string* out_line);

  // Writes one complete newline terminated line to the socket.
  bool SendRaw(const std::string& line);

  int fd_ = -1;
  std::string read_buffer_;
  size_t scan_from_ = 0;
  std::string last_error_;
};

}  // namespace framewire
