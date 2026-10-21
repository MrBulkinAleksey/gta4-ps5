/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <memory>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/filesystem.h>
#include <rex/memory/mapped_memory.h>
#include <rex/logging.h>
#include <rex/platform.h>

namespace rex::memory {

#if REX_PLATFORM_PS5
// [ps5 port] No file mappings on the PS5: paging a big file in through one
// (the 8 GB disc image) froze the whole console. A "mapping" here is a copy
// of the file's bytes in memory, written back on Flush/Close when writable.
namespace {

constexpr size_t kMaxFileCopy = size_t(512) << 20;

bool ReadAll(int fd, uint8_t* p, size_t length, size_t offset) {
  while (length) {
    const ssize_t n = pread(fd, p, length, off_t(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n, offset += size_t(n), length -= size_t(n);
  }
  return true;
}

bool WriteAll(int fd, const uint8_t* p, size_t length, size_t offset) {
  while (length) {
    const ssize_t n = pwrite(fd, p, length, off_t(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n, offset += size_t(n), length -= size_t(n);
  }
  return true;
}

class Ps5FileCopy : public MappedMemory {
 public:
  Ps5FileCopy(void* data, size_t size, int fd, size_t offset, bool writable)
      : MappedMemory(data, size), fd_(fd), offset_(offset), writable_(writable) {}
  ~Ps5FileCopy() override { Close(0); }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      Flush();
      std::free(data_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      if (truncate_size) ftruncate(fd_, off_t(truncate_size));
      close(fd_);
      fd_ = -1;
    }
  }

  void Flush() override {
    if (writable_ && data_ && fd_ >= 0) WriteAll(fd_, data(), size(), offset_);
  }

 private:
  int fd_;
  size_t offset_;
  bool writable_;
};

}  // namespace

std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  const bool writable = mode == Mode::kReadWrite;
  const int fd = open(path.c_str(), writable ? O_RDWR : O_RDONLY);
  if (fd < 0) return nullptr;
  size_t size = length;
  if (!size) {
    struct stat st {};
    if (fstat(fd, &st) != 0 || size_t(st.st_size) < offset) {
      close(fd);
      return nullptr;
    }
    size = size_t(st.st_size) - offset;
  }
  if (size > kMaxFileCopy) {
    REXLOG_WARN("ps5: {} is too big to load whole ({} MB)", path.string(), size >> 20);
    close(fd);
    return nullptr;
  }
  auto* data = static_cast<uint8_t*>(std::calloc(std::max<size_t>(size, 1), 1));
  if (!data) {
    close(fd);
    return nullptr;
  }
  if (!ReadAll(fd, data, size, offset) && !length) {
    std::free(data);
    close(fd);
    return nullptr;
  }
  return std::make_unique<Ps5FileCopy>(data, size, fd, offset, writable);
}
#endif  // REX_PLATFORM_PS5

class PosixMappedMemory : public MappedMemory {
 public:
  PosixMappedMemory(void* data, size_t size, int file_descriptor)
      : MappedMemory(data, size), file_descriptor_(file_descriptor) {}

  ~PosixMappedMemory() override {
    if (data_) {
      munmap(data_, size());
    }
    if (file_descriptor_ >= 0) {
      close(file_descriptor_);
    }
  }

  static std::unique_ptr<PosixMappedMemory> WrapFileDescriptor(int file_descriptor, Mode mode,
                                                               size_t offset = 0,
                                                               size_t length = 0) {
    int protection = 0;
    switch (mode) {
      case Mode::kRead:
        protection |= PROT_READ;
        break;
      case Mode::kReadWrite:
        protection |= PROT_READ | PROT_WRITE;
        break;
    }

    size_t map_length = length;
    if (!length) {
      struct stat64 file_stat;
      if (fstat64(file_descriptor, &file_stat)) {
        close(file_descriptor);
        return nullptr;
      }
      map_length = size_t(file_stat.st_size);
    }

    void* data = mmap(0, map_length, protection, MAP_SHARED, file_descriptor, offset);
    if (!data || data == MAP_FAILED) {
      close(file_descriptor);
      return nullptr;
    }

    return std::make_unique<PosixMappedMemory>(data, map_length, file_descriptor);
  }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      munmap(data_, size());
      data_ = nullptr;
    }
    if (file_descriptor_ >= 0) {
      if (truncate_size) {
        ftruncate64(file_descriptor_, off64_t(truncate_size));
      }
      close(file_descriptor_);
      file_descriptor_ = -1;
    }
  }

  void Flush() override { msync(data(), size(), MS_ASYNC); }

 private:
  int file_descriptor_;
};

#if !REX_PLATFORM_PS5
std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  int open_flags = 0;
  switch (mode) {
    case Mode::kRead:
      open_flags |= O_RDONLY;
      break;
    case Mode::kReadWrite:
      open_flags |= O_RDWR;
      break;
  }
  int file_descriptor = open(path.c_str(), open_flags);
  if (file_descriptor < 0) {
    return nullptr;
  }
  return PosixMappedMemory::WrapFileDescriptor(file_descriptor, mode, offset, length);
}
#endif  // !REX_PLATFORM_PS5

#if REX_PLATFORM_ANDROID
std::unique_ptr<MappedMemory> MappedMemory::OpenForAndroidContentUri(const std::string_view uri,
                                                                     Mode mode, size_t offset,
                                                                     size_t length) {
  const char* open_mode = nullptr;
  switch (mode) {
    case Mode::kRead:
      open_mode = "r";
      break;
    case Mode::kReadWrite:
      open_mode = "rw";
      break;
  }
  int file_descriptor = rex::filesystem::OpenAndroidContentFileDescriptor(uri, open_mode);
  if (file_descriptor < 0) {
    return nullptr;
  }
  return PosixMappedMemory::WrapFileDescriptor(file_descriptor, mode, offset, length);
}
#endif  // REX_PLATFORM_ANDROID

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size, bool low_address_space) {
  // TODO: Implement if needed
  return nullptr;
}

}  // namespace rex::memory
