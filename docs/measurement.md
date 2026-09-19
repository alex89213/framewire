# Measurement methodology

What the numbers mean, how they are taken, and the conditions under which they
stop being trustworthy. For how the code is built see [design.md](design.md).

The short version: cost is a runtime property and has to be sampled live, while
quality is deterministic given a shader and a frame. They are measured in
separate passes and joined afterwards, and that separation is required rather
than a convenience.

## How the quality pass works

Quality cannot ride the same path as cost, for two reasons. Cost is a runtime
property that moves with load and contention, so it has to be sampled live.
Quality is a deterministic function of the shader and the input frame, so there
is nothing to gain from measuring it in real time. More to the point, computing
quality during a cost run would ruin the cost run, because reading the
framebuffer back from the GPU is a synchronisation point that stalls the very
pipeline being timed. Separating the two passes is required, not a compromise.

Getting shader output out of mpv is the awkward part, and two of the three
obvious routes silently do not work:

| Route | Applies GLSL shaders |
| --- | --- |
| `--o=out.mkv`, the encoder | no |
| `--vo=image` | no |
| screenshot of the rendered window | yes |

Both failing routes produce output that looks right and is pixel identical to
the unshaded version. The encoder case is worse than it sounds, because the two
files differ in md5 through container metadata, so a checksum comparison says
the shader worked. Only comparing decoded pixels shows MSE zero.

So capture goes through a real rendered window: one paused mpv instance per
configuration, advanced with `frame-step` rather than by seeking, since a seek
can round to a keyframe and shift the alignment. Each frame is screenshotted,
cropped to the video rectangle mpv reports through `osd-dimensions`, and scored
against a reference with ffmpeg's `psnr` and `ssim` filters. The offset between
capture and reference is measured once from the first frame and reused, because
mpv and ffmpeg can disagree by a frame about where a start position lands.

Two guards matter. Every configuration must capture at the same geometry or the
run aborts, since scores from different sizes are not comparable. And the
capture geometry is printed rather than hidden.

## Sizing the experiment to the desktop

Window size is not portable and cannot be made portable. A tiling compositor
sizes windows by layout, a floating one by request, and neither reliably
honours what mpv asks for. On the scrolling compositor this was developed
against, every geometry flag mpv has is ignored outright.

Fighting that is a losing game, so the experiment is sized to it instead.
`scripts/compare.sh` opens one throwaway window, reads the render area back
through `osd-dimensions`, and builds the clips as exactly half of it. A 2x
shader then lands on the render target at a scale factor of exactly 1.0, which
is an identity, so nothing is resampled between the shader and the capture. The
measurement is exact on whatever desktop it runs on, and the geometry is
recorded in the output so two runs can be checked for comparability.

Both passes are then pinned to that geometry. The quality pass takes
`--expect-geometry` and refuses to run if a capture lands anywhere else, since
cost and quality measured at different render sizes are two experiments, not
one.

This also removes any need for a nested compositor. Running under `gamescope`
or `cage` would fix the size, but it is not required, and `Xvfb` would actively
be wrong here because software rendering makes the cost half meaningless.

PSNR and SSIM also disagree more often than people expect. In the runs above
FSRCNNX scores the highest PSNR of any configuration while scoring the lowest
SSIM, because it sharpens in a way PSNR rewards and SSIM does not. Report both.

## Knowing when a comparison is meaningless

Two streams are only comparable when they were captured under the same
conditions, and nothing about that is visible in the timings. Each producer
records its environment into the ring header before any telemetry flows:

```
[a] capture environment
  display          DP-3
  gpu_context      waylandvk
  hwdec            no
  mpv              mpv v0.41.0
  render           1274x716
  video            637x358
  vo               gpu-next
```

The consumer compares every stream against the baseline and refuses to present
the result as like for like when the renderer, the decode path or the render
size differ:

```
[warning] streams were not captured under the same conditions,
          so the comparison below is not a like for like measurement:
  lanczos vs espcn, vo: a has 'gpu-next', b has 'gpu'
```

Only keys that change what a number means are treated as blocking. A different
display name is recorded but does not invalidate anything. On a machine with
switchable graphics this is the guard that catches one player landing on the
integrated GPU and the other on the discrete one, which is otherwise invisible.

The window size is re-read every two seconds while a run is in progress. A
window that moves to another output or gets resized changes the render target,
so timings either side of the change describe different work, and the report
says so rather than averaging across it.

The report also warns when the players present at rates more than ten percent
apart, which is the visible symptom of a window a compositor has throttled
because it is hidden or off screen. Discard runs that warn.

## Shaders that are not 2x

The sizing above assumes a shader that doubles. `--scale` on the quality pass,
or `SCALE` on `compare.sh`, sets the factor the shader actually supplies, and
the input clip becomes the render area divided by it.

A factor that does not divide the render area evenly leaves a small residual
resample, so the quality pass reports which case a run is in rather than
folding it silently into the score:

```
  2x shader output lands on the render target exactly, nothing is resampled
```

or

```
  warning: a 4x shader on a 318x179 clip lands at 1272x716, not 1274x716.
  the output is resampled by 1.002x1.000 before capture, which compresses the
  differences between configs
```

## Ring names

A ring name identifies a running stream, so taking a name that is already in
use would leave the first producer writing into memory nothing reads while both
sides reported success. Creating a ring is therefore exclusive:

```
framewire-producer: ring '/framewire-a' is already in use, a producer with pid
98675 is still streaming to it. pick a different --shm name, or stop the other
run
```

A segment left behind by a crashed run is a different case and is reclaimed
automatically, told apart by whether the previous producer is still checking in.
The scripts append their process id to the default names, so two comparisons can
run at once without coordinating.

## Reading these numbers carefully

Both players share one GPU, so they contend with each other. That is fair in
the sense that both sides pay it, but absolute timings from a paired run sit
below what the same shader costs alone. Cross checking FSRCNNX_x2_16 against a
solo measurement taken straight from mpv gave 10.6ms against the 10.08ms the
paired run reported, which is close enough to trust the comparison.

One run produced an implausible result, a 30 pass network reporting less GPU
time than a 9 pass one, and it did not reproduce. The likely cause is a window
that was not rendering normally, since a compositor can throttle a surface that
is hidden or off screen. The report now prints a warning when players present
at rates more than 10 percent apart, because that split is the visible symptom
of the problem. Keep both windows fully visible when measuring.
