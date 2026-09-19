/*
 * Description: Implementation of the HDR style latency histogram and the
 *   sliding sample window.
 * Author: Alex Wu
 * Dependencies: framewire/histogram.h
 * Usage:
 */

#include "framewire/histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace framewire {
namespace {

// Rounds a power of ten up to the next power of two exponent.
unsigned MagnitudeForDigits(unsigned significant_digits) {
  uint64_t target = 1;
  for (unsigned i = 0; i < significant_digits; ++i) target *= 10;

  unsigned magnitude = 0;
  while ((1ull << magnitude) < target) ++magnitude;
  return magnitude;
}

}  // namespace

Histogram::Histogram(uint64_t max_trackable, unsigned significant_digits) {
  if (significant_digits < 1 || significant_digits > 5) {
    throw std::runtime_error("significant digits must be between 1 and 5");
  }
  if (max_trackable < 2) max_trackable = 2;

  // one linear slot per step inside an exponent. three digits gives 1024 slots
  // and a worst case relative error just under 0.1 percent
  sub_bucket_count_magnitude_ = MagnitudeForDigits(significant_digits);
  sub_bucket_count_ = 1u << sub_bucket_count_magnitude_;
  sub_bucket_half_count_ = sub_bucket_count_ / 2;
  sub_bucket_half_count_magnitude_ = sub_bucket_count_magnitude_ - 1;
  sub_bucket_mask_ = static_cast<uint64_t>(sub_bucket_count_) - 1;
  leading_zero_count_base_ = 64 - sub_bucket_count_magnitude_;

  // each extra bucket doubles the range covered, so the count grows with the
  // log of the range and the memory stays small even for a 60 second ceiling
  uint32_t buckets = 1;
  uint64_t smallest_untrackable = sub_bucket_count_;
  while (smallest_untrackable <= max_trackable) {
    if (smallest_untrackable > (UINT64_MAX / 2)) {
      ++buckets;
      break;
    }
    smallest_untrackable <<= 1;
    ++buckets;
  }
  bucket_count_ = buckets;

  counts_.assign(static_cast<size_t>(bucket_count_ + 1) * sub_bucket_half_count_, 0);
}

size_t Histogram::CountsIndex(uint64_t value) const {
  // oring in the mask keeps small values from producing a negative bucket,
  // which is the trick that lets bucket zero hold the full linear range
  const auto bucket_index =
      static_cast<uint32_t>(leading_zero_count_base_ -
                            static_cast<unsigned>(std::countl_zero(value | sub_bucket_mask_)));
  const auto sub_bucket_index = static_cast<uint32_t>(value >> bucket_index);

  // the offset is negative for bucket zero, where sub_bucket_index runs below
  // the half count. computing in signed width is required, an unsigned
  // subtraction wraps here and throws every small sample into the top slot
  const int64_t bucket_base = static_cast<int64_t>(bucket_index + 1)
                              << sub_bucket_half_count_magnitude_;
  const int64_t offset =
      static_cast<int64_t>(sub_bucket_index) - static_cast<int64_t>(sub_bucket_half_count_);
  return static_cast<size_t>(bucket_base + offset);
}

uint64_t Histogram::ValueFromIndex(size_t index) const {
  auto bucket_index =
      static_cast<int64_t>(index >> sub_bucket_half_count_magnitude_) - 1;
  auto sub_bucket_index =
      static_cast<int64_t>(index & (sub_bucket_half_count_ - 1)) + sub_bucket_half_count_;

  if (bucket_index < 0) {
    sub_bucket_index -= sub_bucket_half_count_;
    bucket_index = 0;
  }
  return static_cast<uint64_t>(sub_bucket_index) << bucket_index;
}

uint64_t Histogram::HighestEquivalent(uint64_t value) const {
  const auto bucket_index =
      static_cast<uint32_t>(leading_zero_count_base_ -
                            static_cast<unsigned>(std::countl_zero(value | sub_bucket_mask_)));
  // every value inside one slot is reported as the top of that slot, so a
  // quoted percentile is never optimistic
  const uint64_t slot_width = 1ull << bucket_index;
  return (value & ~(slot_width - 1)) + slot_width - 1;
}

void Histogram::Record(uint64_t value) {
  if (value == 0) value = 1;  // bucket zero starts at one, treat a zero sample as the floor

  size_t index = CountsIndex(value);
  if (index >= counts_.size()) index = counts_.size() - 1;  // clamp a stall into the top slot

  ++counts_[index];
  ++total_count_;
  sum_ += static_cast<double>(value);
  if (value < min_) min_ = value;
  if (value > max_) max_ = value;
}

