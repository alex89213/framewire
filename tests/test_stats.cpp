/*
 * Description: Unit tests for the stream aggregator, the frame correlator and
 *   the duration formatting helpers.
 * Author: Alex Wu
 * Dependencies: framewire core library, tests/test_util.h
 * Usage: ctest, or run the binary directly
 */

#include <unistd.h>

#include <string>
#include <vector>

#include "framewire/stats.h"
#include "test_util.h"

using namespace framewire;

namespace {

std::string SegmentName(const char* suffix) {
  return std::string("/framewire-stats-") + suffix + "-" + std::to_string(getpid());
}

/*
 * Builds a record with a chosen timestamp and gpu time.
 *
 * Args:
 *   seq: Sequence number.
 *   t_ns: Monotonic timestamp.
 *   gpu_ns: Total gpu time, split evenly across the passes.
 *   passes: Pass count to report.
 * Returns:
 *   A checksummed record.
 */
TelemetryRecord Rec(uint64_t seq, uint64_t t_ns, uint64_t gpu_ns, unsigned passes = 2) {
  TelemetryRecord r{};
  r.seq = seq;
  r.t_mono_ns = t_ns;
  r.frame_time_ns = 16666666;
  r.gpu_total_ns = gpu_ns;
  r.pass_count = static_cast<uint8_t>(passes);
  for (unsigned i = 0; i < passes; ++i) {
    r.pass_ns[i] = static_cast<uint32_t>(gpu_ns / (passes == 0 ? 1 : passes));
  }
  StampChecksum(r);
  return r;
}

StreamSnapshot FakeSnapshot(const char* label, uint64_t p50, size_t passes) {
  StreamSnapshot s;
  s.label = label;
  s.gpu_life.p50 = p50;
  for (size_t i = 0; i < passes; ++i) {
    PassView v;
    v.name = "pass" + std::to_string(i);
    s.passes.push_back(v);
  }
  return s;
}

void TestFormatting() {
  TEST_CASE("durations format into readable units") {
    CHECK(FormatNanos(0) == "-");
    CHECK(FormatNanos(500) == "500ns");
    CHECK(FormatNanos(1500) == "1.5us");
    CHECK(FormatNanos(1500000) == "1.50ms");
    CHECK(FormatNanos(2500000000ull) == "2.50s");
  }

  TEST_CASE("signed durations carry an explicit sign") {
    CHECK(FormatSignedNanos(0) == "0");
    CHECK(FormatSignedNanos(1500000) == "+1.50ms");
    CHECK(FormatSignedNanos(-1500000) == "-1.50ms");
    CHECK(FormatSignedNanos(-500) == "-500ns");
  }
}

void TestCorrelatorPairing() {
  TEST_CASE("aligned streams pair one to one") {
    Correlator c(8000000);  // eight millisecond window

    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 10; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000));
      b.push_back(Rec(i + 1, i * 16666666, 2000000));
    }
    c.PushA(a);
    c.PushB(b);
    c.Process(true);

    const auto snap = c.Snapshot(FakeSnapshot("a", 1000000, 2), FakeSnapshot("b", 2000000, 2));
    CHECK_EQ(snap.paired, 10);
    CHECK_EQ(snap.unmatched_a, 0);
    CHECK_EQ(snap.unmatched_b, 0);

    // b spends a millisecond more per frame, so the difference is positive
    CHECK_EQ(snap.gpu_delta_p50, 1000000);
    CHECK_NEAR(snap.a_faster_fraction, 1.0, 1e-9);
    CHECK_NEAR(snap.speedup, 0.5, 1e-9);
  }

  TEST_CASE("a small offset still pairs") {
    Correlator c(8000000);

    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 10; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000));
      // four milliseconds of drift is inside the window, so pairing holds
      b.push_back(Rec(i + 1, i * 16666666 + 4000000, 1000000));
    }
    c.PushA(a);
    c.PushB(b);
    c.Process(true);

    const auto snap = c.Snapshot(FakeSnapshot("a", 1000000, 2), FakeSnapshot("b", 1000000, 2));
    CHECK_EQ(snap.paired, 10);
    CHECK_EQ(snap.gpu_delta_p50, 0);
  }

  TEST_CASE("a record with no partner is retired as unmatched") {
    Correlator c(1000000);  // one millisecond window

    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    // a runs at twice the rate, so every other frame has no partner
    for (uint64_t i = 0; i < 10; ++i) a.push_back(Rec(i + 1, i * 8000000, 1000000));
    for (uint64_t i = 0; i < 5; ++i) b.push_back(Rec(i + 1, i * 16000000, 1000000));

    c.PushA(a);
    c.PushB(b);
    c.Process(true);

    const auto snap = c.Snapshot(FakeSnapshot("a", 1, 2), FakeSnapshot("b", 1, 2));
    CHECK_EQ(snap.paired, 5);
    CHECK_EQ(snap.unmatched_a, 5);
    CHECK_EQ(snap.unmatched_b, 0);
  }

  TEST_CASE("nothing pairs while one side is silent") {
    Correlator c(8000000);

    std::vector<TelemetryRecord> a;
    for (uint64_t i = 0; i < 10; ++i) a.push_back(Rec(i + 1, i * 16666666, 1000000));
    c.PushA(a);
    c.Process();

    // with no b records the a side has to be held, not guessed at
    CHECK_EQ(c.PendingA(), 10);
    CHECK_EQ(c.PendingB(), 0);

    const auto snap = c.Snapshot(FakeSnapshot("a", 1, 2), FakeSnapshot("b", 1, 2));
    CHECK_EQ(snap.paired, 0);
  }

  TEST_CASE("a flush retires whatever is left over") {
    Correlator c(8000000);
    std::vector<TelemetryRecord> a;
    for (uint64_t i = 0; i < 4; ++i) a.push_back(Rec(i + 1, i * 16666666, 1000000));
    c.PushA(a);
    c.Process(true);

    CHECK_EQ(c.PendingA(), 0);
    const auto snap = c.Snapshot(FakeSnapshot("a", 1, 2), FakeSnapshot("b", 1, 2));
    CHECK_EQ(snap.unmatched_a, 4);
  }

  TEST_CASE("per pass deltas need a matching pass count") {
    Correlator same(8000000);
    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 10; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000, 2));
      b.push_back(Rec(i + 1, i * 16666666, 2000000, 2));
    }
    same.PushA(a);
    same.PushB(b);
    same.Process(true);
    const auto with_match =
        same.Snapshot(FakeSnapshot("a", 1000000, 2), FakeSnapshot("b", 2000000, 2));
    CHECK_EQ(with_match.pass_deltas.size(), 2);
    CHECK_EQ(with_match.pass_deltas[0].delta_p50, 500000);

    // a different chain length makes pass to pass comparison meaningless, so
    // the deltas are left out rather than lining up unrelated work
    Correlator differ(8000000);
    std::vector<TelemetryRecord> c;
    std::vector<TelemetryRecord> d;
    for (uint64_t i = 0; i < 10; ++i) {
      c.push_back(Rec(i + 1, i * 16666666, 1000000, 2));
      d.push_back(Rec(i + 1, i * 16666666, 2000000, 5));
    }
    differ.PushA(c);
    differ.PushB(d);
    differ.Process(true);
    const auto no_match =
        differ.Snapshot(FakeSnapshot("a", 1000000, 2), FakeSnapshot("b", 2000000, 5));
    CHECK_EQ(no_match.paired, 10);
    CHECK_EQ(no_match.pass_deltas.size(), 0);
  }

  TEST_CASE("reset clears every counter") {
    Correlator c(8000000);
    std::vector<TelemetryRecord> a{Rec(1, 0, 1000000)};
    std::vector<TelemetryRecord> b{Rec(1, 0, 2000000)};
    c.PushA(a);
    c.PushB(b);
    c.Process(true);
    c.Reset();

    const auto snap = c.Snapshot(FakeSnapshot("a", 1, 2), FakeSnapshot("b", 1, 2));
    CHECK_EQ(snap.paired, 0);
    CHECK_EQ(snap.unmatched_a, 0);
    CHECK_EQ(c.PendingA(), 0);
  }
}

