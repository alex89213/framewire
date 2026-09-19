#!/usr/bin/env bash
#
# Description: Runs the ring buffer stress harness across a few ring sizes and
#   prints a table, used to produce the numbers in the readme.
# Author: Alex Wu
# Dependencies: framewire binaries
# Usage: scripts/bench.sh [RECORDS]
#

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
RECORDS="${1:-20000000}"
STRESS="$BUILD_DIR/framewire-stress"

if [[ ! -x "$STRESS" ]]; then
  echo "missing $STRESS, build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

echo "framewire ring benchmark"
echo "  records per run: $RECORDS"
echo "  cpu:             $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "  kernel:          $(uname -r)"
echo
# two modes on purpose. the integrity run rebuilds and compares every record,
# which is the right cost for proving the queue never tears but says more about
# the checks than the queue. the throughput run turns the checks off and times
# the queue itself
for mode in throughput integrity; do
  flag=""
  [ "$mode" = throughput ] && flag="--no-verify"
  echo "mode: $mode"
  printf '  %-10s %14s %14s %12s %10s\n' "capacity" "M records/s" "MiB/s" "ns/record" "verdict"
  printf '  %-10s %14s %14s %12s %10s\n' "--------" "-----------" "-----" "---------" "-------"

for capacity in 256 1024 4096 16384 65536; do
  output=$("$STRESS" --records "$RECORDS" --capacity "$capacity" \
             --shm "/framewire-bench-$capacity" --quiet $flag)

  # anchored on the leading spaces, otherwise "throughput" also matches the
  # section heading of the same name and the field comes back empty
  rate=$(awk '/^  rate /{print $2}' <<<"$output")
  mib=$(awk '/^  throughput /{print $2}' <<<"$output")
  nsper=$(awk '/^  ns per record /{print $4}' <<<"$output")
  verdict=$(grep -oE '^(PASS|FAIL)' <<<"$output" || echo "FAIL")

  printf '  %-10s %14s %14s %12s %10s\n' "$capacity" "$rate" "$mib" "$nsper" "$verdict"
done
echo
done

echo "backpressure behaviour with a deliberately slow consumer"
"$STRESS" --records 2000000 --capacity 1024 --slow-consumer 50000 \
  --shm /framewire-bench-slow | tail -n 14