uint64_t Histogram::ValueAtQuantile(double quantile) const {
  if (total_count_ == 0) return 0;
  if (quantile < 0.0) quantile = 0.0;
  if (quantile > 1.0) quantile = 1.0;

  // rounding up matters at the tail. with 10000 samples the 99.9th percentile
  // is the 9990th sample, and truncating instead would report the 9989th
  auto wanted = static_cast<uint64_t>(std::ceil(quantile * static_cast<double>(total_count_)));
  if (wanted == 0) wanted = 1;
  if (wanted > total_count_) wanted = total_count_;

  uint64_t seen = 0;
  for (size_t i = 0; i < counts_.size(); ++i) {
    seen += counts_[i];
    if (seen >= wanted) return HighestEquivalent(ValueFromIndex(i));
  }
  return max_;
}

void Histogram::Merge(const Histogram& other) {
  if (other.counts_.size() != counts_.size()) {
    throw std::runtime_error("cannot merge histograms with different layouts");
  }
  for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];

  total_count_ += other.total_count_;
  sum_ += other.sum_;
  if (other.total_count_ > 0) {
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }
}

void Histogram::Reset() {
  std::fill(counts_.begin(), counts_.end(), 0ull);
  total_count_ = 0;
  sum_ = 0.0;
  min_ = UINT64_MAX;
  max_ = 0;
}

double Histogram::Mean() const {
  if (total_count_ == 0) return 0.0;
  return sum_ / static_cast<double>(total_count_);
}

SampleWindow::SampleWindow(size_t capacity) : values_(capacity == 0 ? 1 : capacity, 0) {
  scratch_.reserve(values_.size());
}

void SampleWindow::Record(uint64_t value) {
  values_[next_] = value;
  next_ = (next_ + 1) % values_.size();
  if (size_ < values_.size()) ++size_;
}

void SampleWindow::Reset() {
  next_ = 0;
  size_ = 0;
}

uint64_t SampleWindow::ValueAtQuantile(double quantile) const {
  uint64_t out = 0;
  ValuesAtQuantiles(&quantile, &out, 1);
  return out;
}

void SampleWindow::ValuesAtQuantiles(const double* quantiles, uint64_t* out, size_t n) const {
  if (size_ == 0) {
    for (size_t i = 0; i < n; ++i) out[i] = 0;
    return;
  }

  scratch_.assign(values_.begin(), values_.begin() + static_cast<ptrdiff_t>(size_));
  // a full sort is simpler than repeated nth_element once more than a couple
  // of quantiles are wanted, and a few thousand elements sort in microseconds
  std::sort(scratch_.begin(), scratch_.end());

  for (size_t i = 0; i < n; ++i) {
    double q = quantiles[i];
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;

    auto rank = static_cast<size_t>(std::ceil(q * static_cast<double>(size_)));
    if (rank == 0) rank = 1;
    if (rank > size_) rank = size_;
    out[i] = scratch_[rank - 1];
  }
}

double SampleWindow::Mean() const {
  if (size_ == 0) return 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < size_; ++i) sum += static_cast<double>(values_[i]);
  return sum / static_cast<double>(size_);
}

uint64_t SampleWindow::Newest() const {
  if (size_ == 0) return 0;
  const size_t last = (next_ + values_.size() - 1) % values_.size();
  return values_[last];
}

SignedWindow::SignedWindow(size_t capacity) : values_(capacity == 0 ? 1 : capacity, 0) {
  scratch_.reserve(values_.size());
}

void SignedWindow::Record(int64_t value) {
  values_[next_] = value;
  next_ = (next_ + 1) % values_.size();
  if (size_ < values_.size()) ++size_;
}

void SignedWindow::Reset() {
  next_ = 0;
  size_ = 0;
}

int64_t SignedWindow::ValueAtQuantile(double quantile) const {
  if (size_ == 0) return 0;
  if (quantile < 0.0) quantile = 0.0;
  if (quantile > 1.0) quantile = 1.0;

  scratch_.assign(values_.begin(), values_.begin() + static_cast<ptrdiff_t>(size_));
  std::sort(scratch_.begin(), scratch_.end());

  auto rank = static_cast<size_t>(std::ceil(quantile * static_cast<double>(size_)));
  if (rank == 0) rank = 1;
  if (rank > size_) rank = size_;
  return scratch_[rank - 1];
}

double SignedWindow::Mean() const {
  if (size_ == 0) return 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < size_; ++i) sum += static_cast<double>(values_[i]);
  return sum / static_cast<double>(size_);
}

double SignedWindow::FractionNegative() const {
  if (size_ == 0) return 0.0;
  size_t negative = 0;
  for (size_t i = 0; i < size_; ++i) {
    if (values_[i] < 0) ++negative;
  }
  return static_cast<double>(negative) / static_cast<double>(size_);
}

void SampleWindow::CopyRecent(uint64_t* out, size_t n) const {
  for (size_t i = 0; i < n; ++i) out[i] = 0;
  if (size_ == 0) return;

  const size_t take = std::min(n, size_);
  // walk backwards from the newest sample so the caller gets the tail of the
  // stream, then fill the destination oldest first for left to right drawing
  for (size_t i = 0; i < take; ++i) {
    const size_t offset = take - i;
    const size_t idx = (next_ + values_.size() - offset) % values_.size();
    out[n - take + i] = values_[idx];
  }
}

}  // namespace framewire
