#ifndef REX_GRAPHICS_GTA4_NATIVE_STATEFUL_CONSTANT_STATE_H_
#define REX_GRAPHICS_GTA4_NATIVE_STATEFUL_CONSTANT_STATE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "dirty_state_delta.h"
#include "native_snapshot_resource.h"

namespace rex::graphics::gta4_native {

// Payload offsets refer to bytes owned by ConstantPayloadDelta. Destination
// offsets refer to the canonical guest-endian constant block. Commands own
// these bytes, so guest memory may change immediately after queue submission.
struct ConstantDeltaRange {
  uint32_t destination_offset = 0;
  uint32_t payload_offset = 0;
  uint32_t byte_count = 0;

  constexpr bool operator==(const ConstantDeltaRange&) const = default;
};

// Constant payloads larger than a recycled command keeps (full snapshots for
// lighting draws, bone palettes) were freed when their command was recycled
// and allocated again for the next one: 4 KB heap round trips on the game's
// render thread. A bounded number of them wait here instead. Put runs where
// commands are recycled, Take on the producer; a spin lock covers both.
// [ps5 port] Kept by power-of-two capacity class, a request taking from the
// smallest class that fits: with one LIFO list, a small buffer on top (a
// DrawPrimitiveUP's vertex data) sent every larger request to the heap.
class LargeConstantPayloadStash {
 public:
  static constexpr size_t kMaximumBuffers = 1024;  // per class
  static constexpr size_t kMaximumBytes = 16384;
  static constexpr size_t kMinimumBytes = 64;
  static constexpr size_t kMaximumStashedBytes = kMaximumBuffers * kMaximumBytes;
  static void Put(std::vector<uint8_t>&& buffer) {
    if (buffer.capacity() > kMaximumBytes || buffer.capacity() < kMinimumBytes) return;
    buffer.clear();
    Stash& stash = Get();
    Lock(stash);
    Keep(stash, buffer);
    stash.lock.clear(std::memory_order_release);
  }
  // Gives `buffer` (when it is too small) a stashed buffer of at least `bytes`.
  static void Reserve(std::vector<uint8_t>& buffer, size_t bytes) {
    if (buffer.capacity() >= bytes) return;
    if (bytes <= kMaximumBytes) {
      Stash& stash = Get();
      Lock(stash);
      for (size_t index = RequestClass(bytes); index < kClassCount; ++index) {
        auto& list = stash.classes[index];
        if (list.empty()) continue;
        std::vector<uint8_t> taken = std::move(list.back());
        list.pop_back();
        stash.bytes -= taken.capacity();
        buffer.swap(taken);
        buffer.clear();
        // The smaller buffer it replaces waits for a smaller request.
        if (taken.capacity() >= kMinimumBytes) {
          taken.clear();
          Keep(stash, taken);
        }
        stash.lock.clear(std::memory_order_release);
        return;
      }
      stash.lock.clear(std::memory_order_release);
      // A whole class: the buffer also fits a slightly larger next request.
      buffer.reserve(std::bit_ceil(std::max(bytes, kMinimumBytes)));
      return;
    }
    buffer.reserve(bytes);
  }

 private:
  // Class k holds capacities [64 << k, 128 << k); 64 << 8 is kMaximumBytes.
  static constexpr size_t kClassCount = 9;
  static_assert((kMinimumBytes << (kClassCount - 1)) == kMaximumBytes);
  static size_t CapacityClass(size_t capacity) {
    return size_t(std::bit_width(capacity / kMinimumBytes)) - 1;
  }
  static size_t RequestClass(size_t bytes) {
    return CapacityClass(std::bit_ceil(std::max(bytes, kMinimumBytes)));
  }
  struct Stash {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::array<std::vector<std::vector<uint8_t>>, kClassCount> classes;
    size_t bytes = 0;
    Stash() {
      for (auto& list : classes) list.reserve(kMaximumBuffers);
    }
  };
  // Lock held; `buffer` is empty, with kMinimumBytes..kMaximumBytes capacity.
  static void Keep(Stash& stash, std::vector<uint8_t>& buffer) {
    auto& list = stash.classes[CapacityClass(buffer.capacity())];
    if (list.size() < kMaximumBuffers &&
        stash.bytes + buffer.capacity() <= kMaximumStashedBytes) {
      stash.bytes += buffer.capacity();
      list.push_back(std::move(buffer));
    }
  }
  static Stash& Get() {
    static Stash stash;
    return stash;
  }
  static void Lock(Stash& stash) {
    while (stash.lock.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__)
      __builtin_ia32_pause();
#endif
    }
  }
};

