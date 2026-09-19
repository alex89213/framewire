/*
 * Description: Raw ANSI terminal control and a row diffing screen buffer, used
 *   instead of a curses library so the tool carries no external dependency.
 * Author: Alex Wu
 * Dependencies:
 * Usage:
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace framewire {

// ANSI select graphic rendition codes, kept as short names for readability
namespace ansi {
inline constexpr const char* kReset = "\x1b[0m";
inline constexpr const char* kBold = "\x1b[1m";
inline constexpr const char* kDim = "\x1b[2m";
inline constexpr const char* kReverse = "\x1b[7m";

inline constexpr const char* kRed = "\x1b[31m";
inline constexpr const char* kGreen = "\x1b[32m";
inline constexpr const char* kYellow = "\x1b[33m";
inline constexpr const char* kBlue = "\x1b[34m";
inline constexpr const char* kMagenta = "\x1b[35m";
inline constexpr const char* kCyan = "\x1b[36m";
inline constexpr const char* kWhite = "\x1b[37m";
inline constexpr const char* kGrey = "\x1b[90m";
}  // namespace ansi

/*
 * Owns the terminal mode for the life of the dashboard.
 *
 * Entering switches to the alternate screen, turns off echo and line
 * buffering, and hides the cursor. Leaving puts all of that back. The
 * destructor also restores, so a crash or a signal still gives the user a
 * usable shell instead of a terminal with echo turned off.
 */
class Terminal {
 public:
  Terminal() = default;
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;
  ~Terminal();

  /*
   * Switches the terminal into dashboard mode.
   *
   * Returns:
   *   True when the terminal was switched, false when output is not a tty.
   */
  bool Enter();

  void Leave();

  /*
   * Reads the current terminal size.
   *
   * Args:
   *   width: Receives the column count.
   *   height: Receives the row count.
   * Returns:
   *   True when the size was read.
   */
  bool GetSize(int* width, int* height) const;

  /*
   * Reads one keypress without blocking.
   *
   * Returns:
   *   The character read, or negative one when no key is waiting.
   */
  int ReadKey() const;

  bool active() const { return active_; }

  /*
   * Reports whether the terminal was resized since the last check.
   *
   * Returns:
   *   True once per resize, and clears the flag.
   */
  static bool TakeResizeFlag();

  // Installs handlers for interrupt, terminate and window change.
  static void InstallSignalHandlers();

  /*
   * Reports whether a shutdown signal arrived.
   *
   * Returns:
   *   True after interrupt or terminate.
   */
  static bool ShutdownRequested();

  // Asks any running loop to stop, used when a producer disappears.
  static void RequestShutdown();

 private:
  bool active_ = false;
};

/*
 * Double buffered screen that only writes rows that changed.
 *
 * Rows are held as fully formatted strings including escape codes. Comparing
 * whole rows is coarse, but a dashboard row changes as a unit anyway, and the
 * approach avoids tracking per cell attributes. A refresh where nothing moved
 * costs zero bytes on the wire, so an idle dashboard does not keep the
 * terminal busy.
 */
class Screen {
 public:
  /*
   * Starts a frame and sizes the buffer.
   *
   * Args:
   *   width: Column count.
   *   height: Row count.
   */
  void BeginFrame(int width, int height);

  /*
   * Sets the content of one row.
   *
   * Args:
   *   y: Row index from the top, starting at zero.
   *   content: Row text, may contain escape codes.
   */
  void PutRow(int y, std::string content);

  // Writes only the rows that differ from the previous frame.
  void EndFrame();

  // Forces every row to be rewritten on the next frame, used after a resize.
  void Invalidate();

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  std::vector<std::string> front_;
  std::vector<std::string> back_;
  std::string out_;
  int width_ = 0;
  int height_ = 0;
  bool force_redraw_ = true;
};

/*
 * Counts the printed width of a string, skipping escape codes.
 *
 * Args:
 *   text: String that may contain escape codes and multi byte characters.
 * Returns:
 *   Number of terminal columns the string occupies.
 */
size_t DisplayWidth(std::string_view text);

/*
 * Pads or cuts a string to an exact printed width.
 *
 * Args:
 *   text: String that may contain escape codes.
 *   width: Target column count.
 * Returns:
 *   A string that prints in exactly width columns.
 */
std::string FitWidth(std::string_view text, size_t width);

/*
 * Draws a sparkline from a series of values.
 *
 * Args:
 *   values: Samples to draw, oldest first.
 *   count: Number of samples.
 * Returns:
 *   A string of block characters, one column per sample.
 */
std::string Sparkline(const uint64_t* values, size_t count);

/*
 * Draws a horizontal bar of a given filled fraction.
 *
 * Args:
 *   fraction: Fill level, 0.0 through 1.0.
 *   width: Total column count for the bar.
 * Returns:
 *   A string of block characters.
 */
std::string Bar(double fraction, size_t width);

}  // namespace framewire
