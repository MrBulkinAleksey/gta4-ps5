// GTA IV PS5 - Xbox 360 saves into the PS5 app (see save_import.h).
#include "save_import.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>

#include <fmt/format.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/system/xcontent.h>

namespace gta4ps5 {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kGta4TitleId = 0x545407F2;
constexpr uint64_t kDefaultProfile = 0xB13EBABEBABEBABEull;  // the runtime's profile

struct FileDestroyer {
  void operator()(rex::filesystem::File* f) const {
    if (f) f->Destroy();
  }
};

bool IsDir(const rex::filesystem::Entry* e) {
  return e && (e->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
}

bool SafeName(const std::string& n) {
  return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:") == std::string::npos;
}

bool CopyEntry(rex::filesystem::Entry* entry, const fs::path& to, std::string& error) {
  if (IsDir(entry)) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const auto& child : entry->children()) {
      if (!SafeName(child->name())) continue;
      if (!CopyEntry(child.get(), to / child->name(), error)) return false;
    }
    return true;
  }
  rex::filesystem::File* raw = nullptr;
  if (entry->Open(rex::filesystem::FileAccess::kGenericRead, &raw) != 0 || !raw) {
    error = "couldn't open " + entry->path();
    return false;
  }
  std::unique_ptr<rex::filesystem::File, FileDestroyer> file(raw);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "couldn't write " + to.string();
    return false;
  }
  std::vector<uint8_t> buf(1 << 20);
  size_t offset = 0;
  while (offset < entry->size()) {
    const size_t want = std::min(buf.size(), size_t(entry->size() - offset));
    size_t got = 0;
    if (file->ReadSync(std::span<uint8_t>(buf).first(want), offset, &got) != 0 || got == 0) {
      error = "couldn't read " + entry->path();
      return false;
    }
    out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(got));
    offset += got;
  }
  return bool(out);
}

// The profile folders saves go in: the ones the game already uses, else the
// runtime's default profile.
std::vector<fs::path> ProfileFolders(const fs::path& saves_root) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(saves_root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (!it->is_directory(ec) || n.size() != 16) continue;
    bool hex = true;
    for (char c : n) hex &= std::isxdigit(static_cast<unsigned char>(c)) != 0;
    if (hex) out.push_back(it->path());
  }
  if (out.empty()) out.push_back(saves_root / fmt::format("{:016X}", kDefaultProfile));
  return out;
}

}  // namespace

fs::path SaveImportFolder(const fs::path& app) { return app / "import"; }

int PendingSaveImports(const fs::path& app) {
  int n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(SaveImportFolder(app), ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().filename().string()[0] != '.') ++n;
  return n;
}

SaveImportResult ImportXbox360Saves(const fs::path& app, const fs::path& saves_root) {
  SaveImportResult result;
  const fs::path folder = SaveImportFolder(app);
  std::error_code ec;
  fs::create_directories(folder, ec);
  std::vector<fs::path> files;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().filename().string()[0] != '.') files.push_back(it->path());

  for (const fs::path& file : files) {
    const std::string name = file.filename().string();
    auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(file);
    if (!header) {
      result.errors.push_back(name + ": not an Xbox 360 save package");
      continue;
    }
    const uint32_t title = header->metadata.execution_info.title_id;
    const auto type = header->metadata.content_type;
    if (title != kGta4TitleId) {
      result.errors.push_back(fmt::format("{}: another game's ({:08X})", name, title));
      continue;
    }
    if (type != rex::filesystem::XContentType::kSavedGame) {
      result.errors.push_back(name + ": not a saved game");
      continue;
    }
    rex::filesystem::StfsContainerDevice device("import:", file, false);
    if (!device.Initialize()) {
      result.errors.push_back(name + ": the package is damaged");
      continue;
    }
    auto* root = device.ResolvePath("");
    if (!root) {
      result.errors.push_back(name + ": the package is empty");
      continue;
    }
    bool ok = true;
    for (const fs::path& profile : ProfileFolders(saves_root)) {
      const fs::path dest = profile / "545407F2" / "00000001" / name;
      fs::remove_all(dest, ec);
      std::string error;
      if (!CopyEntry(root, dest, error)) {
        result.errors.push_back(name + ": " + error);
        ok = false;
        break;
      }
      REXLOG_INFO("PS5 save import: {} -> {}", name, dest.string());
    }
    if (!ok) continue;
    ++result.imported;
    result.names.push_back(name);
    fs::create_directories(folder / "done", ec);
    fs::rename(file, folder / "done" / name, ec);
  }
  for (const auto& e : result.errors) REXLOG_WARN("PS5 save import: {}", e);
  return result;
}

}  // namespace gta4ps5
