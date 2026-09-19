/*
 * Description: Rendering for the live comparison dashboard and the plain text
 *   report.
 * Author: Alex Wu
 * Dependencies: framewire/dashboard.h
 * Usage:
 */

#include "framewire/dashboard.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace framewire {
namespace {

// below this width the two panels stop fitting next to each other
constexpr int kMinSideBySide = 88;
constexpr int kPanelGap = 2;

std::string Repeat(const char* glyph, size_t n) {
  std::string out;
  out.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) out += glyph;
  return out;
}

const char* Colour(const DashboardState& st, const char* code) {
  return st.color ? code : "";
}

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return buf;
}

// Builds the block of lines describing one stream.
std::vector<std::string> BuildPanel(const StreamSnapshot& s, const DashboardState& st,
                                    size_t width, const char* accent) {
  std::vector<std::string> rows;

  const char* live = s.producer_alive ? Colour(st, ansi::kGreen) : Colour(st, ansi::kRed);
  const char* live_text = s.producer_alive ? "live" : "gone";

  rows.push_back(Format("%s%s%s%s %s%s%s", Colour(st, ansi::kBold), Colour(st, accent),
                        s.label.c_str(), Colour(st, ansi::kReset), live, live_text,
                        Colour(st, ansi::kReset)));
  rows.push_back(Format("%s%s%s", Colour(st, ansi::kGrey), Repeat("─", width).c_str(),
                        Colour(st, ansi::kReset)));

  rows.push_back(Format("gpu    p50 %-8s p99 %-8s p999 %s", FormatNanos(s.gpu_recent.p50).c_str(),
                        FormatNanos(s.gpu_recent.p99).c_str(),
                        FormatNanos(s.gpu_recent.p999).c_str()));
  rows.push_back(Format("frame  p50 %-8s p99 %-8s p999 %s",
                        FormatNanos(s.frame_time_recent.p50).c_str(),
                        FormatNanos(s.frame_time_recent.p99).c_str(),
                        FormatNanos(s.frame_time_recent.p999).c_str()));
  rows.push_back(Format("life   p50 %-8s p99 %-8s max  %s", FormatNanos(s.gpu_life.p50).c_str(),
                        FormatNanos(s.gpu_life.p99).c_str(), FormatNanos(s.gpu_life.max).c_str()));

  rows.push_back(Format("fps    %-8.1f frames %-10llu", s.fps,
                        static_cast<unsigned long long>(s.records)));

  const double drop_rate =
      s.records > 0 ? static_cast<double>(s.dropped_frames) * 100.0 / static_cast<double>(s.records)
                    : 0.0;
  const char* drop_colour = s.dropped_frames > 0 ? Colour(st, ansi::kYellow) : "";
  rows.push_back(Format("%sdropped %llu (%.2f%%)  delayed %llu%s", drop_colour,
                        static_cast<unsigned long long>(s.dropped_frames), drop_rate,
                        static_cast<unsigned long long>(s.delayed_frames),
                        Colour(st, ansi::kReset)));

  // ring health. a non zero lost or crc count is the signal that the transport
  // itself is in trouble, so the row turns red rather than blending in
  const bool ring_bad = s.producer_drops > 0 || s.checksum_errors > 0;
  rows.push_back(Format("%sring   pend %-5llu lost %-5llu crc %llu gaps %llu%s",
                        ring_bad ? Colour(st, ansi::kRed) : Colour(st, ansi::kGrey),
                        static_cast<unsigned long long>(s.ring_pending),
                        static_cast<unsigned long long>(s.producer_drops),
                        static_cast<unsigned long long>(s.checksum_errors),
                        static_cast<unsigned long long>(s.sequence_gaps),
                        Colour(st, ansi::kReset)));

  const size_t spark_width = std::min<size_t>(width, s.spark.size());
  if (spark_width > 0) {
    std::vector<uint64_t> tail(s.spark.end() - static_cast<ptrdiff_t>(spark_width), s.spark.end());
    rows.push_back(Format("%s%s%s", Colour(st, accent),
                          Sparkline(tail.data(), tail.size()).c_str(),
                          Colour(st, ansi::kReset)));
  }

  rows.push_back("");
  rows.push_back(Format("%spasses%s", Colour(st, ansi::kBold), Colour(st, ansi::kReset)));
  if (s.passes.empty()) {
    rows.push_back(Format("%s  waiting for vo-passes%s", Colour(st, ansi::kGrey),
                          Colour(st, ansi::kReset)));
  }
  for (size_t i = 0; i < s.passes.size(); ++i) {
    const PassView& p = s.passes[i];
    // the name column is capped rather than given all the leftover space, so
    // the timing columns stay near the names instead of drifting to the far
    // edge of a wide panel
    const size_t name_room = std::min<size_t>(28, width > 26 ? width - 26 : 8);
    std::string name = p.name.empty() ? std::string("pass ") + std::to_string(i) : p.name;
    if (name.size() > name_room) name.resize(name_room);
    rows.push_back(Format(" %-*s %-8s %s", static_cast<int>(name_room), name.c_str(),
                          FormatNanos(p.p50).c_str(), FormatNanos(p.p99).c_str()));
  }
  return rows;
}

