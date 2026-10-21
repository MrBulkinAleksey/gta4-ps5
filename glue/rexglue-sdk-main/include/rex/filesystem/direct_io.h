/**
 * [ps5 port] Reading past the system's file cache (O_DIRECT).
 *
 * On the PS5 everything read or written through the file cache stays in
 * memory, and while an app runs the console has little to spare: after a
 * couple of GB the cache is full and every read and write waits on the
 * console freeing memory, which it does slowly (an install fell from 85 MB/s
 * to a few). Games read past the cache, and so do these.
 *
 * A direct read is widened to 64 KiB-aligned bounds into an aligned buffer
 * (kept for the next read) and copied out. Where the kernel refuses a direct
 * transfer, the file goes on through the cache.
 */
#pragma once

#if !defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/platform.h>

namespace rex::filesystem::direct {

constexpr size_t kAlign = size_t(64) << 10;

// Past the cache only on the PS5 (elsewhere the cache is the fast path).
#if REX_PLATFORM_PS5
constexpr bool kEnabled = true;
#else
constexpr bool kEnabled = false;
#endif

inline uint8_t* AlignedAlloc(size_t bytes) {
  void* p = nullptr;
  return posix_memalign(&p, kAlign, std::max<size_t>(bytes, kAlign)) == 0 ? static_cast<uint8_t*>(p)
                                                                         : nullptr;
}

// From here on, `fd` goes through the cache.
inline void TurnOff(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags != -1) fcntl(fd, F_SETFL, flags & ~O_DIRECT);
}

inline uint64_t AlignDown(uint64_t v) { return v & ~uint64_t(kAlign - 1); }
inline uint64_t AlignUp(uint64_t v) { return (v + kAlign - 1) & ~uint64_t(kAlign - 1); }

// A file read at any offset, from several threads at once.
class Reader {
 public:
  Reader() = default;
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  ~Reader() {
    Close();
    for (auto& buffer : free_) std::free(buffer.data);
  }

  bool Open(const char* path) {
    Close();
    if (kEnabled) {
      fd_ = ::open(path, O_RDONLY | O_DIRECT);
      direct_ = fd_ >= 0;
    }
    if (fd_ < 0) fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) return false;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
      Close();
      return false;
    }
    size_ = uint64_t(st.st_size);
    return true;
  }

  void Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  int fd() const { return fd_; }
  uint64_t size() const { return size_; }
  bool direct() const { return direct_.load(std::memory_order_relaxed); }

  // Exactly `length` bytes at `offset` (false past the end or on an error).
  bool ReadAt(uint64_t offset, void* out, size_t length) {
    if (length == 0) return true;
    if (offset > size_ || length > size_ - offset) return false;
    if (direct_.load(std::memory_order_relaxed)) {
      const uint64_t start = AlignDown(offset), end = AlignUp(offset + length);
      const size_t window = size_t(end - start), need = size_t(offset + length - start);
      Buffer buffer = Take(window);
      if (buffer.data) {
        size_t got = 0;
        bool refused = false;
        while (got < need) {
          const ssize_t n = ::pread(fd_, buffer.data + got, window - got, off_t(start + got));
          if (n < 0 && errno == EINTR) continue;
          if (n < 0 && errno == EINVAL) {
            refused = true;
            break;
          }
          if (n <= 0) break;
          got += size_t(n);
        }
        const bool ok = got >= need;
        if (ok) std::memcpy(out, buffer.data + (offset - start), length);
        Give(buffer);
        if (ok) return true;
        if (!refused) return false;
        // The kernel won't read this file directly: through the cache from now on.
        if (direct_.exchange(false)) TurnOff(fd_);
      }
    }
    auto* p = static_cast<uint8_t*>(out);
    while (length) {
      const ssize_t n = ::pread(fd_, p, length, off_t(offset));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      p += n, offset += uint64_t(n), length -= size_t(n);
    }
    return true;
  }

 private:
  struct Buffer {
    uint8_t* data = nullptr;
    size_t size = 0;
  };

  Buffer Take(size_t bytes) {
    {
      std::lock_guard lock(mutex_);
      for (size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].size >= bytes) {
          const Buffer buffer = free_[i];
          free_.erase(free_.begin() + std::ptrdiff_t(i));
          return buffer;
        }
      }
    }
    const size_t size = (bytes + (size_t(1) << 20) - 1) & ~((size_t(1) << 20) - 1);  // (1 MiB steps)
    Buffer buffer{AlignedAlloc(size), size};
    if (!buffer.data) buffer.size = 0;
    return buffer;
  }

  void Give(Buffer buffer) {
    if (!buffer.data) return;
    std::lock_guard lock(mutex_);
    if (free_.size() < 8) {
      free_.push_back(buffer);
      return;
    }
    // (full: the smallest goes)
    auto smallest = std::min_element(free_.begin(), free_.end(),
                                     [](const Buffer& a, const Buffer& b) { return a.size < b.size; });
    if (smallest->size < buffer.size) std::swap(*smallest, buffer);
    std::free(buffer.data);
  }

  int fd_ = -1;
  uint64_t size_ = 0;
  std::atomic<bool> direct_{false};
  std::mutex mutex_;
  std::vector<Buffer> free_;
};

}  // namespace rex::filesystem::direct

#endif  // !_WIN32
