/*
 * Description: Lock free single producer single consumer ring buffer that lives
 *   in POSIX shared memory, used to move telemetry records from one mpv
 *   producer process to the aggregator.
 * Author: Alex Wu
 * Dependencies: framewire/shm.h, framewire/telemetry.h
 * Usage:
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "framewire/shm.h"
#include "framewire/telemetry.h"

namespace framewire {

// x86_64 and arm64 both use 64 byte cache lines. the destructive interference
// size from <new> is not used because the value has to stay identical across
// every process mapping the segment, and a compiler flag must not change it
inline constexpr size_t kCacheLine = 64;

inline constexpr uint32_t kRingMagic = 0x4657524Bu;  // "FWRK"
inline constexpr uint32_t kRingAbiVersion = 2;

// room for the key=value description of the environment a stream was captured
// in. two streams recorded under different renderers or at different window
// sizes are not comparable, and the only way to know is to record it
inline constexpr unsigned kEnvironmentLen = 512;

/*
 * Control block at the front of the shared memory segment.
 *
 * The field grouping is the whole point of the struct. head and tail each get a
 * private cache line, so the producer bumping head never invalidates the line
 * the consumer owns for tail. Sharing one line between the two indices is the
 * classic false sharing bug in this kind of queue and costs a coherence miss on
 * every single push.
 *
 * Note that the cached copies of the far index are deliberately not stored
 * here. A cached value is written often and read by one side only, so parking
 * the value in shared memory would drag the other side's line back and forth
 * and undo the padding. Those copies live in the handle objects below, in
 * memory private to each process.
 */
struct alignas(kCacheLine) RingHeader {
  // --- immutable after Create, both sides read only ---
  uint32_t magic;
  uint32_t abi_version;
  uint32_t capacity;  // slot count, always a power of two
  uint32_t record_size;
  uint64_t created_mono_ns;
  char label[32];

  // each alignas below opens a fresh cache line, which is what keeps head and
  // tail apart. explicit padding members are left out on purpose, hand counted
  // padding silently rots the moment a field above changes width
  // --- producer line, written by the producer and read by the consumer ---
  alignas(kCacheLine) std::atomic<uint64_t> head;

  // --- consumer line, written by the consumer and read by the producer ---
  alignas(kCacheLine) std::atomic<uint64_t> tail;

  // --- producer side counters, read by the consumer for display only ---
  alignas(kCacheLine) std::atomic<uint64_t> push_dropped;  // records lost to a full ring
  std::atomic<uint64_t> producer_heartbeat_ns;
  std::atomic<uint32_t> layout_version;  // bumped when pass_names changes
  std::atomic<uint32_t> pass_count;
  std::atomic<uint32_t> producer_pid;
  std::atomic<uint32_t> producer_done;
  std::atomic<uint32_t> environment_version;  // non zero once environment is written
  std::atomic<uint32_t> geometry_changes;     // window resized mid run this many times

  // --- pass name directory, producer writes then publishes layout_version ---
  alignas(kCacheLine) char pass_names[kMaxPasses][kPassNameLen];

  // --- capture environment, written once before streaming starts ---
  alignas(kCacheLine) char environment[kEnvironmentLen];
};

static_assert(sizeof(RingHeader) % kCacheLine == 0, "header must end on a cache line");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "a lock in the index would defeat the whole design");

// the padding claim is worth checking rather than trusting. if a future field
// change ever lands head and tail on one line the queue still works, it just
// quietly gets much slower, and a silent performance cliff is the worst kind
static_assert(offsetof(RingHeader, tail) - offsetof(RingHeader, head) >= kCacheLine,
              "head and tail must not share a cache line");
static_assert(offsetof(RingHeader, head) % kCacheLine == 0, "head must start a cache line");
static_assert(offsetof(RingHeader, tail) % kCacheLine == 0, "tail must start a cache line");

/*
 * Computes the segment size needed for a ring of the given capacity.
 *
 * Args:
 *   capacity: Slot count, must be a power of two.
 * Returns:
 *   Total bytes to allocate for header plus slots.
 */
constexpr size_t RingBytes(uint32_t capacity) {
  return sizeof(RingHeader) + static_cast<size_t>(capacity) * sizeof(TelemetryRecord);
}

/*
 * Maps a ring segment and exposes the header and slot array.
 *
 * Producer and consumer both build one of these, then wrap the result in
 * RingProducer or RingConsumer. Splitting the roles into separate types keeps
 * a consumer from calling Push by accident, which would break the single
 * producer assumption the memory ordering relies on.
 */
class RingMapping {
 public:
  /*
   * Creates a new ring segment and initialises the header.
   *
   * Args:
   *   name: Shared memory name, must start with a slash.
   *   capacity: Slot count, must be a power of two.
   *   label: Short human readable tag stored in the header.
   * Returns:
   *   A mapping that owns the segment and removes the segment on destruction.
   */
  static RingMapping Create(const std::string& name, uint32_t capacity, const std::string& label);

  /*
   * Attaches to a ring segment that a producer already created.
   *
   * The header magic, abi version and record size are all checked, because
   * attaching to a segment written by a different build would hand back
   * records with a scrambled layout.
   *
   * Args:
   *   name: Shared memory name, must start with a slash.
   * Returns:
   *   A mapping that leaves the segment in place on destruction.
   */
  static RingMapping Open(const std::string& name);

