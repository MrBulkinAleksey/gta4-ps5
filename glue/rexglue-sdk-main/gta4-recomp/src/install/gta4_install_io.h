// The installer's file reading and writing: positional reads, big sequential
// writes, and a few threads to copy files side by side.
//
// [ps5 port] What made installing on the PS5 crawl:
//  - C++ file streams hand the file 4 KB at a time, and each small write is
//    slow there. Here a write goes out 8 MB at a time.
//  - Everything read or written through the file cache stays in memory, and
//    while an app runs the console has little to spare: after about 2 GB the
//    cache is full and every read and write waits on the console freeing
//    memory (85 MB/s fell to a few). So reads and writes go past the cache
//    (O_DIRECT, rex/filesystem/direct_io.h), as games do: data gathers in an
//    aligned buffer and goes out in aligned 8 MB pieces.
//  - The written data is flushed as it goes (each file every 16 MB, small
//    files every 64 MB), from the writing thread.
//  - Files are copied three at a time, each thread with buffers made once
//    (a big allocation per file wears the console's heap down).
// The time spent reading, writing and flushing is counted for the log and
// the install screen.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/platform.h>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/filesystem/direct_io.h>
#endif

namespace gta4::install::io {

// Bytes and time spent reading, writing and flushing (since the app started).
struct Stats {
  std::atomic<uint64_t> read_bytes{0}, read_ns{0}, write_bytes{0}, write_ns{0}, sync_ns{0}, syncs{0};
  std::atomic<uint64_t> direct_files{0}, cached_files{0}, reserve_ns{0};
};

// How the installer writes, for the drive it writes to (set before an install).
//  - preallocate: each file is made its full size before it's written. On an
//    exFAT drive a file that grows write by write gets slower the bigger it
//    gets (its cluster chain is extended again and again: 6 MB/s falling to
//    1); a file made full size first is written straight through.
//  - direct: writes go past the file cache (an internal UFS drive).
//  - threads: files copied at once (one on an external drive).
struct WritePolicy {
  bool preallocate = false;
  uint64_t reserve_step = 0;  // made bigger this much ahead of the writing (0: all at once)
  bool direct = true;
  int threads = 3;
};
inline WritePolicy& Policy() {
  static WritePolicy policy;
  return policy;
}

// The file being written now (the last one opened), for the install screen.
namespace detail {
struct CurrentFile {
  std::mutex mutex;
  std::string path;
  uint64_t size = 0;
};
inline CurrentFile& Now() {
  static CurrentFile current;
  return current;
}
}  // namespace detail
inline void SetCurrent(const std::filesystem::path& path, uint64_t size) {
  std::lock_guard lock(detail::Now().mutex);
  detail::Now().path = path.string();
  detail::Now().size = size;
}
inline void Current(std::string& path, uint64_t& size) {
  std::lock_guard lock(detail::Now().mutex);
  path = detail::Now().path;
  size = detail::Now().size;
}
inline Stats& GetStats() {
  static Stats stats;
  return stats;
}

namespace detail {
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

#if REX_PLATFORM_PS5
constexpr bool kFlushAsWeGo = true;
#else
constexpr bool kFlushAsWeGo = false;
#endif
// A file's own data is flushed every this many bytes, and at its end when
// it's at least kFlushFileMin; smaller files add up toward kFlushSmall.
constexpr uint64_t kFlushEvery = 16ull << 20;
constexpr uint64_t kFlushFileMin = 1ull << 20;
constexpr uint64_t kFlushSmall = 64ull << 20;

inline std::atomic<uint64_t>& SmallUnflushed() {
  static std::atomic<uint64_t> bytes{0};
  return bytes;
}
}  // namespace detail

// Memory aligned for direct transfers (64 KiB), grown as needed and kept.
class AlignedBuffer {
 public:
  static constexpr size_t kAlign = size_t(64) << 10;

  AlignedBuffer() = default;
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  ~AlignedBuffer() { Free(); }

  // At least `bytes` (false if that much couldn't be had).
  bool Reserve(size_t bytes) {
    if (bytes <= size_) return true;
    Free();
#if defined(_WIN32)
    data_ = static_cast<uint8_t*>(_aligned_malloc(bytes, kAlign));
#else
    void* p = nullptr;
    data_ = posix_memalign(&p, kAlign, bytes) == 0 ? static_cast<uint8_t*>(p) : nullptr;
#endif
    size_ = data_ ? bytes : 0;
    return data_ != nullptr;
  }
  uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  void Free() {
#if defined(_WIN32)
    _aligned_free(data_);
#else
    std::free(data_);
#endif
    data_ = nullptr;
    size_ = 0;
  }
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// A file read at any offset (safe from several threads at once).
class InFile {
 public:
  InFile() = default;
  InFile(const InFile&) = delete;
  InFile& operator=(const InFile&) = delete;
  ~InFile() { Close(); }

