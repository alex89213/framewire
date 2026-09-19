/*
 * Description: Side by side comparison view drawn from the stream snapshots,
 *   plus the plain text report used when output is not a terminal.
 * Author: Alex Wu
 * Dependencies: framewire/stats.h, framewire/term.h
 * Usage:
 */

#pragma once

#include <cstdint>
#include <string>

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
 * Layout is chosen from the terminal width. A wide terminal gets the two
 * streams side by side, and a narrow one stacks them, so the view stays
 * readable in a split pane.
 *
 * Args:
 *   screen: Destination screen buffer, already sized for the frame.
 *   a: Snapshot for the first stream.
 *   b: Snapshot for the second stream.
 *   cmp: Paired frame comparison.
 *   state: Elapsed time and view flags.
 */
void RenderDashboard(Screen& screen, const StreamSnapshot& a, const StreamSnapshot& b,
                     const ComparisonSnapshot& cmp, const DashboardState& state);

/*
 * Builds a plain text report with no escape codes.
 *
 * Used when stdout is a pipe or a file, and printed once at exit so a
 * benchmark run leaves numbers behind in the scrollback.
 *
 * Args:
 *   a: Snapshot for the first stream.
 *   b: Snapshot for the second stream.
 *   cmp: Paired frame comparison.
 *   elapsed_ns: Wall time the run covered.
 * Returns:
 *   A multi line report.
 */
std::string BuildTextReport(const StreamSnapshot& a, const StreamSnapshot& b,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns);

/*
 * Builds the same report as JSON.
 *
 * Exists so a quality pass can join against cost numbers by key instead of
 * scraping the text report with regular expressions.
 *
 * Args:
 *   a: Snapshot for the first stream.
 *   b: Snapshot for the second stream.
 *   cmp: Paired frame comparison.
 *   elapsed_ns: Wall time the run covered.
 * Returns:
 *   A JSON document ending in a newline.
 */
std::string BuildJsonReport(const StreamSnapshot& a, const StreamSnapshot& b,
                            const ComparisonSnapshot& cmp, uint64_t elapsed_ns);

}  // namespace framewire
