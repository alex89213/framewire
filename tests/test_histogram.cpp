/*
 * Description: Unit tests for the latency histogram, the sliding sample window
 *   and the signed difference window.
 * Author: Alex Wu
 * Dependencies: framewire core library, tests/test_util.h
 * Usage: ctest, or run the binary directly
 */

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "framewire/histogram.h"
#include "test_util.h"

using namespace framewire;

namespace {

// exact quantile over a sorted copy, used as the reference for the histogram
uint64_t ExactQuantile(std::vector<uint64_t> values, double quantile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  auto rank = static_cast<size_t>(std::ceil(quantile * static_cast<double>(values.size())));
  if (rank == 0) rank = 1;
  if (rank > values.size()) rank = values.size();
  return values[rank - 1];
}

void TestEmptyAndBasics() {
  TEST_CASE("an empty histogram reports zeroes") {
    Histogram h;
    CHECK_EQ(h.count(), 0);
    CHECK_EQ(h.min(), 0);
    CHECK_EQ(h.max(), 0);
    CHECK_EQ(h.ValueAtQuantile(0.5), 0);
    CHECK_NEAR(h.Mean(), 0.0, 1e-9);
  }

  TEST_CASE("counts, min, max and mean track the samples") {
    Histogram h;
    for (uint64_t v : {100ull, 200ull, 300ull, 400ull}) h.Record(v);
    CHECK_EQ(h.count(), 4);
    CHECK_EQ(h.min(), 100);
    CHECK_EQ(h.max(), 400);
    CHECK_NEAR(h.Mean(), 250.0, 0.001);

    h.Reset();
    CHECK_EQ(h.count(), 0);
    CHECK_EQ(h.max(), 0);
  }
}

void TestSmallValueBucketing() {
  TEST_CASE("small values land in their own buckets") {
    // this range is where the bucket zero index math is easiest to get wrong,
    // so every value below the sub bucket count is checked for exactness
    Histogram h;
    for (uint64_t v = 1; v <= 1023; ++v) h.Record(v);

    CHECK_EQ(h.count(), 1023);
    CHECK_EQ(h.min(), 1);
    CHECK_EQ(h.max(), 1023);

    // below the linear range the histogram is exact, not approximate
    CHECK_EQ(h.ValueAtQuantile(0.0), 1);
    CHECK_EQ(h.ValueAtQuantile(1.0), 1023);
    CHECK_EQ(h.ValueAtQuantile(0.5), 512);
  }

  TEST_CASE("a zero sample is folded into the floor bucket") {
    Histogram h;
    h.Record(0);
    CHECK_EQ(h.count(), 1);
    CHECK_EQ(h.ValueAtQuantile(0.5), 1);
  }
}

void TestAccuracy() {
  TEST_CASE("quantiles stay inside the precision bound") {
    Histogram h(60ull * 1000 * 1000 * 1000, 3);
    std::vector<uint64_t> raw;

    std::mt19937_64 rng(1234);
    std::lognormal_distribution<double> dist(std::log(1500000.0), 0.4);
    for (int i = 0; i < 200000; ++i) {
      const auto v = static_cast<uint64_t>(dist(rng));
      h.Record(v);
      raw.push_back(v);
    }

    // three significant digits promises well under one percent relative error
    for (double q : {0.5, 0.9, 0.99, 0.999}) {
      const double exact = static_cast<double>(ExactQuantile(raw, q));
      const double got = static_cast<double>(h.ValueAtQuantile(q));
      CHECK_NEAR(got / exact, 1.0, 0.01);
    }
  }

  TEST_CASE("a uniform ramp matches the exact quantiles") {
    Histogram h;
    std::vector<uint64_t> raw;
    for (uint64_t v = 1; v <= 100000; ++v) {
      h.Record(v);
      raw.push_back(v);
    }
    for (double q : {0.5, 0.99, 0.999, 1.0}) {
      const double exact = static_cast<double>(ExactQuantile(raw, q));
      const double got = static_cast<double>(h.ValueAtQuantile(q));
      CHECK_NEAR(got / exact, 1.0, 0.01);
    }
  }

  TEST_CASE("values past the ceiling still reach the tail") {
    Histogram h(1000000, 3);
    for (int i = 0; i < 99; ++i) h.Record(1000);
    h.Record(50000000000ull);  // a stall far beyond the tracked range

    CHECK_EQ(h.count(), 100);
    CHECK_EQ(h.max(), 50000000000ull);
    // the clamped sample must still be the largest thing the tail reports
    CHECK(h.ValueAtQuantile(1.0) > h.ValueAtQuantile(0.5));
  }
}

void TestMerge() {
  TEST_CASE("merging adds the counts of both sides") {
    Histogram a;
    Histogram b;
    for (uint64_t v = 1; v <= 1000; ++v) a.Record(v);
    for (uint64_t v = 1001; v <= 2000; ++v) b.Record(v);

    a.Merge(b);
    CHECK_EQ(a.count(), 2000);
    CHECK_EQ(a.min(), 1);
    CHECK_EQ(a.max(), 2000);
    CHECK_NEAR(a.Mean(), 1000.5, 1.0);
  }

  TEST_CASE("merging mismatched layouts is refused") {
    Histogram a(1000000, 3);
    Histogram b(1000000, 2);
    bool threw = false;
    try {
      a.Merge(b);
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }
}

void TestSampleWindow() {
  TEST_CASE("the window keeps only the newest samples") {
    SampleWindow w(8);
    CHECK(w.empty());
    CHECK_EQ(w.ValueAtQuantile(0.5), 0);

    for (uint64_t v = 1; v <= 20; ++v) w.Record(v);
    CHECK_EQ(w.size(), 8);
    CHECK_EQ(w.Newest(), 20);

    // only 13 through 20 remain, so the median comes from that range
    CHECK_EQ(w.ValueAtQuantile(0.0), 13);
    CHECK_EQ(w.ValueAtQuantile(1.0), 20);
    CHECK_NEAR(w.Mean(), 16.5, 0.001);

    w.Reset();
    CHECK(w.empty());
    CHECK_EQ(w.Newest(), 0);
  }

  TEST_CASE("several quantiles in one pass match one at a time") {
    SampleWindow w(1000);
    for (uint64_t v = 1; v <= 1000; ++v) w.Record(v);

    const double quantiles[3] = {0.5, 0.99, 0.999};
    uint64_t batch[3] = {0, 0, 0};
    w.ValuesAtQuantiles(quantiles, batch, 3);

    for (int i = 0; i < 3; ++i) CHECK_EQ(batch[i], w.ValueAtQuantile(quantiles[i]));
    CHECK_EQ(batch[0], 500);
    CHECK_EQ(batch[1], 990);
  }

  TEST_CASE("recent samples come back oldest first") {
    SampleWindow w(16);
    for (uint64_t v = 1; v <= 5; ++v) w.Record(v);

    uint64_t out[8];
    w.CopyRecent(out, 8);
    // only five samples exist, so the front of the destination stays empty
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[2], 0);
    CHECK_EQ(out[3], 1);
    CHECK_EQ(out[7], 5);

    for (uint64_t v = 6; v <= 30; ++v) w.Record(v);
    w.CopyRecent(out, 8);
    CHECK_EQ(out[0], 23);
    CHECK_EQ(out[7], 30);
  }
}

void TestSignedWindow() {
  TEST_CASE("the signed window keeps the direction of a difference") {
    SignedWindow w(100);
    CHECK(w.empty());
    CHECK_EQ(w.ValueAtQuantile(0.5), 0);
    CHECK_NEAR(w.FractionNegative(), 0.0, 1e-9);

    for (int v = -50; v < 50; ++v) w.Record(v);
    CHECK_EQ(w.size(), 100);
    CHECK_EQ(w.ValueAtQuantile(0.0), -50);
    CHECK_EQ(w.ValueAtQuantile(1.0), 49);
    CHECK_EQ(w.ValueAtQuantile(0.5), -1);
    CHECK_NEAR(w.Mean(), -0.5, 0.001);
    CHECK_NEAR(w.FractionNegative(), 0.5, 1e-9);
  }

  TEST_CASE("an all negative window reports a negative median") {
    SignedWindow w(10);
    for (int i = 0; i < 10; ++i) w.Record(-1000 - i);
    CHECK(w.ValueAtQuantile(0.5) < 0);
    CHECK_NEAR(w.FractionNegative(), 1.0, 1e-9);
  }
}

}  // namespace

int main() {
  std::printf("test_histogram\n");
  TestEmptyAndBasics();
  TestSmallValueBucketing();
  TestAccuracy();
  TestMerge();
  TestSampleWindow();
  TestSignedWindow();
  return fwtest::Finish("test_histogram");
}