  RingHeader* header() const { return header_; }
  TelemetryRecord* slots() const { return slots_; }
  uint32_t capacity() const { return header_->capacity; }
  ShmRegion& region() { return region_; }

 private:
  ShmRegion region_;
  RingHeader* header_ = nullptr;
  TelemetryRecord* slots_ = nullptr;
};

/*
 * Write side of the ring.
 *
 * Exactly one process, and one thread inside that process, may hold a producer
 * for a given segment. Every relaxed load below depends on that.
 */
class RingProducer {
 public:
  explicit RingProducer(RingMapping mapping);

  /*
   * Pushes one record, failing instead of blocking when the ring is full.
   *
   * A full ring means the aggregator fell behind. Dropping the newest sample
   * and counting the drop is the honest choice for telemetry, because
   * overwriting the oldest unread slot would race with a consumer that is
   * mid copy and would silently bias the latency stats toward recent frames.
   *
   * Args:
   *   rec: Record to copy into the ring, checksummed by the caller.
   * Returns:
   *   True when the record was stored, false when the ring was full.
   */
  bool TryPush(const TelemetryRecord& rec);

  /*
   * Publishes the pass name directory so the consumer can label pass timings.
   *
   * Args:
   *   names: Pass descriptions in the same order as TelemetryRecord::pass_ns.
   *   count: Number of valid entries in names, capped at kMaxPasses.
   * Returns:
   *   The layout version that records should now carry.
   */
  uint8_t PublishLayout(const char* const* names, unsigned count);

  /*
   * Publishes the environment this stream was captured in.
   *
   * Written once before streaming, as key=value lines. The consumer compares
   * the two streams and refuses to treat them as one experiment when the
   * renderer, the decode path or the window size differ.
   *
   * Args:
   *   text: Newline separated key=value lines.
   */
  void PublishEnvironment(const std::string& text);

  // Counts a window resize, which invalidates comparisons made across it.
  void NoteGeometryChange();

  // Records a liveness stamp so the consumer can flag a stalled producer.
  void Heartbeat();

  // Marks the stream as finished so the consumer stops waiting for more.
  void MarkDone();

  uint64_t dropped() const {
    return header_->push_dropped.load(std::memory_order_relaxed);
  }

  RingHeader* header() const { return header_; }

 private:
  RingMapping mapping_;
  RingHeader* header_;
  TelemetryRecord* slots_;
  uint64_t mask_;

  // producer private copy of the consumer's tail. reading the real tail touches
  // a line the consumer owns, so the value is cached and only refreshed when
  // the ring looks full. on a healthy run that refresh almost never happens
  uint64_t cached_tail_ = 0;
};

/*
 * Read side of the ring.
 *
 * Exactly one process, and one thread inside that process, may hold a consumer
 * for a given segment.
 */
class RingConsumer {
 public:
  explicit RingConsumer(RingMapping mapping);

  /*
   * Pops one record if the ring is not empty.
   *
   * Args:
   *   out: Destination record, only written when the call returns true.
   * Returns:
   *   True when a record was copied out.
   */
  bool TryPop(TelemetryRecord& out);

  /*
   * Pops up to max_records in one pass.
   *
   * Batching matters because the acquire load of head is the expensive part of
   * a pop. One load covers the whole batch, so draining a burst of frames
   * costs roughly one coherence miss instead of one per record.
   *
   * Args:
   *   out: Destination array with room for max_records entries.
   *   max_records: Cap on how many records to copy.
   * Returns:
   *   Number of records copied.
   */
  size_t PopBatch(TelemetryRecord* out, size_t max_records);

  /*
   * Copies the pass name directory published by the producer.
   *
   * Args:
   *   out_names: Destination array of kMaxPasses name buffers.
   *   out_version: Receives the layout version the names belong to.
   * Returns:
   *   Number of names copied.
   */
  unsigned ReadLayout(char out_names[kMaxPasses][kPassNameLen], uint32_t* out_version) const;

  /*
   * Reads the environment the producer recorded.
   *
   * Returns:
   *   The key=value block, or an empty string when the producer wrote none.
   */
  std::string ReadEnvironment() const;

  // How many times the window changed size while the stream was running.
  uint32_t geometry_changes() const {
    return header_->geometry_changes.load(std::memory_order_relaxed);
  }

  // Number of records sitting in the ring right now.
  uint64_t PendingCount() const;

  uint64_t producer_dropped() const {
    return header_->push_dropped.load(std::memory_order_relaxed);
  }

  /*
   * Reports whether the producer has stopped sending.
   *
   * Args:
   *   stale_after_ns: Age of the last heartbeat that counts as stalled.
   * Returns:
   *   True when the producer exited cleanly or went quiet for too long.
   */
  bool ProducerGone(uint64_t stale_after_ns) const;

  RingHeader* header() const { return header_; }

 private:
  RingMapping mapping_;
  RingHeader* header_;
  const TelemetryRecord* slots_;
  uint64_t mask_;

  // consumer private copy of the producer's head, cached for the same reason
  // the producer caches tail
  uint64_t cached_head_ = 0;
};

}  // namespace framewire
