#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <mutex>
#include <new>
#include <vector>

namespace rex::graphics::gta4_native {

// Pipeline and shader-state snapshots are allocated by the render worker for
// nearly every draw and released on the retirement threads. The single
// allocating thread pops from a private free list; releases push onto a
// lock-free list per size, which the allocator takes whole when its own runs
// out. (Releases used to take a mutex each, against the allocator's refills:
// on the PS5 the recorder spent a quarter of its time waiting on such locks.)
// Memory is kept for reuse until the resource dies.
class NativeSnapshotResource final : public std::pmr::memory_resource {
 public:
  ~NativeSnapshotResource() override {
    for (void* block : blocks_) ::operator delete(block, std::align_val_t(kAlignment));
  }

 private:
  static constexpr size_t kAlignment = 16;
  static constexpr size_t kChunksPerBlock = 64;
  static constexpr size_t kMaximumClasses = 8;
  struct FreeChunk {
    FreeChunk* next;
  };
  static_assert(sizeof(FreeChunk) <= kAlignment);
  struct SizeClass {
    size_t size = 0;                           // set before class_count_ publishes it
    std::vector<void*> local;                  // allocating thread only
    std::atomic<FreeChunk*> returned{nullptr};  // pushed by any thread
  };

  static size_t Rounded(size_t bytes) { return (bytes + kAlignment - 1) & ~(kAlignment - 1); }

  SizeClass* Find(size_t size) {
    const size_t count = class_count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
      if (classes_[i].size == size) return &classes_[i];
    return nullptr;
  }

  void* do_allocate(size_t bytes, size_t alignment) override {
    if (alignment > kAlignment || !bytes) return ::operator new(bytes, std::align_val_t(alignment));
    const size_t size = Rounded(bytes);
    SizeClass* size_class = Find(size);
    if (!size_class) {
      const size_t count = class_count_.load(std::memory_order_relaxed);
      if (count == kMaximumClasses) {
        return ::operator new(bytes, std::align_val_t(alignment));
      }
      size_class = &classes_[count];
      size_class->size = size;
      class_count_.store(count + 1, std::memory_order_release);
    }
    if (size_class->local.empty()) {
      for (FreeChunk* chunk = size_class->returned.exchange(nullptr, std::memory_order_acquire);
           chunk;) {
        FreeChunk* next = chunk->next;
        size_class->local.push_back(chunk);
        chunk = next;
      }
      if (size_class->local.empty()) {
        auto* block = static_cast<std::byte*>(
            ::operator new(size * kChunksPerBlock, std::align_val_t(kAlignment)));
        {
          std::lock_guard lock(mutex_);
          blocks_.push_back(block);
        }
        size_class->local.reserve(kChunksPerBlock);
        for (size_t i = 0; i < kChunksPerBlock; ++i) size_class->local.push_back(block + i * size);
      }
    }
    void* chunk = size_class->local.back();
    size_class->local.pop_back();
    return chunk;
  }

  void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
    if (alignment > kAlignment || !bytes) {
      ::operator delete(pointer, std::align_val_t(alignment));
      return;
    }
    SizeClass* size_class = Find(Rounded(bytes));
    if (!size_class) {
      // Allocated after the class table filled up.
      ::operator delete(pointer, std::align_val_t(alignment));
      return;
    }
    auto* chunk = static_cast<FreeChunk*>(pointer);
    FreeChunk* head = size_class->returned.load(std::memory_order_relaxed);
    do {
      chunk->next = head;
    } while (!size_class->returned.compare_exchange_weak(head, chunk, std::memory_order_release,
                                                         std::memory_order_relaxed));
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::mutex mutex_;  // blocks_
  SizeClass classes_[kMaximumClasses];
  std::atomic<size_t> class_count_{0};
  std::vector<void*> blocks_;
};

}  // namespace rex::graphics::gta4_native
