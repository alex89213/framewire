# framewire

Compare two GPU upscaling shaders on the same video, in real time, with numbers
instead of impressions.

framewire runs two `mpv` instances side by side on the same clip with different
shaders, captures per frame GPU render timings from each over mpv's JSON IPC
socket, moves them through a lock free shared memory ring per instance, and
aggregates both streams in a separate process that pairs frames and draws a
live terminal dashboard. An optional second pass scores upscaling quality, so
cost and quality land in one table.

It was built to answer one question about a custom GLSL ESPCN upscaler: does it
actually cost less GPU time than the alternatives, and where does the
difference land in the pass breakdown.

```
  mpv instance A                    mpv instance B
  --input-ipc-server=/tmp/a.sock    --input-ipc-server=/tmp/b.sock
        |  playback-time tick             |  playback-time tick
        |  get_property vo-passes         |  get_property vo-passes
        v                                 v
  framewire-producer                framewire-producer
        |  lock free push                 |  lock free push
        v                                 v
  /dev/shm/framewire-a              /dev/shm/framewire-b
        |                                 |
        +---------------+-----------------+
                        v
                    framewire
          correlate by media position
          p50 / p99 / p999, per pass deltas
                        v
                 terminal dashboard
```

## Requirements

| For | Needs |
| --- | --- |
| Building and testing | a C++20 compiler, CMake 3.16 or newer |
| Measuring real shaders | `mpv` run with `--vo=gpu` or `--vo=gpu-next` |
| The quality pass | `ffmpeg`, `python3` |

mpv only fills in the `vo-passes` property for the GPU video outputs, so one of
those two is required. The scripts pass it for you.

The tool itself links nothing outside the C++ standard library and POSIX. The
unit tests and the ring benchmark run headless, with no GPU and no display.

## Building

```sh
cmake -S . -B build -G Ninja
cmake --build build -j
```

To build with the strict warning set treated as errors, which is how the code
is kept clean:

```sh
cmake -S . -B build -G Ninja -DFRAMEWIRE_WERROR=ON
```

## Usage

A configuration to compare is given as a **spec**:

| Spec | Meaning |
| --- | --- |
| `shader:/path/to/upscaler.glsl` | a custom GLSL shader |
| `builtin:ewa_lanczossharp` | one of mpv's own scalers |
| `none` | mpv defaults |

### Cost and quality in one run

The usual entry point. Prepares the clips, runs the live cost dashboard, then
the quality pass, then prints one table with both.

```sh
scripts/compare.sh video.mkv shader:espcn_x2_8.glsl builtin:ewa_lanczossharp
```

```
  cost and quality together
  config                          GPU p50    GPU p99   PSNR dB     SSIM
  ---------------------------- ---------- ---------- --------- --------
  ewa_lanczossharp                 1.65ms     3.62ms    46.525   0.9860
  espcn_x2_8                       0.53ms     1.27ms    46.506   0.9883

  quality is a wash: 0.02 dB apart, inside the 0.39 dB frame to frame spread
  espcn_x2_8 costs 3.09x less GPU time for the same picture, so it wins on cost alone
```

The verdict compares the quality gap against the frame to frame spread rather
than a fixed threshold. A 0.02 dB difference across frames that vary by 0.39 dB
between themselves is not a result, and calling it one would be the same
mistake as quoting a truncated pass total.

Every mpv instance is launched with `--no-config`, because a user `mpv.conf`
can set a scaler, a shader or a profile that would change the measurement
without appearing anywhere in the output.

Tunable through the environment:

| Variable | Default | Meaning |
| --- | --- | --- |
| `CLIP_START` | 60 | seconds into the source to sample from |
| `CLIP_LENGTH` | 12 | clip length in seconds |
| `COST_SECONDS` | 20 | how long the live pass runs |
| `QUALITY_FRAMES` | 12 | frames scored per config |
| `SCALE` | 2 | upscale factor the shaders provide |
| `BUILD_DIR` | `build` | where the binaries are |

