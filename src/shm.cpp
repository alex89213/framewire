/*
 * Description: POSIX shared memory helper, wrapping shm_open, ftruncate, mmap
 *   and teardown behind a move only owner.
 * Author: Alex Wu
 * Dependencies: framewire/shm.h
 * Usage:
 */

#include "framewire/shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace framewire {
namespace {

// Builds an error message that keeps the failing call, the name and errno.
std::runtime_error ShmError(const char* what, const std::string& name) {
  return std::runtime_error(std::string(what) + " failed for shm '" + name +
                            "': " + std::strerror(errno));
}

void CheckName(const std::string& name) {
  if (name.empty() || name[0] != '/' || name.find('/', 1) != std::string::npos) {
    throw std::runtime_error("shm name must be a leading slash followed by no other slash: " +
                             name);
  }
}

}  // namespace

ShmRegion::ShmRegion(ShmRegion&& other) noexcept
    : addr_(other.addr_),
      size_(other.size_),
      name_(std::move(other.name_)),
      unlink_on_close_(other.unlink_on_close_) {
  other.addr_ = nullptr;
  other.size_ = 0;
  other.unlink_on_close_ = false;
}

ShmRegion& ShmRegion::operator=(ShmRegion&& other) noexcept {
  if (this != &other) {
    Close();
    addr_ = other.addr_;
    size_ = other.size_;
    name_ = std::move(other.name_);
    unlink_on_close_ = other.unlink_on_close_;
    other.addr_ = nullptr;
    other.size_ = 0;
    other.unlink_on_close_ = false;
  }
  return *this;
}

ShmRegion::~ShmRegion() { Close(); }

void ShmRegion::Close() {
  if (addr_ != nullptr) {
    munmap(addr_, size_);
    addr_ = nullptr;
  }
  if (unlink_on_close_ && !name_.empty()) {
    shm_unlink(name_.c_str());
    unlink_on_close_ = false;
  }
  size_ = 0;
}

ShmRegion ShmRegion::Create(const std::string& name, size_t bytes) {
  CheckName(name);
  if (bytes == 0) throw std::runtime_error("refusing to create a zero byte shm segment");

  // O_EXCL on purpose. removing an existing segment here would pull the ring
  // out from under a producer that is still writing to it, and that producer
  // would keep reporting success while filling a segment nobody can reach
  const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    if (errno == EEXIST) {
      throw std::runtime_error("shm segment '" + name + "' already exists");
    }
    throw ShmError("shm_open", name);
  }

  ShmRegion region;
  region.name_ = name;
  region.unlink_on_close_ = true;

  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    const auto err = ShmError("ftruncate", name);
    close(fd);
    shm_unlink(name.c_str());
    region.unlink_on_close_ = false;
    throw err;
  }

  void* addr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  // the mapping keeps the segment alive, so the descriptor is not needed past
  // this point and holding the descriptor open would just leak on long runs
  close(fd);

  if (addr == MAP_FAILED) {
    const auto err = ShmError("mmap", name);
    shm_unlink(name.c_str());
    region.unlink_on_close_ = false;
    throw err;
  }

  region.addr_ = addr;
  region.size_ = bytes;
  return region;
}

ShmRegion ShmRegion::Open(const std::string& name) {
  CheckName(name);

  const int fd = shm_open(name.c_str(), O_RDWR, 0);
  if (fd < 0) throw ShmError("shm_open", name);

  struct stat st {};
  if (fstat(fd, &st) != 0) {
    const auto err = ShmError("fstat", name);
    close(fd);
    throw err;
  }
  const auto bytes = static_cast<size_t>(st.st_size);
  if (bytes == 0) {
    close(fd);
    throw std::runtime_error("shm segment '" + name + "' has zero size, producer may still be starting");
  }

  void* addr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (addr == MAP_FAILED) throw ShmError("mmap", name);

  ShmRegion region;
  region.addr_ = addr;
  region.size_ = bytes;
  region.name_ = name;
  // the attaching side never owns the segment lifetime, the creator does
  region.unlink_on_close_ = false;
  return region;
}

bool ShmRegion::Exists(const std::string& name) {
  CheckName(name);
  const int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) return false;
  close(fd);
  return true;
}

bool ShmRegion::Remove(const std::string& name) {
  CheckName(name);
  return shm_unlink(name.c_str()) == 0;
}

}  // namespace framewire