std::string TwoColumn(const std::vector<std::string>& left, const std::vector<std::string>& right,
                      size_t index, size_t col_width) {
  const std::string l = index < left.size() ? left[index] : std::string();
  const std::string r = index < right.size() ? right[index] : std::string();
  return FitWidth(l, col_width) + std::string(kPanelGap, ' ') + FitWidth(r, col_width);
}

}  // namespace

void RenderDashboard(Screen& screen, const StreamSnapshot& a, const StreamSnapshot& b,
                     const ComparisonSnapshot& cmp, const DashboardState& state) {
  const auto width = static_cast<size_t>(std::max(screen.width(), 20));
  const int height = screen.height();
  int y = 0;

  const uint64_t secs = state.elapsed_ns / 1000000000ull;
  std::string title = Format(" framewire  %s  %02llu:%02llu:%02llu  paired %llu ",
                             state.paused ? "PAUSED" : "live",
                             static_cast<unsigned long long>(secs / 3600),
                             static_cast<unsigned long long>((secs / 60) % 60),
                             static_cast<unsigned long long>(secs % 60),
                             static_cast<unsigned long long>(cmp.paired));
  screen.PutRow(y++, Format("%s%s%s", Colour(state, ansi::kReverse),
                            FitWidth(title, width).c_str(), Colour(state, ansi::kReset)));
  screen.PutRow(y++, "");

  const bool side_by_side = static_cast<int>(width) >= kMinSideBySide;
  const size_t col_width = side_by_side ? (width - kPanelGap) / 2 : width;

  const auto panel_a = BuildPanel(a, state, col_width, ansi::kCyan);
  const auto panel_b = BuildPanel(b, state, col_width, ansi::kMagenta);

  if (side_by_side) {
    const size_t rows = std::max(panel_a.size(), panel_b.size());
    for (size_t i = 0; i < rows && y < height; ++i) {
      screen.PutRow(y++, TwoColumn(panel_a, panel_b, i, col_width));
    }
  } else {
    for (const auto& row : panel_a) {
      if (y >= height) break;
      screen.PutRow(y++, FitWidth(row, width));
    }
    if (y < height) screen.PutRow(y++, "");
    for (const auto& row : panel_b) {
      if (y >= height) break;
      screen.PutRow(y++, FitWidth(row, width));
    }
  }

  if (y < height) screen.PutRow(y++, "");

  // comparison block
  if (y < height) {
    const std::string heading = Format(" comparison  b minus a  ");
    screen.PutRow(y++, Format("%s%s%s", Colour(state, ansi::kBold),
                              FitWidth(heading + Repeat("─", width > heading.size() + 1
                                                                 ? width - heading.size() - 1
                                                                 : 0),
                                       width)
                                  .c_str(),
                              Colour(state, ansi::kReset)));
  }

  if (cmp.paired == 0) {
    if (y < height) {
      screen.PutRow(y++, Format("%s waiting for frames from both streams%s",
                                Colour(state, ansi::kGrey), Colour(state, ansi::kReset)));
    }
  } else {
    // a negative delta means stream b spent less gpu time, so b is the winner
    const bool b_wins = cmp.gpu_delta_p50 < 0;
    const char* verdict_colour = b_wins ? Colour(state, ansi::kMagenta) : Colour(state, ansi::kCyan);
    const char* winner = b_wins ? b.label.c_str() : a.label.c_str();

    if (y < height) {
      screen.PutRow(y++, Format(" gpu delta   p50 %-10s p99 %-10s mean %s",
                                FormatSignedNanos(cmp.gpu_delta_p50).c_str(),
                                FormatSignedNanos(cmp.gpu_delta_p99).c_str(),
                                FormatSignedNanos(static_cast<int64_t>(cmp.gpu_delta_mean)).c_str()));
    }
    if (y < height) {
      screen.PutRow(y++,
                    Format(" %sfaster%s      %s%s%s on %.1f%% of paired frames   speedup %.3fx",
                           Colour(state, ansi::kBold), Colour(state, ansi::kReset), verdict_colour,
                           winner, Colour(state, ansi::kReset),
                           b_wins ? (1.0 - cmp.a_faster_fraction) * 100.0
                                  : cmp.a_faster_fraction * 100.0,
                           cmp.speedup));
    }
    if (y < height) {
      screen.PutRow(y++, Format("%s unmatched   a %llu   b %llu%s", Colour(state, ansi::kGrey),
                                static_cast<unsigned long long>(cmp.unmatched_a),
                                static_cast<unsigned long long>(cmp.unmatched_b),
                                Colour(state, ansi::kReset)));
    }

    if (!cmp.pass_deltas.empty() && y < height) {
      screen.PutRow(y++, "");
      if (y < height) {
        screen.PutRow(y++, Format("%s per pass    %-20s %-10s %-10s %s%s",
                                  Colour(state, ansi::kBold), "name", "a p50", "b p50", "delta",
                                  Colour(state, ansi::kReset)));
      }
      for (const auto& d : cmp.pass_deltas) {
        if (y >= height - 1) break;
        std::string name = d.name;
        if (name.size() > 20) name.resize(20);
        const char* dc = d.delta_p50 < 0 ? Colour(state, ansi::kMagenta)
                                         : Colour(state, ansi::kCyan);
        screen.PutRow(y++, Format("             %-20s %-10s %-10s %s%s%s", name.c_str(),
                                  FormatNanos(d.a_p50).c_str(), FormatNanos(d.b_p50).c_str(), dc,
                                  FormatSignedNanos(d.delta_p50).c_str(),
                                  Colour(state, ansi::kReset)));
      }
    }
  }

  // footer pinned to the last row
  const std::string keys = state.status.empty()
                               ? std::string(" q quit   p pause   r reset stats ")
                               : " " + state.status + " ";
  screen.PutRow(height - 1, Format("%s%s%s", Colour(state, ansi::kReverse),
                                   FitWidth(keys, width).c_str(), Colour(state, ansi::kReset)));
}

