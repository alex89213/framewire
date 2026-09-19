/*
 * Description: Implementation of raw ANSI terminal control, the row diffing
 *   screen buffer and the small drawing helpers.
 * Author: Alex Wu
 * Dependencies: framewire/term.h
 * Usage:
 */

#include "framewire/term.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace framewire {
namespace {

// signal handlers may only touch a lock free flag of this type, so the state
// shared with the main loop is kept to exactly that
std::atomic<bool> g_resized{false};
std::atomic<bool> g_shutdown{false};

termios g_saved_termios{};
bool g_termios_saved = false;

void HandleWinch(int) { g_resized.store(true, std::memory_order_relaxed); }
void HandleShutdown(int) { g_shutdown.store(true, std::memory_order_relaxed); }

void WriteAll(std::string_view s) {
  size_t sent = 0;
  while (sent < s.size()) {
    const ssize_t n = write(STDOUT_FILENO, s.data() + sent, s.size() - sent);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return;
    }
    sent += static_cast<size_t>(n);
  }
}

// block characters used by the sparkline, from lowest to full height
const char* const kBlocks[8] = {"▁", "▂", "▃", "▄",
                                "▅", "▆", "▇", "█"};

}  // namespace

Terminal::~Terminal() { Leave(); }

void Terminal::InstallSignalHandlers() {
  struct sigaction sa {};
  sa.sa_handler = HandleWinch;
  sigemptyset(&sa.sa_mask);
  // SA_RESTART is deliberately left off so a blocking poll returns on a signal
  // and the main loop gets a chance to notice the shutdown flag
  sa.sa_flags = 0;
  sigaction(SIGWINCH, &sa, nullptr);

  sa.sa_handler = HandleShutdown;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // a closed pipe should surface as a failed write, not as a killed process
  signal(SIGPIPE, SIG_IGN);
}

bool Terminal::TakeResizeFlag() { return g_resized.exchange(false, std::memory_order_relaxed); }

bool Terminal::ShutdownRequested() { return g_shutdown.load(std::memory_order_relaxed); }

void Terminal::RequestShutdown() { g_shutdown.store(true, std::memory_order_relaxed); }

bool Terminal::Enter() {
  if (active_) return true;
  if (isatty(STDOUT_FILENO) == 0 || isatty(STDIN_FILENO) == 0) return false;

  if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) return false;
  g_termios_saved = true;

  termios raw = g_saved_termios;
  // turn off canonical mode and echo so keys arrive one at a time and do not
  // print over the dashboard
  raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;

  // alternate screen, then hide the cursor and clear
  WriteAll("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
  active_ = true;
  return true;
}

void Terminal::Leave() {
  if (!active_) {
    if (g_termios_saved) {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
      g_termios_saved = false;
    }
    return;
  }

  // show the cursor and drop back to the normal screen, so the shell looks
  // exactly as it did before the dashboard started
  WriteAll("\x1b[0m\x1b[?25h\x1b[?1049l");
  if (g_termios_saved) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    g_termios_saved = false;
  }
  active_ = false;
}

bool Terminal::GetSize(int* width, int* height) const {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
    *width = 100;
    *height = 30;
    return false;
  }
  *width = ws.ws_col;
  *height = ws.ws_row;
  return true;
}

int Terminal::ReadKey() const {
  if (!active_) return -1;
  char c = 0;
  const ssize_t n = read(STDIN_FILENO, &c, 1);
  if (n == 1) return static_cast<unsigned char>(c);
  return -1;
}

void Screen::BeginFrame(int width, int height) {
  if (width != width_ || height != height_) {
    width_ = width;
    height_ = height;
    front_.assign(static_cast<size_t>(height <= 0 ? 0 : height), std::string());
    force_redraw_ = true;
  }
  back_.assign(static_cast<size_t>(height <= 0 ? 0 : height), std::string());
}

void Screen::PutRow(int y, std::string content) {
  if (y < 0 || static_cast<size_t>(y) >= back_.size()) return;
  back_[static_cast<size_t>(y)] = std::move(content);
}

void Screen::Invalidate() { force_redraw_ = true; }

