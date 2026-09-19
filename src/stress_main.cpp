/*
 * Description: Stress harness for the shared memory ring. Forks a real producer
 *   process and a real consumer process, drives records through the ring at
 *   full speed and checks for torn reads, lost records and ordering faults.
 * Author: Alex Wu
 * Dependencies: framewire core library
 * Usage: framewire-stress --records 20000000 --capacity 4096
 */

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define FRAMEWIRE_HAVE_RDTSC 1
#endif

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "framewire/histogram.h"
#include "framewire/spsc_ring.h"
#include "framewire/telemetry.h"

namespace {

using namespace framewire;

// how long the producer tolerates a permanently full ring before calling the
// consumer dead, and how long the consumer waits on a silent producer
constexpr uint64_t kStallLimitNs = 10ull * 1000 * 1000 * 1000;
constexpr uint64_t kProducerSilenceNs = 5ull * 1000 * 1000 * 1000;

/*
 * Cycle counter used to time a single queue operation.
 *
 * clock_gettime is the portable choice and costs more than the operation being
 * measured, which would bury the result. The time stamp counter is invariant on
 * anything modern, so it is calibrated against the monotonic clock once and
 * then read directly.
 *
 * The cost of a back to back read is measured too and subtracted from every
 * sample. At these timescales the measurement floor is a real fraction of the
 * result, so leaving it in would overstate every number.
 */
class TscClock {
 public:
  bool Calibrate() {
#ifdef FRAMEWIRE_HAVE_RDTSC
    const uint64_t wall_start = MonotonicNanos();
    const uint64_t tsc_start = __rdtsc();
    const timespec nap{0, 50 * 1000 * 1000};
    nanosleep(&nap, nullptr);
    const uint64_t tsc_end = __rdtsc();
    const uint64_t wall_end = MonotonicNanos();

    const double elapsed_ns = static_cast<double>(wall_end - wall_start);
    if (elapsed_ns <= 0.0) return false;
    ticks_per_ns_ = static_cast<double>(tsc_end - tsc_start) / elapsed_ns;

    // the floor is the smallest observed gap between two adjacent reads, which
    // is as close to zero work as the counter can resolve
    uint64_t floor = ~0ull;
    for (int i = 0; i < 200000; ++i) {
      const uint64_t a = __rdtsc();
      const uint64_t b = __rdtsc();
      if (b - a < floor) floor = b - a;
    }
    floor_ticks_ = floor;
    return ticks_per_ns_ > 0.1;
#else
    return false;
#endif
  }

  uint64_t Now() const {
#ifdef FRAMEWIRE_HAVE_RDTSC
    return __rdtsc();
#else
    return 0;
#endif
  }

  // Converts a raw tick span into nanoseconds, with the read cost removed.
  uint64_t SpanNanos(uint64_t ticks) const {
    const uint64_t net = ticks > floor_ticks_ ? ticks - floor_ticks_ : 0;
    return static_cast<uint64_t>(static_cast<double>(net) / ticks_per_ns_);
  }

  double ticks_per_ns() const { return ticks_per_ns_; }
  uint64_t floor_ticks() const { return floor_ticks_; }

 private:
  double ticks_per_ns_ = 0.0;
  uint64_t floor_ticks_ = 0;
};

struct Options {
  uint64_t records = 20000000;
  uint32_t capacity = 4096;
  std::string shm_name = "/framewire-stress";
  unsigned slow_consumer_every = 0;  // pause the consumer every N records
  unsigned slow_producer_every = 0;
  bool quiet = false;
  bool verify = true;  // off measures the queue itself rather than the checks
  bool latency = false;  // time every operation instead of only the total
};

/*
 * Results the two processes share, mapped as anonymous shared memory.
 *
 * A pipe would work too, but the child has to publish counters while running,
 * not only at exit, so a shared struct is the simpler fit.
 */
struct Results {
  std::atomic<uint64_t> produced;
  std::atomic<uint64_t> dropped;
  std::atomic<uint64_t> consumed;
  std::atomic<uint64_t> checksum_errors;
  std::atomic<uint64_t> payload_errors;
  std::atomic<uint64_t> order_errors;
  std::atomic<uint64_t> gap_records;
  std::atomic<uint64_t> producer_ns;
  std::atomic<uint64_t> consumer_ns;
  std::atomic<uint64_t> consumer_ready;

