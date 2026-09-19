/*
 * Description: Implementation of per stream aggregation and the timestamp
 *   correlator that pairs frames across the two mpv instances.
 * Author: Alex Wu
 * Dependencies: framewire/stats.h
 * Usage:
 */

#include "framewire/stats.h"

#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>

namespace framewire {
namespace {

// a producer that has not checked in for this long is treated as gone
constexpr uint64_t kProducerStaleNs = 2ull * 1000 * 1000 * 1000;

// standard normal quantile for a two sided 95 percent interval
constexpr double kZ95 = 1.959964;

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

  // a run long average barely moves when a player starts stuttering, so the
  // live rate is derived from the recent frame gaps instead
  const double recent_gap = frame_time_window_.Mean();
  if (recent_gap > 0.0) s.fps_recent = 1e9 / recent_gap;

  s.gpu_last = gpu_window_.Newest();
  s.frame_last = frame_time_window_.Newest();

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
  s.environment = consumer_.ReadEnvironment();
  s.geometry_changes = consumer_.geometry_changes();

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

Correlator::Correlator(size_t stream_count, uint64_t tolerance_ns)
    : tolerance_ns_(tolerance_ns) {
  if (stream_count < 2) stream_count = 2;

  media_seen_.assign(stream_count, false);
  queues_.resize(stream_count);
  gpu_delta_.resize(stream_count);
  gpu_grouped_.resize(stream_count);
  cheaper_than_baseline_.assign(stream_count, 0);
  unmatched_.assign(stream_count, 0);

  pass_delta_.resize(kMaxPasses);
  pass_a_.resize(kMaxPasses);
  pass_b_.resize(kMaxPasses);
}

void Correlator::Push(size_t stream, const std::vector<TelemetryRecord>& records) {
  if (stream >= queues_.size()) return;

  for (const auto& r : records) {
    if (r.media_time_ns > 0) media_seen_[stream] = true;
    queues_[stream].push_back(r);
  }

  // media position is only usable as the key when every stream carries one
  use_media_time_ = true;
  for (const bool seen : media_seen_) {
    if (!seen) use_media_time_ = false;
  }
}

void Correlator::EmitGroup() {
  const TelemetryRecord& base = queues_[0].front();
  const auto base_gpu = static_cast<int64_t>(base.gpu_total_ns);
  gpu_grouped_[0].Record(base.gpu_total_ns);

  for (size_t i = 1; i < queues_.size(); ++i) {
    const TelemetryRecord& rec = queues_[i].front();
    const auto delta = static_cast<int64_t>(rec.gpu_total_ns) - base_gpu;
    gpu_delta_[i].Record(delta);
    gpu_grouped_[i].Record(rec.gpu_total_ns);
    if (delta < 0) ++cheaper_than_baseline_[i];
  }

  // per pass differences only mean something when both sides ran the same
  // number of passes, otherwise pass three of one chain is compared against
  // unrelated work in the other. only tracked for a straight two way run
  if (queues_.size() == 2) {
    const TelemetryRecord& other = queues_[1].front();
    if (base.pass_count == other.pass_count) {
      const unsigned passes = std::min<unsigned>(base.pass_count, kMaxPasses);
      for (unsigned p = 0; p < passes; ++p) {
        pass_delta_[p].Record(static_cast<int64_t>(other.pass_ns[p]) -
                              static_cast<int64_t>(base.pass_ns[p]));
        pass_a_[p].Record(base.pass_ns[p]);
        pass_b_[p].Record(other.pass_ns[p]);
      }
    }
  }

  ++grouped_;
  for (auto& q : queues_) q.pop_front();
}

void Correlator::Process(bool flush) {
  const auto window = static_cast<int64_t>(tolerance_ns_);

  for (;;) {
    // every stream has to have something before any decision can be made
    for (const auto& q : queues_) {
      if (q.empty()) goto drain;
    }

    {
      size_t earliest = 0;
      int64_t min_key = PairingKey(queues_[0].front());
      int64_t max_key = min_key;

      for (size_t i = 1; i < queues_.size(); ++i) {
        const int64_t key = PairingKey(queues_[i].front());
        if (key < min_key) {
          min_key = key;
          earliest = i;
        }
        if (key > max_key) max_key = key;
      }

      if (max_key - min_key <= window) {
        EmitGroup();
        continue;
      }

      // the earliest front is further from the latest than the window allows,
      // and every record still to come is later still, so it can never match
      ++unmatched_[earliest];
      queues_[earliest].pop_front();
    }
  }

drain:
  if (flush) {
    for (size_t i = 0; i < queues_.size(); ++i) {
      unmatched_[i] += queues_[i].size();
      queues_[i].clear();
    }
    return;
  }

  // one stream can run far ahead when another stalls. the queues are capped so
  // a stalled partner costs bounded memory instead of growing until the
  // process is killed
  constexpr size_t kMaxQueued = 1u << 16;
  for (size_t i = 0; i < queues_.size(); ++i) {
    while (queues_[i].size() > kMaxQueued) {
      ++unmatched_[i];
      queues_[i].pop_front();
    }
  }
}

ComparisonSnapshot Correlator::Snapshot(const std::vector<StreamSnapshot>& streams) const {
  ComparisonSnapshot c;
  c.grouped = grouped_;
  c.unmatched = unmatched_;

  const uint64_t baseline_p50 = gpu_grouped_.empty() ? 0 : gpu_grouped_[0].ValueAtQuantile(kQ50);

  for (size_t i = 0; i < queues_.size(); ++i) {
    StreamComparison sc;
    sc.label = i < streams.size() ? streams[i].label : ("stream " + std::to_string(i));
    sc.is_baseline = (i == 0);
    sc.gpu_p50 = gpu_grouped_[i].ValueAtQuantile(kQ50);

    if (i != 0) {
      sc.delta_p50 = gpu_delta_[i].ValueAtQuantile(kQ50);
      if (grouped_ > 0) {
        sc.cheaper_fraction =
            static_cast<double>(cheaper_than_baseline_[i]) / static_cast<double>(grouped_);
        WilsonInterval(cheaper_than_baseline_[i], grouped_, kZ95, &sc.cheaper_low,
                       &sc.cheaper_high);
      }
      if (sc.gpu_p50 > 0) {
        sc.speedup = static_cast<double>(baseline_p50) / static_cast<double>(sc.gpu_p50);
      }

      sc.interval_valid = gpu_delta_[i].QuantileInterval(kQ50, kZ95, &sc.delta_p50_low,
                                                         &sc.delta_p50_high);
      // a difference is only worth calling a result when the whole interval
      // sits on one side of zero
      sc.significant = sc.interval_valid &&
                       ((sc.delta_p50_low > 0 && sc.delta_p50_high > 0) ||
                        (sc.delta_p50_low < 0 && sc.delta_p50_high < 0));
    } else {
      sc.speedup = 1.0;
    }
    c.streams.push_back(std::move(sc));
  }

  if (queues_.size() == 2 && streams.size() == 2) {
    const size_t pass_count = std::min(streams[0].passes.size(), streams[1].passes.size());
    for (size_t p = 0; p < pass_count; ++p) {
      if (pass_delta_[p].empty()) continue;
      PassDelta d;
      d.name = streams[0].passes[p].name;
      d.delta_p50 = pass_delta_[p].ValueAtQuantile(kQ50);
      d.a_p50 = pass_a_[p].ValueAtQuantile(kQ50);
      d.b_p50 = pass_b_[p].ValueAtQuantile(kQ50);
      c.pass_deltas.push_back(std::move(d));
    }
  }
  return c;
}

void Correlator::Reset() {
  for (auto& q : queues_) q.clear();
  for (auto& w : gpu_delta_) w.Reset();
  for (auto& w : gpu_grouped_) w.Reset();
  std::fill(cheaper_than_baseline_.begin(), cheaper_than_baseline_.end(), 0);
  std::fill(unmatched_.begin(), unmatched_.end(), 0);
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    pass_delta_[i].Reset();
    pass_a_[i].Reset();
    pass_b_[i].Reset();
  }
  grouped_ = 0;
  use_media_time_ = false;
  std::fill(media_seen_.begin(), media_seen_.end(), false);
}

void WilsonInterval(uint64_t successes, uint64_t trials, double z, double* low, double* high) {
  if (trials == 0) {
    *low = 0.0;
    *high = 0.0;
    return;
  }

  const double n = static_cast<double>(trials);
  const double p = static_cast<double>(successes) / n;
  const double z2 = z * z;
  const double denom = 1.0 + z2 / n;
  const double centre = (p + z2 / (2.0 * n)) / denom;
  const double margin = (z * std::sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n))) / denom;

  *low = std::max(0.0, centre - margin);
  *high = std::min(1.0, centre + margin);
}

std::map<std::string, std::string> ParseEnvironment(const std::string& text) {
  std::map<std::string, std::string> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();

    const std::string line = text.substr(start, end - start);
    const size_t eq = line.find('=');
    if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    start = end + 1;
  }
  return out;
}

std::vector<std::string> EnvironmentMismatches(const std::string& a, const std::string& b) {
  std::vector<std::string> out;
  if (a.empty() || b.empty()) return out;

  const auto ea = ParseEnvironment(a);
  const auto eb = ParseEnvironment(b);

  // only the keys that change what a measurement means. the mpv version and
  // the display name are recorded for the report but do not by themselves make
  // two streams incomparable
  static const char* const kCritical[] = {"vo", "gpu_context", "hwdec", "render", "video"};

  for (const char* key : kCritical) {
    const auto ia = ea.find(key);
    const auto ib = eb.find(key);
    if (ia == ea.end() || ib == eb.end()) continue;
    if (ia->second == ib->second) continue;
    out.push_back(std::string(key) + ": a has '" + ia->second + "', b has '" + ib->second + "'");
  }
  return out;
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
