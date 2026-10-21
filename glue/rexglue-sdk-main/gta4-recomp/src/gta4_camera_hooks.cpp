// GTA IV - the car camera's sideways offset, as an option (gta4_centered_vehicle_camera).
//
// The chase camera behind a car doesn't sit straight behind it: sub_82535D98,
// called by the car camera's update (sub_8253AB20, CCamFollowVehicle), gives
// an offset half a metre to the side in the vehicle's own space (0x82AAAFB0:
// -0.5, 0, 0; 0x82AAAFC0 the same for The Ballad of Gay Tony's case), turned
// with the vehicle, while the byte at 0x82AAADF3 is set (1 in the game's
// image; the camera setup, sub_822148A0, may clear it). That byte is the PC
// version's CCamFollowVehicle::ms_bAllowSideOffset, which FusionFix's
// Centered Vehicle Camera clears. Centred, it's off for the call: the offset
// is zero and the camera sits right behind. Off, the game's own way.
//
// The same update swings the camera back behind the car each frame by a step
// (the turn still to go, divided by the frame's time step): sideways at
// 0x8253B8DC, up and down at 0x8253B704. FusionFix hooks the same two
// divisions on PC for its "centering delay" and "turn speed" options, and
// GTA4_VehicleCameraReturn (called right after each, by a line added to the
// recompiled code) does what it does: no step for a while after the right
// stick was last moved, and a bigger step for a faster swing.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "gta4_init.h"

REXCVAR_DEFINE_BOOL(gta4_centered_vehicle_camera, false, "GTA IV/Camera",
                    "Car camera straight behind the vehicle, without the game's sideways offset");
REXCVAR_DEFINE_UINT32(gta4_vehicle_camera_return_delay, 0, "GTA IV/Camera",
                      "Milliseconds the car camera waits after the right stick before it swings "
                      "back behind the car (0 is the game's)")
    .range(0, 5000);
REXCVAR_DEFINE_UINT32(gta4_vehicle_camera_turn_speed, 0, "GTA IV/Camera",
                      "How much faster the car camera swings back behind the car: the step is "
                      "divided by 1 - 0.1 x this, as in FusionFix (0 is the game's)")
    .range(0, 7);

namespace {
constexpr uint32_t kAllowSideOffset = 0x82AAADF3;
// The right stick's dead zone, XInput's.
constexpr int kRightStickDeadZone = 8689;

std::atomic<int64_t> g_return_after_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool RightStickMoved() {
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  rex::input::X_INPUT_STATE state{};
  if (!input || !input->TryGetLastState(0, &state)) return false;
  const int x = int16_t(state.gamepad.thumb_rx), y = int16_t(state.gamepad.thumb_ry);
  return std::abs(x) > kRightStickDeadZone || std::abs(y) > kRightStickDeadZone;
}
}  // namespace

double GTA4_VehicleCameraReturn(double step) {
  const uint32_t delay = REXCVAR_GET(gta4_vehicle_camera_return_delay);
  if (delay) {
    const int64_t now = NowMs();
    if (RightStickMoved()) g_return_after_ms.store(now + delay, std::memory_order_relaxed);
    if (now < g_return_after_ms.load(std::memory_order_relaxed)) return 0.0;
  }
  if (const uint32_t speed = REXCVAR_GET(gta4_vehicle_camera_turn_speed); speed) {
    step = double(float(step / (1.0 - 0.1 * double(speed))));
  }
  return step;
}

extern "C" void sub_82535D98(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gta4_centered_vehicle_camera)) {
    __imp__sub_82535D98(ctx, base);
    return;
  }
  const uint8_t allow = REX_LOAD_U8(kAllowSideOffset);
  static bool noted = false;  // (once: a report shows it was on)
  if (!noted) {
    noted = true;
    REXLOG_INFO("gta4-camera: centered vehicle camera on (the game's side offset was {})", allow ? "on" : "off");
  }
  REX_STORE_U8(kAllowSideOffset, 0);
  __imp__sub_82535D98(ctx, base);
  REX_STORE_U8(kAllowSideOffset, allow);
}