void Screen::EndFrame() {
  out_.clear();

  if (force_redraw_) {
    out_ += "\x1b[2J";  // clear everything once, then fall through to draw all rows
  }

  for (size_t y = 0; y < back_.size(); ++y) {
    if (!force_redraw_ && y < front_.size() && front_[y] == back_[y]) continue;

    // move to the row, clear to end of line, then write. clearing first means
    // a shorter row does not leave the tail of the previous frame behind
    char move[32];
    std::snprintf(move, sizeof(move), "\x1b[%zu;1H\x1b[K", y + 1);
    out_ += move;
    out_ += back_[y];
    out_ += ansi::kReset;
  }

  if (!out_.empty()) {
    // park the cursor at the bottom so a stray character lands somewhere
    // harmless rather than in the middle of the dashboard
    char park[32];
    std::snprintf(park, sizeof(park), "\x1b[%d;1H", height_);
    out_ += park;
    WriteAll(out_);
  }

  front_ = back_;
  force_redraw_ = false;
}

size_t DisplayWidth(std::string_view text) {
  size_t width = 0;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '\x1b') {
      // skip a control sequence, which prints nothing. sequences used here all
      // end in a letter, so scanning to the first alphabetic byte is enough
      ++i;
      if (i < text.size() && text[i] == '[') {
        ++i;
        while (i < text.size() && !((text[i] >= 'A' && text[i] <= 'Z') ||
                                    (text[i] >= 'a' && text[i] <= 'z'))) {
          ++i;
        }
      }
      if (i < text.size()) ++i;
      continue;
    }

    const auto byte = static_cast<unsigned char>(text[i]);
    // count one column per code point. every character drawn here is either
    // ascii or a single width block glyph, so no wide character table is needed
    if (byte < 0x80) {
      i += 1;
    } else if ((byte >> 5) == 0x6) {
      i += 2;
    } else if ((byte >> 4) == 0xE) {
      i += 3;
    } else if ((byte >> 3) == 0x1E) {
      i += 4;
    } else {
      i += 1;
    }
    ++width;
  }
  return width;
}

std::string FitWidth(std::string_view text, size_t width) {
  const size_t current = DisplayWidth(text);
  if (current == width) return std::string(text);
  if (current < width) return std::string(text) + std::string(width - current, ' ');

  // cut on a code point boundary and keep every escape code seen so far, so a
  // truncated row cannot leave the terminal stuck in a colour
  std::string out;
  size_t printed = 0;
  size_t i = 0;
  while (i < text.size() && printed < width) {
    if (text[i] == '\x1b') {
      const size_t start = i;
      ++i;
      if (i < text.size() && text[i] == '[') {
        ++i;
        while (i < text.size() &&
               !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))) {
          ++i;
        }
      }
      if (i < text.size()) ++i;
      out.append(text, start, i - start);
      continue;
    }

    const auto byte = static_cast<unsigned char>(text[i]);
    size_t len = 1;
    if ((byte >> 5) == 0x6) {
      len = 2;
    } else if ((byte >> 4) == 0xE) {
      len = 3;
    } else if ((byte >> 3) == 0x1E) {
      len = 4;
    }
    if (i + len > text.size()) break;
    out.append(text, i, len);
    i += len;
    ++printed;
  }
  return out;
}

std::string Sparkline(const uint64_t* values, size_t count) {
  if (count == 0) return {};

  uint64_t lo = UINT64_MAX;
  uint64_t hi = 0;
  bool any = false;
  for (size_t i = 0; i < count; ++i) {
    if (values[i] == 0) continue;  // an unfilled slot, not a real sample
    any = true;
    if (values[i] < lo) lo = values[i];
    if (values[i] > hi) hi = values[i];
  }
  if (!any) return std::string(count, ' ');

  std::string out;
  out.reserve(count * 3);
  const double span = static_cast<double>(hi - lo);

  for (size_t i = 0; i < count; ++i) {
    if (values[i] == 0) {
      out.push_back(' ');
      continue;
    }
    // a flat series has no span to scale against, so draw it mid height
    // rather than dividing by zero
    const double t = span > 0.0 ? static_cast<double>(values[i] - lo) / span : 0.5;
    auto level = static_cast<int>(t * 7.0 + 0.5);
    if (level < 0) level = 0;
    if (level > 7) level = 7;
    out += kBlocks[level];
  }
  return out;
}

std::string Bar(double fraction, size_t width) {
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;

  const auto filled = static_cast<size_t>(fraction * static_cast<double>(width));
  std::string out;
  out.reserve(width * 3);
  for (size_t i = 0; i < width; ++i) out += (i < filled) ? kBlocks[7] : "░";
  return out;
}

}  // namespace framewire
