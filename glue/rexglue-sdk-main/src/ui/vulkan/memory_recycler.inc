// [ps5 port] Device-memory recycling. Included inside namespace
// rex::ui::vulkan by vulkan_device.cpp.
//
// On the PS5 every vkAllocateMemory is a direct-memory allocation and a
// mapping in the kernel, and every vkFreeMemory an unmap and a release. The
// renderer gives each texture and surface its own allocation, and streaming
// replaces textures constantly while driving, so the recorder spent about 8%
// of its time in those calls and some frames took 100-500 ms. Freed
// device-local memory of up to kLargest bytes is kept here instead (at most
// kBudget in all, oldest given back first) and handed to the next allocation
// of the same memory type and nearly the same size. Contents are undefined
// either way. Host-visible memory and allocations with extension structures
// other than a dedicated-allocation hint go straight to the driver.
namespace memory_recycler {

constexpr VkDeviceSize kPage = 16 * 1024;              // the PS5 driver's page
constexpr VkDeviceSize kLargest = 48ull * 1024 * 1024;  // a 1440p RGBA16F target fits
constexpr VkDeviceSize kBudget = 128ull * 1024 * 1024;

PFN_vkAllocateMemory allocate_fn;
PFN_vkFreeMemory free_fn;
VkDevice installed_device = VK_NULL_HANDLE;
uint32_t recyclable_types = 0;  // device-local, nothing else

using Key = std::pair<uint32_t, VkDeviceSize>;  // (memory type, size)
struct Cached {
  VkDeviceMemory memory;
  uint64_t serial;
};
std::mutex mutex;
// Ours, live: memory -> (size, type).
std::unordered_map<VkDeviceMemory, std::pair<VkDeviceSize, uint32_t>> live;
// Free, by key; each list oldest first (reuse takes the newest).
std::map<Key, std::vector<Cached>> cached;
// Every entry ever cached, oldest first; entries reused since are skipped.
std::deque<std::pair<uint64_t, Key>> order;
VkDeviceSize cached_bytes = 0;
size_t cached_count = 0;
uint64_t next_serial = 0;
std::atomic<uint64_t> hits{0}, misses{0}, given_back{0};

VkDeviceSize Rounded(VkDeviceSize size) { return (size + kPage - 1) & ~(kPage - 1); }

// Under mutex: the oldest free entry gives its memory back to the driver.
bool ReleaseOldestLocked(VkDevice device) {
  while (!order.empty()) {
    const auto [serial, key] = order.front();
    order.pop_front();
    auto it = cached.find(key);
    // A list's front is its oldest entry; an older serial was reused.
    if (it == cached.end() || it->second.front().serial != serial) continue;
    const VkDeviceMemory memory = it->second.front().memory;
    it->second.erase(it->second.begin());
    if (it->second.empty()) cached.erase(it);
    cached_bytes -= key.second;
    --cached_count;
    free_fn(device, memory, nullptr);
    given_back.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

// Under mutex: drops the order's entries for memory reused since.
void CompactOrderLocked() {
  if (order.size() < 4 * cached_count + 4096) return;
  std::vector<std::pair<uint64_t, Key>> present;
  present.reserve(cached_count);
  for (const auto& [key, list] : cached)
    for (const Cached& entry : list) present.emplace_back(entry.serial, key);
  std::sort(present.begin(), present.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  order.assign(present.begin(), present.end());
}

VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                        const VkAllocationCallbacks* allocator,
                                        VkDeviceMemory* memory) {
  const bool plain_next =
      !info->pNext || (static_cast<const VkBaseInStructure*>(info->pNext)->sType ==
                           VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO &&
                       !static_cast<const VkBaseInStructure*>(info->pNext)->pNext);
  if (allocator || !plain_next || info->memoryTypeIndex >= 32 ||
      !(recyclable_types & (1u << info->memoryTypeIndex)) || info->allocationSize > kLargest) {
    return allocate_fn(device, info, allocator, memory);
  }
  const VkDeviceSize size = Rounded(info->allocationSize);
  {
    std::lock_guard lock(mutex);
    // The same size, or up to a quarter larger.
    auto it = cached.lower_bound({info->memoryTypeIndex, size});
    if (it != cached.end() && it->first.first == info->memoryTypeIndex &&
        it->first.second <= size + size / 4) {
      const VkDeviceSize found_size = it->first.second;
      *memory = it->second.back().memory;
      it->second.pop_back();
      if (it->second.empty()) cached.erase(it);
      cached_bytes -= found_size;
      --cached_count;
      CompactOrderLocked();
      live.emplace(*memory, std::make_pair(found_size, info->memoryTypeIndex));
      hits.fetch_add(1, std::memory_order_relaxed);
      return VK_SUCCESS;
    }
  }
  // Without the dedicated hint the allocation can serve any image later.
  VkMemoryAllocateInfo plain = *info;
  plain.pNext = nullptr;
  plain.allocationSize = size;
  VkResult result = allocate_fn(device, &plain, nullptr, memory);
  if (result != VK_SUCCESS) {
    // Short of memory: give back everything held here and try once more.
    {
      std::lock_guard lock(mutex);
      while (ReleaseOldestLocked(device)) {
      }
    }
    result = allocate_fn(device, &plain, nullptr, memory);
    if (result != VK_SUCCESS) return result;
  }
  misses.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(mutex);
  live.emplace(*memory, std::make_pair(size, info->memoryTypeIndex));
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Free(VkDevice device, VkDeviceMemory memory,
                                const VkAllocationCallbacks* allocator) {
  if (memory != VK_NULL_HANDLE) {
    std::lock_guard lock(mutex);
    auto it = live.find(memory);
    if (it != live.end()) {
      const auto [size, type] = it->second;
      live.erase(it);
      if (size <= kBudget) {
        while (cached_bytes + size > kBudget && ReleaseOldestLocked(device)) {
        }
        const uint64_t serial = ++next_serial;
        cached[{type, size}].push_back({memory, serial});
        order.emplace_back(serial, Key{type, size});
        cached_bytes += size;
        ++cached_count;
        return;
      }
    }
  }
  free_fn(device, memory, allocator);
}

void FreeAll(VkDevice device) {
  if (!free_fn) return;
  std::lock_guard lock(mutex);
  while (ReleaseOldestLocked(device)) {
  }
}

}  // namespace memory_recycler

void MemoryRecyclerReleaseAll() {
  if (memory_recycler::installed_device) memory_recycler::FreeAll(memory_recycler::installed_device);
}

std::string MemoryRecyclerSummary() {
  using namespace memory_recycler;
  VkDeviceSize held;
  {
    std::lock_guard lock(mutex);
    held = cached_bytes;
  }
  return fmt::format("memory-recycler: reused={} new={} given-back={} held={} MB",
                     hits.load(std::memory_order_relaxed), misses.load(std::memory_order_relaxed),
                     given_back.load(std::memory_order_relaxed), held >> 20);
}

static void InstallMemoryRecycler(VulkanDevice::Functions& dfn,
                                  const VkPhysicalDeviceMemoryProperties& properties,
                                  VkDevice device) {
  using namespace memory_recycler;
  if (allocate_fn) return;  // one device
  installed_device = device;
  for (uint32_t i = 0; i < properties.memoryTypeCount && i < 32; ++i) {
    if (properties.memoryTypes[i].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      recyclable_types |= 1u << i;
  }
  if (!recyclable_types || !dfn.vkAllocateMemory || !dfn.vkFreeMemory) return;
  allocate_fn = dfn.vkAllocateMemory;
  free_fn = dfn.vkFreeMemory;
  dfn.vkAllocateMemory = Allocate;
  dfn.vkFreeMemory = Free;
}