struct ConstantPayloadDelta {
  std::vector<ConstantDeltaRange> ranges;
  std::vector<uint8_t> payload;
  bool complete_snapshot = false;

  bool empty() const { return ranges.empty(); }
};

inline bool ValidateConstantPayloadDelta(const ConstantPayloadDelta& delta, size_t block_size,
                                         bool require_complete_snapshot = false) {
  if (require_complete_snapshot && !delta.complete_snapshot) {
    return false;
  }
  if (delta.complete_snapshot &&
      (delta.ranges.size() != 1 || delta.ranges[0].destination_offset != 0 ||
       delta.ranges[0].payload_offset != 0 || delta.ranges[0].byte_count != block_size ||
       delta.payload.size() != block_size)) {
    return false;
  }
  size_t previous_end = 0;
  bool first = true;
  for (const ConstantDeltaRange& range : delta.ranges) {
    if (!range.byte_count || range.destination_offset > block_size ||
        range.byte_count > block_size - range.destination_offset ||
        range.payload_offset > delta.payload.size() ||
        range.byte_count > delta.payload.size() - range.payload_offset) {
      return false;
    }
    if (!first && range.destination_offset < previous_end) {
      return false;
    }
    previous_end = size_t(range.destination_offset) + range.byte_count;
    first = false;
  }
  return true;
}

inline bool CaptureConstantPayloadDelta(std::span<const uint8_t> source,
                                        const DirtyRangeSet& dirty_ranges,
                                        uint32_t bytes_per_element,
                                        ConstantPayloadDelta& result) {
  // Cleared, not reassigned: recycled commands keep these buffers' capacity.
  result.ranges.clear();
  result.payload.clear();
  result.complete_snapshot = false;
  if (!bytes_per_element) {
    return false;
  }
  // One allocation per vector: payload growth by insert reallocated several
  // times per draw on the producer thread.
  {
    uint64_t total = 0;
    for (const DirtyElementRange& dirty : dirty_ranges.ranges) total += uint64_t(dirty.count) * bytes_per_element;
    if (total <= source.size()) LargeConstantPayloadStash::Reserve(result.payload, size_t(total));
    result.ranges.reserve(dirty_ranges.ranges.size());
  }
  for (const DirtyElementRange& dirty : dirty_ranges.ranges) {
    if (!dirty.count || dirty.first > std::numeric_limits<uint32_t>::max() / bytes_per_element ||
        dirty.count > std::numeric_limits<uint32_t>::max() / bytes_per_element) {
      result = {};
      return false;
    }
    const uint32_t destination_offset = dirty.first * bytes_per_element;
    const uint32_t byte_count = dirty.count * bytes_per_element;
    if (destination_offset > source.size() || byte_count > source.size() - destination_offset ||
        result.payload.size() > std::numeric_limits<uint32_t>::max() - byte_count) {
      result = {};
      return false;
    }
    const uint32_t payload_offset = static_cast<uint32_t>(result.payload.size());
    // [ps5 port] Ranges must ascend. With this, the checks above make the delta
    // valid by construction: ValidateConstantPayloadDelta is not run again here
    // (the render worker still validates it before applying).
    if (!result.ranges.empty() &&
        destination_offset < uint64_t(result.ranges.back().destination_offset) +
                                 result.ranges.back().byte_count) {
      return false;
    }
    const bool coalesce =
        !result.ranges.empty() &&
        result.ranges.back().destination_offset <=
            std::numeric_limits<uint32_t>::max() - result.ranges.back().byte_count &&
        result.ranges.back().destination_offset + result.ranges.back().byte_count ==
            destination_offset &&
        result.ranges.back().payload_offset <=
            std::numeric_limits<uint32_t>::max() - result.ranges.back().byte_count &&
        result.ranges.back().payload_offset + result.ranges.back().byte_count == payload_offset &&
        result.ranges.back().byte_count <= std::numeric_limits<uint32_t>::max() - byte_count;
    if (coalesce) {
      result.ranges.back().byte_count += byte_count;
    } else {
      result.ranges.push_back({destination_offset, payload_offset, byte_count});
    }
    result.payload.insert(result.payload.end(), source.begin() + destination_offset,
                          source.begin() + destination_offset + byte_count);
  }
  return true;
}

