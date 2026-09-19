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
SOCK_C=/tmp/framewire-e2e-c-$$.sock
SHM_A=/framewire-e2e-a-$$
SHM_B=/framewire-e2e-b-$$
SHM_C=/framewire-e2e-c-$$
REPORT=$(mktemp /tmp/framewire-e2e-XXXXXX.json)
DURATION=4
FPS=120

PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -f "$SOCK_A" "$SOCK_B" "$SOCK_C" "$REPORT"
  rm -f "/dev/shm${SHM_A}" "/dev/shm${SHM_B}" "/dev/shm${SHM_C}" 2>/dev/null || true
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
# a third stream, so the k way merge is exercised and not just the two way path
"$BUILD_DIR/framewire-mock-mpv" --socket "$SOCK_C" --profile baseline --fps "$FPS" \
  --duration $((DURATION + 3)) --seed 33 2>/dev/null &
PIDS+=($!)
sleep 0.5

"$BUILD_DIR/framewire-producer" --socket "$SOCK_A" --shm "$SHM_A" --label light 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-producer" --socket "$SOCK_B" --shm "$SHM_B" --label heavy 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-producer" --socket "$SOCK_C" --shm "$SHM_C" --label plain 2>/dev/null &
PIDS+=($!)
sleep 0.5

"$BUILD_DIR/framewire" --shm "$SHM_A" --shm "$SHM_B" --shm "$SHM_C" \
  --duration "$DURATION" --plain --json "$REPORT" >/dev/null 2>&1

[[ -s "$REPORT" ]] || fail "no json report was written"

# python is only used to read the report, the tool itself needs none
python3 - "$REPORT" <<'CHECKS' || exit 1
import json, sys

doc = json.load(open(sys.argv[1]))
problems = []

if doc.get("schema") != "framewire.cost.v2":
    problems.append(f"unexpected schema {doc.get('schema')}")

streams = doc.get("streams", [])
if len(streams) != 3:
    problems.append(f"expected 3 streams, got {len(streams)}")

for s in streams:
    who = s["label"]
    if s["frames"] < 100:
        problems.append(f"{who}: only {s['frames']} frames captured")
    for counter in ("ring_lost", "checksum_errors", "sequence_gaps"):
        if s[counter] != 0:
            problems.append(f"{who}: {counter} is {s[counter]}, capture was not lossless")
    if not s["passes"]:
        problems.append(f"{who}: no pass breakdown came through")
    if s["gpu_p50_ns"] <= 0:
        problems.append(f"{who}: gpu p50 is {s['gpu_p50_ns']}")

cmp = doc["comparison"]
if cmp["grouped"] < 100:
    problems.append(f"only {cmp['grouped']} frames grouped across all three streams")
if sum(cmp["unmatched"]) > cmp["grouped"] * 0.05:
    problems.append(f"too many unmatched: {cmp['unmatched']}")

by_label = {s["label"]: s for s in cmp["streams"]}
if not by_label.get("light", {}).get("is_baseline"):
    problems.append("the first --shm should be the baseline")

# the profiles have known relative costs, so the ranking is checkable. this is
# what catches a k way merge that groups the wrong frames together
heavy = by_label.get("heavy", {})
if heavy.get("delta_p50_ns", 0) <= 0:
    problems.append(f"espcn-heavy should cost more than espcn, got {heavy.get('delta_p50_ns')}")
if heavy.get("cheaper_fraction", 1.0) > 0.05:
    problems.append(f"espcn-heavy should almost never be cheaper, got {heavy.get('cheaper_fraction')}")

if doc.get("environment_mismatches"):
    problems.append(f"unexpected environment mismatch: {doc['environment_mismatches']}")

if problems:
    for p in problems:
        print(f"FAIL: {p}")
    sys.exit(1)

print(f"end to end ok: {len(streams)} streams, "
      f"{', '.join(str(s['frames']) for s in streams)} frames, "
      f"{cmp['grouped']} grouped, no loss")
CHECKS
