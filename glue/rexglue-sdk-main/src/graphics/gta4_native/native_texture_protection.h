#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace rex::graphics::gta4_native {

// Include implicit host-pass inputs as well as title sampler bindings. A queued
// composite owns its half-scene generation even when the title never binds it.
template <typename Command, typename Visitor>
void VisitNativeCommandTextureGenerations(const Command& command, Visitor&& visit) {
  const auto texture = [&](const auto& resource) {
    if (!resource) return;
    visit(resource->generation);
    if (resource->packed_depth_source) visit(resource->packed_depth_source->generation);
  };
  texture(command.resolve_destination);
  texture(command.depth_handoff_source);
  texture(command.present_source);
  texture(command.postfx_half_scene);
  for (const auto& resource : command.textures) texture(resource);
}

// A set of texture generations: open addressing over one flat table, plus the
// members in insertion order for iteration. clear() is O(1) (a slot is full
// only in the epoch it was filled in) and keeps both allocations.
// [ps5 port] Replaces std::unordered_set for the protection sets, rebuilt and
// cleared every frame: a node per insert, prime-modulo buckets, chased nodes.
class NativeGenerationSet {
 public:
  using const_iterator = std::vector<uint64_t>::const_iterator;

  // Returns true when the key was not present.
  bool insert(uint64_t key) {
    if ((members_.size() + 1) * 2 > slots_.size()) Grow(members_.size() + 1);
    Slot& slot = slots_[Probe(key)];
    if (slot.epoch == epoch_) return false;
    slot.key = key;
    slot.epoch = epoch_;
    members_.push_back(key);
    return true;
  }
  template <typename Iterator>
  void insert(Iterator first, Iterator last) {
    for (; first != last; ++first) insert(uint64_t(*first));
  }
  bool contains(uint64_t key) const {
    return !slots_.empty() && slots_[Probe(key)].epoch == epoch_;
  }
  void clear() {
    members_.clear();
    if (++epoch_ == 0) {  // wrapped: empty every slot explicitly
      for (Slot& slot : slots_) slot.epoch = 0;
      epoch_ = 1;
    }
  }
  void reserve(size_t count) {
    if (count * 2 > slots_.size()) Grow(count);
  }
  size_t size() const { return members_.size(); }
  bool empty() const { return members_.empty(); }
  const_iterator begin() const { return members_.begin(); }
  const_iterator end() const { return members_.end(); }
  void swap(NativeGenerationSet& other) noexcept {
    slots_.swap(other.slots_);
    members_.swap(other.members_);
    std::swap(epoch_, other.epoch_);
  }
  // Same members, in any order.
  bool operator==(const NativeGenerationSet& other) const {
    if (size() != other.size()) return false;
    for (uint64_t key : members_)
      if (!other.contains(key)) return false;
    return true;
  }

 private:
  struct Slot {
    uint64_t key = 0;
    uint32_t epoch = 0;
  };
  static size_t Hash(uint64_t key) { return size_t((key * 0x9E3779B97F4A7C15ull) >> 17); }
  // The slot holding key, or the empty slot where it goes (load stays <= 1/2).
  size_t Probe(uint64_t key) const {
    const size_t mask = slots_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      const Slot& slot = slots_[i];
      if (slot.epoch != epoch_ || slot.key == key) return i;
    }
  }
  void Grow(size_t count) {
    size_t capacity = slots_.empty() ? 1024 : slots_.size();
    while (count * 2 > capacity) capacity *= 2;
    slots_.assign(capacity, Slot{});
    epoch_ = 1;
    for (uint64_t key : members_) {
      Slot& slot = slots_[Probe(key)];
      slot.key = key;
      slot.epoch = epoch_;
    }
  }
  std::vector<Slot> slots_;
  std::vector<uint64_t> members_;
  uint32_t epoch_ = 1;
};

// Queue lock owns this index. Counts are logical references, not shared_ptrs or
// GPU ownership. Every zero count is removed immediately. If an invariant is
// violated the renderer falls back to its original queue scan, never guessing
// that an image is safe to retire.
class NativeTextureProtectionIndex {
 public:
  bool Retain(uint64_t generation) {
    if (!generation) return true;
    uint64_t& count = values_[Slot(generation, true)];
    if (count == UINT64_MAX) { valid_ = false; return false; }
    if (!count) ++live_;
    ++count; return true;
  }
  // Zero counts stay in the table (the same textures are retained and
  // released for nearly every draw) and are pruned in bulk.
  bool Release(uint64_t generation) {
    if (!generation) return true;
    const size_t slot = Slot(generation, false);
    if (slot == kNone || !values_[slot]) { valid_ = false; return false; }
    if (!--values_[slot]) --live_;
    return true;
  }
  template <typename Set>
  void AppendTo(Set& destination) const {
    destination.reserve(destination.size() + live_);
    for (size_t i = 0; i < keys_.size(); ++i)
      if (keys_[i] && values_[i]) destination.insert(keys_[i]);
  }
  bool Contains(uint64_t generation) const {
    const size_t slot = Find(generation);
    return slot != kNone && values_[slot];
  }
  size_t size() const { return live_; }
  bool valid() const { return valid_; }
  void Reset() { keys_.clear(); values_.clear(); used_ = 0; live_ = 0; valid_ = true; }

 private:
  // Open addressing over two flat arrays; key 0 marks an empty slot (0 is
  // never a texture generation). No per-insert allocation, no node chasing.
  static constexpr size_t kNone = SIZE_MAX;
  static size_t Hash(uint64_t key) { return size_t((key * 0x9E3779B97F4A7C15ull) >> 17); }
  size_t Find(uint64_t key) const {
    if (keys_.empty() || !key) return kNone;
    const size_t mask = keys_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      if (keys_[i] == key) return i;
      if (!keys_[i]) return kNone;
    }
  }
  size_t Slot(uint64_t key, bool insert) {
    if (!insert) return Find(key);
    if ((used_ + 1) * 2 > keys_.size()) Rehash();
    const size_t mask = keys_.size() - 1;
    for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
      if (keys_[i] == key) return i;
      if (!keys_[i]) { keys_[i] = key; values_[i] = 0; ++used_; return i; }
    }
  }
  // Grows, and drops zero-count entries, keeping the load factor under 1/2.
  void Rehash() {
    size_t capacity = 1024;
    while (capacity < (live_ + 1) * 4) capacity *= 2;
    std::vector<uint64_t> keys(capacity, 0), values(capacity, 0);
    const size_t mask = capacity - 1;
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (!keys_[i] || !values_[i]) continue;
      size_t j = Hash(keys_[i]) & mask;
      while (keys[j]) j = (j + 1) & mask;
      keys[j] = keys_[i]; values[j] = values_[i];
    }
    keys_.swap(keys); values_.swap(values);
    used_ = live_;
  }
  std::vector<uint64_t> keys_;
  std::vector<uint64_t> values_;
  size_t used_ = 0;  // occupied slots, including zero counts
  size_t live_ = 0;  // slots with a nonzero count
  bool valid_ = true;
};
}  // namespace rex::graphics::gta4_native
