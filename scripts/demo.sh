#!/usr/bin/env bash
#
# Description: Runs the whole pipeline against the mock mpv server, so the
#   dashboard can be seen without a GPU, a video file or a real mpv.
# Author: Alex Wu
# Dependencies: framewire binaries
# Usage: scripts/demo.sh [DURATION_SECONDS]
#

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DURATION="${1:-30}"
FPS="${FPS:-60}"
PROFILE_A="${PROFILE_A:-espcn}"
PROFILE_B="${PROFILE_B:-baseline}"

SOCK_A=/tmp/framewire-demo-a-$$.sock
SOCK_B=/tmp/framewire-demo-b-$$.sock
SHM_A=/framewire-demo-a-$$
SHM_B=/framewire-demo-b-$$

for binary in framewire framewire-producer framewire-mock-mpv; do
  if [[ ! -x "$BUILD_DIR/$binary" ]]; then
    echo "missing $BUILD_DIR/$binary, build first with: cmake -S . -B build && cmake --build build -j" >&2
    exit 1
  fi
done

PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -f "$SOCK_A" "$SOCK_B"
  rm -f "/dev/shm${SHM_A}" "/dev/shm${SHM_B}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# the mock runs a little longer than the dashboard, so the producers never see
# the socket close while the view is still up
MOCK_DURATION=$((DURATION + 3))

"$BUILD_DIR/framewire-mock-mpv" --socket "$SOCK_A" --profile "$PROFILE_A" --fps "$FPS" \
  --duration "$MOCK_DURATION" --drop-rate 0.004 --seed 11 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-mock-mpv" --socket "$SOCK_B" --profile "$PROFILE_B" --fps "$FPS" \
  --duration "$MOCK_DURATION" --drop-rate 0.004 --seed 22 2>/dev/null &
PIDS+=($!)

"$BUILD_DIR/framewire-producer" --socket "$SOCK_A" --shm "$SHM_A" --label "$PROFILE_A" 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-producer" --socket "$SOCK_B" --shm "$SHM_B" --label "$PROFILE_B" 2>/dev/null &
PIDS+=($!)

"$BUILD_DIR/framewire" --shm-a "$SHM_A" --shm-b "$SHM_B" --duration "$DURATION" "${@:2}"
