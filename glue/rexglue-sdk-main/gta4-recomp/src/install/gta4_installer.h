#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gta4_source_inspector.h"

namespace gta4::install {

enum class Episode {
  kTlad,
  kTbogt,
};

struct DlcSelection {
  Episode episode;
  std::filesystem::path source;
};

struct Selection {
  std::filesystem::path game_source;
  std::filesystem::path update_source;
  std::vector<DlcSelection> dlc_sources;
};

struct Progress {
  std::atomic<uint64_t> copied_bytes{0};
  std::atomic<uint64_t> total_bytes{0};
  std::atomic<bool> cancel_requested{false};
};

struct Result {
  bool success = false;
  std::string error;
};

bool IsInstallReady(const std::filesystem::path& game_root, std::string* reason = nullptr);
bool IsEpisodeReady(const std::filesystem::path& marketplace_root, Episode episode);
Result VerifyInstall(const std::filesystem::path& game_root,
                     const std::filesystem::path& marketplace_root);
Result Install(const Selection& selection, const std::filesystem::path& install_root,
               Progress& progress);

// [ps5 port] An install of the game and its title update whose writing is
// done somewhere else: on the PS5 a title may only write slowly, so the
// installer payload writes it (ps5/payload/installer). PlanInstall checks the
// sources as Install() does and says what to copy; the writer puts the game in
// <install_root>/.install-staging/game the way Install() does (the source's
// files, the update's files in update/, default.xexp beside default.xex,
// aes_key.bin, the archives unpacked); FinishInstall checks that against the
// sources, writes the manifest and moves it into place.
struct InstallPlan {
  std::string disc_root;         // the source's folder with default.xex ("": its top), '/' between
  bool update_package = false;   // Title Update 8 is a content package (else a bare .xexp)
  std::string update_root;       // the package's folder with default.xexp
  std::string update_xexp;       // default.xexp's path in the package
  uint64_t copy_bytes = 0;       // what's copied before the archives are unpacked
  std::vector<uint8_t> aes_key;  // the archives' key (32 bytes)
};
bool PlanInstall(const Selection& selection, InstallPlan& plan, std::string& error);
Result FinishInstall(const Selection& selection, const std::filesystem::path& install_root);

// [ps5 port] An episode, written by the installer payload as the game is:
// PlanEpisode says which episode a source holds (a content package from the
// Xbox 360, or a folder of its files) and which of its folders to copy; the
// writer copies that folder's files into
// <install_root>/.install-staging/dlc/<EpisodeDirectory>; FinishEpisode checks
// them against the source and moves them into <install_root>/dlc.
struct EpisodePlan {
  Episode episode = Episode::kTlad;
  bool package = false;     // a content package (else a folder)
  std::string root;         // the folder with setup2.xml in it ("": the top), '/' between
  uint64_t copy_bytes = 0;  // what's copied
};
bool PlanEpisode(const std::filesystem::path& source, EpisodePlan& plan, std::string& error);
Result FinishEpisode(const std::filesystem::path& source, Episode episode,
                     const std::filesystem::path& install_root);

const char* EpisodeDirectory(Episode episode);

}  // namespace gta4::install
