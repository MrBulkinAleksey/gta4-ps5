// GTA IV PS5 - the TV's refresh rate and frame pacing (see ps5_display.h).
#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include "ps5_display.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(ps5_refresh_rate, 60, "PS5",
                      "The TV's refresh rate, 60 or 120 Hz (120 needs a TV that takes it); read when the app starts");
REXCVAR_DECLARE(uint32_t, gta4_frame_limit);

extern "C" {
int __real_sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void* reserved1,
                                        const void* reserved2, const void* reserved3);
int __real_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void* reserved1,
                                      const void* reserved2, const void* reserved3);
int __real_sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutWaitVblank(int32_t handle);
}

namespace gta4ps5::display {
namespace {

constexpr uint32_t kHighFrameRateMode = 15;  // (videoout.h: 119.88 Hz)
constexpr uint32_t kRestoreMode = 1;

// After the 120 Hz mode is taken (ps5_display.h): a TV at a fixed 60 Hz has
// every vblank kFixed60MinNs..kFixed60MaxNs apart (59.94 Hz: 16.683 ms). The
// median under kHighRefreshLimitNs is a fixed 120 Hz; past kFixed60MaxNs, the
// TV idling at its VRR floor; any one interval under kFixed60MinNs, a TV
// refreshing faster than 60 Hz (120, or VRR following flips).
constexpr int64_t kHighRefreshLimitNs = 12500000;
constexpr int64_t kFixed60MinNs = 14500000;
constexpr int64_t kFixed60MaxNs = 18500000;

std::atomic<int32_t> g_handle{-1};
std::atomic<bool> g_high{false};
std::atomic<bool> g_vrr{false};
std::atomic<bool> g_game{false};
std::atomic<int32_t> g_rate{-1};  // (the rate VideoOut has: -1 unknown)
std::mutex g_switch_mutex;               // (one switch at a time)

// The hub's setting, straight from its file: VideoOut opens before the config
// is loaded (as ps5_save_logs in windowed_app_main_sdl.cpp).
bool WantsHighRefresh() {
  bool high = false;
  if (std::FILE* f = std::fopen("/app0/UserData/LibertyRecomp/native.toml", "rb")) {
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
      const char* start = line + std::strspn(line, " \t");
      if (std::strncmp(start, "ps5_refresh_rate", 16) != 0) continue;
      const char* value = std::strchr(start, '=');
      if (!value) continue;
      value += 1 + std::strspn(value + 1, " \t\"");
      high = std::strtoul(value, nullptr, 10) >= 100;
    }
    std::fclose(f);
  }
  return high;
}

// Vblanks a frame stays for, less one (sceVideoOutSetFlipRate's rate).
int32_t WantedRate() {
  if (!g_game.load(std::memory_order_acquire)) return 0;
  const uint32_t fps = REXCVAR_GET(gta4_frame_limit);
  // VRR: the TV refreshes when a frame comes; the frame limit's own pacing
  // (Ps5LimitFrameRate) spaces them. (30 and 40 are under most TVs' VRR
  // floor: held to vblanks as at a fixed 120 Hz.)
  if (g_vrr.load(std::memory_order_acquire) && (fps == 0 || fps == 60 || fps == 120)) return 0;
  if (g_high.load(std::memory_order_acquire)) {
    if (fps == 40) return 2;
    if (fps == 30 || fps == 60) return 1;
    return 0;
  }
  return fps == 30 ? 1 : 0;
}

void Apply(const char* why) {
  const int32_t handle = g_handle.load(std::memory_order_acquire);
  const int32_t rate = WantedRate();
  if (handle < 0 || g_rate.load(std::memory_order_acquire) == rate) return;
  const int result = __real_sceVideoOutSetFlipRate(handle, rate);
  if (result == 0) g_rate.store(rate, std::memory_order_release);
  REXLOG_INFO("PS5 display: each frame shows for at least {} vblank{} at {} Hz{} ({}; {})", rate + 1,
              rate ? "s" : "", g_high.load() ? "119.88" : "59.94", g_vrr.load() ? " VRR" : "", why,
              result == 0 ? std::string("ok") : fmt::format("failed {:08X}", uint32_t(result)));
}

// The time between vblanks over eight of them, the first wait finding the
// phase, sorted (a wait that misses a vblank gives a longer interval, never a
// shorter one). False when a wait fails.
bool VblankIntervals(int32_t handle, std::array<int64_t, 8>& intervals) {
  if (sceVideoOutWaitVblank(handle) != 0) return false;
  auto previous = std::chrono::steady_clock::now();
  for (int64_t& interval : intervals) {
    if (sceVideoOutWaitVblank(handle) != 0) return false;
    const auto now = std::chrono::steady_clock::now();
    interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
    previous = now;
  }
  std::sort(intervals.begin(), intervals.end());
  return true;
}

}  // namespace

bool HighRefresh() { return g_high.load(std::memory_order_acquire); }
bool Vrr() { return g_vrr.load(std::memory_order_acquire); }
bool Ready() { return g_handle.load(std::memory_order_acquire) >= 0; }

