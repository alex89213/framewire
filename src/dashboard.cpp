/*
 * Description: Rendering for the live comparison dashboard and the plain text
 *   report.
 * Author: Alex Wu
 * Dependencies: framewire/dashboard.h
 * Usage:
 */

#include "framewire/dashboard.h"

#include "framewire/json.h"

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

  rows.push_back(Format("gpu    now %-8s p50 %-8s p99 %-8s p999 %s",
                        FormatNanos(s.gpu_last).c_str(), FormatNanos(s.gpu_recent.p50).c_str(),
                        FormatNanos(s.gpu_recent.p99).c_str(),
                        FormatNanos(s.gpu_recent.p999).c_str()));
  rows.push_back(Format("frame  now %-8s p50 %-8s p99 %-8s p999 %s",
                        FormatNanos(s.frame_last).c_str(),
                        FormatNanos(s.frame_time_recent.p50).c_str(),
                        FormatNanos(s.frame_time_recent.p99).c_str(),
                        FormatNanos(s.frame_time_recent.p999).c_str()));
  rows.push_back(Format("life   p50 %-8s p99 %-8s max  %s", FormatNanos(s.gpu_life.p50).c_str(),
                        FormatNanos(s.gpu_life.p99).c_str(), FormatNanos(s.gpu_life.max).c_str()));

  rows.push_back(Format("fps    %.1f now  %.1f avg   frames %llu", s.fps_recent, s.fps,
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

// Builds one compact row per stream, used when panels stop fitting.
std::vector<std::string> BuildStreamTable(const std::vector<StreamSnapshot>& streams,
                                          const ComparisonSnapshot& cmp,
                                          const DashboardState& st, size_t width) {
  std::vector<std::string> rows;
  const size_t name_room = width > 62 ? std::min<size_t>(28, width - 62) : 14;

  rows.push_back(Format("%s %-*s %9s %9s %9s %7s %7s %6s%s", Colour(st, ansi::kBold),
                        static_cast<int>(name_room), "stream", "gpu p50", "gpu p99", "delta",
                        "fps", "frames", "drop", Colour(st, ansi::kReset)));

  for (size_t i = 0; i < streams.size(); ++i) {
    const StreamSnapshot& s = streams[i];
    std::string name = s.label;
    if (name.size() > name_room) name.resize(name_room);

    std::string delta = "baseline";
    if (i < cmp.streams.size() && !cmp.streams[i].is_baseline) {
      delta = FormatSignedNanos(cmp.streams[i].delta_p50);
    }

    const char* accent = s.producer_alive ? "" : Colour(st, ansi::kRed);
    rows.push_back(Format("%s %-*s %9s %9s %9s %7.1f %7llu %5.1f%%%s", accent,
                          static_cast<int>(name_room), name.c_str(),
                          FormatNanos(s.gpu_recent.p50).c_str(),
                          FormatNanos(s.gpu_recent.p99).c_str(), delta.c_str(), s.fps_recent,
                          static_cast<unsigned long long>(s.records),
                          s.records > 0 ? 100.0 * static_cast<double>(s.dropped_frames) /
                                              static_cast<double>(s.records)
                                        : 0.0,
                          Colour(st, ansi::kReset)));
  }
  return rows;
}

void RenderDashboard(Screen& screen, const std::vector<StreamSnapshot>& streams,
                     const ComparisonSnapshot& cmp, const DashboardState& state) {
  const auto width = static_cast<size_t>(std::max(screen.width(), 20));
  const int height = screen.height();
  int y = 0;

  if (streams.empty()) return;

  const uint64_t secs = state.elapsed_ns / 1000000000ull;
  std::string title = Format(" framewire  %s  %02llu:%02llu:%02llu  %zu streams  grouped %llu ",
                             state.paused ? "PAUSED" : "live",
                             static_cast<unsigned long long>(secs / 3600),
                             static_cast<unsigned long long>((secs / 60) % 60),
                             static_cast<unsigned long long>(secs % 60), streams.size(),
                             static_cast<unsigned long long>(cmp.grouped));
  screen.PutRow(y++, Format("%s%s%s", Colour(state, ansi::kReverse),
                            FitWidth(title, width).c_str(), Colour(state, ansi::kReset)));
  screen.PutRow(y++, "");

  const bool two_panels = streams.size() == 2 && static_cast<int>(width) >= kMinSideBySide;

  if (two_panels) {
    const size_t col_width = (width - kPanelGap) / 2;
    const auto panel_a = BuildPanel(streams[0], state, col_width, ansi::kCyan);
    const auto panel_b = BuildPanel(streams[1], state, col_width, ansi::kMagenta);
    const size_t rows = std::max(panel_a.size(), panel_b.size());
    for (size_t i = 0; i < rows && y < height; ++i) {
      screen.PutRow(y++, TwoColumn(panel_a, panel_b, i, col_width));
    }
  } else if (streams.size() == 2) {
    for (const auto& s : streams) {
      for (const auto& row : BuildPanel(s, state, width, ansi::kCyan)) {
        if (y >= height) break;
        screen.PutRow(y++, FitWidth(row, width));
      }
      if (y < height) screen.PutRow(y++, "");
    }
  } else {
    for (const auto& row : BuildStreamTable(streams, cmp, state, width)) {
      if (y >= height) break;
      screen.PutRow(y++, FitWidth(row, width));
    }
  }

  if (y < height) screen.PutRow(y++, "");

  if (y < height) {
    const std::string heading = Format(" comparison against %s  ", streams[0].label.c_str());
    screen.PutRow(y++,
                  Format("%s%s%s", Colour(state, ansi::kBold),
                         FitWidth(heading + Repeat("─", width > heading.size() + 1
                                                            ? width - heading.size() - 1
                                                            : 0),
                                  width)
                             .c_str(),
                         Colour(state, ansi::kReset)));
  }

  if (cmp.grouped == 0) {
    if (y < height) {
      screen.PutRow(y++, Format("%s waiting for frames from every stream%s",
                                Colour(state, ansi::kGrey), Colour(state, ansi::kReset)));
    }
  } else {
    for (size_t i = 1; i < cmp.streams.size() && y < height - 1; ++i) {
      const StreamComparison& sc = cmp.streams[i];
      // cheaper than the baseline is the interesting direction, so it is the
      // one that gets the colour
      const char* dc = !sc.significant
                           ? Colour(state, ansi::kGrey)
                           : (sc.delta_p50 < 0 ? Colour(state, ansi::kGreen)
                                               : Colour(state, ansi::kYellow));
      const std::string interval =
          sc.interval_valid
              ? Format("[%s, %s]", FormatSignedNanos(sc.delta_p50_low).c_str(),
                       FormatSignedNanos(sc.delta_p50_high).c_str())
              : std::string("[too few]");
      screen.PutRow(y++, Format(" %-20s %s%-10s %-22s%s %5.1f%%  %.3fx%s", sc.label.c_str(), dc,
                                FormatSignedNanos(sc.delta_p50).c_str(), interval.c_str(),
                                Colour(state, ansi::kReset), sc.cheaper_fraction * 100.0,
                                sc.speedup,
                                sc.significant ? "" : "  (inside the noise)"));
    }

    if (y < height - 1) {
      std::string unmatched = " unmatched  ";
      for (size_t i = 0; i < cmp.unmatched.size(); ++i) {
        unmatched += Format("%s %llu   ", streams[i].label.c_str(),
                            static_cast<unsigned long long>(cmp.unmatched[i]));
      }
      screen.PutRow(y++, Format("%s%s%s", Colour(state, ansi::kGrey), unmatched.c_str(),
                                Colour(state, ansi::kReset)));
    }

    if (!cmp.pass_deltas.empty() && y < height - 2) {
      screen.PutRow(y++, "");
      if (y < height - 1) {
        screen.PutRow(y++, Format("%s per pass    %-20s %-10s %-10s %s%s",
                                  Colour(state, ansi::kBold), "name", "baseline", "other",
                                  "delta", Colour(state, ansi::kReset)));
      }
      for (const auto& d : cmp.pass_deltas) {
        if (y >= height - 1) break;
        std::string name = d.name;
        if (name.size() > 20) name.resize(20);
        const char* dc =
            d.delta_p50 < 0 ? Colour(state, ansi::kGreen) : Colour(state, ansi::kYellow);
        screen.PutRow(y++, Format("             %-20s %-10s %-10s %s%s%s", name.c_str(),
                                  FormatNanos(d.a_p50).c_str(), FormatNanos(d.b_p50).c_str(), dc,
                                  FormatSignedNanos(d.delta_p50).c_str(),
                                  Colour(state, ansi::kReset)));
      }
    }
  }

  const std::string keys = state.status.empty()
                               ? std::string(" q quit   p pause   r reset stats ")
                               : " " + state.status + " ";
  screen.PutRow(height - 1, Format("%s%s%s", Colour(state, ansi::kReverse),
                                   FitWidth(keys, width).c_str(), Colour(state, ansi::kReset)));
}

std::string BuildTextReport(const std::vector<StreamSnapshot>& streams,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns) {
  std::string out;
  auto line = [&out](const std::string& s) {
    out += s;
    out.push_back('\n');
  };

  line("framewire comparison report");
  line(Format("elapsed            %.1fs", static_cast<double>(elapsed_ns) / 1e9));
  line(Format("streams            %zu", streams.size()));
  line("");

  for (size_t i = 0; i < streams.size(); ++i) {
    const StreamSnapshot& s = streams[i];
    const char* tag = i == 0 ? "baseline" : "stream";
    if (s.environment.empty()) continue;
    line(Format("[%s %zu] %s, capture environment", tag, i, s.label.c_str()));
    for (const auto& kv : ParseEnvironment(s.environment)) {
      if (kv.second.empty()) continue;
      line(Format("  %-16s %s", kv.first.c_str(), kv.second.c_str()));
    }
    if (s.geometry_changes > 0) {
      line(Format("  %-16s %u times during the run", "window resized", s.geometry_changes));
    }
    line("");
  }

  // any pair captured under different conditions poisons the whole comparison,
  // so every stream is checked against the baseline
  std::vector<std::string> all_mismatches;
  for (size_t i = 1; i < streams.size(); ++i) {
    for (const auto& m : EnvironmentMismatches(streams[0].environment, streams[i].environment)) {
      all_mismatches.push_back(streams[i].label + " vs " + streams[0].label + ", " + m);
    }
  }
  if (!all_mismatches.empty()) {
    line("[warning] streams were not captured under the same conditions,");
    line("          so the comparison below is not a like for like measurement:");
    for (const auto& m : all_mismatches) line("  " + m);
    line("");
  }

  bool resized = false;
  for (const auto& s : streams) {
    if (s.geometry_changes > 0) resized = true;
  }
  if (resized) {
    line("[warning] a window changed size mid run, so timings before and after the");
    line("          change describe different render targets");
    line("");
  }

  // players rendering the same file should present at the same rate. a large
  // split means one window is not rendering normally, which on a compositor
  // that throttles hidden surfaces produces timings that look fast but
  // describe almost no work
  double fps_hi = 0.0;
  double fps_lo = 1e9;
  for (const auto& s : streams) {
    fps_hi = std::max(fps_hi, s.fps);
    fps_lo = std::min(fps_lo, s.fps);
  }
  if (fps_hi > 1.0 && (fps_hi - fps_lo) / fps_hi > 0.10) {
    line(Format("[warning] frame rates differ by %.0f%% across streams (%.1f to %.1f). one "
                "player may be occluded or throttled, so the comparison is not trustworthy",
                100.0 * (fps_hi - fps_lo) / fps_hi, fps_lo, fps_hi));
    line("");
  }

  for (size_t i = 0; i < streams.size(); ++i) {
    const StreamSnapshot& s = streams[i];
    line(Format("[%zu] %s%s", i, s.label.c_str(), i == 0 ? "  (baseline)" : ""));
    line(Format("  frames           %llu", static_cast<unsigned long long>(s.records)));
    line(Format("  fps              %.2f run, %.2f recent", s.fps, s.fps_recent));
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
    for (size_t p = 0; p < s.passes.size(); ++p) {
      const PassView& v = s.passes[p];
      line(Format("  pass %-2zu %-24s p50 %-9s p99 %s", p,
                  v.name.empty() ? "(unnamed)" : v.name.c_str(), FormatNanos(v.p50).c_str(),
                  FormatNanos(v.p99).c_str()));
    }
    line("");
  }

  line("[comparison] grouped frames only");
  line(Format("  grouped          %llu", static_cast<unsigned long long>(cmp.grouped)));
  for (size_t i = 0; i < cmp.unmatched.size() && i < streams.size(); ++i) {
    line(Format("  unmatched %-14s %llu", streams[i].label.c_str(),
                static_cast<unsigned long long>(cmp.unmatched[i])));
  }
  line("");
  line(Format("  %-22s %-11s %-11s %-24s %-22s %s", "stream", "gpu p50", "delta",
              "delta 95% interval", "cheaper on", "speedup"));
  for (const auto& sc : cmp.streams) {
    if (sc.is_baseline) {
      line(Format("  %-22s %-11s %-11s %-24s %-22s %s", sc.label.c_str(),
                  FormatNanos(sc.gpu_p50).c_str(), "baseline", "-", "-", "1.000x"));
      continue;
    }
    const std::string interval =
        sc.interval_valid ? Format("[%s, %s]", FormatSignedNanos(sc.delta_p50_low).c_str(),
                                   FormatSignedNanos(sc.delta_p50_high).c_str())
                          : std::string("too few samples");
    const std::string cheaper = Format("%.1f%% [%.1f, %.1f]", sc.cheaper_fraction * 100.0,
                                       sc.cheaper_low * 100.0, sc.cheaper_high * 100.0);
    line(Format("  %-22s %-11s %-11s %-24s %-22s %.3fx", sc.label.c_str(),
                FormatNanos(sc.gpu_p50).c_str(), FormatSignedNanos(sc.delta_p50).c_str(),
                interval.c_str(), cheaper.c_str(), sc.speedup));
  }

  // an interval that straddles zero means the run did not separate the two, and
  // saying so is more useful than quoting the point estimate and staying quiet
  line("");
  for (const auto& sc : cmp.streams) {
    if (sc.is_baseline) continue;
    if (!sc.interval_valid) {
      line(Format("  %s: too few grouped frames to say anything", sc.label.c_str()));
    } else if (!sc.significant) {
      line(Format("  %s: no separation from %s, the interval crosses zero",
                  sc.label.c_str(), cmp.streams[0].label.c_str()));
    } else {
      line(Format("  %s: %s than %s, and the whole interval agrees", sc.label.c_str(),
                  sc.delta_p50 < 0 ? "cheaper" : "dearer", cmp.streams[0].label.c_str()));
    }
  }

  for (const auto& d : cmp.pass_deltas) {
    line(Format("  pass %-24s baseline %-9s other %-9s delta %s",
                d.name.empty() ? "(unnamed)" : d.name.c_str(), FormatNanos(d.a_p50).c_str(),
                FormatNanos(d.b_p50).c_str(), FormatSignedNanos(d.delta_p50).c_str()));
  }
  return out;
}

std::string BuildJsonReport(const std::vector<StreamSnapshot>& streams,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns) {
  std::string out;

  auto stream_object = [](const StreamSnapshot& s, size_t index) {
    std::string o = "{\"index\":" + std::to_string(index) + ",\"label\":";
    JsonEscapeTo(o, s.label);
    o += Format(",\"frames\":%llu", static_cast<unsigned long long>(s.records));
    o += Format(",\"fps\":%.4f", s.fps);
    o += Format(",\"gpu_p50_ns\":%llu", static_cast<unsigned long long>(s.gpu_life.p50));
    o += Format(",\"gpu_p99_ns\":%llu", static_cast<unsigned long long>(s.gpu_life.p99));
    o += Format(",\"gpu_p999_ns\":%llu", static_cast<unsigned long long>(s.gpu_life.p999));
    o += Format(",\"gpu_max_ns\":%llu", static_cast<unsigned long long>(s.gpu_life.max));
    o += Format(",\"gpu_mean_ns\":%.1f", s.gpu_mean);
    o += Format(",\"frame_p50_ns\":%llu", static_cast<unsigned long long>(s.frame_time_life.p50));
    o += Format(",\"frame_p99_ns\":%llu", static_cast<unsigned long long>(s.frame_time_life.p99));
    o += Format(",\"dropped\":%llu", static_cast<unsigned long long>(s.dropped_frames));
    o += Format(",\"delayed\":%llu", static_cast<unsigned long long>(s.delayed_frames));
    o += Format(",\"ring_lost\":%llu", static_cast<unsigned long long>(s.producer_drops));
    o += Format(",\"checksum_errors\":%llu", static_cast<unsigned long long>(s.checksum_errors));
    o += Format(",\"sequence_gaps\":%llu", static_cast<unsigned long long>(s.sequence_gaps));
    o += Format(",\"geometry_changes\":%u", s.geometry_changes);

    o += ",\"environment\":{";
    bool first_env = true;
    for (const auto& kv : ParseEnvironment(s.environment)) {
      if (kv.second.empty()) continue;
      if (!first_env) o.push_back(',');
      first_env = false;
      JsonEscapeTo(o, kv.first);
      o.push_back(':');
      JsonEscapeTo(o, kv.second);
    }
    o.push_back('}');

    o += ",\"passes\":[";
    for (size_t i = 0; i < s.passes.size(); ++i) {
      if (i != 0) o.push_back(',');
      o += "{\"name\":";
      JsonEscapeTo(o, s.passes[i].name);
      o += Format(",\"p50_ns\":%llu", static_cast<unsigned long long>(s.passes[i].p50));
      o += Format(",\"p99_ns\":%llu", static_cast<unsigned long long>(s.passes[i].p99));
      o += Format(",\"samples\":%llu", static_cast<unsigned long long>(s.passes[i].samples));
      o.push_back('}');
    }
    o += "]}";
    return o;
  };

  out += "{\"schema\":\"framewire.cost.v2\"";
  out += Format(",\"elapsed_ns\":%llu", static_cast<unsigned long long>(elapsed_ns));

  out += ",\"streams\":[";
  for (size_t i = 0; i < streams.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += stream_object(streams[i], i);
  }
  out += "]";

  out += ",\"comparison\":{";
  out += Format("\"grouped\":%llu", static_cast<unsigned long long>(cmp.grouped));
  out += ",\"unmatched\":[";
  for (size_t i = 0; i < cmp.unmatched.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += std::to_string(cmp.unmatched[i]);
  }
  out += "],\"streams\":[";
  for (size_t i = 0; i < cmp.streams.size(); ++i) {
    const StreamComparison& sc = cmp.streams[i];
    if (i != 0) out.push_back(',');
    out += "{\"label\":";
    JsonEscapeTo(out, sc.label);
    out += Format(",\"is_baseline\":%s", sc.is_baseline ? "true" : "false");
    out += Format(",\"gpu_p50_ns\":%llu", static_cast<unsigned long long>(sc.gpu_p50));
    out += Format(",\"delta_p50_ns\":%lld", static_cast<long long>(sc.delta_p50));
    out += Format(",\"cheaper_fraction\":%.6f", sc.cheaper_fraction);
    out += Format(",\"speedup_vs_baseline\":%.6f", sc.speedup);
    out += Format(",\"delta_p50_ci95_low_ns\":%lld", static_cast<long long>(sc.delta_p50_low));
    out += Format(",\"delta_p50_ci95_high_ns\":%lld", static_cast<long long>(sc.delta_p50_high));
    out += Format(",\"cheaper_ci95_low\":%.6f", sc.cheaper_low);
    out += Format(",\"cheaper_ci95_high\":%.6f", sc.cheaper_high);
    out += Format(",\"interval_valid\":%s", sc.interval_valid ? "true" : "false");
    out += Format(",\"significant\":%s", sc.significant ? "true" : "false");
    out.push_back('}');
  }
  out += "]}";

  // a reader should be able to see the streams were not comparable without
  // working it out from the environment blocks
  out += ",\"environment_mismatches\":[";
  bool first_mismatch = true;
  for (size_t i = 1; i < streams.size(); ++i) {
    for (const auto& m : EnvironmentMismatches(streams[0].environment, streams[i].environment)) {
      if (!first_mismatch) out.push_back(',');
      first_mismatch = false;
      JsonEscapeTo(out, streams[i].label + " vs " + streams[0].label + ", " + m);
    }
  }
  out += "]}\n";
  return out;
}

}  // namespace framewire
