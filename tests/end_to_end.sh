#!/usr/bin/env bash
#
# Description: Headless end to end test. Drives the mock mpv server through two
#   producers, a ring each, the aggregator and the report, then checks the
#   output for lossless capture and a sane comparison.
# Author: Alex Wu
# Dependencies: framewire binaries
# Usage: tests/end_to_end.sh BUILD_DIR
#

set -euo pipefail

BUILD_DIR="${1:-build}"
SOCK_A=/tmp/framewire-e2e-a-$$.sock
SOCK_B=/tmp/framewire-e2e-b-$$.sock
SHM_A=/framewire-e2e-a-$$
SHM_B=/framewire-e2e-b-$$
REPORT=$(mktemp /tmp/framewire-e2e-XXXXXX.json)
DURATION=4
FPS=120

PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -f "$SOCK_A" "$SOCK_B" "$REPORT"
  rm -f "/dev/shm${SHM_A}" "/dev/shm${SHM_B}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }

# this is the only test that exercises the whole chain in one process tree. it
# runs headless on purpose, because a machine building the project will not
# have a GPU, a display or a video file
"$BUILD_DIR/framewire-mock-mpv" --socket "$SOCK_A" --profile espcn --fps "$FPS" \
  --duration $((DURATION + 3)) --seed 11 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-mock-mpv" --socket "$SOCK_B" --profile espcn-heavy --fps "$FPS" \
  --duration $((DURATION + 3)) --seed 22 2>/dev/null &
PIDS+=($!)
sleep 0.5

"$BUILD_DIR/framewire-producer" --socket "$SOCK_A" --shm "$SHM_A" --label light 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-producer" --socket "$SOCK_B" --shm "$SHM_B" --label heavy 2>/dev/null &
PIDS+=($!)
sleep 0.5

"$BUILD_DIR/framewire" --shm-a "$SHM_A" --shm-b "$SHM_B" \
  --duration "$DURATION" --plain --json "$REPORT" >/dev/null 2>&1

[[ -s "$REPORT" ]] || fail "no json report was written"

# python is only used to read the report, the tool itself needs none
python3 - "$REPORT" <<'CHECKS' || exit 1
import json, sys

doc = json.load(open(sys.argv[1]))
problems = []

if doc.get("schema") != "framewire.cost.v1":
    problems.append(f"unexpected schema {doc.get('schema')}")

for side in ("a", "b"):
    s = doc[side]
    if s["frames"] < 100:
        problems.append(f"{side}: only {s['frames']} frames captured, expected a few hundred")
    for counter in ("ring_lost", "checksum_errors", "sequence_gaps"):
        if s[counter] != 0:
            problems.append(f"{side}: {counter} is {s[counter]}, capture was not lossless")
    if not s["passes"]:
        problems.append(f"{side}: no pass breakdown came through")
    if s["gpu_p50_ns"] <= 0:
        problems.append(f"{side}: gpu p50 is {s['gpu_p50_ns']}")

cmp = doc["comparison"]
if cmp["paired"] < 100:
    problems.append(f"only {cmp['paired']} frames paired")
if cmp["unmatched_a"] + cmp["unmatched_b"] > cmp["paired"] * 0.05:
    problems.append(f"too many unmatched: {cmp['unmatched_a']} / {cmp['unmatched_b']}")

# the heavy profile really is heavier, so the comparison has to say so. this is
# what catches a correlator that pairs the wrong frames
if doc["a"]["gpu_p50_ns"] >= doc["b"]["gpu_p50_ns"]:
    problems.append("the light profile did not measure cheaper than the heavy one")
if cmp["gpu_delta_p50_ns"] <= 0:
    problems.append(f"gpu delta should be positive, got {cmp['gpu_delta_p50_ns']}")
if doc.get("environment_mismatches"):
    problems.append(f"unexpected environment mismatch: {doc['environment_mismatches']}")

if problems:
    for p in problems:
        print(f"FAIL: {p}")
    sys.exit(1)

print(f"end to end ok: {doc['a']['frames']} and {doc['b']['frames']} frames, "
      f"{cmp['paired']} paired, no loss")
CHECKS
