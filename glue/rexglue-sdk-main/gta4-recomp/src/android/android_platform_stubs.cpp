/**
 * Android replacements for the services the macOS host implements in
 * Objective-C++ (Game Center, AVFoundation user music, microphone TCC) and
 * for the curl/OpenSSL community multiplayer backend.
 *
 * Every entry point reports "unavailable" so the title takes its existing
 * offline/no-music paths.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include <rex/logging.h>

#include "achievement_bridge_gc.h"
#include "input/user_music_player.h"
#include "network/community_multiplayer.h"
#include "network/gta4_microphone_permission.h"

namespace gta4::game_center {

void Initialize() {}

void SubmitAchievement(uint32_t) {}

}  // namespace gta4::game_center

namespace gta4::input {

struct UserMusicPlayer::Impl {};

UserMusicPlayer::UserMusicPlayer(std::filesystem::path) : impl_(std::make_unique<Impl>()) {}

UserMusicPlayer::~UserMusicPlayer() = default;

bool UserMusicPlayer::Next() {
  return false;
}

bool UserMusicPlayer::Previous() {
  return false;
}

bool UserMusicPlayer::Stop() {
  return false;
}

bool UserMusicPlayer::HasTracks() const {
  return false;
}

void PublishUserMusicPlayer(UserMusicPlayer*) {}

bool IsUserMusicAvailable() {
  return false;
}

bool RequestUserMusicNext() {
  return false;
}

bool RequestUserMusicPrevious() {
  return false;
}

bool RequestUserMusicStop() {
  return false;
}

}  // namespace gta4::input

namespace gta4::voice {

MicrophonePermissionStatus GetMicrophonePermissionStatus() noexcept {
  return MicrophonePermissionStatus::kDenied;
}

void RequestMicrophonePermission(std::function<void()> completion) {
  if (completion) {
    completion();
  }
}

}  // namespace gta4::voice

namespace LibertyRecomp::Network {

rex::system::xam::LiveBackendServices CreateCommunityMultiplayerBackend(
    const rex::system::xam::LiveConfig&, const rex::system::xam::LiveIdentity&) {
  REXLOG_WARN("Community multiplayer is not available on Android");
  return {};
}

}  // namespace LibertyRecomp::Network
