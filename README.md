# framewire

A lock free telemetry comparison tool for two `mpv` instances rendering the
same video with different GPU shaders.

framewire launches two players, captures per frame GPU render timings from each
over mpv's JSON IPC socket, moves those timings through a shared memory SPSC
ring buffer per instance, and aggregates both streams in a separate process
that computes comparative latency statistics and draws a live terminal
dashboard.

The tool was built to answer one question with real numbers rather than
impressions: does a custom GLSL ESPCN upscaler actually cost less GPU time than
the built in scaler, and where does the difference land in the pass breakdown.

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

## Build

Requires a C++20 compiler and CMake 3.16 or newer. Nothing else.

The optional quality pass additionally needs `mpv`, `ffmpeg` and `python3`. The
tool itself needs none of them.

```sh
cmake -S . -B build -G Ninja
cmake --build build -j
```

Run the tests:

```sh
cd build && ctest --output-on-failure
```

To build with the strict warning set treated as errors, which is how the code
is kept clean:

```sh
cmake -S . -B build -G Ninja -DFRAMEWIRE_WERROR=ON
```

## Running a comparison

### With real mpv

mpv only fills in the `vo-passes` property for the GPU video outputs, so
`--vo=gpu` or `--vo=gpu-next` is required. The helper script starts both
players, both producers and the dashboard, and cleans everything up on exit.

```sh
scripts/run_comparison.sh video.mkv shaders/espcn.glsl
```

The second shader argument is optional. Leaving the argument out runs the
second instance with no shader, which makes the comparison measure the custom
shader against mpv's default scaler.

```sh
scripts/run_comparison.sh video.mkv shaders/espcn.glsl shaders/other.glsl
```

Useful environment variables:

| Variable | Meaning |
| --- | --- |
| `FRAMEWIRE_VO` | mpv video output, defaults to `gpu-next` |
| `LABEL_A`, `LABEL_B` | panel labels, default to the shader file names |
| `DURATION` | stop after this many seconds and print the report |
| `BUILD_DIR` | where the binaries live, defaults to `build` |

### Cost and quality in one run

`scripts/compare.sh` runs both halves and prints one table. The live cost
dashboard comes up first, then the quality pass runs, then the two are joined.

```sh
scripts/compare.sh movie.mkv shader:espcn_x2_8.glsl builtin:ewa_lanczossharp
```

A configuration is given as `shader:PATH` for a custom GLSL shader,
`builtin:NAME` for one of mpv's own scalers, or `none` for mpv defaults. Both
passes run on the same clip at the same upscale factor, so the joined table
describes one experiment rather than two.

Output ends like this:

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
than against a fixed threshold. A 0.02 dB difference across 8 frames that vary
by 0.39 dB between themselves is not a result, and calling it one would be the
same mistake as quoting a truncated pass total.

Every mpv instance is launched with `--no-config`. A user `mpv.conf` can set a
scaler, a shader or a profile that would change the measurement without
appearing anywhere in the output.

### How the quality pass works

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

### Sizing the experiment to the desktop

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

### Without a GPU

A mock mpv server ships with the project and speaks enough of the IPC protocol
to drive the whole pipeline. The mock is how the dashboard and the correlator
get tested, and the mock is useful for seeing the view without a video file.

```sh
scripts/demo.sh 30
```

### Starting the pieces by hand

Each binary runs on its own, which is handy when attaching to an mpv instance
that is already playing.

```sh
# one producer per player
build/framewire-producer --socket /tmp/mpv-a.sock --shm /framewire-a --label espcn
build/framewire-producer --socket /tmp/mpv-b.sock --shm /framewire-b --label baseline

# the dashboard
build/framewire --shm-a /framewire-a --shm-b /framewire-b
```

## Reading the dashboard

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

## Design notes

### The ring buffer

One producer and one consumer, a power of two slot count, and 64 bit indices
that only ever increase. Indices are masked to find a slot, and at a thousand
frames a second a 64 bit counter takes longer than the age of the universe to
wrap, so wraparound is not handled and does not need to be.

`head` and `tail` each get a private cache line. Putting both on one line is
the classic false sharing bug in this kind of queue and costs a coherence miss
on every push. The layout is checked by `static_assert` rather than trusted,
because losing the padding would not break the queue, it would only make the
queue quietly much slower.

The cached copies of the far index live in the handle objects, in memory
private to each process, and deliberately not in the shared header. A cached
value is written often and read by one side only, so parking the value in
shared memory would drag the other side's cache line back and forth and undo
the padding.

Memory ordering is acquire and release, never sequentially consistent. Every
atomic operation carries a comment explaining the choice, but the short version
is:

