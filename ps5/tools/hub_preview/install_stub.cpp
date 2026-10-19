// The hub preview's stand-in for hub_install.cpp (no game, no runtime).
// HUB_INSTALLED=0: the game isn't installed (START GAME opens the installer).
// HUB_INSTALL=checking|ready|problem|installing|unpacking|done|failed: the
// installer's state to show.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hub_install.h"

namespace gta4ps5::hub::install {
std::vector<fs::directory_entry> ListDir(const fs::path& dir) {
  std::vector<fs::directory_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(*it);
  return out;
}
std::string Bytes(uint64_t n) {
  char buf[32];
  if (n >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", double(n) / double(1ull << 30));
  else std::snprintf(buf, sizeof(buf), "%.0f MB", double(n) / double(1ull << 20));
  return buf;
}
fs::path InstallFolder(const fs::path& app) { return app / "install"; }
fs::path GameRoot(const fs::path& app) { return app / "UserData" / "LibertyRecomp" / "game"; }
State Look(const fs::path&) {
  State s;
  const char* e = std::getenv("HUB_INSTALLED");
  s.installed = !e || std::strcmp(e, "0") != 0;
  s.iso = s.update = true;
  return s;
}
Sources Find(const fs::path&) {
  Sources s;
  s.disc = "/app0/install/Grand Theft Auto IV (USA) (En,Fr,De,Es,It).iso";
  s.disc_bytes = 7838695424ull;
  s.update = "/app0/install/TU_1A581VI_000000K000000.0000000000205";
  s.update_bytes = 32571392ull;
  return s;
}
uint64_t FreeSpace(const fs::path&) { return 0; }
std::string Drive::Name() const { return type == "ufs" ? "internal storage" : "an exFAT drive"; }
Drive DriveOf(const fs::path&) {
  Drive d;
  d.type = std::getenv("HUB_DRIVE") ? std::getenv("HUB_DRIVE") : "exfatfs";
  d.folder = "/mnt/usb0/homebrew/PPSA94004/UserData/LibertyRecomp";
  d.free = 412ull << 30;
  return d;
}

struct Job::Impl {
  Sources found;
  Status st;
};
Job::Job(const fs::path& app, const fs::path&) : impl_(std::make_unique<Impl>()) {
  impl_->found = Find(app);
  const std::string k = std::getenv("HUB_INSTALL") ? std::getenv("HUB_INSTALL") : "ready";
  Status& s = impl_->st;
  s.disc = "USA  \xC2\xB7  Retail 1.00";
  if (k == "checking") s.phase = Phase::kChecking, s.disc.clear();
  else if (k == "ready") s.phase = Phase::kReady;
  else if (k == "problem") {
    s.phase = Phase::kProblem;
    impl_->found.update.clear();
    s.problem = "Title Update 8 is missing. Copy TU_1A581VI_000000K000000.0000000000205 into the install folder, next "
                "to the disc image, and press Triangle.";
  } else if (k == "installing" || k == "unpacking") {
    s.phase = Phase::kInstalling;
    s.step = k == "installing" ? 1 : 2;
    s.progress = k == "installing" ? 0.31f : 0.72f;
    s.written = k == "installing" ? 4724464025ull : 11811160064ull;
    s.seconds = 300;
    s.rate = 214.0f * (1 << 20);
    s.eta = 410;
    s.read_rate = 1650.0f * (1 << 20);
    s.write_rate = 240.0f * (1 << 20);
    s.drive = "an exFAT drive";
  } else if (k == "done") {
    s.phase = Phase::kDone;
    s.progress = 1;
    s.seconds = 1130;
  } else if (k == "failed") {
    s.phase = Phase::kFailed;
    s.problem = "There is not enough free space for the GTA IV installation.";
  }
}
Job::~Job() = default;
const Sources& Job::Found() const { return impl_->found; }
Job::Status Job::Get() const { return impl_->st; }
void Job::Start() {}
void Job::Cancel() {}
}  // namespace gta4ps5::hub::install
