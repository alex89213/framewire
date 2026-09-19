#!/usr/bin/env python3
"""
Description: Measures upscaling quality for a set of mpv shader configurations by
  capturing real GPU rendered frames and scoring them against a reference.
Author: Alex Wu
Dependencies: mpv, ffmpeg, python3
Usage: scripts/quality.py --reference REF.mkv --input IN.mkv --spec shader:a.glsl --spec builtin:ewa_lanczossharp
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

# mpv only applies GLSL shaders in the GPU render path. Encoding with --o= and
# writing frames with --vo=image both bypass that path and silently produce
# unshaded output, so capture has to go through a real rendered window.
CAPTURE_SETTLE_S = 0.06

# how far to search for the frame alignment between capture and reference
ALIGN_SEARCH = 4


def base_mpv_args(ipc_path):
    """
    Returns the mpv flags every capture shares.

    --no-config is the important one. A user mpv.conf can set scalers, shaders
    or profiles that would silently change what is being measured.

    Args:
        ipc_path: Socket path for the JSON IPC server.
    Returns:
        A list of mpv arguments.
    """
    return [
        "--no-config",
        "--no-resume-playback",
        "--no-audio",
        "--no-osc",
        "--no-osd-bar",
        "--osd-level=0",
        "--pause",
        "--keep-open=yes",
        "--vo=gpu-next",
        "--screenshot-format=png",
        "--screenshot-sw=no",
        "--msg-level=all=error",
        f"--input-ipc-server={ipc_path}",
    ]


def parse_spec(spec):
    """
    Turns a configuration string into a label and mpv flags.

    Accepted forms are shader:PATH for a custom GLSL shader, builtin:NAME for
    one of mpv's own scalers, and none for mpv defaults.

    Args:
        spec: Configuration string.
    Returns:
        A tuple of label and list of mpv arguments.
    """
    if spec == "none":
        return "mpv-default", []

    kind, _, value = spec.partition(":")
    if kind == "shader":
        if not os.path.isfile(value):
            raise SystemExit(f"no such shader: {value}")
        label = os.path.basename(value)
        label = re.sub(r"\.glsl$", "", label)
        # bilinear for the finishing scale, so the measurement reflects the
        # shader rather than whatever scaler mpv would otherwise apply on top
        return label, [f"--glsl-shaders={value}", "--scale=bilinear"]
    if kind == "builtin":
        return value, [f"--scale={value}"]
    raise SystemExit(f"bad spec '{spec}', expected shader:PATH, builtin:NAME or none")


class MpvSession:
    """
    Drives one paused mpv instance over the JSON IPC socket.

    Frames are advanced with frame-step rather than by seeking, because
    frame-step lands on exactly the next frame while a seek can round to a
    keyframe and quietly shift the alignment against the reference.
    """

    def __init__(self, clip, extra_args, start):
        self.dir = tempfile.mkdtemp(prefix="framewire-q-")
        self.ipc = os.path.join(self.dir, "ipc.sock")
        self.request_id = 0
        args = ["mpv", clip] + base_mpv_args(self.ipc) + [f"--start={start}"] + extra_args
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deadline = time.time() + 20
        self.sock = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.ipc)
                s.settimeout(10)
                self.sock = s
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            self.close()
            raise SystemExit("mpv did not open its IPC socket")
        self.buffer = b""
        # give the first frame time to reach the screen before anything is read
        time.sleep(1.5)

    def command(self, *words):
        self.request_id += 1
        want = self.request_id
        self.sock.sendall((json.dumps({"command": list(words), "request_id": want}) + "\n").encode())
        deadline = time.time() + 10
        while time.time() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                msg = json.loads(line)
                if msg.get("request_id") == want:
                    return msg
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buffer += chunk
        return {"error": "timeout"}

    def video_rect(self):
        """
        Reads the rectangle the video occupies inside the window.

        Returns:
            A tuple of left, top, width and height in pixels.
        """
        d = self.command("get_property", "osd-dimensions").get("data")
        if not d:
            raise SystemExit("mpv did not report osd-dimensions")
        left, top = int(d["ml"]), int(d["mt"])
        width = int(d["w"]) - left - int(d["mr"])
        height = int(d["h"]) - top - int(d["mb"])
        return left, top, width, height

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def capture(spec, clip, start, frames, outdir):
    """
    Captures rendered frames for one configuration.

    Args:
        spec: Configuration string.
        clip: Input video to play.
        start: Position in seconds to begin at.
        frames: How many frames to capture.
        outdir: Directory to write PNGs into.
    Returns:
        A tuple of label, video rectangle and list of captured paths.
    """
    label, extra = parse_spec(spec)
    os.makedirs(outdir, exist_ok=True)

    session = MpvSession(clip, extra, start)
    try:
        rect = session.video_rect()
        paths = []
        for i in range(frames):
            path = os.path.join(outdir, f"cap_{i:04d}.png")
            reply = session.command("screenshot-to-file", path, "window")
            if reply.get("error") != "success":
                raise SystemExit(f"screenshot failed for {label}: {reply.get('error')}")
            paths.append(path)
            if i + 1 < frames:
                session.command("frame-step")
                time.sleep(CAPTURE_SETTLE_S)
        return label, rect, paths
    finally:
        session.close()


def crop_frames(paths, rect, outdir):
    """
    Crops the video area out of full window screenshots.

    Args:
        paths: Captured PNG paths.
        rect: Video rectangle as left, top, width, height.
        outdir: Directory for the cropped output.
    Returns:
        The cropped paths.
    """
    left, top, width, height = rect
    os.makedirs(outdir, exist_ok=True)
    out = []
    for i, path in enumerate(paths):
        dest = os.path.join(outdir, f"crop_{i:04d}.png")
        r = run(["ffmpeg", "-y", "-v", "error", "-i", path,
                 "-vf", f"crop={width}:{height}:{left}:{top}", dest])
        if r.returncode != 0:
            raise SystemExit(f"crop failed: {r.stderr.strip()}")
        out.append(dest)
    return out


def build_reference(reference, start, count, width, height, outdir):
    """
    Extracts reference frames scaled to the capture size.

    Args:
        reference: Ground truth clip.
        start: Position in seconds to begin at.
        count: How many frames to extract.
        width: Capture width.
        height: Capture height.
        outdir: Directory for the output.
    Returns:
        The reference frame paths.
    """
    os.makedirs(outdir, exist_ok=True)
    r = run(["ffmpeg", "-y", "-v", "error", "-ss", str(start), "-i", reference,
             "-frames:v", str(count + 2 * ALIGN_SEARCH),
             "-vf", f"scale={width}:{height}:flags=lanczos",
             os.path.join(outdir, "ref_%04d.png")])
    if r.returncode != 0:
        raise SystemExit(f"reference extraction failed: {r.stderr.strip()}")
    return sorted(os.path.join(outdir, f) for f in os.listdir(outdir) if f.startswith("ref_"))


def score_pair(captured, reference):
    """
    Computes PSNR and SSIM between two images.

    Args:
        captured: Rendered frame.
        reference: Ground truth frame.
    Returns:
        A tuple of psnr and ssim, either may be None.
    """
    # each input can only be consumed once in a filter graph, so both have to be
    # split before psnr and ssim can read them
    graph = ("[0:v]split=2[c1][c2];[1:v]split=2[r1][r2];"
             "[c1][r1]psnr;[c2][r2]ssim")
    r = run(["ffmpeg", "-v", "info", "-i", captured, "-i", reference,
             "-lavfi", graph, "-f", "null", "-"])
    text = r.stderr + r.stdout
    psnr = re.search(r"average:([0-9.]+|inf)", text)
    ssim = re.search(r"All:([0-9.]+)", text)
    if psnr is None and ssim is None:
        raise SystemExit(f"ffmpeg produced no metrics for {captured}:\n{text.strip()[-500:]}")
    return (float(psnr.group(1)) if psnr and psnr.group(1) != "inf" else None,
            float(ssim.group(1)) if ssim else None)


def find_offset(first_capture, refs):
    """
    Finds which reference frame the first captured frame lines up with.

    mpv and ffmpeg can disagree by a frame or two about where a start position
    lands. Scoring against the wrong frame would look like a quality collapse,
    so the offset is measured once and reused.

    Args:
        first_capture: First cropped capture.
        refs: Candidate reference frames.
    Returns:
        Index into refs that matches best.
    """
    best_index, best_psnr = 0, -1.0
    for i, ref in enumerate(refs[: 2 * ALIGN_SEARCH + 1]):
        psnr, _ = score_pair(first_capture, ref)
        if psnr is not None and psnr > best_psnr:
            best_index, best_psnr = i, psnr
    return best_index


def main():
    ap = argparse.ArgumentParser(description="measure upscaling quality for mpv shader configs")
    ap.add_argument("--reference", help="ground truth clip at the target size")
    ap.add_argument("--input", help="downscaled clip the shaders upscale")
    ap.add_argument("--spec", action="append", default=[],
                    help="shader:PATH, builtin:NAME or none, repeatable")
    ap.add_argument("--frames", type=int, default=24, help="frames to score per config")
    ap.add_argument("--start", type=float, default=2.0, help="seconds into the clip to begin")
    ap.add_argument("--workdir", default=None, help="where to keep frames, a temp dir by default")
    ap.add_argument("--json", default=None, help="write results as JSON to this path")
    ap.add_argument("--cost-json", default=None,
                    help="framewire cost report to join against, by label")
    ap.add_argument("--print-label", default=None,
                    help="print the canonical label for a spec and exit")
    args = ap.parse_args()

    # the orchestrator needs the same label the results are keyed on, and
    # deriving it in two places would let them drift apart
    if args.print_label:
        print(parse_spec(args.print_label)[0])
        return 0

    if not args.spec:
        raise SystemExit("at least one --spec is required")
    if not args.reference or not args.input:
        raise SystemExit("--reference and --input are required")

    for tool in ("mpv", "ffmpeg"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required but was not found")

    workdir = args.workdir or tempfile.mkdtemp(prefix="framewire-quality-")
    os.makedirs(workdir, exist_ok=True)

    results = []
    geometry = None

    for spec in args.spec:
        label, _ = parse_spec(spec)
        print(f"  capturing {label} ...", flush=True)
        raw = os.path.join(workdir, f"{label}-raw")
        crop = os.path.join(workdir, f"{label}-crop")

        label, rect, paths = capture(spec, args.input, args.start, args.frames, raw)
        cropped = crop_frames(paths, rect, crop)
        _, _, width, height = rect

        # every configuration has to be captured at the same size, otherwise the
        # scores describe different experiments and cannot be ranked together
        if geometry is None:
            geometry = (width, height)
            refs = build_reference(args.reference, args.start, args.frames,
                                   width, height, os.path.join(workdir, "reference"))
            offset = find_offset(cropped[0], refs)
            print(f"  capture geometry {width}x{height}, reference offset {offset}", flush=True)
        elif geometry != (width, height):
            raise SystemExit(
                f"{label} captured at {width}x{height} but an earlier config used "
                f"{geometry[0]}x{geometry[1]}, refusing to compare different experiments")

        psnrs, ssims = [], []
        for i, shot in enumerate(cropped):
            ref_index = offset + i
            if ref_index >= len(refs):
                break
            psnr, ssim = score_pair(shot, refs[ref_index])
            if psnr is not None:
                psnrs.append(psnr)
            if ssim is not None:
                ssims.append(ssim)

        if not psnrs:
            raise SystemExit(f"no frames scored for {label}")

        results.append({
            "label": label,
            "spec": spec,
            "frames_scored": len(psnrs),
            "psnr_mean": sum(psnrs) / len(psnrs),
            "psnr_min": min(psnrs),
            "psnr_max": max(psnrs),
            "ssim_mean": (sum(ssims) / len(ssims)) if ssims else None,
        })

    results.sort(key=lambda r: r["psnr_mean"], reverse=True)

    print()
    print(f"  quality, {geometry[0]}x{geometry[1]} capture, "
          f"{results[0]['frames_scored']} frames, higher is better")
    print(f"  {'config':<28} {'PSNR dB':>9} {'min':>8} {'SSIM':>8}")
    print(f"  {'-' * 28} {'-' * 9} {'-' * 8} {'-' * 8}")
    for r in results:
        ssim = f"{r['ssim_mean']:.4f}" if r["ssim_mean"] is not None else "-"
        print(f"  {r['label'][:28]:<28} {r['psnr_mean']:9.3f} {r['psnr_min']:8.3f} {ssim:>8}")

    cost = {}
    if args.cost_json:
        with open(args.cost_json) as f:
            doc = json.load(f)
        for side in ("a", "b"):
            entry = doc.get(side)
            if entry:
                cost[entry["label"]] = entry

    if cost:
        print()
        print("  cost and quality together")
        print(f"  {'config':<28} {'GPU p50':>10} {'GPU p99':>10} {'PSNR dB':>9} {'SSIM':>8}")
        print(f"  {'-' * 28} {'-' * 10} {'-' * 10} {'-' * 9} {'-' * 8}")
        for r in results:
            c = cost.get(r["label"])
            p50 = f"{c['gpu_p50_ns'] / 1e6:.2f}ms" if c else "-"
            p99 = f"{c['gpu_p99_ns'] / 1e6:.2f}ms" if c else "-"
            ssim = f"{r['ssim_mean']:.4f}" if r["ssim_mean"] is not None else "-"
            print(f"  {r['label'][:28]:<28} {p50:>10} {p99:>10} "
                  f"{r['psnr_mean']:9.3f} {ssim:>8}")

        scored = [r for r in results if r["label"] in cost]
        if len(scored) == 2:
            best_q, other = scored[0], scored[1]
            delta_db = best_q["psnr_mean"] - other["psnr_mean"]
            delta_ms = (cost[best_q["label"]]["gpu_p50_ns"]
                        - cost[other["label"]]["gpu_p50_ns"]) / 1e6

            # a quality gap smaller than the frame to frame spread is not a
            # result. reporting it as one would be exactly the kind of
            # confident looking number that means nothing
            spread = max(r["psnr_max"] - r["psnr_min"] for r in scored)
            print()
            if abs(delta_db) < spread:
                cheaper = min(scored, key=lambda r: cost[r["label"]]["gpu_p50_ns"])
                dearer = max(scored, key=lambda r: cost[r["label"]]["gpu_p50_ns"])
                ratio = (cost[dearer["label"]]["gpu_p50_ns"]
                         / max(cost[cheaper["label"]]["gpu_p50_ns"], 1))
                print(f"  quality is a wash: {abs(delta_db):.2f} dB apart, inside the "
                      f"{spread:.2f} dB frame to frame spread")
                print(f"  {cheaper['label']} costs {ratio:.2f}x less GPU time for the "
                      f"same picture, so it wins on cost alone")
            elif delta_ms <= 0:
                print(f"  {best_q['label']} is both cheaper and higher quality, "
                      f"by {abs(delta_db):.2f} dB and {abs(delta_ms):.2f}ms per frame")
            else:
                print(f"  tradeoff: {best_q['label']} buys {delta_db:.2f} dB "
                      f"for {delta_ms:.2f}ms more GPU time per frame")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "schema": "framewire.quality.v1",
                "capture_width": geometry[0],
                "capture_height": geometry[1],
                "results": results,
                "cost": cost or None,
            }, f, indent=2)
        print(f"\n  written to {args.json}")

    if args.workdir is None:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