| Operation | Ordering | Why |
| --- | --- | --- |
| producer loads `head` | relaxed | the producer is the only writer, nothing is published by reading |
| producer loads `tail` | acquire | pairs with the consumer's release, proves the consumer finished reading a slot before that slot is reused |
| producer stores `head` | release | publishes the record bytes written just before, a relaxed store here is the bug that lets a consumer see the index move while the record is still in flight |
| consumer loads `tail` | relaxed | the consumer is the only writer |
| consumer loads `head` | acquire | pairs with the producer's release, makes the record bytes visible |
| consumer stores `tail` | release | keeps the record copy from sinking past the index bump |

Sequential consistency would work and would be easier to argue about, but it
forces a full barrier on x86 stores for a guarantee this queue never needs.
There is no total order requirement across the two indices, only the pairwise
happens before relationship between one side's store and the other side's load.

When the ring is full the producer drops the newest record and counts the drop.
Overwriting the oldest unread slot would race with a consumer that is mid copy,
and would also bias the latency statistics toward recent frames. Dropping and
counting is the honest choice for telemetry, and the count is visible on the
dashboard so a reader always knows when a number is incomplete.

### Records

`TelemetryRecord` is exactly 192 bytes, trivially copyable, and made only of
fixed width integers with no pointers, because the struct lives in memory that
each process maps at a different address. The size is pinned by `static_assert`
and is part of the shared memory ABI, which the header version guards.

The checksum is the last field on purpose, so the covered range is every other
byte of the record. A torn read anywhere is then caught, which is the property
the stress harness relies on.

Pass names are not stored in the record. Names are stable for the life of a
shader chain, so the names live once in the ring header behind a version
counter and `pass_ns` is indexed against that directory. The producer publishes
the directory with a release store and only when the chain actually changes.

### Quantiles

Two structures, for two different questions.

Lifetime percentiles use a histogram laid out the way HdrHistogram does it:
buckets by exponent, with a fixed number of linear slots inside each exponent.
Storage is constant at about 112 KB, relative error stays under 0.1 percent
across the whole range, and recording a sample is a few shifts and an
increment. Keeping every sample and sorting would be exact, but a long
benchmark run would end up holding millions of samples just to read three
numbers off the tail.

Recent percentiles use a bounded ring of raw samples, sorted on demand. A
histogram cannot forget old samples, and the dashboard needs to show what the
last few seconds look like. Sorting a few thousand values ten times a second
costs nothing and is exact.

### Correlating the two streams

Frames are matched on media position, not arrival time. This was the single
biggest correctness fix that real usage forced.

Arrival time seems like the obvious key and does not work. Two players started
by hand are never phase locked, so their frames land at some arbitrary constant
offset from each other. That offset is bounded by one frame interval, but a
frame interval at 24 fps is 41ms, and any tolerance small enough to be
meaningful is smaller than the typical offset. Measured against a real pair of
mpv instances, matching on arrival time paired 29 frames out of 435.

Media position does not have that problem. Both players decode the same file,
so the same frame carries the same position in both regardless of when either
one got around to drawing it. mpv reports the position through `playback-time`,
which the producer is already watching as its frame clock, so the key costs
nothing extra to collect. The same pair of players then matched 434 frames out
of 436.

Looping breaks the key by resetting the position to zero, so the producer adds
an epoch offset on every restart and hands the correlator a value that only
increases. Arrival time is still used as a fallback when a stream carries no
media position at all.

Matching itself is a merge over both queues, and the decision only ever needs
the front record of each side. If the earlier record is outside the tolerance
window of the other side's front, no future record can be closer, so the record
retires as unmatched rather than being paired with something unrelated.

Every comparative number comes from paired frames only. Comparing the two
independent averages would be misleading, because the two instances can render
a different number of frames over the same wall time.

Per pass differences are only reported when both chains have the same pass
count. Lining up pass three of a five pass chain against pass three of a three
pass chain would produce a confident looking number that means nothing.

### The terminal

Raw ANSI, not a TUI library. Two reasons. The dependency list stays empty,
which matters for a tool meant to measure something, since every library linked
in is more code running next to the thing under test. And a dashboard row
changes as a unit, so a row level diff is the natural granularity: rows are held
as fully formatted strings, compared whole, and only the rows that changed are
written. A refresh where nothing moved costs zero bytes on the wire.

The terminal is put back the way it was found on exit, including after a
signal, so a crash never leaves a shell without echo.

## Dependencies

None beyond the C++20 standard library and POSIX.

The prompt allowed dependencies for JSON parsing and the terminal UI. Both were
dropped after looking at what each would actually buy:

