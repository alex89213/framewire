/*
 * Description: Implementation of the shared memory SPSC ring, including the
 *   acquire and release protocol that makes the queue safe without a lock.
 * Author: Alex Wu
 * Dependencies: framewire/spsc_ring.h
 * Usage:
 */

#include "framewire/spsc_ring.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace framewire {
namespace {

bool IsPowerOfTwo(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

// a producer that checked in this recently is treated as still running
constexpr uint64_t kLiveProducerNs = 3ull * 1000 * 1000 * 1000;

/*
 * Decides what to do about a segment that already exists.
 *
 * A leftover from a crashed run should be replaced. A segment a live producer
 * is still writing to must not be, because removing it would leave that
 * producer filling memory no consumer can reach while it reports success.
 *
 * Args:
 *   name: Segment name.
 * Returns:
 *   An empty string when the segment is safe to replace, otherwise a
 *   description of who is using it.
 */
std::string DescribeLiveOwner(const std::string& name) {
  try {
    ShmRegion probe = ShmRegion::Open(name);
    if (probe.size() < sizeof(RingHeader)) return {};

    const auto* header = static_cast<const RingHeader*>(probe.data());
    if (header->magic != kRingMagic) return {};
    if (header->producer_done.load(std::memory_order_acquire) != 0) return {};

    const uint64_t beat = header->producer_heartbeat_ns.load(std::memory_order_relaxed);
    const uint64_t now = MonotonicNanos();
    if (now > beat && (now - beat) > kLiveProducerNs) return {};

    const uint32_t pid = header->producer_pid.load(std::memory_order_relaxed);
    return "a producer with pid " + std::to_string(pid) + " is still streaming to it";
  } catch (const std::exception&) {
    // unreadable or half built, so replacing it is the right move
    return {};
  }
}

}  // namespace

RingMapping RingMapping::Create(const std::string& name, uint32_t capacity,
                                const std::string& label) {
  if (!IsPowerOfTwo(capacity)) {
    throw std::runtime_error("ring capacity must be a power of two, got " +
                             std::to_string(capacity));
  }

  if (ShmRegion::Exists(name)) {
    const std::string owner = DescribeLiveOwner(name);
    if (!owner.empty()) {
      throw std::runtime_error(
          "ring '" + name + "' is already in use, " + owner +
          ". pick a different --shm name, or stop the other run. reusing the name would "
          "leave one producer writing where nothing reads");
    }
    // stale leftover from a run that did not shut down cleanly
    ShmRegion::Remove(name);
  }

  RingMapping m;
  m.region_ = ShmRegion::Create(name, RingBytes(capacity));

  // shm_open hands back zeroed pages, but placement new states the intent and
  // starts the lifetime of the atomics properly instead of casting over raw
  // bytes and hoping the object model agrees
  m.header_ = new (m.region_.data()) RingHeader{};
  m.header_->magic = kRingMagic;
  m.header_->abi_version = kRingAbiVersion;
  m.header_->capacity = capacity;
  m.header_->record_size = sizeof(TelemetryRecord);
  m.header_->created_mono_ns = MonotonicNanos();
  std::snprintf(m.header_->label, sizeof(m.header_->label), "%s", label.c_str());
  m.header_->head.store(0, std::memory_order_relaxed);
  m.header_->tail.store(0, std::memory_order_relaxed);
  m.header_->push_dropped.store(0, std::memory_order_relaxed);
  m.header_->producer_pid.store(static_cast<uint32_t>(getpid()), std::memory_order_relaxed);
  m.header_->producer_done.store(0, std::memory_order_relaxed);
  m.header_->layout_version.store(0, std::memory_order_relaxed);
  m.header_->pass_count.store(0, std::memory_order_relaxed);
  m.header_->producer_heartbeat_ns.store(m.header_->created_mono_ns, std::memory_order_relaxed);
  m.header_->environment_version.store(0, std::memory_order_relaxed);
  m.header_->geometry_changes.store(0, std::memory_order_relaxed);
  m.header_->environment[0] = '\0';

  m.slots_ = reinterpret_cast<TelemetryRecord*>(static_cast<char*>(m.region_.data()) +
                                                sizeof(RingHeader));
  return m;
}

RingMapping RingMapping::Open(const std::string& name) {
  RingMapping m;
  m.region_ = ShmRegion::Open(name);

  if (m.region_.size() < sizeof(RingHeader)) {
    throw std::runtime_error("shm segment '" + name + "' is too small to hold a ring header");
  }

  m.header_ = static_cast<RingHeader*>(m.region_.data());

  // every check below guards against attaching to a segment that a different
  // build wrote. a mismatched record size would hand back shifted fields and
  // look like corrupt telemetry rather than a version problem
  if (m.header_->magic != kRingMagic) {
    throw std::runtime_error("shm segment '" + name + "' is not a framewire ring");
  }
  if (m.header_->abi_version != kRingAbiVersion) {
    throw std::runtime_error("ring '" + name + "' has abi version " +
                             std::to_string(m.header_->abi_version) + ", this build expects " +
                             std::to_string(kRingAbiVersion));
  }
  if (m.header_->record_size != sizeof(TelemetryRecord)) {
    throw std::runtime_error("ring '" + name + "' has record size " +
                             std::to_string(m.header_->record_size) + ", this build expects " +
                             std::to_string(sizeof(TelemetryRecord)));
  }
  if (!IsPowerOfTwo(m.header_->capacity)) {
    throw std::runtime_error("ring '" + name + "' has a non power of two capacity");
  }
  if (m.region_.size() < RingBytes(m.header_->capacity)) {
    throw std::runtime_error("ring '" + name + "' is smaller than the declared capacity");
  }

  m.slots_ = reinterpret_cast<TelemetryRecord*>(static_cast<char*>(m.region_.data()) +
                                                sizeof(RingHeader));
  return m;
}

RingProducer::RingProducer(RingMapping mapping)
    : mapping_(std::move(mapping)),
      header_(mapping_.header()),
      slots_(mapping_.slots()),
      mask_(mapping_.capacity() - 1) {
  cached_tail_ = header_->tail.load(std::memory_order_acquire);
}

bool RingProducer::TryPush(const TelemetryRecord& rec) {
  // relaxed is correct for head here. the producer is the only writer of head,
  // so no other thread can change the value between this load and the store
  // below, and nothing is being published by reading it
  const uint64_t head = header_->head.load(std::memory_order_relaxed);

  if (head - cached_tail_ >= mask_ + 1) {
    // the cached tail says full, so pay for a real read before giving up.
    // acquire pairs with the consumer's release store of tail and guarantees
    // the consumer finished copying out of any slot now being reclaimed. a
    // relaxed load here could let the slot write below land while the consumer
    // is still reading the old record out of the same bytes
    cached_tail_ = header_->tail.load(std::memory_order_acquire);

    if (head - cached_tail_ >= mask_ + 1) {
      header_->push_dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }

  // plain non atomic store, published by the release below. the slot is not
  // reachable by the consumer yet because head still points before it
  slots_[head & mask_] = rec;

  // release is the publish step. every write above, including the whole record
  // copy, is visible to a consumer that reads this new head with acquire.
  // dropping to relaxed here is the classic bug, the consumer would be allowed
  // to observe the bumped index while the record bytes are still in flight
  header_->head.store(head + 1, std::memory_order_release);
  return true;
}

uint8_t RingProducer::PublishLayout(const char* const* names, unsigned count) {
  if (count > kMaxPasses) count = kMaxPasses;

  for (unsigned i = 0; i < count; ++i) {
    std::snprintf(header_->pass_names[i], kPassNameLen, "%s", names[i] != nullptr ? names[i] : "");
  }
  for (unsigned i = count; i < kMaxPasses; ++i) header_->pass_names[i][0] = '\0';

  header_->pass_count.store(count, std::memory_order_relaxed);

  // release publishes the name writes above. the consumer acquire loads
  // layout_version and only then trusts the names, so a half written directory
  // is never observed with the new version number
  const uint32_t next = header_->layout_version.load(std::memory_order_relaxed) + 1;
  header_->layout_version.store(next, std::memory_order_release);

  // records carry the low byte so the consumer can spot timings that belong to
  // an older directory without widening the record
  return static_cast<uint8_t>(next & 0xFF);
}

void RingProducer::PublishEnvironment(const std::string& text) {
  std::snprintf(header_->environment, kEnvironmentLen, "%s", text.c_str());
  // release publishes the text above, so a consumer that sees a non zero
  // version never reads a half written block
  header_->environment_version.store(1, std::memory_order_release);
}

void RingProducer::NoteGeometryChange() {
  header_->geometry_changes.fetch_add(1, std::memory_order_relaxed);
}

void RingProducer::Heartbeat() {
  header_->producer_heartbeat_ns.store(MonotonicNanos(), std::memory_order_relaxed);
}

void RingProducer::MarkDone() {
  header_->producer_heartbeat_ns.store(MonotonicNanos(), std::memory_order_relaxed);
  // release so a consumer that sees the done flag also sees every record
  // pushed before the flag was set
  header_->producer_done.store(1, std::memory_order_release);
}

RingConsumer::RingConsumer(RingMapping mapping)
    : mapping_(std::move(mapping)),
      header_(mapping_.header()),
      slots_(mapping_.slots()),
      mask_(mapping_.capacity() - 1) {
  cached_head_ = header_->head.load(std::memory_order_acquire);
}

bool RingConsumer::TryPop(TelemetryRecord& out) {
  // relaxed for the same reason the producer reads head relaxed. the consumer
  // owns tail and is the only writer
  const uint64_t tail = header_->tail.load(std::memory_order_relaxed);

  if (tail == cached_head_) {
    // acquire pairs with the producer's release store of head. without the
    // acquire the record copy below could be reordered ahead of this load and
    // read bytes the producer has not finished writing
    cached_head_ = header_->head.load(std::memory_order_acquire);
    if (tail == cached_head_) return false;
  }

  out = slots_[tail & mask_];

  // release keeps the copy above from sinking past the index bump. the
  // producer acquire loads tail before reusing this slot, so the copy has to
  // be complete by the time the new tail becomes visible
  header_->tail.store(tail + 1, std::memory_order_release);
  return true;
}

size_t RingConsumer::PopBatch(TelemetryRecord* out, size_t max_records) {
  if (max_records == 0) return 0;

  const uint64_t tail = header_->tail.load(std::memory_order_relaxed);

  if (tail == cached_head_) {
    cached_head_ = header_->head.load(std::memory_order_acquire);
    if (tail == cached_head_) return 0;
  }

  // one acquire load covers the whole batch, which is the point of batching.
  // everything the producer wrote before publishing cached_head_ is visible,
  // so each slot below can be copied with a plain load
  uint64_t available = cached_head_ - tail;
  if (available > max_records) available = max_records;

  for (uint64_t i = 0; i < available; ++i) {
    out[i] = slots_[(tail + i) & mask_];
  }

  // a single release for the batch, so the producer reclaims every drained
  // slot at once instead of one store per record
  header_->tail.store(tail + available, std::memory_order_release);
  return static_cast<size_t>(available);
}

unsigned RingConsumer::ReadLayout(char out_names[kMaxPasses][kPassNameLen],
                                  uint32_t* out_version) const {
  // acquire pairs with the producer's release store in PublishLayout, so the
  // name bytes belonging to this version are all visible before being copied
  const uint32_t version = header_->layout_version.load(std::memory_order_acquire);
  unsigned count = header_->pass_count.load(std::memory_order_relaxed);
  if (count > kMaxPasses) count = kMaxPasses;

  for (unsigned i = 0; i < count; ++i) {
    std::memcpy(out_names[i], header_->pass_names[i], kPassNameLen);
    out_names[i][kPassNameLen - 1] = '\0';
  }
  for (unsigned i = count; i < kMaxPasses; ++i) out_names[i][0] = '\0';

  if (out_version != nullptr) *out_version = version;
  return count;
}

std::string RingConsumer::ReadEnvironment() const {
  // acquire pairs with the producer's release in PublishEnvironment
  if (header_->environment_version.load(std::memory_order_acquire) == 0) return {};

  char buffer[kEnvironmentLen];
  std::memcpy(buffer, header_->environment, kEnvironmentLen);
  buffer[kEnvironmentLen - 1] = '\0';
  return std::string(buffer);
}

uint64_t RingConsumer::PendingCount() const {
  const uint64_t head = header_->head.load(std::memory_order_acquire);
  const uint64_t tail = header_->tail.load(std::memory_order_relaxed);
  return head - tail;
}

bool RingConsumer::ProducerGone(uint64_t stale_after_ns) const {
  if (header_->producer_done.load(std::memory_order_acquire) != 0) return true;

  const uint64_t beat = header_->producer_heartbeat_ns.load(std::memory_order_relaxed);
  const uint64_t now = MonotonicNanos();
  return now > beat && (now - beat) > stale_after_ns;
}

}  // namespace framewire