inline bool CaptureCompleteConstantSnapshot(std::span<const uint8_t> source,
                                            ConstantPayloadDelta& result) {
  if (source.size() > std::numeric_limits<uint32_t>::max()) {
    result = {};
    return false;
  }
  result.ranges = {{0, 0, static_cast<uint32_t>(source.size())}};
  LargeConstantPayloadStash::Reserve(result.payload, source.size());
  result.payload.assign(source.begin(), source.end());
  result.complete_snapshot = true;
  // [ps5 port] One range over the whole block: valid unless the block is empty.
  return !source.empty();
}

struct ConstantStateVersion {
  StateVersionStamp token{};
  uint64_t content_hash = 0;
  size_t byte_size = 0;
  ConstantPayloadDelta delta;
  uint32_t deferred_ancestor_count = 0;
  // The parent and materialized bytes are render-worker-owned memoization.
  // State bytes and token are immutable once the version is published.
  mutable std::shared_ptr<const ConstantStateVersion> parent;
  mutable std::shared_ptr<const std::vector<uint8_t>> materialized;
  // Valid prefix of materialized when the render worker snapshots only what
  // the consuming draws read (the buffer is always byte_size long). Render
  // worker only; while the version is current, the canonical bank equals its
  // content, so the prefix can still be extended for a later draw. 0: all.
  mutable size_t materialized_bytes = 0;
};

// Versions and snapshot control blocks are allocated for nearly every draw
// that changes constants and released on retirement threads. Only the render
// worker (the owner of every AuthoritativeConstantState) allocates from this
// resource. Never destroyed: versions may outlive any static teardown order.
inline NativeSnapshotResource& ConstantVersionResource() {
  static auto* resource = new NativeSnapshotResource();
  return *resource;
}

inline std::shared_ptr<ConstantStateVersion> MakeConstantStateVersion() {
  return std::allocate_shared<ConstantStateVersion>(
      std::pmr::polymorphic_allocator<ConstantStateVersion>(&ConstantVersionResource()));
}

enum class ConstantApplyStatus : uint8_t {
  kApplied,
  kInvalidDelta,
  kNeedsCompleteSnapshot,
  kVersionSpaceExhausted,
};

struct ConstantApplyResult {
  ConstantApplyStatus status = ConstantApplyStatus::kApplied;
  bool changed = false;
  std::shared_ptr<const ConstantStateVersion> version;

  explicit operator bool() const { return status == ConstantApplyStatus::kApplied; }
};

