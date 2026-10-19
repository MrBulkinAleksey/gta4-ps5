// The hub preview's stand-in for hub_install.cpp (no game, no runtime).
// HUB_INSTALLED=0: the game isn't installed (START GAME opens the installer).
// HUB_INSTALL=checking|ready|problem|installing|unpacking|finishing|stopping|
// inapp|done|failed: the installer's state to show (inapp: installing without
// the installer payload, no loader on port 9021).
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
  d.device = "/dev/da0s1";
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
  } else if (k == "installing" || k == "unpacking" || k == "finishing" || k == "stopping" || k == "inapp") {
    s.phase = Phase::kInstalling;
    s.step = k == "unpacking" ? 2 : k == "finishing" ? 3 : 1;
    s.stopping = k == "stopping";
    s.progress = k == "unpacking" ? 0.72f : k == "finishing" ? 0.99f : 0.31f;
    s.written = k == "unpacking" ? 11811160064ull : k == "finishing" ? 17179869184ull : 4724464025ull;
    s.seconds = 140;
    s.rate = k == "inapp" ? 4.0f * (1 << 20) : 412.0f * (1 << 20);
    s.eta = k == "finishing" ? -1 : k == "inapp" ? 2900 : 95;
    s.read_rate = 1650.0f * (1 << 20);
    s.write_rate = k == "inapp" ? 2.0f * (1 << 20) : 520.0f * (1 << 20);
    s.drive = "internal storage";
    if (k == "inapp") {
      s.note = "No payload loader answered on port 9021, so the app is installing by itself. That's much slower.";
      s.diag_file = "Writing bronx_w.img (52 MB)";
      s.diag_time = "Time spent: reading 2 s, writing 289 s, flushing 0 s (228), sizing files 0 s  \xC2\xB7  298 MB memory free";
      s.diag_drive = "/app0/UserData/LibertyRecomp  \xC2\xB7  drive not known  \xC2\xB7  0 KB free  \xC2\xB7  disc image on ?";
    } else if (k != "finishing") {
      s.diag_file = k == "unpacking" ? "Writing bronx_e.img (61 MB)" : "Writing audio.rpf (2.9 GB)";
      s.diag_time = "Installer payload  \xC2\xB7  " + std::string(k == "unpacking" ? "unpacking" : "copying") +
                    "  \xC2\xB7  reading 9 s, writing 31 s";
      s.diag_drive = "/data/homebrew/PPSA94004  \xC2\xB7  ufs /dev/ssd0.user  \xC2\xB7  412.0 GB free";
    }
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
