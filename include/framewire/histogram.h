/*
 * Description: Fixed memory latency histogram with bounded relative error, plus
 *   a sliding window of raw samples for exact recent percentiles.
 * Author: Alex Wu
 * Dependencies:
 * Usage:
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace framewire {

/*
 * Histogram for nanosecond latencies, laid out the way HdrHistogram does it.
 *
 * Values are bucketed by exponent with a fixed number of linear slots inside
 * each exponent, so the storage stays constant while the relative error stays
 * under a chosen bound across the whole range. Recording a value is a few
 * shifts and one increment, which is what makes the structure safe to call on
 * every frame.
 *
 * The alternative of keeping every sample and sorting is exact but grows
 * without bound, and a long benchmark run would end up holding millions of
 * samples just to read three percentiles off the tail.
 */
class Histogram {
 public:
  /*
   * Builds a histogram sized for a value range and precision.
   *
   * Args:
   *   max_trackable: Largest value that gets a dedicated bucket.
   *   significant_digits: Decimal digits kept exact, 1 through 5.
   */
  explicit Histogram(uint64_t max_trackable = 60ull * 1000 * 1000 * 1000,
                     unsigned significant_digits = 3);

  /*
   * Adds one sample.
   *
   * Values above max_trackable are clamped into the top bucket rather than
   * dropped, so a stall still shows up in the tail percentiles.
   *
   * Args:
   *   value: Sample value, normally a duration in nanoseconds.
   */
  void Record(uint64_t value);

  /*
   * Reads the value at a quantile.
   *
   * Args:
   *   quantile: Position in the distribution, 0.0 through 1.0.
   * Returns:
   *   The largest value in the bucket holding that quantile, or zero when the
   *   histogram is empty.
   */
  uint64_t ValueAtQuantile(double quantile) const;

  /*
   * Folds another histogram into this one.
   *
   * Args:
   *   other: Histogram built with the same range and precision.
   */
  void Merge(const Histogram& other);

  void Reset();

  uint64_t count() const { return total_count_; }
  uint64_t min() const { return total_count_ == 0 ? 0 : min_; }
  uint64_t max() const { return max_; }
  double Mean() const;

 private:
  size_t CountsIndex(uint64_t value) const;
  uint64_t ValueFromIndex(size_t index) const;
  uint64_t HighestEquivalent(uint64_t value) const;

  unsigned sub_bucket_count_magnitude_ = 0;
  uint32_t sub_bucket_count_ = 0;
  uint32_t sub_bucket_half_count_ = 0;
  unsigned sub_bucket_half_count_magnitude_ = 0;
  uint64_t sub_bucket_mask_ = 0;
  unsigned leading_zero_count_base_ = 0;
  uint32_t bucket_count_ = 0;

  std::vector<uint64_t> counts_;
  uint64_t total_count_ = 0;
  uint64_t min_ = UINT64_MAX;
  uint64_t max_ = 0;
  double sum_ = 0.0;
};

/*
 * Ring of the most recent samples, kept for exact percentiles over a short
 * window.
 *
 * The dashboard shows what the last few seconds look like, and a histogram
 * cannot forget old samples. Holding a bounded ring of raw values and running
 * a partial sort on demand is exact, and at a few thousand samples refreshed
 * ten times a second the cost does not matter.
 */
class SampleWindow {
 public:
  explicit SampleWindow(size_t capacity = 4096);

  void Record(uint64_t value);
  void Reset();

  /*
   * Reads the value at a quantile over the samples currently held.
   *
   * Args:
   *   quantile: Position in the distribution, 0.0 through 1.0.
   * Returns:
   *   The sample at that position, or zero when the window is empty.
   */
  uint64_t ValueAtQuantile(double quantile) const;

  /*
   * Reads several quantiles in one pass.
   *
   * Sorting once beats calling ValueAtQuantile repeatedly, which would redo
   * the partial sort for every quantile asked for.
   *
   * Args:
   *   quantiles: Positions to read, each 0.0 through 1.0.
   *   out: Destination array with one slot per quantile.
   *   n: Number of quantiles.
   */
  void ValuesAtQuantiles(const double* quantiles, uint64_t* out, size_t n) const;

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  double Mean() const;
  uint64_t Newest() const;

  // Most recent samples in arrival order, oldest first, used by the sparkline.
  void CopyRecent(uint64_t* out, size_t n) const;

 private:
  std::vector<uint64_t> values_;
  mutable std::vector<uint64_t> scratch_;
  size_t next_ = 0;
  size_t size_ = 0;
};

/*
 * Sliding window of signed values, used for the timing difference between the
 * two shaders.
 *
 * A difference can point either way, and the latency histogram only holds
 * unsigned values. Folding the sign away would hide which shader was faster,
 * which is the one number this tool exists to report.
 */
class SignedWindow {
 public:
  explicit SignedWindow(size_t capacity = 4096);

  void Record(int64_t value);
  void Reset();

  /*
   * Reads the value at a quantile over the samples currently held.
   *
   * Args:
   *   quantile: Position in the distribution, 0.0 through 1.0.
   * Returns:
   *   The sample at that position, or zero when the window is empty.
   */
  int64_t ValueAtQuantile(double quantile) const;

  /*
   * Reads a distribution free confidence interval for a quantile.
   *
   * The interval comes from order statistics rather than a bootstrap. Both are
   * defensible, but this one is exact under nothing more than independent
   * samples, needs a single sort instead of hundreds of resamples, and has no
   * random seed, so the same data always gives the same interval. That matters
   * for a number the dashboard recomputes ten times a second.
   *
   * Args:
   *   quantile: Position in the distribution, 0.0 through 1.0.
   *   z: Standard normal quantile for the confidence wanted, 1.96 for 95%.
   *   low: Receives the lower bound.
   *   high: Receives the upper bound.
   * Returns:
   *   True when the window held enough samples for an interval.
   */
  bool QuantileInterval(double quantile, double z, int64_t* low, int64_t* high) const;

  double Mean() const;
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Fraction of samples strictly below zero, 0.0 through 1.0.
  double FractionNegative() const;

 private:
  std::vector<int64_t> values_;
  mutable std::vector<int64_t> scratch_;
  size_t next_ = 0;
  size_t size_ = 0;
};

}  // namespace framewire
