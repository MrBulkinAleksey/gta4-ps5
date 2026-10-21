#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rex::graphics::gta4_native {

// A saturated lifetime disables memoization permanently. Reusing an epoch after
// destroying pipelines could otherwise resurrect an old Vulkan handle.
class NativePipelineLookupLifetime {
 public:
  explicit NativePipelineLookupLifetime(uint64_t epoch = 1) : epoch_(epoch) {}
  uint64_t epoch() const { return epoch_; }
  void Invalidate() {
    epoch_ = epoch_ && epoch_ != std::numeric_limits<uint64_t>::max() ? epoch_ + 1 : 0;
  }

 private:
  uint64_t epoch_;
};

template <size_t ColorTargetCount>
struct NativePipelineLookupContext {
  uint64_t lifetime = 0;
  uint64_t pipeline_layout = 0;
  std::array<uint32_t, ColorTargetCount> color_formats{};
  uint32_t depth_format = 0;
  uint32_t color_attachment_mask = 0;
  uint32_t color_write_mask = 0;
  uint32_t samples = 0;
  uint32_t guest_samples = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t primitive_type = 0;
  uint32_t user_pointer_stride = 0;
  uint32_t descriptor_backend = 0;
  uint32_t shader_override_mode = 0;
  uint32_t modern_shader_settings = 0;
  bool depth_stencil_attachment_active = false;
  bool uses_presenter = false;
  bool primitive_restart_enable = false;
  bool host_fog = false;
  bool early_fragment_tests = false;
  bool colorless_fragment = false;
  bool empty_fragment = false;

  bool operator==(const NativePipelineLookupContext&) const = default;
};

// Lives inline in an immutable pipeline-state snapshot. Successful validation
// can be reused by its later draw without rebuilding or hashing a pipeline key.
// The owner pointer rejects a memo copied into another snapshot. The context
// covers renderer/target inputs; fixed state is compared memberwise, not hashed
// or compared through padding bytes. Null results are never cached, preserving
// asynchronous compilation and every failure/retry path.
template <typename FixedState, size_t ColorTargetCount, typename Pipeline>
class NativePipelineLookupMemo {
 public:
  using Context = NativePipelineLookupContext<ColorTargetCount>;

  Pipeline Find(const void* owner, const FixedState& fixed, const Context& context) const {
    if (!owner || !context.lifetime) return Pipeline{};
    for (size_t i = 0; i < kEntries; ++i) {
      if (pipelines_[i] && owner == owners_[i] && context == contexts_[i] && fixed == fixed_[i])
        return pipelines_[i];
    }
    return Pipeline{};
  }

  // signature: a 64-bit hash of the memo fixed state taken where the state
  // was captured (0 = none). Equal signatures stand for equal fixed states.
  Pipeline FindSigned(const void* owner, uint64_t signature, const Context& context) const {
    if (!owner || !context.lifetime || !signature) return Pipeline{};
    for (size_t i = 0; i < kEntries; ++i) {
      if (pipelines_[i] && signatures_[i] == signature && owner == owners_[i] &&
          context == contexts_[i]) return pipelines_[i];
    }
    return Pipeline{};
  }

  void Store(const void* owner, const FixedState& fixed, const Context& context, Pipeline pipeline,
             uint64_t signature = 0) {
    if (!owner || !context.lifetime || !pipeline) {
      return;
    }
    for (size_t i = 0; i < kEntries; ++i) {
      if (owners_[i] == owner && contexts_[i] == context && fixed_[i] == fixed) {
        pipelines_[i] = pipeline;
        if (signature) signatures_[i] = signature;
        return;
      }
    }
    owners_[next_] = owner;
    fixed_[next_] = fixed;
    contexts_[next_] = context;
    pipelines_[next_] = pipeline;
    signatures_[next_] = signature;
    next_ = (next_ + 1) % kEntries;
  }

 private:
  // [ps5 port] By field, not by entry: a signed lookup reads the signatures,
  // pipelines and owners, then only the matching entry's context.
  static constexpr size_t kEntries = 4;
  std::array<uint64_t, kEntries> signatures_{};
  std::array<Pipeline, kEntries> pipelines_{};
  std::array<const void*, kEntries> owners_{};
  std::array<Context, kEntries> contexts_{};
  std::array<FixedState, kEntries> fixed_{};
  size_t next_ = 0;
};

}  // namespace rex::graphics::gta4_native
