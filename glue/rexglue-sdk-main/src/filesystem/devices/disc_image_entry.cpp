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

#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/devices/disc_image_file.h>

#include <algorithm>
#include <vector>

#include <rex/filesystem/devices/disc_image_device.h>

#include <rex/math.h>

namespace rex::filesystem {

DiscImageEntry::DiscImageEntry(Device* device, Entry* parent, const std::string_view path,
                               memory::MappedMemory* mmap)
    : Entry(device, parent, path), mmap_(mmap), data_offset_(0), data_size_(0) {}

DiscImageEntry::~DiscImageEntry() = default;

std::unique_ptr<DiscImageEntry> DiscImageEntry::Create(Device* device, Entry* parent,
                                                       const std::string_view name,
                                                       memory::MappedMemory* mmap) {
  auto path = rex::string::utf8_join_guest_paths(parent->path(), name);
  auto entry = std::make_unique<DiscImageEntry>(device, parent, path, mmap);
  return std::move(entry);
}

X_STATUS DiscImageEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new DiscImageFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

std::unique_ptr<memory::MappedMemory> DiscImageEntry::OpenMapped(memory::MappedMemory::Mode mode,
                                                                 size_t offset, size_t length) {
  if (mode != memory::MappedMemory::Mode::kRead) {
    // Only allow reads.
    return nullptr;
  }

  size_t real_offset = data_offset_ + offset;
  size_t real_length = length ? std::min(length, data_size_) : data_size_;
  if (mmap_) {
    return mmap_->Slice(real_offset, real_length);
  }
  // [ps5 port] No mapping of the image: the bytes, read into memory.
  class Copy final : public memory::MappedMemory {
   public:
    explicit Copy(size_t n) : bytes_(n) {
      data_ = bytes_.data();
      size_ = n;
    }

   private:
    std::vector<uint8_t> bytes_;
  };
  if (offset > data_size_) {
    return nullptr;
  }
  real_length = std::min(real_length, data_size_ - offset);
  auto copy = std::make_unique<Copy>(real_length);
  if (!static_cast<DiscImageDevice*>(device())->ReadAt(real_offset, copy->data(), real_length)) {
    return nullptr;
  }
  return copy;
}

}  // namespace rex::filesystem
