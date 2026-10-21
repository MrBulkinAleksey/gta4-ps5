// GTA IV PS5 - the hub's check of the game's files, and its installer: the
// game is installed from the disc image and Title Update 8 in the app's
// install folder; after that it runs from UserData/LibertyRecomp/game. The
// episodes (The Lost and Damned, The Ballad of Gay Tony) are installed the same
// way from their Xbox 360 content packages, into UserData/LibertyRecomp/dlc.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gta4ps5::hub::install {

namespace fs = std::filesystem;

// Where the disc image and the title update go (<app>/install).
fs::path InstallFolder(const fs::path& app);
// The installed game (<app>/UserData/LibertyRecomp/game).
fs::path GameRoot(const fs::path& app);

// The episodes, in gta4::install::Episode's order.
constexpr int kEpisodeCount = 2;
// "The Lost and Damned", "The Ballad of Gay Tony" (as the game names them, in
// every language).
const char* EpisodeName(int episode);

struct FileProblem {
  std::string path;
  uint64_t expected = 0, found = 0;
  bool missing = false;
};

// What the app folder has.
struct State {
  bool installed = false;  // the game is installed and ready
  bool iso = false;        // a disc image waits in install/
  bool update = false;     // ...and a title update next to it
  std::string reason;      // why it isn't ready (from the installer's check)
  std::vector<FileProblem> problems;
  bool Installed() const { return installed || iso; }
  bool xex_ok = true;
  bool episode[kEpisodeCount] = {};        // installed
  bool episode_found[kEpisodeCount] = {};  // its package waits in install/
};
State Look(const fs::path& app);

// A folder's entries (none if it can't be read; never throws).
std::vector<fs::directory_entry> ListDir(const fs::path& dir);

// "6.6 GB" etc. for messages.
std::string Bytes(uint64_t n);

// What's in install/: the disc image (or an extracted game folder), the
// title update, and the episodes' content packages (or folders of their files).
struct Sources {
  fs::path disc, update;
  uint64_t disc_bytes = 0, update_bytes = 0;
  fs::path episode[kEpisodeCount];
  uint64_t episode_bytes[kEpisodeCount] = {};
  std::vector<fs::path> other_episodes;  // episode packages whose header's name doesn't say which
};
Sources Find(const fs::path& app);

// Free space where the game goes, in bytes.
uint64_t FreeSpace(const fs::path& install_root);

// The drive the game installs to: its filesystem ("ufs" the internal storage,
// "exfatfs" an external drive), the real folder behind /app0, its free space.
struct Drive {
  std::string type;    // empty: not known
  std::string folder;  // the install folder's real path (empty: not known)
  std::string device;  // what's mounted ("/dev/...", the real folder of a nullfs)
  uint64_t free = 0;
  bool internal() const { return type == "ufs"; }
  // "internal storage", "an exFAT drive"...
  std::string Name() const;
};
Drive DriveOf(const fs::path& path);  // (a folder or a file)

// About how much room the install takes, from the disc image's size.
inline uint64_t SpaceNeeded(uint64_t disc_bytes) { return disc_bytes * 9 / 4 + (512ull << 20); }

// The installer's work, on a thread of its own: it checks the disc image
// right away (the USA disc, the original release), and installs once started.
// The writing is done by the installer payload (ps5/payload/installer), sent to
// the console's payload loader on port 9021: the PS5 lets a title write only
// about a gigabyte quickly before slowing it to a few MB/s, and a payload
// isn't held back. With no loader the app installs by itself (slowly).
class Job {
 public:
  enum class Phase { kChecking, kReady, kProblem, kInstalling, kDone, kFailed, kCancelled };
  struct Status {
    Phase phase = Phase::kChecking;
    std::string disc;     // what the disc image is ("USA  ·  Retail 1.00"), or the episode's package
    std::string source;     // an episode's package (its file name), once found
    uint64_t source_bytes = 0;
    std::string problem;  // why it can't install, or why the install stopped
    // installing: 1 copying from the disc image, 2 unpacking the archives,
    // 3 checking the installed game and putting it in place
    int step = 0;
    bool stopping = false;  // stopped: what was copied is being removed
    std::string note;       // something to know about this install (no payload loader...)
    float progress = 0;     // 0..1
    uint64_t written = 0;  // bytes so far
    float seconds = 0;     // since the install started
    float rate = 0;        // bytes a second, lately
    float eta = -1;        // seconds left (-1: not known yet)
    float read_rate = 0, write_rate = 0;  // one thread's reading and writing speed, lately
    std::string drive;                    // what it installs to ("an exFAT drive")
    // what's going on, in detail (small print on the install screen)
    std::string diag_drive, diag_time, diag_file;
  };
  // `install_root`: UserData/LibertyRecomp (the game goes in its game/).
  // `episode`: one of the episodes instead (into its dlc/), 0 or 1.
  Job(const fs::path& app, const fs::path& install_root, int episode = -1);
  ~Job();
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  const Sources& Found() const;
  Status Get() const;
  void Start();   // once kReady
  void Cancel();  // while installing (what was copied is removed)

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace gta4ps5::hub::install
