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
| Measuring real shaders | `mpv` built with the `gpu` or `gpu-next` video output |
| The quality pass | `ffmpeg`, `python3` |

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

Tunable through the environment:

| Variable | Default | Meaning |
| --- | --- | --- |
| `CLIP_START` | 60 | seconds into the source to sample from |
| `CLIP_LENGTH` | 12 | clip length in seconds |
| `COST_SECONDS` | 20 | how long the live pass runs |
| `QUALITY_FRAMES` | 12 | frames scored per config |
| `BUILD_DIR` | `build` | where the binaries are |

Clips are cached in `testclips/` and reused. Delete that directory to re-cut
from a different scene.

### Cost only, live dashboard

Runs until you quit, with no quality pass.

```sh
scripts/run_comparison.sh video.mkv shaders/a.glsl shaders/b.glsl
```

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
seen with no GPU, no display and no video file.

```sh
scripts/demo.sh 30
```

### Ring benchmark

```sh
scripts/bench.sh
build/framewire-stress --records 20000000 --capacity 4096
```

### Starting the pieces by hand

Each binary runs on its own, which is handy when attaching to an mpv instance
that is already playing.

```sh

## Reading the output

```
 framewire  live  00:00:03  paired 262
 espcn-x2                       live      espcn-heavy                    live
 ─────────────────────────────────────    ─────────────────────────────────────
 gpu    now 4.19ms  p50 2.88ms  p99 ...  gpu    now 5.83ms  p50 3.40ms  p99 ...
 frame  now 41.14ms p50 41.66ms p99 ...  frame  now 41.63ms p50 41.67ms p99 ...
 life   p50 2.88ms  p99 4.29ms  max ...  life   p50 3.40ms  p99 6.65ms  max ...
 fps    24.0 now  24.0 avg  frames 202  fps    23.9 now  23.9 avg  frames 201
 dropped 4 (1.53%)  delayed 2            dropped 3 (1.15%)  delayed 1
 ring   pend 0  lost 0  crc 0  gaps 0    ring   pend 0  lost 0  crc 0  gaps 0
 ▃▅▃▅▂▅▃▅▄▃▅▅▅▃▄▄▄▆▅▅▁▅▃▄▅▄▆▇▃▄▂▅▆▃█▅    ▁▂█▃▃▃▂▂▅▄▄▄▄▄▄▂▂▆▁▃▃▁▄▆▂▁▄▄▄▅▂▂▄▂▄▅

 passes                                  passes
  espcn conv1 relu       634.9us 1.08ms    espcn conv1 relu      1.16ms 2.40ms
  ...                                      ...

 comparison  b minus a  ──────────────────────────────────────────────────────
 gpu delta   p50 +1.81ms   p99 +3.12ms   mean +1.82ms
 faster      espcn-x2 on 100.0% of paired frames   speedup 0.461x
 unmatched   a 0   b 0
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

A comparison is only meaningful when both streams were captured under the same
conditions. framewire records the renderer, decode path and render size for
each stream and refuses to present mismatched captures as like for like. See
[docs/measurement.md](docs/measurement.md).

## Testing

Four unit suites and a stress test, all wired into CTest:

| Suite | Covers |
| --- | --- |
| `ring` | ordering, wraparound, the full ring policy, batch pop across the array end, the pass directory, producer liveness, a threaded handoff of 200000 records |
| `json` | scalars, escapes and surrogate pairs, containers, 22 malformed inputs, depth limits, real mpv message shapes, writer round trip |
| `histogram` | quantile accuracy against an exact sorted reference, the bucket zero linear range, clamping past the ceiling, merging, both sliding windows |
| `stats` | frame pairing, unmatched retirement, flushing, per pass gating, corrupt record handling, sequence gap counting, duration formatting |
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