void TestAggregator() {
  TEST_CASE("the aggregator folds ring records into stats") {
    const std::string name = SegmentName("agg");
    RingProducer producer(RingMapping::Create(name, 1024, "agg-label"));

    const char* names[2] = {"upload", "scale"};
    const uint8_t version = producer.PublishLayout(names, 2);

    for (uint64_t i = 0; i < 100; ++i) {
      TelemetryRecord r = Rec(i + 1, i * 16666666, 2000000, 2);
      r.layout_version = version;
      if (i % 10 == 0) r.flags |= kFlagDroppedFrame;
      StampChecksum(r);
      producer.TryPush(r);
    }

    StreamAggregator agg("agg-label", RingConsumer(RingMapping::Open(name)));
    std::vector<TelemetryRecord> drained;
    CHECK_EQ(agg.Pump(1024, &drained), 100);
    CHECK_EQ(drained.size(), 100);
    CHECK_EQ(agg.records(), 100);

    const StreamSnapshot s = agg.Snapshot();
    CHECK(s.label == "agg-label");
    CHECK_EQ(s.records, 100);
    CHECK_EQ(s.dropped_frames, 10);
    CHECK_EQ(s.checksum_errors, 0);
    CHECK_EQ(s.sequence_gaps, 0);
    CHECK_EQ(s.ring_pending, 0);
    CHECK_EQ(s.passes.size(), 2);
    CHECK(s.passes[0].name == "upload");
    CHECK(s.passes[1].name == "scale");
    CHECK_EQ(s.gpu_life.p50 >= 1900000 && s.gpu_life.p50 <= 2100000, 1);

    // frames land one sixtieth of a second apart, so the rate lands near sixty
    CHECK_NEAR(s.fps, 60.0, 1.0);

    agg.ResetStats();
    CHECK_EQ(agg.Snapshot().records, 0);
  }

  TEST_CASE("a corrupt record is counted and skipped") {
    const std::string name = SegmentName("corrupt");
    RingProducer producer(RingMapping::Create(name, 64, "corrupt"));

    producer.TryPush(Rec(1, 0, 1000000));

    TelemetryRecord bad = Rec(2, 16666666, 1000000);
    bad.gpu_total_ns = 999;  // changed after the checksum was stamped
    producer.TryPush(bad);

    producer.TryPush(Rec(3, 33333332, 1000000));

    StreamAggregator agg("corrupt", RingConsumer(RingMapping::Open(name)));
    std::vector<TelemetryRecord> drained;
    CHECK_EQ(agg.Pump(64, &drained), 3);

    const StreamSnapshot s = agg.Snapshot();
    CHECK_EQ(s.checksum_errors, 1);
    CHECK_EQ(s.records, 2);  // the bad record never reached the statistics
    CHECK_EQ(drained.size(), 2);
  }

  TEST_CASE("a sequence gap is counted") {
    const std::string name = SegmentName("gap");
    RingProducer producer(RingMapping::Create(name, 64, "gap"));

    producer.TryPush(Rec(1, 0, 1000000));
    producer.TryPush(Rec(5, 16666666, 1000000));  // four records never arrived

    StreamAggregator agg("gap", RingConsumer(RingMapping::Open(name)));
    std::vector<TelemetryRecord> drained;
    agg.Pump(64, &drained);

    CHECK_EQ(agg.Snapshot().sequence_gaps, 1);
  }
}

}  // namespace

int main() {
  std::printf("test_stats\n");
  TestFormatting();
  TestCorrelatorPairing();
  TestAggregator();
  return fwtest::Finish("test_stats");
}
