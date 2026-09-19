/*
 * Description: Unit tests for the stream aggregator, the frame correlator and
 *   the duration formatting helpers.
 * Author: Alex Wu
 * Dependencies: framewire core library, tests/test_util.h
 * Usage: ctest, or run the binary directly
 */

#include <unistd.h>

#include <string>
#include <initializer_list>
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

std::vector<StreamSnapshot> Snaps(std::initializer_list<const char*> labels, size_t passes = 2) {
  std::vector<StreamSnapshot> out;
  for (const char* l : labels) out.push_back(FakeSnapshot(l, 1000000, passes));
  return out;
}

void TestCorrelatorPairing() {
  TEST_CASE("aligned streams group one to one") {
    Correlator c(2, 8000000);  // eight millisecond window

    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 10; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000));
      b.push_back(Rec(i + 1, i * 16666666, 2000000));
    }
    c.Push(0, a);
    c.Push(1, b);
    c.Process(true);

    const auto snap = c.Snapshot(Snaps({"a", "b"}));
    CHECK_EQ(snap.grouped, 10);
    CHECK_EQ(snap.unmatched[0], 0);
    CHECK_EQ(snap.unmatched[1], 0);

    // b spends a millisecond more per frame, so the delta is positive and b is
    // never the cheaper of the two
    CHECK_EQ(snap.streams.size(), 2);
    CHECK(snap.streams[0].is_baseline);
    CHECK_EQ(snap.streams[1].delta_p50, 1000000);
    CHECK_NEAR(snap.streams[1].cheaper_fraction, 0.0, 1e-9);
    CHECK_NEAR(snap.streams[1].speedup, 0.5, 1e-9);
  }

  TEST_CASE("a small offset still groups") {
    Correlator c(2, 8000000);

    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 10; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000));
      // four milliseconds of drift is inside the window, so grouping holds
      b.push_back(Rec(i + 1, i * 16666666 + 4000000, 1000000));
    }
    c.Push(0, a);
    c.Push(1, b);
    c.Process(true);

    const auto snap = c.Snapshot(Snaps({"a", "b"}));
    CHECK_EQ(snap.grouped, 10);
    CHECK_EQ(snap.streams[1].delta_p50, 0);
  }

  TEST_CASE("a record with no partner is retired as unmatched") {
    Correlator c(2, 1000000);  // one millisecond window

    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    // a runs at twice the rate, so every other frame has no partner
    for (uint64_t i = 0; i < 10; ++i) a.push_back(Rec(i + 1, i * 8000000, 1000000));
    for (uint64_t i = 0; i < 5; ++i) b.push_back(Rec(i + 1, i * 16000000, 1000000));

    c.Push(0, a);
    c.Push(1, b);
    c.Process(true);

    const auto snap = c.Snapshot(Snaps({"a", "b"}));
    CHECK_EQ(snap.grouped, 5);
    CHECK_EQ(snap.unmatched[0], 5);
    CHECK_EQ(snap.unmatched[1], 0);
  }

  TEST_CASE("nothing groups while one stream is silent") {
    Correlator c(2, 8000000);

    std::vector<TelemetryRecord> a;
    for (uint64_t i = 0; i < 10; ++i) a.push_back(Rec(i + 1, i * 16666666, 1000000));
    c.Push(0, a);
    c.Process();

    // with no b records the a side has to be held, not guessed at
    CHECK_EQ(c.Pending(0), 10);
    CHECK_EQ(c.Pending(1), 0);
    CHECK_EQ(c.Snapshot(Snaps({"a", "b"})).grouped, 0);
  }

  TEST_CASE("a flush retires whatever is left over") {
    Correlator c(2, 8000000);
    std::vector<TelemetryRecord> a;
    for (uint64_t i = 0; i < 4; ++i) a.push_back(Rec(i + 1, i * 16666666, 1000000));
    c.Push(0, a);
    c.Process(true);

    CHECK_EQ(c.Pending(0), 0);
    CHECK_EQ(c.Snapshot(Snaps({"a", "b"})).unmatched[0], 4);
  }

  TEST_CASE("per pass deltas need a matching pass count") {
    Correlator same(2, 8000000);
    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 10; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000, 2));
      b.push_back(Rec(i + 1, i * 16666666, 2000000, 2));
    }
    same.Push(0, a);
    same.Push(1, b);
    same.Process(true);
    const auto with_match = same.Snapshot(Snaps({"a", "b"}, 2));
    CHECK_EQ(with_match.pass_deltas.size(), 2);
    CHECK_EQ(with_match.pass_deltas[0].delta_p50, 500000);

    // a different chain length makes pass to pass comparison meaningless, so
    // the deltas are left out rather than lining up unrelated work
    Correlator differ(2, 8000000);
    std::vector<TelemetryRecord> c;
    std::vector<TelemetryRecord> d;
    for (uint64_t i = 0; i < 10; ++i) {
      c.push_back(Rec(i + 1, i * 16666666, 1000000, 2));
      d.push_back(Rec(i + 1, i * 16666666, 2000000, 5));
    }
    differ.Push(0, c);
    differ.Push(1, d);
    differ.Process(true);
    const auto no_match = differ.Snapshot(Snaps({"a", "b"}, 2));
    CHECK_EQ(no_match.grouped, 10);
    CHECK_EQ(no_match.pass_deltas.size(), 0);
  }

  TEST_CASE("reset clears every counter") {
    Correlator c(2, 8000000);
    std::vector<TelemetryRecord> a{Rec(1, 0, 1000000)};
    std::vector<TelemetryRecord> b{Rec(1, 0, 2000000)};
    c.Push(0, a);
    c.Push(1, b);
    c.Process(true);
    c.Reset();

    const auto snap = c.Snapshot(Snaps({"a", "b"}));
    CHECK_EQ(snap.grouped, 0);
    CHECK_EQ(snap.unmatched[0], 0);
    CHECK_EQ(c.Pending(0), 0);
  }
}

