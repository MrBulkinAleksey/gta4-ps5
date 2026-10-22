#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "gta4_frontend_menu_policy.h"

struct PPCContext;

namespace gta4::frontend_menu {

void SetConfigPath(std::filesystem::path path);
// What the Display page's Exit Game row does (desktop builds).
void SetExitRequest(std::function<void()> request);
void DrawSliders(PPCContext& ctx, uint8_t* base);
bool SwitchPauseTab(PPCContext& parent, uint8_t* base, policy::PauseTabDirection direction,
                    uint32_t* previous_screen = nullptr, uint32_t* target_screen = nullptr);

}  // namespace gta4::frontend_menu
