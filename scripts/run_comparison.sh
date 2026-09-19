#!/usr/bin/env bash
#
# Description: Launches one mpv instance per upscaler configuration on the same
#   video, starts a producer for each and opens the live framewire dashboard.
#   Cost only, with no quality pass.
# Author: Alex Wu
# Dependencies: mpv built with the gpu video output, framewire binaries
# Usage: scripts/run_comparison.sh VIDEO SPEC [SPEC ...]
#

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUALITY="$HERE/quality.py"

VIDEO="${1:-}"
shift || true
SPECS=("$@")

# mpv only fills in vo-passes for the gpu video outputs, so the choice is not
# optional. gpu-next is preferred when the local mpv supports it
VO="${FRAMEWIRE_VO:-gpu-next}"

if [[ -z "$VIDEO" || ${#SPECS[@]} -lt 2 ]]; then
  cat >&2 <<'USAGE'
usage: scripts/run_comparison.sh VIDEO SPEC [SPEC ...]

  At least two configurations are needed. The first is the baseline that every
  other one is reported against.

  SPEC is one of:
    shader:/path/to/upscaler.glsl    a custom GLSL shader
    builtin:NAME                     an mpv scaler, for example ewa_lanczossharp
    none                             mpv defaults

environment:
  BUILD_DIR       where the framewire binaries live, default build
  FRAMEWIRE_VO    mpv video output, default gpu-next
  DURATION        stop after this many seconds and print the report
  JSON            also write the report as JSON to this path

examples:
  scripts/run_comparison.sh clip.mkv shader:espcn.glsl builtin:ewa_lanczossharp
  scripts/run_comparison.sh clip.mkv shader:a.glsl shader:b.glsl shader:c.glsl none
USAGE
  exit 2
fi

[[ -f "$VIDEO" ]] || { echo "no such video: $VIDEO" >&2; exit 1; }
for binary in framewire framewire-producer; do
  [[ -x "$BUILD_DIR/$binary" ]] || {
    echo "missing $BUILD_DIR/$binary, build with: cmake -S . -B build && cmake --build build -j" >&2
    exit 1; }
done

PIDS=()
SOCKETS=()
RINGS=()
LABELS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  for s in "${SOCKETS[@]:-}"; do rm -f "$s"; done
  for r in "${RINGS[@]:-}"; do rm -f "/dev/shm${r}" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

spec_to_args() {
  case "$1" in
    none) ;;
    shader:*) printf -- '--glsl-shaders=%s\n--scale=bilinear\n' "${1#shader:}" ;;
    builtin:*) printf -- '--scale=%s\n' "${1#builtin:}" ;;
    *) echo "bad spec: $1" >&2; exit 2 ;;
  esac
}

for i in "${!SPECS[@]}"; do
  LABELS+=("$(python3 "$QUALITY" --print-label "${SPECS[$i]}")")
  SOCKETS+=("/tmp/framewire-cmp-${i}-$$.sock")
  RINGS+=("/framewire-cmp-${i}-$$")
done

echo "framewire: ${#SPECS[@]} streams with vo=$VO"
for i in "${!SPECS[@]}"; do
  if [[ $i -eq 0 ]]; then
    echo "  [$i] ${LABELS[$i]}  (baseline)"
  else
    echo "  [$i] ${LABELS[$i]}"
  fi
done

for i in "${!SPECS[@]}"; do
  extra=()
  mapfile -t extra < <(spec_to_args "${SPECS[$i]}")

  # --no-config matters as much as any measurement choice here. a user mpv.conf
  # can set a scaler, a shader or a profile that would silently change the
  # result without appearing anywhere in the output
  mpv "$VIDEO" \
    --no-config --no-resume-playback --no-audio --no-osc --osd-level=0 \
    --input-ipc-server="${SOCKETS[$i]}" --vo="$VO" --loop-file=inf --keep-open=no \
    --title="framewire: ${LABELS[$i]}" --msg-level=all=error \
    "${extra[@]}" >/dev/null 2>&1 &
  PIDS+=($!)
done

# the producers retry the connect themselves, so this only gives mpv a moment
# to get past its own startup
sleep 1

CONSUMER_ARGS=()
for i in "${!SPECS[@]}"; do
  "$BUILD_DIR/framewire-producer" --socket "${SOCKETS[$i]}" --shm "${RINGS[$i]}" \
    --label "${LABELS[$i]}" 2>/dev/null &
  PIDS+=($!)
  CONSUMER_ARGS+=(--shm "${RINGS[$i]}" --label "${LABELS[$i]}")
done

if [[ -n "${DURATION:-}" ]]; then
  CONSUMER_ARGS+=(--duration "$DURATION")
fi
if [[ -n "${JSON:-}" ]]; then
  CONSUMER_ARGS+=(--json "$JSON")
fi

"$BUILD_DIR/framewire" "${CONSUMER_ARGS[@]}"
