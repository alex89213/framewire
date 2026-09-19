/*
 * Description: Comparison view drawn from the stream snapshots, plus the plain
 *   text and JSON reports.
 * Author: Alex Wu
 * Dependencies: framewire/stats.h, framewire/term.h
 * Usage:
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "framewire/stats.h"
#include "framewire/term.h"

namespace framewire {

struct DashboardState {
  uint64_t elapsed_ns = 0;
  uint64_t refreshes = 0;
  bool paused = false;
  bool color = true;
  std::string status;  // short message shown in the footer
};

/*
 * Draws the comparison dashboard into a screen buffer.
 *
 * Two streams get the side by side panel view, which has room for the full
 * pass breakdown. More than two get a row per stream instead, because panels
 * stop fitting and the per stream detail stops being the interesting part once
 * there is a ranking to read.
 *
 * Args:
 *   screen: Destination screen buffer, already sized for the frame.
 *   streams: One snapshot per stream, baseline first.
 *   cmp: Comparison across grouped frames.
 *   state: Elapsed time and view flags.
 */
void RenderDashboard(Screen& screen, const std::vector<StreamSnapshot>& streams,
                     const ComparisonSnapshot& cmp, const DashboardState& state);

/*
 * Builds a plain text report with no escape codes.
 *
 * Used when stdout is a pipe or a file, and printed once at exit so a
 * benchmark run leaves numbers behind in the scrollback.
 *
 * Args:
 *   streams: One snapshot per stream, baseline first.
 *   cmp: Comparison across grouped frames.
 *   elapsed_ns: Wall time the run covered.
 * Returns:
 *   A multi line report.
 */
std::string BuildTextReport(const std::vector<StreamSnapshot>& streams,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns);

/*
 * Builds the same report as JSON.
 *
 * Exists so a quality pass can join against cost numbers by key instead of
 * scraping the text report with regular expressions.
 *
 * Args:
 *   streams: One snapshot per stream, baseline first.
 *   cmp: Comparison across grouped frames.
 *   elapsed_ns: Wall time the run covered.
 * Returns:
 *   A JSON document ending in a newline.
 */
std::string BuildJsonReport(const std::vector<StreamSnapshot>& streams,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns);

}  // namespace framewire
