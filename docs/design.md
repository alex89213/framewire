# Design notes

How framewire is built and why. For how measurements are taken and when to
distrust them, see [measurement.md](measurement.md). For numbers, see
[benchmarks.md](benchmarks.md).

## The ring buffer

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

## Records

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

## Quantiles

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

## Grouping frames across the streams

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

Matching itself is a merge over every queue, and the decision only ever needs
the front record of each. If the earliest and the latest front are further
apart than the tolerance, the earliest can never be matched by anything still
to come, so it retires as unmatched rather than being grouped with something
unrelated. Otherwise every front is close enough and the whole group is emitted
together.

That generalises to any number of streams without changing shape, which is what
lets the cost pass compare four upscalers at once. A frame only counts when
every stream has one close to it, so one stream out of step costs the group
rather than a single pairing.

## Saying how sure the numbers are

Differences carry a 95% interval rather than a bare point estimate.

The median delta uses a distribution free interval built from order statistics:
the count of samples below a quantile is binomial, so the order statistics
either side of its normal approximation bracket the quantile. That is exact
under nothing more than independent samples, needs one sort rather than
hundreds of resamples, and has no random seed, so the same data always gives
the same interval. A bootstrap would also have worked and would have cost far
more per refresh for a weaker guarantee.

The share of frames where one stream is cheaper uses a Wilson score interval.
The textbook normal interval misbehaves near zero and one, which is exactly
where a shader comparison lands when one side wins almost every frame.

An interval that straddles zero means the run did not separate the two, and the
report says so instead of quoting the point estimate and staying quiet.

Every comparative number comes from grouped frames only. Comparing independent
averages would be misleading, because the instances can render a different
number of frames over the same wall time.

Per pass differences are only reported when both chains have the same pass
count. Lining up pass three of a five pass chain against pass three of a three
pass chain would produce a confident looking number that means nothing.

## The terminal

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
- **Terminal UI.** See [The terminal](#the-terminal). ncurses would bring a dependency, a
  global screen model and its own input handling, in exchange for a cell level
  diff this layout does not need.

`librt` is linked only when `shm_open` is found there, since the symbol moved
into libc on newer glibc.
