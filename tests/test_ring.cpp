/*
 * Description: Unit tests for the shared memory SPSC ring, covering ordering,
 *   wraparound, the full ring policy and the pass name directory.
 * Author: Alex Wu
 * Dependencies: framewire core library, tests/test_util.h
 * Usage: ctest, or run the binary directly
 */

#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "framewire/spsc_ring.h"
#include "test_util.h"

using namespace framewire;

namespace {

// each test uses a private segment name so a parallel ctest run cannot collide
std::string SegmentName(const char* suffix) {
  return std::string("/framewire-test-") + suffix + "-" + std::to_string(getpid());
}

TelemetryRecord MakeRecord(uint64_t seq) {
  TelemetryRecord rec{};
  rec.seq = seq;
  rec.t_mono_ns = seq * 1000;
  rec.frame_time_ns = seq * 7;
  rec.gpu_total_ns = seq * 13;
  rec.pass_count = 3;
  for (unsigned i = 0; i < 3; ++i) rec.pass_ns[i] = static_cast<uint32_t>(seq + i);
  StampChecksum(rec);
  return rec;
}

void TestLayoutInvariants() {
  TEST_CASE("record and header layout are pinned") {
    CHECK_EQ(sizeof(TelemetryRecord), 192);
    CHECK_EQ(alignof(TelemetryRecord), 64);
    CHECK_EQ(kChecksumCoverage, sizeof(TelemetryRecord) - sizeof(uint32_t));

    // the whole point of the padding is that these two never share a line
    const size_t head_line = offsetof(RingHeader, head) / kCacheLine;
    const size_t tail_line = offsetof(RingHeader, tail) / kCacheLine;
    CHECK(head_line != tail_line);
    CHECK_EQ(sizeof(RingHeader) % kCacheLine, 0);
  }
}

void TestChecksum() {
  TEST_CASE("checksum catches a single flipped byte") {
    TelemetryRecord rec = MakeRecord(42);
    CHECK(VerifyChecksum(rec));

    rec.pass_ns[1] ^= 1u;
    CHECK(!VerifyChecksum(rec));

    StampChecksum(rec);
    CHECK(VerifyChecksum(rec));
  }
}

void TestPushPop() {
  TEST_CASE("records come out in order and unchanged") {
    const std::string name = SegmentName("basic");
    RingProducer producer(RingMapping::Create(name, 8, "basic"));
    RingConsumer consumer(RingMapping::Open(name));

    TelemetryRecord out{};
    CHECK(!consumer.TryPop(out));  // empty ring yields nothing
    CHECK_EQ(consumer.PendingCount(), 0);

    for (uint64_t seq = 1; seq <= 5; ++seq) CHECK(producer.TryPush(MakeRecord(seq)));
    CHECK_EQ(consumer.PendingCount(), 5);

    for (uint64_t seq = 1; seq <= 5; ++seq) {
      CHECK(consumer.TryPop(out));
      CHECK_EQ(out.seq, seq);
      CHECK_EQ(out.gpu_total_ns, seq * 13);
      CHECK(VerifyChecksum(out));
    }
    CHECK(!consumer.TryPop(out));
  }
}

void TestFullRingDropsNewest() {
  TEST_CASE("a full ring drops the newest and counts the drop") {
    const std::string name = SegmentName("full");
    RingProducer producer(RingMapping::Create(name, 4, "full"));
    RingConsumer consumer(RingMapping::Open(name));

    for (uint64_t seq = 1; seq <= 4; ++seq) CHECK(producer.TryPush(MakeRecord(seq)));

    // the fifth push has nowhere to go, and must not overwrite an unread slot
    CHECK(!producer.TryPush(MakeRecord(5)));
    CHECK_EQ(producer.dropped(), 1);

    TelemetryRecord out{};
    CHECK(consumer.TryPop(out));
    CHECK_EQ(out.seq, 1);  // the oldest record survived, the newest was refused

    // one slot freed means exactly one more push fits
    CHECK(producer.TryPush(MakeRecord(6)));
    CHECK(!producer.TryPush(MakeRecord(7)));
    CHECK_EQ(producer.dropped(), 2);
  }
}

void TestWraparound() {
  TEST_CASE("indices keep working past the slot count") {
    const std::string name = SegmentName("wrap");
    RingProducer producer(RingMapping::Create(name, 4, "wrap"));
    RingConsumer consumer(RingMapping::Open(name));

    TelemetryRecord out{};
    // ten times around a four slot ring, so the masking is exercised well past
    // the point where head and tail have both wrapped many times
    for (uint64_t seq = 1; seq <= 40; ++seq) {
      CHECK(producer.TryPush(MakeRecord(seq)));
      CHECK(consumer.TryPop(out));
      CHECK_EQ(out.seq, seq);
    }
    CHECK_EQ(producer.dropped(), 0);
  }
}

void TestBatchPop() {
  TEST_CASE("batch pop drains what is available") {
    const std::string name = SegmentName("batch");
    RingProducer producer(RingMapping::Create(name, 64, "batch"));
    RingConsumer consumer(RingMapping::Open(name));

    for (uint64_t seq = 1; seq <= 20; ++seq) CHECK(producer.TryPush(MakeRecord(seq)));

    TelemetryRecord out[32];
    CHECK_EQ(consumer.PopBatch(out, 8), 8);
    CHECK_EQ(out[0].seq, 1);
    CHECK_EQ(out[7].seq, 8);

    CHECK_EQ(consumer.PopBatch(out, 32), 12);  // only twelve left, not thirty two
    CHECK_EQ(out[0].seq, 9);
    CHECK_EQ(out[11].seq, 20);

    CHECK_EQ(consumer.PopBatch(out, 32), 0);
    CHECK_EQ(consumer.PopBatch(out, 0), 0);
  }
}

void TestBatchPopAcrossWrap() {
  TEST_CASE("batch pop spans the end of the slot array") {
    const std::string name = SegmentName("batchwrap");
    RingProducer producer(RingMapping::Create(name, 8, "batchwrap"));
    RingConsumer consumer(RingMapping::Open(name));

    TelemetryRecord out[16];
    // push and drain six to move the indices off zero, then fill across the end
    for (uint64_t seq = 1; seq <= 6; ++seq) producer.TryPush(MakeRecord(seq));
    CHECK_EQ(consumer.PopBatch(out, 16), 6);

    for (uint64_t seq = 7; seq <= 14; ++seq) CHECK(producer.TryPush(MakeRecord(seq)));
    CHECK_EQ(consumer.PopBatch(out, 16), 8);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK_EQ(out[i].seq, 7 + i);
      CHECK(VerifyChecksum(out[i]));
    }
  }
}

