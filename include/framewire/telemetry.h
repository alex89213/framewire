/*
 * Description: Fixed size telemetry record that the mpv producers write and the
 *   aggregator reads, plus the monotonic clock helper used to stamp records.
 * Author: Alex Wu
 * Dependencies:
 * Usage:
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <type_traits>

namespace framewire {

// upper bound on shader passes stored per frame. real chains are bigger than
// they look: mpv's own scaler uses 3, a small ESPCN 9, FSRCNNX x2_8 18 and
// FSRCNNX x2_16 30. an earlier cap of 16 silently truncated the FSRCNNX chains
// and undercounted their gpu total, which made the comparison favour them
inline constexpr unsigned kMaxPasses = 32;

// byte cap for a pass description copied out of vo-passes, including the null
inline constexpr unsigned kPassNameLen = 48;

enum RecordFlags : uint16_t {
  kFlagNone = 0,
  kFlagDroppedFrame = 1u << 0,  // mpv counted a new dropped frame at this sample
  kFlagDelayedFrame = 1u << 1,  // mpv counted a new delayed (late) frame
  kFlagRedraw = 1u << 2,        // sample came from the redraw list, not fresh
  kFlagLayoutChange = 1u << 3,   // pass layout changed relative to the last record
  kFlagPassOverflow = 1u << 4,   // chain had more passes than pass_ns can hold
};

/*
 * One captured frame of gpu render telemetry.
 *
 * The layout is pinned at 192 bytes and 64 byte aligned, so a record covers a
 * whole number of cache lines and never straddles an extra one. Every field is
 * a fixed width integer and there are no pointers, because the struct lives in
 * shared memory that each process maps at a different address.
 *
 * Pass names are not stored here. Names are stable for the lifetime of a
 * shader chain, so they live once in the ring header and pass_ns is indexed
 * against that directory.
 */
struct alignas(64) TelemetryRecord {
  uint64_t seq;            // producer counter, starts at 1 and never resets
  uint64_t t_mono_ns;      // CLOCK_MONOTONIC stamp taken when the sample arrived
  uint64_t frame_time_ns;  // gap between this sample and the previous sample
  uint64_t gpu_total_ns;   // sum of every pass time in this sample

  // position inside the video this frame came from, which is what lets two
  // free running players be compared. the two instances are never phase
  // locked, so arrival time carries an arbitrary offset between them, while
  // the media position of a given frame is identical in both
  int64_t media_time_ns;
  uint64_t reserved;

  uint32_t pass_ns[kMaxPasses];  // per pass gpu time, indexed by the header directory

  uint8_t pass_count;
  uint8_t layout_version;  // ring header layout_version in effect for pass_ns
  uint16_t flags;
  uint32_t dropped_total;  // mpv frame-drop-count at capture time
  uint32_t delayed_total;  // mpv vo-delayed-frame-count at capture time

  // checksum is deliberately the last field, so the covered range is every
  // other byte of the record. a torn read anywhere, including the padding
  // fields, then shows up as a mismatch instead of slipping through
  uint32_t checksum;
};

static_assert(sizeof(TelemetryRecord) == 192, "record size is part of the shm layout");
static_assert(alignof(TelemetryRecord) == 64, "records must not straddle a third cache line");
static_assert(std::is_trivially_copyable_v<TelemetryRecord>,
              "records are copied byte for byte through shared memory");

/*
 * Reads the monotonic clock in nanoseconds.
 *
 * CLOCK_MONOTONIC is the right clock here because both producers and the
 * aggregator run on one machine and the values only ever get subtracted from
 * each other. A wall clock would let an NTP step corrupt a frame time.
 *
 * Returns:
 *   Nanoseconds since an unspecified boot relative origin.
 */
inline uint64_t MonotonicNanos() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * UINT64_C(1000000000) +
         static_cast<uint64_t>(ts.tv_nsec);
}

/*
 * Converts a millisecond timeout into nanoseconds.
 *
 * Args:
 *   ms: Timeout in milliseconds, negative values become zero.
 * Returns:
 *   The same timeout in nanoseconds.
 */
inline uint64_t MillisToNanos(int ms) {
  if (ms < 0) return 0;
  return static_cast<uint64_t>(ms) * UINT64_C(1000000);
}

/*
 * Computes the fnv1a hash of a byte range.
 *
 * Used for the record checksum and for detecting a changed pass layout. fnv1a
 * is not a security hash, the only job here is catching a torn record in the
 * stress harness.
 *
 * Args:
 *   data: Start of the range to hash.
 *   len: Number of bytes to hash.
 * Returns:
 *   The 32 bit hash of the range.
 */
inline uint32_t Fnv1a(const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

// bytes of a record that the checksum covers, everything before the field itself
inline constexpr size_t kChecksumCoverage = offsetof(TelemetryRecord, checksum);

/*
 * Fills in the checksum field of a record.
 *
 * Args:
 *   rec: Record to stamp, modified in place.
 */
inline void StampChecksum(TelemetryRecord& rec) {
  rec.checksum = Fnv1a(&rec, kChecksumCoverage);
}

/*
 * Checks a record against the checksum written by the producer.
 *
 * Args:
 *   rec: Record to verify.
 * Returns:
 *   True when the checksum matches the record contents.
 */
inline bool VerifyChecksum(const TelemetryRecord& rec) {
  return rec.checksum == Fnv1a(&rec, kChecksumCoverage);
}

}  // namespace framewire
