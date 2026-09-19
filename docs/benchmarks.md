# Benchmarks

Every number here was measured on the machine described below, not estimated.
Methodology and caveats live in [measurement.md](measurement.md).

Hardware: Intel Core i9-13900H, Linux 7.2.5, gcc 16.2.1, release build.

## Ring buffer throughput

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

## End to end

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

## Real shaders

Measured with `scripts/run_comparison.sh` against real mpv on real hardware, not
the mock. Source is a 960x540 h264 clip, each shader is a 2x luma upscaler, and
each run puts two mpv instances side by side for 20 seconds with
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
