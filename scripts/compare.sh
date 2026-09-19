#!/usr/bin/env bash
#
# Description: Runs the full comparison for two upscaler configurations. First
#   the live cost dashboard against real mpv, then the offline quality pass,
#   then one table with both.
# Author: Alex Wu
# Dependencies: framewire binaries, mpv, ffmpeg, python3
# Usage: scripts/compare.sh SOURCE_VIDEO SPEC_A SPEC_B
#

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUALITY="$HERE/quality.py"

SOURCE="${1:-}"
SPEC_A="${2:-}"
SPEC_B="${3:-}"

# the render size is whatever the compositor hands out, and that is not
# portable: a tiling compositor sizes by layout, a floating one by request.
# rather than fight it, the experiment is sized to it, so the shader lands on
# the render target exactly and nothing is resampled on any desktop
CLIP_START="${CLIP_START:-60}"
CLIP_LENGTH="${CLIP_LENGTH:-12}"
COST_SECONDS="${COST_SECONDS:-20}"
QUALITY_FRAMES="${QUALITY_FRAMES:-12}"
WORKDIR="${WORKDIR:-testclips}"

if [[ -z "$SOURCE" || -z "$SPEC_A" || -z "$SPEC_B" ]]; then
  cat >&2 <<'USAGE'
usage: scripts/compare.sh SOURCE_VIDEO SPEC_A SPEC_B

  SPEC is one of:
    shader:/path/to/upscaler.glsl    a custom GLSL shader
    builtin:NAME                     an mpv scaler, for example ewa_lanczossharp
    none                             mpv defaults

environment:
  CLIP_START           seconds into the source to sample, default 60
  CLIP_LENGTH          clip length in seconds, default 12
  COST_SECONDS         how long the live cost pass runs, default 20
  QUALITY_FRAMES       frames scored per config, default 12
  BUILD_DIR            where the framewire binaries live, default build

example:
  scripts/compare.sh movie.mkv shader:espcn.glsl builtin:ewa_lanczossharp
USAGE
  exit 2
fi

for tool in mpv ffmpeg python3; do
  command -v "$tool" >/dev/null || { echo "$tool is required but not installed" >&2; exit 1; }
done
for binary in framewire framewire-producer; do
  [[ -x "$BUILD_DIR/$binary" ]] || {
    echo "missing $BUILD_DIR/$binary, build with: cmake -S . -B build && cmake --build build -j" >&2
    exit 1; }
done
[[ -f "$SOURCE" ]] || { echo "no such video: $SOURCE" >&2; exit 1; }

LABEL_A="$(python3 "$QUALITY" --print-label "$SPEC_A")"
LABEL_B="$(python3 "$QUALITY" --print-label "$SPEC_B")"

COST_JSON="$(mktemp /tmp/framewire-cost-XXXXXX.json)"
mkdir -p "$WORKDIR"

# a throwaway clip just to get a window on screen and read its size back
PROBE="$WORKDIR/.probe.mkv"
if [[ ! -f "$PROBE" ]]; then
  ffmpeg -y -v error -ss "$CLIP_START" -i "$SOURCE" -t 1 -vf "scale=320:180" \
    -c:v libx264 -preset ultrafast -an "$PROBE"
fi

echo "probing the render size this compositor gives mpv"
GEOMETRY="$(python3 "$QUALITY" --probe-geometry "$PROBE")"
TARGET_W="${GEOMETRY%x*}"
TARGET_H="${GEOMETRY#*x}"
HALF_W=$((TARGET_W / 2))
HALF_H=$((TARGET_H / 2))
echo "  render area ${TARGET_W}x${TARGET_H}, so the experiment is ${HALF_W}x${HALF_H} upscaled 2x"
echo

REF="$WORKDIR/ref_${TARGET_W}x${TARGET_H}.mkv"
IN="$WORKDIR/in_${HALF_W}x${HALF_H}.mkv"

# lossless and 4:4:4, so the ground truth is not degraded before anything is
# compared against it, and so an odd half width is still encodable
if [[ ! -f "$REF" || ! -f "$IN" ]]; then
  echo "preparing clips from $SOURCE"
  ffmpeg -y -v error -ss "$CLIP_START" -i "$SOURCE" -t "$CLIP_LENGTH" \
    -vf "scale=${TARGET_W}:${TARGET_H}:flags=lanczos" \
    -c:v libx264 -qp 0 -pix_fmt yuv444p -preset veryfast -an "$REF"
  ffmpeg -y -v error -i "$REF" \
    -vf "scale=${HALF_W}:${HALF_H}:flags=lanczos" \
    -c:v libx264 -qp 0 -pix_fmt yuv444p -preset veryfast -an "$IN"
fi
echo "  reference $REF  input $IN"
echo

SOCK_A=/tmp/framewire-cmp-a-$$.sock
SOCK_B=/tmp/framewire-cmp-b-$$.sock
SHM_A=/framewire-cmp-a-$$
SHM_B=/framewire-cmp-b-$$
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -f "$SOCK_A" "$SOCK_B"
  rm -f "/dev/shm${SHM_A}" "/dev/shm${SHM_B}" 2>/dev/null || true
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

start_mpv() {
  local socket="$1" spec="$2" label="$3"
  local extra=()
  mapfile -t extra < <(spec_to_args "$spec")
  # --no-config matters as much as any measurement choice here. a user mpv.conf
  # can set a scaler, a shader or a profile that would silently change the
  # result without appearing anywhere in the output
  mpv "$IN" \
    --no-config --no-resume-playback --no-audio --no-osc --osd-level=0 \
    --input-ipc-server="$socket" --vo=gpu-next --loop-file=inf --keep-open=no \
    --title="framewire: $label" --msg-level=all=error \
    "${extra[@]}" >/dev/null 2>&1 &
  PIDS+=($!)
}

echo "=== pass 1 of 2: live cost, $COST_SECONDS s ==="
echo "  a: $LABEL_A   b: $LABEL_B"
echo
start_mpv "$SOCK_A" "$SPEC_A" "$LABEL_A"
start_mpv "$SOCK_B" "$SPEC_B" "$LABEL_B"
sleep 1

"$BUILD_DIR/framewire-producer" --socket "$SOCK_A" --shm "$SHM_A" --label "$LABEL_A" 2>/dev/null &
PIDS+=($!)
"$BUILD_DIR/framewire-producer" --socket "$SOCK_B" --shm "$SHM_B" --label "$LABEL_B" 2>/dev/null &
PIDS+=($!)

"$BUILD_DIR/framewire" --shm-a "$SHM_A" --shm-b "$SHM_B" \
  --duration "$COST_SECONDS" --json "$COST_JSON"

cleanup
trap - EXIT INT TERM

echo
echo "=== pass 2 of 2: running quality comparison ==="
# strictly sequential, because a second player rendering at the same time would
# contend for the GPU and perturb what is being captured
echo "  capturing $QUALITY_FRAMES frames per config, this takes a minute"
echo

python3 "$QUALITY" \
  --reference "$REF" --input "$IN" \
  --spec "$SPEC_A" --spec "$SPEC_B" \
  --frames "$QUALITY_FRAMES" --start 2 \
  --expect-geometry "$GEOMETRY" \
  --cost-json "$COST_JSON"

rm -f "$COST_JSON"
