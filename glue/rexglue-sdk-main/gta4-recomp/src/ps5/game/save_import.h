// GTA IV PS5 - Xbox 360 saves into the PS5 app. The player copies the save
// packages off a 360 (the files in Content/<profile>/545407F2/00000001 on its
// drive, or a USB stick's Xbox360 folder) into the app's import folder; this
// unpacks each one into the game's save folder.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gta4ps5 {

struct SaveImportResult {
  int imported = 0;                 // packages unpacked
  std::vector<std::string> names;   // ...their names
  std::vector<std::string> errors;  // files that couldn't be used, and why
};

// <app>/import: where the Xbox 360 save packages go.
std::filesystem::path SaveImportFolder(const std::filesystem::path& app);
// How many files wait in the import folder.
int PendingSaveImports(const std::filesystem::path& app);
// Unpacks every GTA IV save package in the import folder into `saves_root`
// (<saves_root>/<profile>/545407F2/00000001/<package name>/). Imported
// packages move to import/done.
SaveImportResult ImportXbox360Saves(const std::filesystem::path& app, const std::filesystem::path& saves_root);

}  // namespace gta4ps5
