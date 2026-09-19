#!/usr/bin/env bash
#
# Description: Launches two mpv instances on the same video with different GPU
#   shaders, starts a producer for each and opens the framewire dashboard.
# Author: Alex Wu
# Dependencies: mpv built with the gpu video output, framewire binaries
# Usage: scripts/run_comparison.sh VIDEO [SHADER_A] [SHADER_B]
#

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
VIDEO="${1:-}"
SHADER_A="${2:-}"
SHADER_B="${3:-}"

SOCK_A=/tmp/framewire-mpv-a.sock
SOCK_B=/tmp/framewire-mpv-b.sock
SHM_A=/framewire-a
SHM_B=/framewire-b

# mpv only fills in vo-passes for the gpu video outputs, so the choice is not
# optional. gpu-next is preferred when the local mpv supports it
VO="${FRAMEWIRE_VO:-gpu-next}"

if [[ -z "$VIDEO" ]]; then
  cat >&2 <<'USAGE'
usage: scripts/run_comparison.sh VIDEO [SHADER_A] [SHADER_B]

  VIDEO       file both instances play
  SHADER_A    glsl shader for the left panel, empty means no shader
  SHADER_B    glsl shader for the right panel, empty means no shader

environment:
  BUILD_DIR       where the framewire binaries live (default build)
  FRAMEWIRE_VO    mpv video output (default gpu-next)
  LABEL_A         dashboard label for the left panel
  LABEL_B         dashboard label for the right panel
  DURATION        stop after this many seconds
USAGE
  exit 2
fi

if [[ ! -f "$VIDEO" ]]; then
  echo "no such video: $VIDEO" >&2
  exit 1
fi

for binary in framewire framewire-producer; do
  if [[ ! -x "$BUILD_DIR/$binary" ]]; then
    echo "missing $BUILD_DIR/$binary, build first with: cmake -S . -B build && cmake --build build -j" >&2
    exit 1
  fi
done

LABEL_A="${LABEL_A:-$([[ -n "$SHADER_A" ]] && basename "$SHADER_A" .glsl || echo baseline-a)}"
LABEL_B="${LABEL_B:-$([[ -n "$SHADER_B" ]] && basename "$SHADER_B" .glsl || echo baseline-b)}"

PIDS=()

cleanup() {
  # kill the whole group rather than one pid at a time, so a half started run
  # does not leave an mpv window or a producer behind
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -f "$SOCK_A" "$SOCK_B"
}
trap cleanup EXIT INT TERM

start_mpv() {
  local socket="$1" shader="$2" label="$3"
  local args=(
    --input-ipc-server="$socket"
    --vo="$VO"
    --no-audio
    --keep-open=no
    --loop-file=inf
    --title="framewire: $label"
    --msg-level=all=error
  )
  if [[ -n "$shader" ]]; then
    args+=(--glsl-shaders="$shader")
  fi
  mpv "${args[@]}" "$VIDEO" &
  PIDS+=($!)
}

echo "framewire: starting mpv instances with vo=$VO"
start_mpv "$SOCK_A" "$SHADER_A" "$LABEL_A"
start_mpv "$SOCK_B" "$SHADER_B" "$LABEL_B"

# the producers retry the connect themselves, so no sleep is needed here beyond
# giving mpv a moment to get past its own startup
sleep 1

"$BUILD_DIR/framewire-producer" --socket "$SOCK_A" --shm "$SHM_A" --label "$LABEL_A" &
PIDS+=($!)
"$BUILD_DIR/framewire-producer" --socket "$SOCK_B" --shm "$SHM_B" --label "$LABEL_B" &
PIDS+=($!)

CONSUMER_ARGS=(--shm-a "$SHM_A" --shm-b "$SHM_B")
if [[ -n "${DURATION:-}" ]]; then
  CONSUMER_ARGS+=(--duration "$DURATION")
fi

"$BUILD_DIR/framewire" "${CONSUMER_ARGS[@]}"