// One instance is owned by the render worker for each device and shader
// stage. Published versions retain only copied deltas. A contiguous host view
// is reconstructed lazily on first consumer use and then memoized.
// Recycles the full-bank snapshot buffers taken for every draw that changes
// constants (two 3.5-4 KB heap blocks per draw otherwise). Releases come from
// any thread (command retirement is asynchronous). They push onto a lock-free
// list: a mutex shared with the allocating render worker, taken twice a draw
// on both sides, kept the PS5's recorder waiting on it.
class ConstantSnapshotPool {
  struct Node {
    std::vector<uint8_t> bytes;
    Node* next = nullptr;
  };
  struct SizeList {
    size_t size = 0;                        // set once, before count publishes it
    std::atomic<Node*> returned{nullptr};   // pushed by any thread
    std::atomic<uint32_t> returned_count{0};
    std::vector<Node*> local;               // under State::allocate
  };
  static constexpr size_t kMaximumLists = 16;
  static constexpr uint32_t kMaximumReturned = 8192;
  struct State {
    std::mutex allocate;  // Copy's side only (the render worker): uncontended
    std::array<SizeList, kMaximumLists> lists;
    std::atomic<size_t> count{0};
  };
  static State& state() {
    static auto* instance = new State();  // never destroyed: buffers outlive statics
    return *instance;
  }
  static SizeList* Find(State& st, size_t size) {
    const size_t count = st.count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
      if (st.lists[i].size == size) return &st.lists[i];
    return nullptr;
  }
  static void Release(Node* node) {
    SizeList* list = Find(state(), node->bytes.size());
    if (!list || list->returned_count.fetch_add(1, std::memory_order_relaxed) >= kMaximumReturned) {
      if (list) list->returned_count.fetch_sub(1, std::memory_order_relaxed);
      delete node;
      return;
    }
    Node* head = list->returned.load(std::memory_order_relaxed);
    do {
      node->next = head;
    } while (!list->returned.compare_exchange_weak(head, node, std::memory_order_release,
                                                   std::memory_order_relaxed));
  }

 public:
  static std::shared_ptr<const std::vector<uint8_t>> Copy(const std::vector<uint8_t>& source,
                                                          size_t copy_bytes = SIZE_MAX) {
    copy_bytes = std::min(copy_bytes, source.size());
    State& st = state();
    Node* node = nullptr;
    {
      std::lock_guard lock(st.allocate);
      SizeList* list = Find(st, source.size());
      if (!list) {
        const size_t count = st.count.load(std::memory_order_relaxed);
        if (count < kMaximumLists) {
          list = &st.lists[count];
          list->size = source.size();
          st.count.store(count + 1, std::memory_order_release);
        }
      }
      if (list) {
        if (list->local.empty()) {
          Node* taken = list->returned.exchange(nullptr, std::memory_order_acquire);
          uint32_t taken_count = 0;
          for (; taken; ++taken_count) {
            Node* next = taken->next;
            list->local.push_back(taken);
            taken = next;
          }
          if (taken_count) list->returned_count.fetch_sub(taken_count, std::memory_order_relaxed);
        }
        if (!list->local.empty()) {
          node = list->local.back();
          list->local.pop_back();
        }
      }
    }
    if (node) {
      std::memcpy(node->bytes.data(), source.data(), copy_bytes);
    } else if (copy_bytes == source.size()) {
      node = new Node{source};
    } else {
      node = new Node{std::vector<uint8_t>(source.size())};
      std::memcpy(node->bytes.data(), source.data(), copy_bytes);
    }
    return std::shared_ptr<const std::vector<uint8_t>>(
        &node->bytes, [node](const std::vector<uint8_t>*) { Release(node); },
        std::pmr::polymorphic_allocator<std::byte>(&ConstantVersionResource()));
  }
};

class AuthoritativeConstantState {
 public:
  static constexpr uint32_t kMaximumDeferredAncestors = 64;
  explicit AuthoritativeConstantState(size_t byte_size = 0,
                                      StateVersionStamp initial_version = {})
      : canonical_(byte_size), version_(initial_version) {}

  size_t byte_size() const { return canonical_.size(); }
  bool initialized() const { return initialized_; }
  const std::vector<uint8_t>& canonical() const { return canonical_; }
  const std::shared_ptr<const ConstantStateVersion>& current_version() const { return current_; }