Clips are cached in `testclips/` and reused. Delete that directory to re-cut
from a different scene.

### Cost only, any number of upscalers

The cost pass compares as many configurations as you give it. The first is the
baseline every other one is reported against.

```sh
scripts/run_comparison.sh clip.mkv \
  builtin:ewa_lanczossharp \
  shader:espcn_x2_8.glsl \
  shader:FSRCNNX_x2_8.glsl \
  builtin:bilinear
```

```
  stream                 gpu p50     delta       delta 95% interval       cheaper on            speedup
  ewa_lanczossharp       726.2us     baseline    -                        -                     1.000x
  espcn_x2_8             648.7us     -47.1us     [-60.3us, -26.3us]       92.0% [87.8, 94.8]    1.119x
  FSRCNNX_x2_8-0-4-1     1.10ms      +364.2us    [+346.9us, +384.1us]     0.4% [0.1, 2.4]       0.661x
  bilinear               63.6us      -630.2us    [-639.2us, -566.0us]     100.0% [98.4, 100.0]  11.427x

  espcn_x2_8: cheaper than ewa_lanczossharp, and the whole interval agrees
```

Two streams get the side by side panel view with the full pass breakdown. More
than two get a row each, since panels stop fitting once there is a ranking to
read.

| Variable | Default | Meaning |
| --- | --- | --- |
| `FRAMEWIRE_VO` | `gpu-next` | mpv video output |
| `LABEL_A`, `LABEL_B` | shader file names | panel labels |
| `DURATION` | unset | stop after this many seconds and print the report |
| `BUILD_DIR` | `build` | where the binaries are |

### Quality only

Takes any number of configs, unlike the cost pass which compares two.

```sh
scripts/quality.py \
  --reference testclips/ref_1274x716.mkv \
  --input testclips/in_637x358.mkv \
  --spec shader:espcn_x2_8.glsl \
  --spec shader:FSRCNNX_x2_8.glsl \
  --spec builtin:ewa_lanczossharp \
  --frames 24
```

### Without a GPU

A mock mpv server drives the whole pipeline headless, so the dashboard can be
seen with no GPU, no display and no video file. It is also what the
`end_to_end` test uses, since a machine building this project will not have any
of those.

```sh
scripts/demo.sh 30
```

The mock is strict about argument types, because an earlier permissive version
answered every command with success and hid a bug that left the real tool
capturing nothing. A mock is never allowed to be the only thing exercising a
protocol path.

### Ring benchmark

```sh
scripts/bench.sh
build/framewire-stress --records 20000000 --capacity 4096
```

### Running the pieces by hand

Each binary runs on its own, which is handy when attaching to an mpv instance
that is already playing.

```sh
# one producer per player
build/framewire-producer --socket /tmp/mpv-a.sock --shm /framewire-a --label espcn
build/framewire-producer --socket /tmp/mpv-b.sock --shm /framewire-b --label baseline

# the dashboard
build/framewire --shm-a /framewire-a --shm-b /framewire-b
```

## Reading the output