  // per operation latency, in nanoseconds, filled in only for a latency run
  std::atomic<uint64_t> push_p50;
  std::atomic<uint64_t> push_p99;
  std::atomic<uint64_t> push_p999;
  std::atomic<uint64_t> push_max;
  std::atomic<uint64_t> pop_p50;
  std::atomic<uint64_t> pop_p99;
  std::atomic<uint64_t> pop_p999;
  std::atomic<uint64_t> pop_max;
  std::atomic<uint64_t> tsc_floor_ticks;
};

void PrintUsage() {
  std::fprintf(stderr,
               "usage: framewire-stress [options]\n"
               "\n"
               "  --records N       records to push (default 20000000)\n"
               "  --capacity N      ring slots, power of two (default 4096)\n"
               "  --shm NAME        segment name (default /framewire-stress)\n"
               "  --slow-consumer N stall the consumer every N records\n"
               "  --slow-producer N stall the producer every N records\n"
               "  --no-verify       skip checksums and payload rebuild, to time the queue\n"
               "  --latency         report per operation p50, p99, p999 and max\n"
               "  --quiet           only print the verdict\n");
}

/*
 * Builds the payload for a record from the sequence number alone.
 *
 * Every byte is a function of seq, so the consumer can rebuild the expected
 * record and compare. A torn read that mixed bytes from two different records
 * would fail this check even if the two halves were each individually valid.
 *
 * Args:
 *   seq: Sequence number for the record.
 *   rec: Record to fill.
 */
void FillRecord(uint64_t seq, TelemetryRecord* rec) {
  std::memset(rec, 0, sizeof(*rec));
  rec->seq = seq;
  // the timestamp is left for the caller to stamp, so this stays a pure
  // function of seq and the verify path does not read the clock per record
  rec->t_mono_ns = 0;
  rec->frame_time_ns = seq * 2654435761ull;
  rec->gpu_total_ns = 0;
  rec->media_time_ns = static_cast<int64_t>(seq * 41666666ull);
  rec->reserved = seq * 1099511628211ull;

  rec->pass_count = static_cast<uint8_t>(1 + (seq % kMaxPasses));
  for (unsigned i = 0; i < kMaxPasses; ++i) {
    rec->pass_ns[i] = i < rec->pass_count
                          ? static_cast<uint32_t>((seq * 2246822519ull + i * 374761393ull) & 0xFFFFFF)
                          : 0;
    rec->gpu_total_ns += rec->pass_ns[i];
  }
  rec->layout_version = static_cast<uint8_t>(seq & 0xFF);
  rec->flags = static_cast<uint16_t>(seq & 0x7);
  rec->dropped_total = static_cast<uint32_t>(seq >> 3);
  rec->delayed_total = static_cast<uint32_t>(seq >> 5);
  StampChecksum(*rec);
}

/*
 * Checks a received record against what the sequence number implies.
 *
 * The timestamp is the one field the consumer cannot predict, so the expected
 * record borrows it before the comparison.
 *
 * Args:
 *   rec: Record as received.
 * Returns:
 *   True when every byte matches the expected payload.
 */
bool PayloadMatches(const TelemetryRecord& rec) {
  TelemetryRecord expected;
  FillRecord(rec.seq, &expected);
  expected.t_mono_ns = rec.t_mono_ns;
  StampChecksum(expected);
  return std::memcmp(&expected, &rec, sizeof(expected)) == 0;
}

void RunProducer(const Options& opt, Results* results) {
  RingProducer producer(RingMapping::Create(opt.shm_name, opt.capacity, "stress"));

  // let the consumer attach before the clock starts, otherwise the first
  // millisecond of the run is measuring process startup rather than the queue
  results->consumer_ready.store(1, std::memory_order_release);
  while (results->consumer_ready.load(std::memory_order_acquire) < 2) {
    const timespec nap{0, 200000};
    nanosleep(&nap, nullptr);
  }

  TscClock tsc;
  const bool timing = opt.latency && tsc.Calibrate();
  // a wide range because the tail is the point. a scheduler preemption in the
  // middle of a push lands in the hundreds of microseconds
  Histogram push_latency(1000000000ull, 3);

  const uint64_t start = MonotonicNanos();
  uint64_t produced = 0;
  uint64_t dropped = 0;

  TelemetryRecord rec;
  FillRecord(1, &rec);  // reused when not verifying, only seq changes per push

  for (uint64_t seq = 1; seq <= opt.records; ++seq) {
    if (opt.verify) {
      // fnv1a over 124 bytes, plus rebuilding the payload, is most of the per
      // record cost. that is the right trade for an integrity run and the
      // wrong one for measuring the queue, hence the two modes
      FillRecord(seq, &rec);
      rec.t_mono_ns = MonotonicNanos();
      StampChecksum(rec);
    } else {
      rec.seq = seq;
    }

    // the heartbeat has to keep ticking through the push loop. without it the
    // consumer sees a stale stamp, decides the producer died and exits early,
    // which only shows up on runs longer than the staleness window
    if ((seq & 0xFFF) == 0) producer.Heartbeat();

    // the timed push is the uncontended path only. a push that has to wait for
    // the consumer is backpressure, not queue cost, so it is excluded below
    if (timing) {
      const uint64_t t0 = tsc.Now();
      const bool ok = producer.TryPush(rec);
      const uint64_t t1 = tsc.Now();
      if (ok) {
        push_latency.Record(tsc.SpanNanos(t1 - t0));
        ++produced;
        if (opt.slow_producer_every != 0 && produced % opt.slow_producer_every == 0) {
          const timespec nap{0, 50000};
          nanosleep(&nap, nullptr);
        }
        continue;
      }
    }

    // retry rather than drop, so a completed run has a known record count and
    // any missing record is a real defect instead of expected backpressure
    uint64_t stall_started = 0;
    while (!producer.TryPush(rec)) {
      ++dropped;

      // a ring that stays full means the consumer is gone, and retrying for
      // ever would hang the harness instead of reporting the failure
      const uint64_t now = MonotonicNanos();
      if (stall_started == 0) {
        stall_started = now;
      } else if (now - stall_started > kStallLimitNs) {
        std::fprintf(stderr,
                     "framewire-stress producer: ring stayed full for %.0f s at record %llu, "
                     "the consumer is not draining\n",
                     static_cast<double>(kStallLimitNs) / 1e9,
                     static_cast<unsigned long long>(seq));
        results->produced.store(produced, std::memory_order_relaxed);
        results->dropped.store(dropped, std::memory_order_relaxed);
        results->producer_ns.store(now - start, std::memory_order_release);
        producer.MarkDone();
        _exit(1);
      }

      producer.Heartbeat();
      const timespec nap{0, 1000};
      nanosleep(&nap, nullptr);
    }
    ++produced;

    if (opt.slow_producer_every != 0 && produced % opt.slow_producer_every == 0) {
      const timespec nap{0, 50000};
      nanosleep(&nap, nullptr);
    }
  }

  const uint64_t elapsed = MonotonicNanos() - start;
  producer.MarkDone();

  results->produced.store(produced, std::memory_order_relaxed);
  results->dropped.store(dropped, std::memory_order_relaxed);

  if (timing && push_latency.count() > 0) {
    results->push_p50.store(push_latency.ValueAtQuantile(0.50), std::memory_order_relaxed);
    results->push_p99.store(push_latency.ValueAtQuantile(0.99), std::memory_order_relaxed);
    results->push_p999.store(push_latency.ValueAtQuantile(0.999), std::memory_order_relaxed);
    results->push_max.store(push_latency.max(), std::memory_order_relaxed);
    results->tsc_floor_ticks.store(tsc.floor_ticks(), std::memory_order_relaxed);
  }
  results->producer_ns.store(elapsed, std::memory_order_release);
}

void RunConsumer(const Options& opt, Results* results) {
  while (results->consumer_ready.load(std::memory_order_acquire) < 1) {
    const timespec nap{0, 200000};
    nanosleep(&nap, nullptr);
  }

  RingConsumer consumer(RingMapping::Open(opt.shm_name));
  results->consumer_ready.store(2, std::memory_order_release);

  const uint64_t start = MonotonicNanos();
  uint64_t consumed = 0;
  uint64_t checksum_errors = 0;
  uint64_t payload_errors = 0;
  uint64_t order_errors = 0;
  uint64_t gap_records = 0;
  uint64_t last_seq = 0;

  TscClock tsc;
  const bool timing = opt.latency && tsc.Calibrate();
  Histogram pop_latency(1000000000ull, 3);

  constexpr size_t kBatch = 256;
  TelemetryRecord batch[kBatch];

  for (;;) {
    if (timing) {
      // one record at a time here on purpose. a batched pop amortises the
      // acquire load away, which is the right thing to do in production and the
      // wrong thing when the question is what a single dequeue costs
      TelemetryRecord one;
      const uint64_t t0 = tsc.Now();
      const bool ok = consumer.TryPop(one);
      const uint64_t t1 = tsc.Now();
      if (ok) {
        pop_latency.Record(tsc.SpanNanos(t1 - t0));
        if (opt.verify) {
          if (!VerifyChecksum(one)) ++checksum_errors;
          if (!PayloadMatches(one)) ++payload_errors;
        }
        if (last_seq != 0) {
          if (one.seq <= last_seq) {
            ++order_errors;
          } else if (one.seq != last_seq + 1) {
            gap_records += one.seq - last_seq - 1;
          }
        }
        last_seq = one.seq;
        ++consumed;
        continue;
      }
      if (consumer.ProducerGone(kProducerSilenceNs) && consumer.PendingCount() == 0) break;
      continue;
    }

    const size_t n = consumer.PopBatch(batch, kBatch);
    if (n == 0) {
      if (consumer.ProducerGone(kProducerSilenceNs) && consumer.PendingCount() == 0) break;
      continue;
    }

    for (size_t i = 0; i < n; ++i) {
      const TelemetryRecord& rec = batch[i];

      if (opt.verify) {
        if (!VerifyChecksum(rec)) ++checksum_errors;
        if (!PayloadMatches(rec)) ++payload_errors;
      }

      // the producer retries instead of dropping, so a gap here means a record
      // went missing, which would be a real fault in the queue
      if (last_seq != 0) {
        if (rec.seq <= last_seq) {
          ++order_errors;
        } else if (rec.seq != last_seq + 1) {
          gap_records += rec.seq - last_seq - 1;
        }
      }
      last_seq = rec.seq;
      ++consumed;
    }

    if (opt.slow_consumer_every != 0 && consumed % opt.slow_consumer_every < kBatch) {
      const timespec nap{0, 100000};
      nanosleep(&nap, nullptr);
    }
  }

  const uint64_t elapsed = MonotonicNanos() - start;
  results->consumed.store(consumed, std::memory_order_relaxed);
  results->checksum_errors.store(checksum_errors, std::memory_order_relaxed);
  results->payload_errors.store(payload_errors, std::memory_order_relaxed);
  results->order_errors.store(order_errors, std::memory_order_relaxed);
  results->gap_records.store(gap_records, std::memory_order_relaxed);

  if (timing && pop_latency.count() > 0) {
    results->pop_p50.store(pop_latency.ValueAtQuantile(0.50), std::memory_order_relaxed);
    results->pop_p99.store(pop_latency.ValueAtQuantile(0.99), std::memory_order_relaxed);
    results->pop_p999.store(pop_latency.ValueAtQuantile(0.999), std::memory_order_relaxed);
    results->pop_max.store(pop_latency.max(), std::memory_order_relaxed);
  }
  results->consumer_ns.store(elapsed, std::memory_order_release);
}

int Run(const Options& opt) {
  // anonymous shared mapping so both forked children write into one struct the
  // parent can read after reaping them
  void* shared = mmap(nullptr, sizeof(Results), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED) {
    std::fprintf(stderr, "framewire-stress: could not map the result block\n");
    return 1;
  }
  auto* results = new (shared) Results{};

  ShmRegion::Remove(opt.shm_name);

  if (!opt.quiet) {
    std::printf("framewire stress test\n");
    std::printf("  records   %" PRIu64 "\n", opt.records);
    std::printf("  capacity  %u slots (%.2f MiB)\n", opt.capacity,
                static_cast<double>(RingBytes(opt.capacity)) / 1048576.0);
    std::printf("  record    %zu bytes\n", sizeof(TelemetryRecord));
    std::printf("  mode      %s\n\n",
                opt.verify ? "integrity, every record checksummed and rebuilt"
                           : "throughput, queue only with checks disabled");
  }

  const pid_t consumer_pid = fork();
  if (consumer_pid < 0) {
    std::fprintf(stderr, "framewire-stress: fork failed\n");
    return 1;
  }
  if (consumer_pid == 0) {
    try {
      RunConsumer(opt, results);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "framewire-stress consumer: %s\n", e.what());
      _exit(1);
    }
    _exit(0);
  }