void TestLayoutDirectory() {
  TEST_CASE("pass names publish and read back") {
    const std::string name = SegmentName("layout");
    RingProducer producer(RingMapping::Create(name, 8, "layout"));
    RingConsumer consumer(RingMapping::Open(name));

    const char* names[3] = {"upload", "espcn conv1", "blit"};
    const uint8_t version = producer.PublishLayout(names, 3);
    CHECK_EQ(version, 1);

    char read_back[kMaxPasses][kPassNameLen];
    uint32_t read_version = 0;
    CHECK_EQ(consumer.ReadLayout(read_back, &read_version), 3);
    CHECK_EQ(read_version, 1);
    CHECK(std::string(read_back[0]) == "upload");
    CHECK(std::string(read_back[1]) == "espcn conv1");
    CHECK(std::string(read_back[2]) == "blit");

    // a longer chain replaces the old directory and bumps the version
    const char* more[2] = {"a", "b"};
    CHECK_EQ(producer.PublishLayout(more, 2), 2);
    CHECK_EQ(consumer.ReadLayout(read_back, &read_version), 2);
    CHECK_EQ(read_version, 2);
    CHECK(std::string(read_back[0]) == "a");
  }

  TEST_CASE("a name longer than the field is truncated") {
    const std::string name = SegmentName("longname");
    RingProducer producer(RingMapping::Create(name, 8, "longname"));
    RingConsumer consumer(RingMapping::Open(name));

    const std::string huge(200, 'x');
    const char* names[1] = {huge.c_str()};
    producer.PublishLayout(names, 1);

    char read_back[kMaxPasses][kPassNameLen];
    consumer.ReadLayout(read_back, nullptr);
    CHECK_EQ(std::string(read_back[0]).size(), kPassNameLen - 1);
  }
}