- **JSON.** The mpv IPC schema is small and fixed. A few hundred lines of
  recursive descent covers the protocol, parses into a flat node array with no
  per node allocation, and reuses the buffers across messages so a steady
  stream of frames settles into zero allocations. A general purpose library
  would add a large header for features this program never uses. The parser is
  strict about the JSON grammar, including rejecting leading zeros, and has a
  depth cap so a corrupt message produces a parse error instead of a blown
  stack.
- **Terminal UI.** See the section above. ncurses would bring a dependency, a
  global screen model and its own input handling, in exchange for a cell level
  diff this layout does not need.

`librt` is linked only when `shm_open` is found there, since the symbol moved
into libc on newer glibc.

## Benchmarks

Measured on an Intel Core i9-13900H, Linux 7.2.5, gcc 16.2.1, release build.

### Ring buffer throughput

Produced by `scripts/bench.sh`. Each run pushes 20 million records through the
ring between two separate forked processes, not threads, so the shared memory
path is what gets exercised.

The harness runs in two modes, because one number cannot answer both questions.

**Throughput mode** times the queue itself, with the checks off:

| Ring slots | M records/s | MiB/s | ns per record |
| --- | --- | --- | --- |
| 256 | 20.4 | 3739 | 48.98 |
| 1024 | 74.0 | 13549 | 13.51 |
| 4096 | 109.5 | 20043 | 9.14 |
| 16384 | 156.4 | 28635 | 6.39 |
| 65536 | 168.9 | 30918 | 5.92 |

**Integrity mode** rebuilds every record from its sequence number and compares
it byte for byte, so a torn read that mixed two records would be caught even if
both halves were individually valid:

| Ring slots | M records/s | MiB/s | ns per record |
| --- | --- | --- | --- |
| 256 | 2.70 | 495 | 369.8 |
| 1024 | 2.67 | 490 | 373.9 |
| 4096 | 2.77 | 506 | 361.6 |
| 16384 | 2.78 | 509 | 359.4 |
| 65536 | 2.71 | 496 | 368.9 |

Both modes pass with zero checksum errors, zero payload mismatches, zero
ordering faults and zero missing records, across 200 million records total.

The gap between the tables is the point. Integrity mode costs about 360ns per
record and barely moves with ring size, because an FNV-1a over 124 bytes plus a
full record rebuild dominates everything else. That number describes the
checks, not the queue. With the checks off the queue runs at 5.92ns per record
and the ring size starts to matter, which is what a queue benchmark should look
like: a 256 slot ring spends most of its time in backpressure at 49ns, and the
cost falls away as the ring gets large enough to absorb scheduling jitter
between the two processes.

Either way there is nothing to optimise for the actual workload. A player
produces 24 to 240 records a second, and the queue moves 168 million.
Instrumentation cannot distort what is being instrumented when the headroom is
six orders of magnitude.

### End to end

Instrumentation overhead, measured with the current protocol, which issues one
`get_property vo-passes` per frame. Both producers and the dashboard running,
CPU read from `/proc` as a share of one core.

| Setup | Producer CPU | Consumer CPU |
| --- | --- | --- |
| real mpv, 24 fps per stream | 0.12% | 0.67% |
| mock, 60 fps per stream | 0.14% | 0.93% |
| mock, 240 fps per stream | 0.68% | 2.16% |

The producer number is the one that matters, since that process shares a
machine with the players being measured. The consumer can be moved to another
terminal or another core and does not touch the render path.

Capture is lossless in all three. A 25 second run against real mpv with the
ESPCN and FSRCNNX shaders captured 609 frames from each player, paired all 609,
and recorded zero unmatched frames, zero records lost to a full ring, zero
checksum failures and zero sequence gaps. The 30 second mock run at 60 fps
captured 1824 frames per stream on the same terms.

These figures replace an earlier set taken before the IPC protocol changed.
Subscribing to `vo-passes` turned out not to work, so each frame now costs a
request and a reply instead of an unsolicited event, which moved the producer
from 0.10% to 0.14% at 60 fps. Worth knowing when reading the number, and still
small enough not to matter.

### Real shaders

Measured with `scripts/run_comparison.sh` against real mpv on real hardware, not
the mock. Source is a 960x540 h264 clip, each shader is a 2x luma upscaler, and
each run pairs two mpv instances side by side for 20 seconds with
`--vo=gpu-next`. Numbers are total GPU time per frame across every pass.

| A | B | A p50 | B p50 | delta p50 | A cheaper on |
| --- | --- | --- | --- | --- | --- |
| espcn_x2_8 | bilinear built in | 5.14ms | 226us | -4.91ms | 0% |
| espcn_x2_8 | FSRCNNX_x2_8 | 4.17ms | 5.70ms | +1.51ms | 99.8% |
| espcn_x2_8 | FSRCNNX_x2_16 | 5.14ms | 10.08ms | +4.73ms | 99.8% |
| FSR | NVScaler | 2.34ms | 4.41ms | +2.04ms | 99.8% |