  // Draw capture owns the authoritative state before later commands mutate it.
  // Copy that complete state once here, rather than reconstructing a chain of
  // deltas on the frame's Vulkan recording critical path. Unconsumed updates
  // stay deferred and allocate no full-size snapshot.
  // bytes: the prefix later consumers read (all of it by default).
  const std::shared_ptr<const ConstantStateVersion>& SnapshotCurrentVersion(
      size_t bytes = SIZE_MAX) const {
    if (!current_) return current_;
    bytes = std::min(bytes, canonical_.size());
    if (!current_->materialized) {
      current_->materialized = ConstantSnapshotPool::Copy(canonical_, bytes);
      current_->materialized_bytes = bytes;
      current_->parent.reset();
    } else if (current_->materialized_bytes && current_->materialized_bytes < bytes) {
      // Bytes past the old prefix are read by no earlier consumer.
      auto* data = const_cast<uint8_t*>(current_->materialized->data());
      std::memcpy(data + current_->materialized_bytes,
                  canonical_.data() + current_->materialized_bytes,
                  bytes - current_->materialized_bytes);
      current_->materialized_bytes = bytes;
    }
    return current_;
  }


  // consume: a draw takes the new state at once. The version then holds the
  // complete bytes and no delta, so nothing is copied twice or allocated for
  // a replay chain that would never be used.
  template <typename HashCallback>
  ConstantApplyResult Apply(const ConstantPayloadDelta& delta, HashCallback&& hash_callback,
                            bool consume = false) {
    if (!ValidateConstantPayloadDelta(delta, canonical_.size())) {
      return {ConstantApplyStatus::kInvalidDelta, false, current_};
    }
    if (!initialized_ && !delta.complete_snapshot) {
      return {ConstantApplyStatus::kNeedsCompleteSnapshot, false, current_};
    }
    if (consume) {
      if (!CanIncrementStateVersion(version_)) {
        // Checked before writing: the state must keep matching current_.
        bool unchanged = initialized_;
        for (const ConstantDeltaRange& range : delta.ranges) {
          unchanged = unchanged && std::memcmp(canonical_.data() + range.destination_offset,
                                               delta.payload.data() + range.payload_offset,
                                               range.byte_count) == 0;
        }
        return {unchanged ? ConstantApplyStatus::kApplied : ConstantApplyStatus::kVersionSpaceExhausted,
                false, current_};
      }
      bool changed = !initialized_;
      for (const ConstantDeltaRange& range : delta.ranges) {
        const uint8_t* source = delta.payload.data() + range.payload_offset;
        uint8_t* destination = canonical_.data() + range.destination_offset;
        if (initialized_ && std::memcmp(destination, source, range.byte_count) == 0) continue;
        changed = true;
        std::memcpy(destination, source, range.byte_count);
      }
      if (!changed) return {ConstantApplyStatus::kApplied, false, current_};
      IncrementStateVersion(version_);
      initialized_ = true;
      auto next = MakeConstantStateVersion();
      next->token = version_;
      next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));
      next->byte_size = canonical_.size();
      // No delta and no parent: the draw snapshots this version from the
      // canonical bank right away (SnapshotCurrentVersion), while it is current.
      next->delta.complete_snapshot = true;
      current_ = std::move(next);
      return {ConstantApplyStatus::kApplied, true, current_};
    }

    ConstantPayloadDelta changed_delta;
    changed_delta.complete_snapshot = !initialized_;
    for (const ConstantDeltaRange& range : delta.ranges) {
      const uint8_t* source = delta.payload.data() + range.payload_offset;
      uint8_t* destination = canonical_.data() + range.destination_offset;
      if (initialized_ && std::memcmp(destination, source, range.byte_count) == 0) {
        continue;
      }
      const uint32_t payload_offset = static_cast<uint32_t>(changed_delta.payload.size());
      changed_delta.ranges.push_back(
          {range.destination_offset, payload_offset, range.byte_count});
      changed_delta.payload.insert(changed_delta.payload.end(), source,
                                   source + range.byte_count);
    }

    if (initialized_ && changed_delta.empty()) {
      return {ConstantApplyStatus::kApplied, false, current_};
    }
    if (!CanIncrementStateVersion(version_)) {
      return {ConstantApplyStatus::kVersionSpaceExhausted, false, current_};
    }
    for (const ConstantDeltaRange& range : changed_delta.ranges) {
      std::memcpy(canonical_.data() + range.destination_offset,
                  changed_delta.payload.data() + range.payload_offset, range.byte_count);
    }
    IncrementStateVersion(version_);
    initialized_ = true;

    auto next = MakeConstantStateVersion();
    next->token = version_;
    next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));
    next->byte_size = canonical_.size();
    // Updates without a consuming draw must not retain an unlimited chain.
    // A complete copied base is equivalent to replaying all prior deltas.
    const bool complete_bytes = changed_delta.ranges.size() == 1 &&
        changed_delta.ranges[0].destination_offset == 0 &&
        changed_delta.ranges[0].payload_offset == 0 &&
        changed_delta.ranges[0].byte_count == canonical_.size() &&
        changed_delta.payload.size() == canonical_.size();
    if (complete_bytes) {
      changed_delta.complete_snapshot = true;
    } else if (current_ && current_->deferred_ancestor_count >= kMaximumDeferredAncestors) {
      if (!CaptureCompleteConstantSnapshot(canonical_, changed_delta)) {
        return {ConstantApplyStatus::kInvalidDelta, false, current_};
      }
    }
    next->delta = std::move(changed_delta);
    if (!next->delta.complete_snapshot) {
      next->parent = current_;
      next->deferred_ancestor_count = current_ ? (current_->materialized ? 1 : current_->deferred_ancestor_count + 1) : 0;
    }
    current_ = std::move(next);
    return {ConstantApplyStatus::kApplied, true, current_};
  }

  // The caller must retain the version for the whole use of this view. Draw
  // commands already do so, and need not retain/release its immutable byte
  // allocation again on every binding. Owning/asynchronous consumers continue
  // to use Materialize below.
  static const std::vector<uint8_t>* MaterializeView(
      const std::shared_ptr<const ConstantStateVersion>& version) {
    if (!version) {
      return nullptr;
    }
    if (!version->materialized) {
      Materialize(version);
    }
    return version->materialized.get();
  }

  static std::shared_ptr<const std::vector<uint8_t>> Materialize(
      const std::shared_ptr<const ConstantStateVersion>& version) {
    if (!version) {
      return {};
    }
    if (version->materialized) {
      return version->materialized;
    }

    // The common ordered-draw case has a memoized direct parent. Avoid a
    // temporary owning chain and its allocator traffic for a single delta.
    if (version->parent && version->parent->materialized &&
        version->parent->materialized->size() == version->byte_size &&
        (!version->parent->materialized_bytes ||
         version->parent->materialized_bytes == version->byte_size) &&
        ValidateConstantPayloadDelta(version->delta, version->byte_size)) {
      auto bytes = std::make_shared<std::vector<uint8_t>>(*version->parent->materialized);
      for (const auto& range : version->delta.ranges)
        std::memcpy(bytes->data() + range.destination_offset,
                    version->delta.payload.data() + range.payload_offset, range.byte_count);
      version->materialized = std::move(bytes);
      version->materialized_bytes = version->byte_size;
      version->parent.reset();
      return version->materialized;
    }

    std::vector<std::shared_ptr<const ConstantStateVersion>> chain;
    std::shared_ptr<const ConstantStateVersion> cursor = version;
    while (cursor && !cursor->materialized) {
      chain.push_back(cursor);
      cursor = cursor->parent;
    }
    if (cursor && cursor->materialized_bytes && cursor->materialized_bytes != cursor->byte_size)
      return {};
    auto bytes = std::make_shared<std::vector<uint8_t>>(
        cursor && cursor->materialized ? *cursor->materialized
                                       : std::vector<uint8_t>(version->byte_size));
    for (auto node = chain.rbegin(); node != chain.rend(); ++node) {
      if ((*node)->byte_size != bytes->size() ||
          !ValidateConstantPayloadDelta((*node)->delta, bytes->size())) {
        return {};
      }
      for (const ConstantDeltaRange& range : (*node)->delta.ranges) {
        std::memcpy(bytes->data() + range.destination_offset,
                    (*node)->delta.payload.data() + range.payload_offset, range.byte_count);
      }
    }
    version->materialized = bytes;
    version->materialized_bytes = version->byte_size;
    // The memoized contiguous state is now a complete immutable base. Cutting
    // its ancestry prevents a long-running device from retaining every prior
    // delta version across frames.
    version->parent.reset();
    return version->materialized;
  }

 private:
  std::vector<uint8_t> canonical_;
  StateVersionStamp version_{};
  bool initialized_ = false;
  std::shared_ptr<const ConstantStateVersion> current_;
};

