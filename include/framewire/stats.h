/*
 * Description: Per stream telemetry aggregation and the correlator that pairs
 *   frames from the two mpv instances by monotonic timestamp.
 * Author: Alex Wu
 * Dependencies: framewire/histogram.h, framewire/spsc_ring.h
 * Usage:
 */

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "framewire/histogram.h"
#include "framewire/spsc_ring.h"

namespace framewire {

// quantiles reported everywhere in the dashboard and the summary
inline constexpr double kQ50 = 0.50;
inline constexpr double kQ99 = 0.99;
inline constexpr double kQ999 = 0.999;

// how many recent gpu times the sparkline draws from
inline constexpr size_t kSparkSamples = 64;

struct QuantileSet {
  uint64_t p50 = 0;
  uint64_t p99 = 0;
  uint64_t p999 = 0;
  uint64_t max = 0;
};

struct PassView {
  std::string name;
  uint64_t p50 = 0;
  uint64_t p99 = 0;
  uint64_t mean = 0;
  uint64_t samples = 0;
};

/*
 * Everything the dashboard needs about one mpv instance.
 *
 * The snapshot is built once per refresh and handed to the renderer by value,
 * so the renderer never reaches back into live counters while they change.
 */
struct StreamSnapshot {
  std::string label;
  uint64_t records = 0;
  double fps = 0.0;         // over the whole run
  double fps_recent = 0.0;  // over the recent window, so a stall shows up

  // most recent sample, for watching a single frame rather than a distribution
  uint64_t gpu_last = 0;
  uint64_t frame_last = 0;

  QuantileSet frame_time_recent;
  QuantileSet frame_time_life;
  QuantileSet gpu_recent;
  QuantileSet gpu_life;
  double gpu_mean = 0.0;

  uint64_t dropped_frames = 0;
  uint64_t delayed_frames = 0;
  uint64_t producer_drops = 0;   // records the producer could not fit in the ring
  uint64_t ring_pending = 0;
  uint64_t checksum_errors = 0;
  uint64_t sequence_gaps = 0;
  bool producer_alive = false;

  // key=value description of where this stream was captured, and how many
  // times the window changed size while it ran
  std::string environment;
  uint32_t geometry_changes = 0;

  std::vector<PassView> passes;
  std::vector<uint64_t> spark;  // recent gpu totals, oldest first
};

struct PassDelta {
  std::string name;
  int64_t delta_p50 = 0;  // b minus a, negative means stream b is faster
  uint64_t a_p50 = 0;
  uint64_t b_p50 = 0;
};

/*
 * Comparative numbers derived only from frames that were matched across both
 * streams.
 *
 * Comparing the two independent averages would be misleading, because the
 * instances can render a different number of frames over the same wall time.
 * Every field here comes from paired samples.
 */
struct ComparisonSnapshot {
  uint64_t paired = 0;
  uint64_t unmatched_a = 0;
  uint64_t unmatched_b = 0;

  int64_t gpu_delta_p50 = 0;  // b minus a
  int64_t gpu_delta_p99 = 0;
  double gpu_delta_mean = 0.0;
  double a_faster_fraction = 0.0;  // share of pairs where stream a used less gpu time
  double speedup = 0.0;            // a p50 divided by b p50, above one means b is faster

  std::vector<PassDelta> pass_deltas;
};

/*
 * Drains one ring and keeps the running statistics for that stream.
 *
 * Records are verified on the way in. A bad checksum or a jump in the sequence
 * number is counted rather than thrown away silently, because those counters
 * are how a torn read would show up in a real run.
 */
class StreamAggregator {
 public:
  StreamAggregator(std::string label, RingConsumer consumer);

  /*
   * Moves records out of the ring and folds them into the statistics.
   *
   * Args:
   *   max_records: Cap on records to read in this call.
   *   out_matched: Receives the records read, for the correlator.
   * Returns:
   *   Number of records read.
   */
  size_t Pump(size_t max_records, std::vector<TelemetryRecord>* out_matched);

  StreamSnapshot Snapshot() const;
  void ResetStats();

  const std::string& label() const { return label_; }
  uint64_t records() const { return records_; }

