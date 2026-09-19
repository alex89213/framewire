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
 * One stream's standing in the comparison, measured against the baseline.
 *
 * Every field here comes from grouped frames only, never from the stream's own
 * lifetime statistics, so the streams are always compared on the same frames.
 */
struct StreamComparison {
  std::string label;
  uint64_t gpu_p50 = 0;           // over grouped frames only
  int64_t delta_p50 = 0;          // this stream minus the baseline
  double cheaper_fraction = 0.0;  // share of groups where this stream cost less
  double speedup = 0.0;           // baseline p50 divided by this p50
  bool is_baseline = false;

  // 95% bounds on the two numbers above. a point estimate with no interval
  // invites a reader to treat noise as a result, which is the failure this
  // whole tool is built to avoid
  int64_t delta_p50_low = 0;
  int64_t delta_p50_high = 0;
  double cheaper_low = 0.0;
  double cheaper_high = 0.0;
  bool interval_valid = false;

  // true when the delta interval stays entirely on one side of zero
  bool significant = false;
};

/*
 * Comparative numbers derived only from frames matched across every stream.
 *
 * Comparing independent averages would be misleading, because the instances
 * can render a different number of frames over the same wall time. A frame
 * counts only when every stream has one close enough to it.
 */
struct ComparisonSnapshot {
  uint64_t grouped = 0;               // frames matched across all streams
  std::vector<uint64_t> unmatched;    // per stream, retired without a partner
  std::vector<StreamComparison> streams;

  // per pass differences only make sense between two chains of equal length,
  // so these are populated for a two stream run and left empty otherwise
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
 * Groups frames across any number of streams and tracks the comparison.
 *
 * The players are never frame locked, so the streams drift against each other.
 * Matching is a merge over all queues and the decision only ever needs the
 * front record of each: if the earliest and the latest front are further apart
 * than the tolerance, the earliest can never be matched by anything still to
 * come, so it retires unmatched. Otherwise every front is close enough and the
 * whole group is emitted together.
 *
 * Stream zero is the baseline every other stream is reported against.
 */
class Correlator {
 public:
  /*
   * Builds a correlator.
   *
   * Args:
   *   stream_count: Number of streams to group across, at least two.
   *   tolerance_ns: Largest key gap that still counts as the same frame.
   */
  Correlator(size_t stream_count, uint64_t tolerance_ns);

  /*
   * Queues records for one stream.
   *
   * Args:
   *   stream: Stream index.
   *   records: Records in arrival order.
   */
  void Push(size_t stream, const std::vector<TelemetryRecord>& records);

  /*
   * Groups whatever can be decided from the queued records.
   *
   * Args:
   *   flush: Retire every remaining record, used once the producers finish.
   */
  void Process(bool flush = false);

  /*
   * Builds the comparison.
   *
   * Args:
   *   streams: Per stream snapshots, in the same order as the stream indices.
   * Returns:
   *   The comparison across grouped frames.
   */
  ComparisonSnapshot Snapshot(const std::vector<StreamSnapshot>& streams) const;

  void Reset();

  size_t stream_count() const { return queues_.size(); }

  // Records held back waiting for a possible partner.
  size_t Pending(size_t stream) const { return queues_[stream].size(); }

  // Pairing key for a record, media position when available.
  int64_t PairingKey(const TelemetryRecord& rec) const;

  bool pairing_on_media_time() const { return use_media_time_; }

 private:
  // Emits one group once every front has been confirmed close enough.
  void EmitGroup();

  uint64_t tolerance_ns_;

  // set once every stream has supplied a media position. mixing keys would be
  // meaningless, so the choice is made from what the streams actually carry
  bool use_media_time_ = false;
  std::vector<bool> media_seen_;

  std::vector<std::deque<TelemetryRecord>> queues_;

  // deltas against stream zero, so index zero stays unused
  std::vector<SignedWindow> gpu_delta_;
  std::vector<SampleWindow> gpu_grouped_;
  std::vector<uint64_t> cheaper_than_baseline_;
  std::vector<uint64_t> unmatched_;

  // pairwise pass detail, only kept for a two stream run
  std::vector<SignedWindow> pass_delta_;
  std::vector<SampleWindow> pass_a_;
  std::vector<SampleWindow> pass_b_;

  uint64_t grouped_ = 0;
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
 * Computes a Wilson score interval for a proportion.
 *
 * The textbook normal interval misbehaves near zero and one, which is exactly
 * where a shader comparison lands when one side wins almost every frame. Wilson
 * stays inside the unit interval and is still a one line calculation.
 *
 * Args:
 *   successes: Count of positive outcomes.
 *   trials: Total count.
 *   z: Standard normal quantile, 1.96 for 95%.
 *   low: Receives the lower bound.
 *   high: Receives the upper bound.
 */
void WilsonInterval(uint64_t successes, uint64_t trials, double z, double* low, double* high);

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