  bool Open(const std::filesystem::path& path) {
    Close();
#if defined(_WIN32)
    file_ = _wfopen(path.c_str(), L"rb");
    if (!file_) return false;
    _fseeki64(file_, 0, SEEK_END);
    size_ = uint64_t(_ftelli64(file_));
    return true;
#else
    if (!reader_.Open(path.c_str())) return false;
    size_ = reader_.size();
    return true;
#endif
  }

  uint64_t size() const { return size_; }

  // Exactly `length` bytes at `offset` (false short of that).
  bool ReadAt(uint64_t offset, void* out, size_t length) {
    const uint64_t start = detail::NowNs();
#if defined(_WIN32)
    bool ok = false;
    {
      std::lock_guard lock(win_mutex_);
      ok = file_ && _fseeki64(file_, int64_t(offset), SEEK_SET) == 0 &&
           std::fread(out, 1, length, file_) == length;
    }
#else
    const bool ok = reader_.ReadAt(offset, out, length);
#endif
    if (ok) GetStats().read_bytes += length;
    GetStats().read_ns += detail::NowNs() - start;
    return ok;
  }

  void Close() {
#if defined(_WIN32)
    if (file_) std::fclose(file_), file_ = nullptr;
#else
    reader_.Close();
#endif
  }

 private:
#if defined(_WIN32)
  FILE* file_ = nullptr;
  std::mutex win_mutex_;
#else
  rex::filesystem::direct::Reader reader_;
#endif
  uint64_t size_ = 0;
};

// A new file written from start to end. What's written gathers in an aligned
// buffer (the caller's, kept between files) and goes out 8 MB at a time.
class OutFile {
 public:
  static constexpr size_t kChunk = size_t(8) << 20;

  OutFile() = default;
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  ~OutFile() {
    if (open_) Close();
  }

  // `gather`: the buffer (null: one of its own). `size`: what the file will
  // be (0: not known), so it can be made that size first (Policy()).
  bool Open(const std::filesystem::path& path, AlignedBuffer* gather = nullptr, uint64_t size = 0) {
    gather_ = gather ? gather : &own_;
    if (!gather_->Reserve(kChunk)) return false;
#if defined(_WIN32)
    file_ = _wfopen(path.c_str(), L"wb");
    open_ = file_ != nullptr;
#else
    direct_ = false;
    if (rex::filesystem::direct::kEnabled && Policy().direct) {
      fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0666);
      direct_ = fd_ >= 0;
    }
    if (fd_ < 0) fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    open_ = fd_ >= 0;
    reserved_ = 0;
    size_ = size;
    if (open_ && size && Policy().preallocate) ReserveFor(0);
#endif
    if (open_) SetCurrent(path, size);
    ok_ = open_;
    pending_ = 0;
    written_ = unflushed_ = 0;
    return open_;
  }

  bool Write(const void* data, size_t length) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (ok_ && length) {
      const size_t take = std::min(length, kChunk - pending_);
      std::memcpy(gather_->data() + pending_, p, take);
      pending_ += take, p += take, length -= take;
      if (pending_ == kChunk) Flush();
    }
    return ok_;
  }

  // Writes what's waiting and closes the file; false if any write failed.
  bool Close() {
    if (!open_) return ok_;
    Flush();
#if !defined(_WIN32)
    (direct_ ? GetStats().direct_files : GetStats().cached_files)++;
    if (detail::kFlushAsWeGo && ok_ && unflushed_) {
      if (written_ >= detail::kFlushFileMin) {
        Sync();
      } else if ((detail::SmallUnflushed() += unflushed_) >= detail::kFlushSmall) {
        // the small files' data, all at once
        detail::SmallUnflushed() = 0;
        const uint64_t start = detail::NowNs();
        ::sync();
        GetStats().sync_ns += detail::NowNs() - start;
        ++GetStats().syncs;
      }
    }
    // (made bigger than what was written: back to what was written)
    if (reserved_ && reserved_ != written_ && ::ftruncate(fd_, off_t(written_)) != 0) ok_ = false;
    if (::close(fd_) != 0) ok_ = false;
    fd_ = -1;
#else
    if (std::fclose(file_) != 0) ok_ = false;
    file_ = nullptr;
#endif
    open_ = false;
    return ok_;
  }

 private:
  void Flush() {
    if (pending_ && ok_) Out(gather_->data(), pending_);
    pending_ = 0;
  }