void TestCorrelatorNWay() {
  TEST_CASE("four streams group together and rank against the baseline") {
    Correlator c(4, 8000000);

    // costs chosen so the ranking is unambiguous: stream 2 is cheapest
    const uint64_t cost[4] = {2000000, 3000000, 1000000, 2000000};
    for (size_t stream = 0; stream < 4; ++stream) {
      std::vector<TelemetryRecord> recs;
      for (uint64_t i = 0; i < 20; ++i) {
        recs.push_back(Rec(i + 1, i * 16666666, cost[stream]));
      }
      c.Push(stream, recs);
    }
    c.Process(true);

    const auto snap = c.Snapshot(Snaps({"base", "dear", "cheap", "same"}));
    CHECK_EQ(snap.grouped, 20);
    CHECK_EQ(snap.streams.size(), 4);
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(snap.unmatched[i], 0);

    CHECK(snap.streams[0].is_baseline);
    CHECK_EQ(snap.streams[1].delta_p50, 1000000);   // dearer than baseline
    CHECK_EQ(snap.streams[2].delta_p50, -1000000);  // cheaper than baseline
    CHECK_EQ(snap.streams[3].delta_p50, 0);         // same as baseline

    CHECK_NEAR(snap.streams[1].cheaper_fraction, 0.0, 1e-9);
    CHECK_NEAR(snap.streams[2].cheaper_fraction, 1.0, 1e-9);
    CHECK_NEAR(snap.streams[2].speedup, 2.0, 1e-9);

    // pass detail is pairwise only, so a four way run does not report it
    CHECK_EQ(snap.pass_deltas.size(), 0);
  }

  TEST_CASE("one lagging stream blocks the group, not just one pair") {
    Correlator c(3, 1000000);

    std::vector<TelemetryRecord> on_time;
    for (uint64_t i = 0; i < 10; ++i) on_time.push_back(Rec(i + 1, i * 16000000, 1000000));

    std::vector<TelemetryRecord> late;
    // far enough out that no group can ever contain one of these
    for (uint64_t i = 0; i < 10; ++i) late.push_back(Rec(i + 1, i * 16000000 + 9000000, 1000000));

    c.Push(0, on_time);
    c.Push(1, on_time);
    c.Push(2, late);
    c.Process(true);

    // a frame only counts when every stream has one close to it, so a third
    // stream that is out of step costs the whole group, not one pairing
    const auto snap = c.Snapshot(Snaps({"a", "b", "c"}));
    CHECK_EQ(snap.grouped, 0);
    CHECK(snap.unmatched[0] > 0);
  }

  TEST_CASE("a correlator always has at least two streams") {
    Correlator c(1, 8000000);
    CHECK_EQ(c.stream_count(), 2);
  }
}