  const pid_t producer_pid = fork();
  if (producer_pid < 0) {
    std::fprintf(stderr, "framewire-stress: fork failed\n");
    return 1;
  }
  if (producer_pid == 0) {
    try {
      RunProducer(opt, results);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "framewire-stress producer: %s\n", e.what());
      _exit(1);
    }
    _exit(0);
  }

  int producer_status = 0;
  int consumer_status = 0;
  waitpid(producer_pid, &producer_status, 0);
  waitpid(consumer_pid, &consumer_status, 0);

  const uint64_t produced = results->produced.load(std::memory_order_relaxed);
  const uint64_t consumed = results->consumed.load(std::memory_order_relaxed);
  const uint64_t backpressure = results->dropped.load(std::memory_order_relaxed);
  const uint64_t checksum_errors = results->checksum_errors.load(std::memory_order_relaxed);
  const uint64_t payload_errors = results->payload_errors.load(std::memory_order_relaxed);
  const uint64_t order_errors = results->order_errors.load(std::memory_order_relaxed);
  const uint64_t gap_records = results->gap_records.load(std::memory_order_relaxed);
  const uint64_t producer_ns = results->producer_ns.load(std::memory_order_acquire);

  ShmRegion::Remove(opt.shm_name);

  const double seconds = static_cast<double>(producer_ns) / 1e9;
  const double rate = seconds > 0 ? static_cast<double>(produced) / seconds : 0.0;

  std::printf("throughput\n");
  std::printf("  produced        %" PRIu64 "\n", produced);
  std::printf("  consumed        %" PRIu64 "\n", consumed);
  std::printf("  elapsed         %.3f s\n", seconds);
  std::printf("  rate            %.2f M records/s\n", rate / 1e6);
  std::printf("  throughput      %.2f MiB/s\n",
              rate * static_cast<double>(sizeof(TelemetryRecord)) / 1048576.0);
  std::printf("  ns per record   %.2f\n", produced > 0 ? static_cast<double>(producer_ns) /
                                                             static_cast<double>(produced)
                                                       : 0.0);
  std::printf("  full ring waits %" PRIu64 "\n\n", backpressure);

  if (opt.latency) {
    const uint64_t floor_ticks = results->tsc_floor_ticks.load(std::memory_order_relaxed);
    std::printf("per operation latency\n");
    std::printf("  %-10s %10s %10s %10s %10s\n", "op", "p50", "p99", "p999", "max");
    std::printf("  %-10s %9llu %9llu %9llu %9llu\n", "push ns",
                (unsigned long long)results->push_p50.load(),
                (unsigned long long)results->push_p99.load(),
                (unsigned long long)results->push_p999.load(),
                (unsigned long long)results->push_max.load());
    std::printf("  %-10s %9llu %9llu %9llu %9llu\n", "pop ns",
                (unsigned long long)results->pop_p50.load(),
                (unsigned long long)results->pop_p99.load(),
                (unsigned long long)results->pop_p999.load(),
                (unsigned long long)results->pop_max.load());
    std::printf("  timed with rdtsc, %llu tick read cost already subtracted.\n",
                (unsigned long long)floor_ticks);
    std::printf("  the median sits near the measurement floor, the tail is real.\n\n");
  }

  std::printf("integrity\n");
  std::printf("  checksum errors %" PRIu64 "\n", checksum_errors);
  std::printf("  payload errors  %" PRIu64 "\n", payload_errors);
  std::printf("  order errors    %" PRIu64 "\n", order_errors);
  std::printf("  missing records %" PRIu64 "\n", gap_records);
  std::printf("  lost at exit    %" PRIu64 "\n\n",
              produced > consumed ? produced - consumed : 0);

  const bool clean = checksum_errors == 0 && payload_errors == 0 && order_errors == 0 &&
                     gap_records == 0 && produced == consumed && produced == opt.records &&
                     WIFEXITED(producer_status) && WEXITSTATUS(producer_status) == 0 &&
                     WIFEXITED(consumer_status) && WEXITSTATUS(consumer_status) == 0;

  std::printf("%s\n", clean ? "PASS: no torn reads, no lost records"
                            : "FAIL: see the integrity counters above");
  return clean ? 0 : 1;
}

bool ParseArgs(int argc, char** argv, Options* opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--records") {
      const char* v = next("--records");
      if (!v) return false;
      opt->records = std::strtoull(v, nullptr, 10);
    } else if (arg == "--capacity") {
      const char* v = next("--capacity");
      if (!v) return false;
      opt->capacity = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    } else if (arg == "--shm") {
      const char* v = next("--shm");
      if (!v) return false;
      opt->shm_name = v;
    } else if (arg == "--slow-consumer") {
      const char* v = next("--slow-consumer");
      if (!v) return false;
      opt->slow_consumer_every = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else if (arg == "--slow-producer") {
      const char* v = next("--slow-producer");
      if (!v) return false;
      opt->slow_producer_every = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else if (arg == "--no-verify") {
      opt->verify = false;
    } else if (arg == "--latency") {
      opt->latency = true;
    } else if (arg == "--quiet") {
      opt->quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return false;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;

  try {
    return Run(opt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "framewire-stress: %s\n", e.what());
    return 1;
  }
}