 private:
  void RefreshLayout();

  std::string label_;
  RingConsumer consumer_;

  Histogram frame_time_hist_;
  Histogram gpu_hist_;
  SampleWindow frame_time_window_;
  SampleWindow gpu_window_;

  std::vector<Histogram> pass_hist_;
  std::vector<SampleWindow> pass_window_;
  char pass_names_[kMaxPasses][kPassNameLen]{};
  unsigned pass_count_ = 0;
  uint32_t layout_version_ = UINT32_MAX;

  uint64_t records_ = 0;
  uint64_t dropped_frames_ = 0;
  uint64_t delayed_frames_ = 0;
  uint64_t checksum_errors_ = 0;
  uint64_t sequence_gaps_ = 0;
  uint64_t last_seq_ = 0;
  uint64_t first_ts_ = 0;
  uint64_t last_ts_ = 0;

  std::vector<TelemetryRecord> scratch_;
};

/*
 * Pairs frames across the two streams and tracks the comparative statistics.
 *
 * The two mpv instances are never frame locked, so the streams drift against
 * each other. Matching is a merge over both queues: the earlier of the two
 * front records is either close enough to pair, or is old enough that no
 * future record from the other side could be closer, in which case the record
 * is retired as unmatched.
 */
class Correlator {
 public:
  /*
   * Builds a correlator.
   *
   * Args:
   *   tolerance_ns: Largest timestamp gap that still counts as the same frame.
   */
  explicit Correlator(uint64_t tolerance_ns);

  void PushA(const std::vector<TelemetryRecord>& records);
  void PushB(const std::vector<TelemetryRecord>& records);

  /*
   * Matches whatever can be decided from the queued records.
   *
   * Args:
   *   flush: Retire every remaining record, used once both producers finish.
   */
  void Process(bool flush = false);

  ComparisonSnapshot Snapshot(const StreamSnapshot& a, const StreamSnapshot& b) const;
  void Reset();

  // Records held back waiting for a possible partner.
  size_t PendingA() const { return queue_a_.size(); }
  size_t PendingB() const { return queue_b_.size(); }

  // Pairing key for a record, media position when available.
  int64_t PairingKey(const TelemetryRecord& rec) const;

  bool pairing_on_media_time() const { return use_media_time_; }

 private:
  uint64_t tolerance_ns_;

  // set once both sides have supplied a media position. mixing keys would be
  // meaningless, so the choice is made from what both streams actually carry
  bool use_media_time_ = false;
  bool media_seen_a_ = false;
  bool media_seen_b_ = false;

  std::deque<TelemetryRecord> queue_a_;
  std::deque<TelemetryRecord> queue_b_;

  SignedWindow gpu_delta_;
  std::vector<SignedWindow> pass_delta_;
  std::vector<SampleWindow> pass_a_;
  std::vector<SampleWindow> pass_b_;

  uint64_t paired_ = 0;
  uint64_t unmatched_a_ = 0;
  uint64_t unmatched_b_ = 0;
  uint64_t a_faster_ = 0;
};

/*
 * Formats a nanosecond duration into a short fixed width string.
 *
 * Args:
 *   ns: Duration in nanoseconds.
 * Returns:
 *   A compact string such as "1.23ms" or "840us".
 */
std::string FormatNanos(uint64_t ns);

/*
 * Formats a signed nanosecond difference with an explicit sign.
 *
 * Args:
 *   ns: Difference in nanoseconds.
 * Returns:
 *   A compact string such as "-1.23ms".
 */
std::string FormatSignedNanos(int64_t ns);

/*
 * Splits a key=value environment block into a map.
 *
 * Args:
 *   text: Newline separated key=value lines.
 * Returns:
 *   The parsed pairs.
 */
std::map<std::string, std::string> ParseEnvironment(const std::string& text);

/*
 * Lists the environment keys where two streams disagree.
 *
 * Args:
 *   a: Environment of the first stream.
 *   b: Environment of the second stream.
 * Returns:
 *   One description per differing key that matters for comparability.
 */
std::vector<std::string> EnvironmentMismatches(const std::string& a, const std::string& b);

}  // namespace framewire