void TestIntervals() {
  TEST_CASE("wilson interval brackets the proportion and stays in range") {
    double lo = 0.0;
    double hi = 0.0;

    WilsonInterval(50, 100, 1.959964, &lo, &hi);
    CHECK(lo < 0.5 && hi > 0.5);
    CHECK_NEAR(lo, 0.404, 0.01);
    CHECK_NEAR(hi, 0.596, 0.01);

    // the textbook normal interval runs past one here, which is the reason
    // this is a wilson interval and not that one
    WilsonInterval(100, 100, 1.959964, &lo, &hi);
    CHECK(hi <= 1.0);
    CHECK(lo > 0.9 && lo < 1.0);

    WilsonInterval(0, 100, 1.959964, &lo, &hi);
    CHECK(lo >= 0.0);
    CHECK(hi < 0.1);

    // more evidence has to mean a tighter interval
    double wide_lo = 0.0, wide_hi = 0.0, tight_lo = 0.0, tight_hi = 0.0;
    WilsonInterval(15, 30, 1.959964, &wide_lo, &wide_hi);
    WilsonInterval(1500, 3000, 1.959964, &tight_lo, &tight_hi);
    CHECK((tight_hi - tight_lo) < (wide_hi - wide_lo));

    WilsonInterval(0, 0, 1.959964, &lo, &hi);
    CHECK_NEAR(lo, 0.0, 1e-9);
    CHECK_NEAR(hi, 0.0, 1e-9);
  }

  TEST_CASE("quantile interval brackets the median and tightens with data") {
    SignedWindow small(8);
    int64_t lo = 0;
    int64_t hi = 0;
    CHECK(!small.QuantileInterval(0.5, 1.959964, &lo, &hi));  // too few to say anything

    SignedWindow w(4096);
    for (int i = 0; i < 1000; ++i) w.Record(i);  // median is 500 ish
    CHECK(w.QuantileInterval(0.5, 1.959964, &lo, &hi));
    CHECK(lo < 500 && hi > 500);
    CHECK((hi - lo) < 100);

    // a window of one repeated value cannot disagree with itself
    SignedWindow flat(1024);
    for (int i = 0; i < 500; ++i) flat.Record(42);
    CHECK(flat.QuantileInterval(0.5, 1.959964, &lo, &hi));
    CHECK_EQ(lo, 42);
    CHECK_EQ(hi, 42);
  }

  TEST_CASE("a difference inside the noise is not called significant") {
    // both streams cost the same on average, with the difference alternating
    // sign. a point estimate could land either way, the interval should not
    Correlator noisy(2, 8000000);
    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 400; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000));
      b.push_back(Rec(i + 1, i * 16666666, (i % 2 == 0) ? 900000 : 1100000));
    }
    noisy.Push(0, a);
    noisy.Push(1, b);
    noisy.Process(true);

    const auto snap = noisy.Snapshot(Snaps({"a", "b"}));
    CHECK_EQ(snap.grouped, 400);
    CHECK(snap.streams[1].interval_valid);
    CHECK(!snap.streams[1].significant);
    CHECK(snap.streams[1].delta_p50_low < 0);
    CHECK(snap.streams[1].delta_p50_high > 0);
  }

  TEST_CASE("a real difference is called significant") {
    Correlator clear(2, 8000000);
    std::vector<TelemetryRecord> a;
    std::vector<TelemetryRecord> b;
    for (uint64_t i = 0; i < 400; ++i) {
      a.push_back(Rec(i + 1, i * 16666666, 1000000));
      b.push_back(Rec(i + 1, i * 16666666, 2000000 + (i % 7) * 1000));
    }
    clear.Push(0, a);
    clear.Push(1, b);
    clear.Process(true);

    const auto snap = clear.Snapshot(Snaps({"a", "b"}));
    CHECK(snap.streams[1].significant);
    CHECK(snap.streams[1].delta_p50_low > 0);
    CHECK_NEAR(snap.streams[1].cheaper_fraction, 0.0, 1e-9);
    CHECK(snap.streams[1].cheaper_high < 0.05);
  }
}

void TestEnvironmentComparison() {
  TEST_CASE("environment parses into keys and values") {
    const auto env = ParseEnvironment("vo=gpu-next\nrender=1274x716\nempty=\n");
    CHECK_EQ(env.size(), 3);
    CHECK(env.at("vo") == "gpu-next");
    CHECK(env.at("render") == "1274x716");
    CHECK(env.at("empty").empty());

    CHECK_EQ(ParseEnvironment("").size(), 0);
    CHECK_EQ(ParseEnvironment("no equals sign here").size(), 0);
  }

  TEST_CASE("only differences that change the meaning are flagged") {
    const std::string base = "vo=gpu-next\ngpu_context=waylandvk\nhwdec=no\n"
                             "render=1274x716\nvideo=637x358\nmpv=v0.41.0\ndisplay=DP-3\n";
    CHECK_EQ(EnvironmentMismatches(base, base).size(), 0);

    // a different renderer or window size makes two streams incomparable
    std::string other = base;
    other.replace(other.find("gpu-next"), 8, "gpu     ");
    CHECK_EQ(EnvironmentMismatches(base, other).size(), 1);

    std::string resized = base;
    resized.replace(resized.find("1274x716"), 8, "1920x108");
    CHECK_EQ(EnvironmentMismatches(base, resized).size(), 1);

    // the display name is recorded for the report but does not by itself make
    // two measurements mean different things
    std::string moved = base;
    moved.replace(moved.find("DP-3"), 4, "DP-9");
    CHECK_EQ(EnvironmentMismatches(base, moved).size(), 0);

    // nothing to compare when a stream carried no environment at all
    CHECK_EQ(EnvironmentMismatches(base, "").size(), 0);
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
  TestCorrelatorNWay();
  TestIntervals();
  TestEnvironmentComparison();
  TestAggregator();
  return fwtest::Finish("test_stats");
}