std::string BuildTextReport(const StreamSnapshot& a, const StreamSnapshot& b,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns) {
  std::string out;

  auto line = [&out](const std::string& s) {
    out += s;
    out.push_back('\n');
  };

  line("framewire comparison report");
  line(Format("elapsed            %.1fs", static_cast<double>(elapsed_ns) / 1e9));
  line("");

  auto stream_block = [&](const StreamSnapshot& s, const char* tag) {
    line(Format("[%s] %s", tag, s.label.c_str()));
    line(Format("  frames           %llu", static_cast<unsigned long long>(s.records)));
    line(Format("  fps              %.2f", s.fps));
    line(Format("  gpu p50/p99/p999 %s / %s / %s", FormatNanos(s.gpu_life.p50).c_str(),
                FormatNanos(s.gpu_life.p99).c_str(), FormatNanos(s.gpu_life.p999).c_str()));
    line(Format("  gpu max          %s", FormatNanos(s.gpu_life.max).c_str()));
    line(Format("  gpu mean         %s", FormatNanos(static_cast<uint64_t>(s.gpu_mean)).c_str()));
    line(Format("  frame p50/p99    %s / %s", FormatNanos(s.frame_time_life.p50).c_str(),
                FormatNanos(s.frame_time_life.p99).c_str()));
    const double drop_rate =
        s.records > 0
            ? static_cast<double>(s.dropped_frames) * 100.0 / static_cast<double>(s.records)
            : 0.0;
    line(Format("  dropped          %llu (%.3f%%)",
                static_cast<unsigned long long>(s.dropped_frames), drop_rate));
    line(Format("  delayed          %llu", static_cast<unsigned long long>(s.delayed_frames)));
    line(Format("  ring lost        %llu", static_cast<unsigned long long>(s.producer_drops)));
    line(Format("  checksum errors  %llu", static_cast<unsigned long long>(s.checksum_errors)));
    line(Format("  sequence gaps    %llu", static_cast<unsigned long long>(s.sequence_gaps)));
    for (size_t i = 0; i < s.passes.size(); ++i) {
      const PassView& p = s.passes[i];
      line(Format("  pass %-2zu %-24s p50 %-9s p99 %s", i,
                  p.name.empty() ? "(unnamed)" : p.name.c_str(), FormatNanos(p.p50).c_str(),
                  FormatNanos(p.p99).c_str()));
    }
    line("");
  };

  stream_block(a, "a");
  stream_block(b, "b");

  line("[comparison] paired frames only");
  line(Format("  paired           %llu", static_cast<unsigned long long>(cmp.paired)));
  line(Format("  unmatched a / b  %llu / %llu", static_cast<unsigned long long>(cmp.unmatched_a),
              static_cast<unsigned long long>(cmp.unmatched_b)));
  line(Format("  gpu delta p50    %s", FormatSignedNanos(cmp.gpu_delta_p50).c_str()));
  line(Format("  gpu delta p99    %s", FormatSignedNanos(cmp.gpu_delta_p99).c_str()));
  line(Format("  gpu delta mean   %s",
              FormatSignedNanos(static_cast<int64_t>(cmp.gpu_delta_mean)).c_str()));
  line(Format("  a faster on      %.2f%% of pairs", cmp.a_faster_fraction * 100.0));
  line(Format("  speedup a over b %.4fx", cmp.speedup));

  for (const auto& d : cmp.pass_deltas) {
    line(Format("  pass %-24s a %-9s b %-9s delta %s",
                d.name.empty() ? "(unnamed)" : d.name.c_str(), FormatNanos(d.a_p50).c_str(),
                FormatNanos(d.b_p50).c_str(), FormatSignedNanos(d.delta_p50).c_str()));
  }
  return out;
}

}  // namespace framewire
