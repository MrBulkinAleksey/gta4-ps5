// GTA IV PS5 - the app's PS5 parts: the hub before the game, the trophy
// popup, the frame rate counter and watermark, Xbox 360 save import.
#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_PS5
#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace gta4ps5 {

// Shows the hub (on the UI thread); `launch` runs on START GAME.
void ShowHub(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const rex::PathConfig& paths,
             const std::filesystem::path& liberty_root, const std::filesystem::path& config,
             std::function<void()> launch);
// The runtime is set up: the trophy list for the hub, the in-game overlay.
void OnGameSetUp(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime, const std::filesystem::path& user_data);

}  // namespace gta4ps5
#endif