void TestProducerLiveness() {
  TEST_CASE("the consumer notices a finished producer") {
    const std::string name = SegmentName("done");
    RingProducer producer(RingMapping::Create(name, 8, "done"));
    RingConsumer consumer(RingMapping::Open(name));

    producer.Heartbeat();
    CHECK(!consumer.ProducerGone(1000000000ull));

    producer.MarkDone();
    CHECK(consumer.ProducerGone(1000000000ull));
  }

  TEST_CASE("a silent producer goes stale and a heartbeat revives it") {
    const std::string name = SegmentName("stale");
    RingProducer producer(RingMapping::Create(name, 8, "stale"));
    RingConsumer consumer(RingMapping::Open(name));

    // a busy producer that forgets to check in looks exactly like a dead one,
    // which once made the stress harness quit early on any run long enough to
    // cross the window
    constexpr uint64_t kTinyWindowNs = 2000000;  // two milliseconds
    producer.Heartbeat();
    CHECK(!consumer.ProducerGone(kTinyWindowNs));

    const timespec nap{0, 20 * 1000 * 1000};
    nanosleep(&nap, nullptr);
    CHECK(consumer.ProducerGone(kTinyWindowNs));

    producer.Heartbeat();
    CHECK(!consumer.ProducerGone(kTinyWindowNs));
  }
}

void TestRejectsBadSegment() {
  TEST_CASE("opening rejects a segment that is not a ring") {
    const std::string name = SegmentName("bogus");
    ShmRegion region = ShmRegion::Create(name, 4096);

    bool threw = false;
    try {
      RingMapping::Open(name);
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }

  TEST_CASE("creating rejects a non power of two capacity") {
    bool threw = false;
    try {
      RingMapping::Create(SegmentName("odd"), 100, "odd");
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }
}

void TestThreadedHandoff() {
  TEST_CASE("threaded producer and consumer lose nothing") {
    const std::string name = SegmentName("threaded");
    constexpr uint64_t kTotal = 200000;

    RingProducer producer(RingMapping::Create(name, 256, "threaded"));
    RingConsumer consumer(RingMapping::Open(name));

    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> bad{0};
    std::atomic<bool> done{false};

    std::thread reader([&] {
      TelemetryRecord out[64];
      uint64_t expect = 1;
      while (true) {
        const size_t n = consumer.PopBatch(out, 64);
        if (n == 0) {
          if (done.load(std::memory_order_acquire) && consumer.PendingCount() == 0) break;
          continue;
        }
        for (size_t i = 0; i < n; ++i) {
          if (!VerifyChecksum(out[i]) || out[i].seq != expect) ++bad;
          ++expect;
          received.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });

    for (uint64_t seq = 1; seq <= kTotal; ++seq) {
      // spin until the slot frees, so the run has an exact expected count
      while (!producer.TryPush(MakeRecord(seq))) {
      }
    }
    done.store(true, std::memory_order_release);
    reader.join();

    CHECK_EQ(received.load(), kTotal);
    CHECK_EQ(bad.load(), 0);
  }
}

}  // namespace

int main() {
  std::printf("test_ring\n");
  TestLayoutInvariants();
  TestChecksum();
  TestPushPop();
  TestFullRingDropsNewest();
  TestWraparound();
  TestBatchPop();
  TestBatchPopAcrossWrap();
  TestLayoutDirectory();
  TestProducerLiveness();
  TestRejectsBadSegment();
  TestThreadedHandoff();
  return fwtest::Finish("test_ring");
}