```
 framewire  live  00:00:06  paired 174

espcn_x2_8 live                                             ewa_lanczossharp live
──────────────────────────────────────────────────────────  ──────────────────────────────────────────────────────────
gpu    now 1.25ms   p50 749.7us  p99 1.34ms   p999 1.77ms   gpu    now 1.44ms   p50 873.2us  p99 3.92ms   p999 3.92ms
frame  now 45.28ms  p50 41.81ms  p99 83.90ms  p999 84.13ms  frame  now 41.75ms  p50 41.82ms  p99 86.53ms  p999 125.95m
life   p50 750.6us  p99 1.34ms   max  1.77ms                life   p50 873.5us  p99 3.92ms   max  3.92ms
fps    23.7 now  23.7 avg   frames 177                      fps    23.9 now  23.9 avg   frames 179
dropped 0 (0.00%)  delayed 0                                dropped 0 (0.00%)  delayed 0
ring   pend 0     lost 0     crc 0 gaps 0                   ring   pend 0     lost 0     crc 0 gaps 0
▁▁▁▁▁▁▂▂▂▁▁▂▂▂▁▁▂▁▁▁█▂▂▂▂▂▂▂▂▂▂▄▂▂▃▃▂▂▂▃▂▂▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃  ▅▁██▁▁▇▁▁▁▄▁▁▆▄▁▁▁▁▁▁▃██▁▁▁▁▁▁▇▇▁▅▄▁▂▁▂▂▂▁▅▂▂▂▂▆█▂▂▂▂▂▇▂▇▂

passes                                                      passes
 espcn_x2_8 conv1 (5x5, 1->8) 107.6us  186.3us               color decoding               49.9us   116.4us
 espcn_x2_8 conv1 (5x5, 1->8) 104.8us  175.7us               polar upscaling (ewa_lanczos 823.6us  3.86ms
 espcn_x2_8 conv2 (3x3, 8->8) 82.1us   137.4us
 espcn_x2_8 conv2 (3x3, 8->8) 79.1us   131.6us
 espcn_x2_8 conv3 + pixel shu 303.9us  507.6us
 color decoding, color encodi 72.1us   262.2us

 comparison  b minus a  ─────────────────────────────────────────────────────────────────────────────────────────────
 gpu delta   p50 +97.8us    p99 +2.81ms    mean +324.3us
 faster      espcn_x2_8 on 97.1% of paired frames   speedup 0.859x
 unmatched   a 3   b 3
```

Row by row:

- **gpu** is total GPU time for the frame, summed across every shader pass.
  `now` is the most recent frame, the percentiles come from a sliding window of
  the last few thousand frames. Percentiles rather than an average, because an
  average hides the tail stalls that make a shader unusable.
- **frame** is the wall clock gap between consecutive frames, so a steady 60 fps
  player sits at 16.67ms.
- **fps** shows `now` from recent frame gaps and `avg` over the whole run. The
  two separate when a player starts stuttering, which a run long average alone
  would hide.
- **life** is the same GPU statistic over the whole run instead of the recent
  window. A gap between the two rows means the workload is changing.
- **dropped** and **delayed** come from mpv's `frame-drop-count` and
  `vo-delayed-frame-count`.
- **ring** is transport health, and is the row that says whether the numbers
  above can be trusted. `pend` is records waiting in the ring, `lost` is records
  the producer could not fit because the dashboard fell behind, `crc` is failed
  checksums, and `gaps` is breaks in the sequence numbering. In a healthy run
  `lost`, `crc` and `gaps` are all zero, and the row turns red when a value is
  not.
- The sparkline shows recent total GPU time, scaled to the visible range.
- **comparison** is computed only from frames that were paired across both
  streams, so the two sides are always compared on equal footing.

Keys: `q` quits, `p` pauses accumulation, `r` resets every statistic.

When stdout is not a terminal the live view is skipped and a plain text report
is printed at exit instead, which is what makes the tool usable in a script.
`--report PATH` writes that report to a file as well.

Every difference carries a 95% interval. A point estimate on its own invites a
reader to treat noise as a result, so the report states plainly whether an
interval clears zero:

```
  espcn_x2_8: cheaper than ewa_lanczossharp, and the whole interval agrees
  some_other: no separation from ewa_lanczossharp, the interval crosses zero
```

A comparison is only meaningful when every stream was captured under the same
conditions. framewire records the renderer, decode path and render size for
each stream and refuses to present mismatched captures as like for like. See
[docs/measurement.md](docs/measurement.md).

## Testing

```sh
cd build && ctest --output-on-failure
```

CI runs the same suite on gcc and clang in Debug and Release, plus the address,
thread and undefined behaviour sanitizers. The thread sanitizer earns its place
more than anywhere else here, since a lock free queue is exactly the kind of
code where a race shows up under load and never in a unit test.

