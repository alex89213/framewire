/*
 * Description: Implementation of per stream aggregation and the timestamp
 *   correlator that pairs frames across the two mpv instances.
 * Author: Alex Wu
 * Dependencies: framewire/stats.h
 * Usage:
 */

#include "framewire/stats.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>

namespace framewire {
namespace {

// a producer that has not checked in for this long is treated as gone
constexpr uint64_t kProducerStaleNs = 2ull * 1000 * 1000 * 1000;

QuantileSet FromWindow(const SampleWindow& w) {
  QuantileSet q;
  const double quantiles[3] = {kQ50, kQ99, kQ999};
  uint64_t values[3] = {0, 0, 0};
  w.ValuesAtQuantiles(quantiles, values, 3);
  q.p50 = values[0];
  q.p99 = values[1];
  q.p999 = values[2];
  q.max = w.ValueAtQuantile(1.0);
  return q;
}

QuantileSet FromHistogram(const Histogram& h) {
  QuantileSet q;
  q.p50 = h.ValueAtQuantile(kQ50);
  q.p99 = h.ValueAtQuantile(kQ99);
  q.p999 = h.ValueAtQuantile(kQ999);
  q.max = h.max();
  return q;
}

}  // namespace

StreamAggregator::StreamAggregator(std::string label, RingConsumer consumer)
    : label_(std::move(label)), consumer_(std::move(consumer)) {
  pass_hist_.reserve(kMaxPasses);
  pass_window_.reserve(kMaxPasses);
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    pass_hist_.emplace_back();
    pass_window_.emplace_back();
  }
  scratch_.resize(1024);
}

void StreamAggregator::RefreshLayout() {
  uint32_t version = 0;
  char names[kMaxPasses][kPassNameLen];
  const unsigned count = consumer_.ReadLayout(names, &version);

  if (version == layout_version_) return;

  // the shader chain changed, so old per pass timings no longer describe the
  // same work. keeping them would blend two different pipelines into one number
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    pass_hist_[i].Reset();
    pass_window_[i].Reset();
  }
  std::memcpy(pass_names_, names, sizeof(pass_names_));
  pass_count_ = count;
  layout_version_ = version;
}

size_t StreamAggregator::Pump(size_t max_records, std::vector<TelemetryRecord>* out_matched) {
  if (scratch_.size() < max_records) scratch_.resize(max_records);

  const size_t n = consumer_.PopBatch(scratch_.data(), max_records);
  if (n == 0) return 0;

  RefreshLayout();

  for (size_t i = 0; i < n; ++i) {
    const TelemetryRecord& rec = scratch_[i];

    if (!VerifyChecksum(rec)) {
      // a mismatch means the bytes moved while being read, which the ordering
      // protocol should make impossible. count it rather than trusting it
      ++checksum_errors_;
      continue;
    }

    if (last_seq_ != 0 && rec.seq != last_seq_ + 1) {
      // the producer drops on a full ring, so a gap is expected under
      // backpressure and is not by itself a bug
      ++sequence_gaps_;
    }
    last_seq_ = rec.seq;

    if (first_ts_ == 0) first_ts_ = rec.t_mono_ns;
    last_ts_ = rec.t_mono_ns;

    if (rec.frame_time_ns > 0) {
      frame_time_hist_.Record(rec.frame_time_ns);
      frame_time_window_.Record(rec.frame_time_ns);
    }
    if (rec.gpu_total_ns > 0) {
      gpu_hist_.Record(rec.gpu_total_ns);
      gpu_window_.Record(rec.gpu_total_ns);
    }

    if ((rec.flags & kFlagDroppedFrame) != 0) ++dropped_frames_;
    if ((rec.flags & kFlagDelayedFrame) != 0) ++delayed_frames_;

    const unsigned passes = std::min<unsigned>(rec.pass_count, kMaxPasses);
    for (unsigned p = 0; p < passes; ++p) {
      if (rec.pass_ns[p] == 0) continue;
      pass_hist_[p].Record(rec.pass_ns[p]);
      pass_window_[p].Record(rec.pass_ns[p]);
    }

    ++records_;
    if (out_matched != nullptr) out_matched->push_back(rec);
  }
  return n;
}

