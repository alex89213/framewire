/*
 * Description: Thin owner for a POSIX shared memory region, handling shm_open,
 *   ftruncate, mmap and the matching teardown.
 * Author: Alex Wu
 * Dependencies: librt on older glibc, linked through CMake
 * Usage:
 */

#pragma once

#include <cstddef>
#include <string>

namespace framewire {

/*
 * Owns one mapped POSIX shared memory object.
 *
 * The class is move only. Copying a mapping would make two owners race to
 * munmap the same address. Setup failures throw, because a missing shm segment
 * is a startup problem and never something the frame path has to handle.
 *
 * Only the creator sets unlink_on_close, so a consumer that attaches and exits
 * leaves the segment in place for the producer that is still running.
 */
class ShmRegion {
 public:
  ShmRegion() = default;
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;
  ShmRegion(ShmRegion&& other) noexcept;
  ShmRegion& operator=(ShmRegion&& other) noexcept;
  ~ShmRegion();

  /*
   * Creates a shared memory object and maps the whole thing.
   *
   * The create is exclusive. An existing segment is a caller problem, because
   * silently removing one would destroy a ring that another producer is still
   * writing to, and that producer would carry on filling a segment nobody can
   * reach.
   *
   * Args:
   *   name: Segment name, must start with a slash.
   *   bytes: Size of the segment.
   * Returns:
   *   A mapped region that unlinks the segment on destruction.
   */
  static ShmRegion Create(const std::string& name, size_t bytes);

  /*
   * Reports whether a shared memory object exists.
   *
   * Args:
   *   name: Segment name, must start with a slash.
   * Returns:
   *   True when a segment of that name is present.
   */
  static bool Exists(const std::string& name);

  /*
   * Attaches to a shared memory object that a producer already created.
   *
   * Args:
   *   name: Segment name, must start with a slash.
   * Returns:
   *   A mapped region that leaves the segment in place on destruction.
   */
  static ShmRegion Open(const std::string& name);

  /*
   * Removes a shared memory object by name without mapping the object.
   *
   * Args:
   *   name: Segment name, must start with a slash.
   * Returns:
   *   True when a segment was removed.
   */
  static bool Remove(const std::string& name);

  void* data() const { return addr_; }
  size_t size() const { return size_; }
  const std::string& name() const { return name_; }
  bool valid() const { return addr_ != nullptr; }

  // Keeps the segment in the filesystem after this handle goes away.
  void Release() { unlink_on_close_ = false; }

 private:
  void Close();

  void* addr_ = nullptr;
  size_t size_ = 0;
  std::string name_;
  bool unlink_on_close_ = false;
};

}  // namespace framewire