Four unit suites, a headless end to end run and a stress smoke test, all wired
into CTest. Everything here runs without a GPU, a display or a video file:

| Suite | Covers |
| --- | --- |
| `ring` | ordering, wraparound, the full ring policy, batch pop across the array end, the pass directory, producer liveness, a threaded handoff of 200000 records |
| `json` | scalars, escapes and surrogate pairs, containers, 22 malformed inputs, depth limits, real mpv message shapes, writer round trip |
| `histogram` | quantile accuracy against an exact sorted reference, the bucket zero linear range, clamping past the ceiling, merging, both sliding windows |
| `stats` | frame grouping across two and four streams, unmatched retirement, per pass gating, confidence intervals including that a noisy tie is not called significant, environment mismatch, corrupt records |
| `end_to_end` | mock mpv through both producers, the rings, the aggregator and the report, checked for lossless capture and a correct verdict |
| `stress_smoke` | a short two process run of the full stress harness |

The stress harness is the real test of the ring. Run a longer one directly:

```sh
build/framewire-stress --records 20000000 --capacity 4096
```

Options exist to stall either side on purpose (`--slow-consumer`,
`--slow-producer`) to exercise the backpressure path.

Bugs found during the build, all of them silent, which is why each one is
named here along with what caught it:

1. The histogram computed its bucket zero index with unsigned arithmetic, so
   every sample below 512 wrapped and landed in the top bucket. p999 came back
   as 68 seconds instead of 99900 ns. Caught by comparing against an exact
   sorted reference.
2. The JSON number scanner accepted `01`, because `from_chars` accepts a
   leading zero even though the JSON grammar does not.
3. The stress harness producer never sent a heartbeat during its push loop, so
   the consumer decided the producer had died and exited early on any run
   longer than the staleness window. The producer then spun forever on a full
   ring. Only visible on runs past two seconds, which is why a short smoke test
   never saw it.
4. `observe_property` was sending the observe id as a quoted string. mpv wants
   a JSON number, answers a quoted one with "invalid parameter", and then
   simply never sends the property. The producer captured nothing at all
   against real mpv. The mock server had been answering every command with
   success, so the whole test suite stayed green while the tool was completely
   broken. The mock now validates argument types the way mpv does, and a test
   asserts the id is encoded as a number.
5. The pass table held 16 entries and quietly dropped anything past that,
   including from the GPU total, which made longer chains look cheaper. See
   [docs/benchmarks.md](docs/benchmarks.md).

The fourth one is the reason the mock is now strict. A mock that accepts
anything reports a green run while the real integration captures nothing, which
is worse than having no mock at all.

## Project layout

```
include/framewire/    public headers
src/                  library sources and the four binaries
tests/                unit suites and the headless end to end test
scripts/              compare, run_comparison, quality, demo, bench
docs/                 design, measurement methodology, benchmarks
```

| Binary | Role |
| --- | --- |
| `framewire` | consumer, correlator and dashboard |
| `framewire-producer` | one per mpv instance, IPC to ring |
| `framewire-stress` | two process ring stress harness |
| `framewire-mock-mpv` | fake mpv for testing without a GPU |

## Documentation

| Document | Covers |
| --- | --- |
| [docs/design.md](docs/design.md) | the lock free ring, memory ordering, record layout, quantiles, frame pairing, the terminal, why there are no dependencies |
| [docs/measurement.md](docs/measurement.md) | how cost and quality are measured, sizing the experiment to the desktop, when a comparison is meaningless |
| [docs/benchmarks.md](docs/benchmarks.md) | ring throughput, instrumentation overhead, real shader results |

## Platform

Linux and POSIX only, as specified. Windows support is not built, but the
platform specific calls are confined to three files (`src/shm.cpp`,
`src/ipc_client.cpp`, `src/term.cpp`), so an abstraction layer has a clear seam
to sit on. Everything else is portable C++.

## License

See [LICENSE](LICENSE).