  void Out(const uint8_t* p, size_t length) {
#if !defined(_WIN32)
    // (made big enough first, when the policy says so)
    if (size_ && Policy().preallocate && written_ + length > reserved_ && reserved_ < size_) {
      ReserveFor(written_ + length);
    }
#endif
    const uint64_t start = detail::NowNs();
    size_t left = length;
#if defined(_WIN32)
    left -= std::fwrite(p, 1, left, file_);
#else
    while (left) {
      const ssize_t n = ::write(fd_, p, left);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EINVAL && direct_) {
        // the kernel won't write this directly (the last piece's size):
        // through the cache from here
        rex::filesystem::direct::TurnOff(fd_);
        direct_ = false;
        continue;
      }
      if (n <= 0) break;
      p += n, left -= size_t(n);
    }
#endif
    GetStats().write_bytes += length - left;
    GetStats().write_ns += detail::NowNs() - start;
    if (left) {
      ok_ = false;
      return;
    }
    written_ += length;
    unflushed_ += length;
#if !defined(_WIN32)
    if (detail::kFlushAsWeGo && unflushed_ >= detail::kFlushEvery) Sync();
#endif
  }

#if !defined(_WIN32)
  // The file made bigger ahead of the writing (Policy()): up to its full size,
  // or one step past what's written.
  void ReserveFor(uint64_t need) {
    const uint64_t step = Policy().reserve_step;
    const uint64_t target = step ? std::min(size_, std::max(need, written_) + step) : size_;
    if (target <= reserved_) return;
    const uint64_t start = detail::NowNs();
    if (::ftruncate(fd_, off_t(target)) == 0) reserved_ = target;
    GetStats().reserve_ns += detail::NowNs() - start;
  }

  void Sync() {
    const uint64_t start = detail::NowNs();
    ::fsync(fd_);
    GetStats().sync_ns += detail::NowNs() - start;
    ++GetStats().syncs;
    unflushed_ = 0;
  }
#endif

#if defined(_WIN32)
  FILE* file_ = nullptr;
#else
  int fd_ = -1;
  bool direct_ = false;
  uint64_t reserved_ = 0, size_ = 0;
#endif
  bool open_ = false, ok_ = false;
  AlignedBuffer* gather_ = nullptr;
  AlignedBuffer own_;
  size_t pending_ = 0;
  uint64_t written_ = 0, unflushed_ = 0;
};

// A few threads running jobs from a queue. Each thread has buffers of its
// own (Scratch), made the first time a job needs them and kept. The first
// job that fails stops the rest; Wait() reports its error.
class WorkerPool {
 public:
  struct Scratch {
    AlignedBuffer read, write, gather;
  };
  using Job = std::function<bool(Scratch&, std::string& error)>;

  explicit WorkerPool(int threads) {
    for (int i = 0; i < std::max(1, threads); ++i) threads_.emplace_back([this] { Run(); });
  }
  ~WorkerPool() {
    {
      std::lock_guard lock(mutex_);
      quit_ = true;
    }
    work_.notify_all();
    for (auto& thread : threads_) thread.join();
  }
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Queues a job (waiting while the queue is long). False once one has failed.
  bool Add(Job job) {
    std::unique_lock lock(mutex_);
    room_.wait(lock, [this] { return queue_.size() < kMaxQueued || failed_; });
    if (failed_) return false;
    queue_.push_back(std::move(job));
    work_.notify_one();
    return true;
  }

  // Waits for every queued job. False, with the first error, if one failed.
  bool Wait(std::string& error) {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && busy_ == 0; });
    if (failed_) {
      error = error_;
      return false;
    }
    return true;
  }

 private:
  static constexpr size_t kMaxQueued = 256;

  void Run() {
    Scratch scratch;
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        work_.wait(lock, [this] { return quit_ || !queue_.empty(); });
        if (queue_.empty()) return;
        job = std::move(queue_.front());
        queue_.pop_front();
        ++busy_;
        room_.notify_one();
      }
      std::string error;
      bool ok = false;
      try {
        ok = job(scratch, error);
      } catch (const std::exception& exception) {
        error = exception.what();
      } catch (...) {
        error = "A file could not be installed.";
      }
      {
        std::lock_guard lock(mutex_);
        --busy_;
        if (!ok && !failed_) {
          failed_ = true;
          error_ = error.empty() ? "A file could not be installed." : std::move(error);
          queue_.clear();
          room_.notify_all();
        }
        if (queue_.empty() && busy_ == 0) idle_.notify_all();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable work_, room_, idle_;
  std::deque<Job> queue_;
  std::vector<std::thread> threads_;
  int busy_ = 0;
  bool quit_ = false, failed_ = false;
  std::string error_;
};

// How many files are copied at once (for the drive written to).
inline int CopyThreads() { return std::max(1, Policy().threads); }

}  // namespace gta4::install::io