template <typename T>
class AuthoritativeScalarState {
 public:
  struct ApplyResult {
    ConstantApplyStatus status = ConstantApplyStatus::kApplied;
    bool changed = false;
  };

  ApplyResult Apply(const T& value) {
    if (initialized_ && value_ == value) {
      return {};
    }
    if (!CanIncrementStateVersion(version_)) {
      return {ConstantApplyStatus::kVersionSpaceExhausted, false};
    }
    value_ = value;
    initialized_ = true;
    IncrementStateVersion(version_);
    return {ConstantApplyStatus::kApplied, true};
  }

  bool initialized() const { return initialized_; }
  const T& value() const { return value_; }
  StateVersionStamp version() const { return version_; }

 private:
  T value_{};
  StateVersionStamp version_{};
  bool initialized_ = false;
};

// This is the complete semantic input identity for the host-built shared
// constant block. It intentionally includes descriptor meaning epochs and
// page/copy ownership in addition to the indices written into the block.
// Keeping it independently testable makes omissions visible when the shared
// block gains another producer input.
template <size_t TextureStageCount>
struct SharedConstantSemanticKey {
  std::array<uint32_t, TextureStageCount> texture_descriptor_indices{};
  std::array<uint32_t, TextureStageCount> sampler_descriptor_indices{};
  // Sampler objects deliberately exclude bias. Reusing the same descriptor
  // with different guest bias must still produce a different shared block.
  std::array<uint32_t, TextureStageCount> sampler_lod_bias_bits{};
  StateVersionStamp boolean_version{};
  uint64_t image_descriptor_epoch = 0;
  uint64_t sampler_descriptor_epoch = 0;
  uint64_t cached_descriptor_epoch = 0;
  uint64_t environmental_data_hash = 0;
  uint64_t environmental_sequence = 0;
  uint64_t tone_lut_address = 0;
  uint64_t cloud_mask_address = 0;
  uint32_t cloud_mask_width = 0, cloud_mask_height = 0;
  uint32_t modern_effects_enabled = 0;
  uint32_t water_reflection = 0;
  std::array<uint32_t, 6> viewport_bits{};
  uint32_t device = 0;
  uint32_t descriptor_copy = 0;
  uint32_t descriptor_page = std::numeric_limits<uint32_t>::max();
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t logical_width = 0;
  uint32_t logical_height = 0;
  uint32_t sample_count = 0;
  uint32_t alpha_reference_bits = 0;
  uint32_t alpha_to_mask = 0;
  std::array<uint32_t, 4> color_output_info{};
  uint32_t color_output_mask = 0;
  std::array<uint32_t, 4> clip_plane_bits{};
  uint32_t clip_plane_enable_mask = 0;
  uint32_t vertex_booleans = 0;
  uint32_t pixel_booleans = 0;
  uint8_t descriptor_backend = 0;
  uint8_t environment_present = 0;
  uint8_t split_postfx_applied = 0;

  constexpr bool operator==(const SharedConstantSemanticKey&) const = default;
};

inline size_t FindFirstConstantMismatch(std::span<const uint8_t> left,
                                        std::span<const uint8_t> right) {
  const size_t common_size = std::min(left.size(), right.size());
  size_t offset = 0;
  while (offset < common_size && left[offset] == right[offset]) {
    ++offset;
  }
  return offset != common_size || left.size() != right.size() ? offset : kNoSemanticMismatch;
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_STATEFUL_CONSTANT_STATE_H_