bool SetHighRefresh(bool high) {
  std::lock_guard lock(g_switch_mutex);
  const int32_t handle = g_handle.load(std::memory_order_acquire);
  if (handle < 0) {
    REXLOG_WARN("PS5 display: no TV open yet; {} Hz not switched", high ? 120 : 60);
    return false;
  }
  if (g_high.load(std::memory_order_acquire) == high) return true;
  if (!high) {
    const int result = __real_sceVideoOutConfigureOutput(handle, kRestoreMode, nullptr, nullptr, nullptr);
    g_high.store(false, std::memory_order_release);
    g_vrr.store(false, std::memory_order_release);
    REXLOG_INFO("PS5 display: back to 60 Hz ({})", result);
    Apply("60 Hz");
    return true;
  }
  const int supported = __real_sceVideoOutIsOutputSupported(handle, kHighFrameRateMode, nullptr, nullptr, nullptr);
  if (supported <= 0) {
    REXLOG_INFO("PS5 display: 120 Hz not offered on the PS5's video output ({}); staying at 60 Hz", supported);
    return false;
  }
  const int configured = __real_sceVideoOutConfigureOutput(handle, kHighFrameRateMode, nullptr, nullptr, nullptr);
  if (configured != 0) {
    REXLOG_INFO("PS5 display: 120 Hz refused ({:08X}); staying at 60 Hz", uint32_t(configured));
    return false;
  }
  std::array<int64_t, 8> intervals{};
  const bool measured = VblankIntervals(handle, intervals);
  const int64_t shortest = intervals.front(), median = intervals[intervals.size() / 2];
  const bool fixed_120 = measured && median <= kHighRefreshLimitNs;
  const bool faster_than_60 = measured && shortest < kFixed60MinNs;
  const bool vrr_floor = measured && median > kFixed60MaxNs;
  if (!fixed_120 && !faster_than_60 && !vrr_floor) {
    __real_sceVideoOutConfigureOutput(handle, kRestoreMode, nullptr, nullptr, nullptr);
    REXLOG_INFO("PS5 display: 120 Hz taken, but a vblank comes every {:.3f} ms (shortest {:.3f}); back to 60 Hz",
                median / 1e6, shortest / 1e6);
    return false;
  }
  g_vrr.store(!fixed_120, std::memory_order_release);
  g_high.store(true, std::memory_order_release);
  REXLOG_INFO("PS5 display: 120 Hz on{} (vblanks {:.3f} to {:.3f} ms apart, median {:.3f})",
              fixed_120 ? "" : " as VRR", shortest / 1e6, intervals.back() / 1e6, median / 1e6);
  Apply("120 Hz");
  return true;
}

void StartGamePacing() {
  g_game.store(true, std::memory_order_release);
  static std::atomic<bool> registered{false};
  if (!registered.exchange(true))
    rex::cvar::RegisterChangeCallback("gta4_frame_limit",
                                      [](std::string_view, std::string_view) { Apply("frame limit changed"); });
  Apply("game started");
}

}  // namespace gta4ps5::display

using namespace gta4ps5::display;

extern "C" int __wrap_sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void* reserved1,
                                                   const void* reserved2, const void* reserved3) {
  if (handle >= 0) g_handle.store(handle, std::memory_order_release);
  if (mode == kHighFrameRateMode && !WantsHighRefresh()) {
    REXLOG_INFO("PS5 display: 60 Hz (the hub's Refresh Rate)");
    return 0;
  }
  const int result = __real_sceVideoOutIsOutputSupported(handle, mode, reserved1, reserved2, reserved3);
  if (mode == kHighFrameRateMode)
    REXLOG_INFO("PS5 display: 120 Hz asked for; the TV {} it ({})", result > 0 ? "takes" : "doesn't take", result);
  return result;
}

extern "C" int __wrap_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void* reserved1,
                                                 const void* reserved2, const void* reserved3) {
  if (handle >= 0) g_handle.store(handle, std::memory_order_release);
  const int result = __real_sceVideoOutConfigureOutput(handle, mode, reserved1, reserved2, reserved3);
  if (mode == kHighFrameRateMode) {
    g_high.store(result == 0, std::memory_order_release);
    REXLOG_INFO("PS5 display: 120 Hz mode {} ({})", result == 0 ? "on" : "refused", result);
  } else if (mode == kRestoreMode && g_high.exchange(false)) {
    g_vrr.store(false, std::memory_order_release);
    // (RADV measured the vblanks and found the TV still at 60 Hz)
    REXLOG_INFO("PS5 display: the TV stayed at 60 Hz; 120 Hz mode off");
  }
  return result;
}

extern "C" int __wrap_sceVideoOutSetFlipRate(int32_t handle, int32_t rate) {
  g_handle.store(handle, std::memory_order_release);
  const int32_t wanted = WantedRate();
  const int result = __real_sceVideoOutSetFlipRate(handle, wanted);
  g_rate.store(result == 0 ? wanted : -1, std::memory_order_release);
  (void)rate;
  // (RADV doesn't ask for 120 Hz with our param.json: the hub switches at its
  // first frame, ps5_display.h)
  return result;
}

#endif  // REX_PLATFORM_PS5