Read the shader against built in scaler rows with care. These were taken before
the experiment was sized to the render area, so the source needed a 2.65x
upscale while every shader here only supplies 2x. A custom shader therefore did
its 2x and handed the remainder to a cheap bilinear finish, while a built in
scaler did the entire 2.65x with its own expensive filter. That is not the same
amount of work.

Shader against shader rows are unaffected, since both sides supply 2x and both
hand off the same remainder.

Re-running espcn against `ewa_lanczossharp` at an exact 2x, where both do the
same job, gives a very different and much less flattering picture:

| Config | GPU p50 | GPU p99 | PSNR dB | SSIM |
| --- | --- | --- | --- | --- |
| espcn_x2_8 | 820us | 2.21ms | 45.991 | 0.9082 |
| ewa_lanczossharp | 896us | 3.51ms | 46.082 | 0.9101 |

Roughly tied on median cost and tied on quality, with the interesting
difference in the tail: the shader's p99 is 2.21ms against 3.51ms, so it is the
more predictable of the two. An earlier run of the same pair reported the
shader as three times cheaper, and that number was an artefact of the unfair
setup above.

Chain lengths differ a lot more than the file sizes suggest:

| Shader | Passes |
| --- | --- |
| built in scaler, no shader | 3 |
| NVScaler | 3 |
| FSR | 4 |
| espcn_x2_8 | 9 |
| FSRCNNX_x2_8 | 18 |
| FSRCNNX_x2_16 | 30 |

Two things are worth saying about how these were read.

The bilinear row is the sanity check, not a result. A neural upscaler costing
roughly 20 times a bilinear filter is the expected shape, and seeing that shape
is what confirms the tool measures what it claims to.

The FSRCNNX rows were wrong the first time. The record held 16 pass slots, both
FSRCNNX chains are longer than that, and the overflow was silently dropped from
the GPU total. That made FSRCNNX_x2_8 look tied with espcn at 4.65ms against
4.68ms. With every pass counted the same comparison is 5.70ms against 4.17ms.
The fix was to raise the cap to 32 and, more importantly, to keep summing the
total past the cap so a chain longer than the table can still report an honest
total. A truncated measurement that looks plausible is worse than one that
obviously fails.

### Reading these numbers carefully

Both players share one GPU, so they contend with each other. That is fair in
the sense that both sides pay it, but absolute timings from a paired run sit
below what the same shader costs alone. Cross checking FSRCNNX_x2_16 against a
solo measurement taken straight from mpv gave 10.6ms against the 10.08ms the
paired run reported, which is close enough to trust the comparison.

One run produced an implausible result, a 30 pass network reporting less GPU
time than a 9 pass one, and it did not reproduce. The likely cause is a window
that was not rendering normally, since a compositor can throttle a surface that
is hidden or off screen. The report now prints a warning when the two players
present at rates more than 10 percent apart, because that split is the visible
symptom of the problem. Keep both windows fully visible when measuring.

## Testing

Four unit suites and a stress test, all wired into CTest:

| Suite | Covers |
| --- | --- |
| `ring` | ordering, wraparound, the full ring policy, batch pop across the array end, the pass directory, producer liveness, a threaded handoff of 200000 records |
| `json` | scalars, escapes and surrogate pairs, containers, 22 malformed inputs, depth limits, real mpv message shapes, writer round trip |
| `histogram` | quantile accuracy against an exact sorted reference, the bucket zero linear range, clamping past the ceiling, merging, both sliding windows |
| `stats` | frame pairing, unmatched retirement, flushing, per pass gating, corrupt record handling, sequence gap counting, duration formatting |
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
   including from the GPU total, which made longer chains look cheaper. Covered
   above in the benchmark section.

The fourth one is the reason the mock is now strict. A mock that accepts
anything reports a green run while the real integration captures nothing, which
is worse than having no mock at all.

## Platform

Linux and POSIX only, as specified. Windows support is not built, but the
platform specific calls are confined to three files (`src/shm.cpp`,
`src/ipc_client.cpp`, `src/term.cpp`), so an abstraction layer has a clear seam
to sit on. Everything else is portable C++.

## Layout

```
include/framewire/    public headers
src/                  library sources and the four binaries
tests/                unit suites
scripts/              run_comparison.sh, demo.sh, bench.sh
```

| Binary | Role |
| --- | --- |
| `framewire` | consumer, correlator and dashboard |
| `framewire-producer` | one per mpv instance, IPC to ring |
| `framewire-stress` | two process ring stress harness |
| `framewire-mock-mpv` | fake mpv for testing without a GPU |