StreamSnapshot StreamAggregator::Snapshot() const {
  StreamSnapshot s;
  s.label = label_;
  s.records = records_;

  const uint64_t span = last_ts_ > first_ts_ ? last_ts_ - first_ts_ : 0;
  if (span > 0 && records_ > 1) {
    s.fps = static_cast<double>(records_ - 1) * 1e9 / static_cast<double>(span);
  }

  s.frame_time_recent = FromWindow(frame_time_window_);
  s.frame_time_life = FromHistogram(frame_time_hist_);
  s.gpu_recent = FromWindow(gpu_window_);
  s.gpu_life = FromHistogram(gpu_hist_);
  s.gpu_mean = gpu_hist_.Mean();

  s.dropped_frames = dropped_frames_;
  s.delayed_frames = delayed_frames_;
  s.checksum_errors = checksum_errors_;
  s.sequence_gaps = sequence_gaps_;
  s.producer_drops = consumer_.producer_dropped();
  s.ring_pending = consumer_.PendingCount();
  s.producer_alive = !consumer_.ProducerGone(kProducerStaleNs);

  for (unsigned p = 0; p < pass_count_; ++p) {
    PassView v;
    v.name = pass_names_[p];
    v.p50 = pass_window_[p].ValueAtQuantile(kQ50);
    v.p99 = pass_window_[p].ValueAtQuantile(kQ99);
    v.mean = static_cast<uint64_t>(pass_hist_[p].Mean());
    v.samples = pass_hist_[p].count();
    s.passes.push_back(std::move(v));
  }

  s.spark.resize(kSparkSamples);
  gpu_window_.CopyRecent(s.spark.data(), kSparkSamples);
  return s;
}

void StreamAggregator::ResetStats() {
  frame_time_hist_.Reset();
  gpu_hist_.Reset();
  frame_time_window_.Reset();
  gpu_window_.Reset();
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    pass_hist_[i].Reset();
    pass_window_[i].Reset();
  }
  records_ = 0;
  dropped_frames_ = 0;
  delayed_frames_ = 0;
  checksum_errors_ = 0;
  sequence_gaps_ = 0;
  first_ts_ = 0;
  last_ts_ = 0;
}

/*
 * Picks the value two records are matched on.
 *
 * Media position is the right key whenever mpv supplies one. Two players
 * started by hand are never phase locked, so arrival times carry an arbitrary
 * constant offset between the streams, and at 24 fps that offset is routinely
 * larger than any sensible tolerance. Media position does not have that
 * problem, because the same frame of the same file carries the same position
 * in both players.
 *
 * Args:
 *   rec: Record to read a key from.
 * Returns:
 *   Media position when present, otherwise the monotonic arrival stamp.
 */
int64_t Correlator::PairingKey(const TelemetryRecord& rec) const {
  if (use_media_time_ && rec.media_time_ns > 0) return rec.media_time_ns;
  return static_cast<int64_t>(rec.t_mono_ns);
}

Correlator::Correlator(uint64_t tolerance_ns) : tolerance_ns_(tolerance_ns) {
  pass_delta_.reserve(kMaxPasses);
  pass_a_.reserve(kMaxPasses);
  pass_b_.reserve(kMaxPasses);
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    pass_delta_.emplace_back();
    pass_a_.emplace_back();
    pass_b_.emplace_back();
  }
}

void Correlator::PushA(const std::vector<TelemetryRecord>& records) {
  for (const auto& r : records) {
    if (r.media_time_ns > 0) media_seen_a_ = true;
    queue_a_.push_back(r);
  }
  use_media_time_ = media_seen_a_ && media_seen_b_;
}

void Correlator::PushB(const std::vector<TelemetryRecord>& records) {
  for (const auto& r : records) {
    if (r.media_time_ns > 0) media_seen_b_ = true;
    queue_b_.push_back(r);
  }
  use_media_time_ = media_seen_a_ && media_seen_b_;
}

void Correlator::Process(bool flush) {
  while (!queue_a_.empty() && !queue_b_.empty()) {
    const TelemetryRecord& a = queue_a_.front();
    const TelemetryRecord& b = queue_b_.front();

    const int64_t key_a = PairingKey(a);
    const int64_t key_b = PairingKey(b);
    const int64_t window = static_cast<int64_t>(tolerance_ns_);

    if (key_a + window < key_b) {
      // every later b is further away still, so this a can never be matched
      ++unmatched_a_;
      queue_a_.pop_front();
      continue;
    }
    if (key_b + window < key_a) {
      ++unmatched_b_;
      queue_b_.pop_front();
      continue;
    }

    const auto delta =
        static_cast<int64_t>(b.gpu_total_ns) - static_cast<int64_t>(a.gpu_total_ns);
    gpu_delta_.Record(delta);
    if (delta > 0) ++a_faster_;

    // per pass differences only mean something when both sides ran the same
    // number of passes, otherwise pass three of one chain is compared against
    // unrelated work in the other
    if (a.pass_count == b.pass_count) {
      const unsigned passes = std::min<unsigned>(a.pass_count, kMaxPasses);
      for (unsigned p = 0; p < passes; ++p) {
        pass_delta_[p].Record(static_cast<int64_t>(b.pass_ns[p]) -
                              static_cast<int64_t>(a.pass_ns[p]));
        pass_a_[p].Record(a.pass_ns[p]);
        pass_b_[p].Record(b.pass_ns[p]);
      }
    }

    ++paired_;
    queue_a_.pop_front();
    queue_b_.pop_front();
  }

  if (flush) {
    unmatched_a_ += queue_a_.size();
    unmatched_b_ += queue_b_.size();
    queue_a_.clear();
    queue_b_.clear();
    return;
  }

  // one side can run far ahead when the other producer stalls. the queues are
  // capped so a stalled partner costs bounded memory instead of growing until
  // the process is killed
  constexpr size_t kMaxQueued = 1u << 16;
  while (queue_a_.size() > kMaxQueued) {
    ++unmatched_a_;
    queue_a_.pop_front();
  }
  while (queue_b_.size() > kMaxQueued) {
    ++unmatched_b_;
    queue_b_.pop_front();
  }
}

ComparisonSnapshot Correlator::Snapshot(const StreamSnapshot& a, const StreamSnapshot& b) const {
  ComparisonSnapshot c;
  c.paired = paired_;
  c.unmatched_a = unmatched_a_;
  c.unmatched_b = unmatched_b_;

  c.gpu_delta_p50 = gpu_delta_.ValueAtQuantile(kQ50);
  c.gpu_delta_p99 = gpu_delta_.ValueAtQuantile(kQ99);
  c.gpu_delta_mean = gpu_delta_.Mean();
  if (paired_ > 0) {
    c.a_faster_fraction = static_cast<double>(a_faster_) / static_cast<double>(paired_);
  }
  if (b.gpu_life.p50 > 0) {
    c.speedup = static_cast<double>(a.gpu_life.p50) / static_cast<double>(b.gpu_life.p50);
  }

  const size_t pass_count = std::min(a.passes.size(), b.passes.size());
  for (size_t p = 0; p < pass_count; ++p) {
    if (pass_delta_[p].empty()) continue;
    PassDelta d;
    d.name = a.passes[p].name;
    d.delta_p50 = pass_delta_[p].ValueAtQuantile(kQ50);
    d.a_p50 = pass_a_[p].ValueAtQuantile(kQ50);
    d.b_p50 = pass_b_[p].ValueAtQuantile(kQ50);
    c.pass_deltas.push_back(std::move(d));
  }
  return c;
}

void Correlator::Reset() {
  queue_a_.clear();
  queue_b_.clear();
  gpu_delta_.Reset();
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    pass_delta_[i].Reset();
    pass_a_[i].Reset();
    pass_b_[i].Reset();
  }
  paired_ = 0;
  unmatched_a_ = 0;
  unmatched_b_ = 0;
  a_faster_ = 0;
  use_media_time_ = false;
  media_seen_a_ = false;
  media_seen_b_ = false;
}

std::string FormatNanos(uint64_t ns) {
  char buf[32];
  if (ns == 0) {
    return "-";
  }
  if (ns < 1000ull) {
    std::snprintf(buf, sizeof(buf), "%" PRIu64 "ns", ns);
  } else if (ns < 1000000ull) {
    std::snprintf(buf, sizeof(buf), "%.1fus", static_cast<double>(ns) / 1e3);
  } else if (ns < 1000000000ull) {
    std::snprintf(buf, sizeof(buf), "%.2fms", static_cast<double>(ns) / 1e6);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2fs", static_cast<double>(ns) / 1e9);
  }
  return buf;
}

std::string FormatSignedNanos(int64_t ns) {
  const char* sign = ns < 0 ? "-" : "+";
  const auto magnitude = static_cast<uint64_t>(ns < 0 ? -ns : ns);
  if (magnitude == 0) return "0";
  return std::string(sign) + FormatNanos(magnitude);
}

}  // namespace framewire
