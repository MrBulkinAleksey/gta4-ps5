// GTA IV PS5 - the hub's check of the game's files and its installer (see
// hub_install.h). The install itself is LibertyRecomp's (install/), written by
// the installer payload (ps5/payload/installer) when a payload loader runs it.
#include "hub_install.h"

#include "hub_lang.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/logging.h>
#include <rex/system/xcontent.h>

#if defined(__PROSPERO__)
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>
extern "C" int _fstatfs(int fd, struct statfs* buf);
extern "C" int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
#endif

#include "../../install/gta4_install_io.h"
#include "../../install/gta4_installer.h"
#include "../../install/gta4_source_inspector.h"

namespace gta4ps5::hub::install {

namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

// An Xbox 360 content package (a title update, an episode): CON / LIVE / PIRS.
bool IsPackage(const fs::path& file) {
  char magic[4] = {};
  std::ifstream in(file, std::ios::binary);
  if (!in.read(magic, 4)) return false;
  return !std::memcmp(magic, "CON ", 4) || !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4);
}

// A content package's kind, from its header (one small read): GTA IV's title
// update or an episode (marketplace content), or something else; for an
// episode, which, from the name in the header (-1: not known from it).
// (Opening an episode's package reads its whole file table: seconds, never
// done here, as the hub waits on this.)
enum class PackageKind { kOther, kUpdate, kEpisode };
struct PackageInfo {
  PackageKind kind = PackageKind::kOther;
  int episode = -1;
};
PackageInfo Inspect(const fs::path& file) {
  PackageInfo info;
  const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(file);
  if (!header || uint32_t(header->metadata.execution_info.title_id) != 0x545407F2) return info;
  const auto type = static_cast<uint32_t>(static_cast<rex::system::XContentType>(header->metadata.content_type));
  if (type == 0x000B0000) {
    info.kind = PackageKind::kUpdate;
  } else if (type == 0x00000002) {
    info.kind = PackageKind::kEpisode;
    std::string name;
    for (const char16_t c : header->metadata.display_name(rex::system::XLanguage::kEnglish))
      name += c < 128 ? char(std::tolower(int(c))) : ' ';
    if (name.find("lost") != std::string::npos && name.find("damned") != std::string::npos) info.episode = 0;
    else if (name.find("gay tony") != std::string::npos || name.find("ballad") != std::string::npos) info.episode = 1;
  }
  return info;
}

// A folder of an episode's files (the package's contents copied out), at its
// top or one folder down: which episode (-1: none). Only a few file checks.
int FolderEpisode(const fs::path& dir) {
  auto at = [](const fs::path& d) {
    std::error_code ec;
    if (!fs::is_regular_file(d / "setup2.xml", ec) || !fs::is_regular_file(d / "content.dat", ec)) return -1;
    const bool e1 = fs::is_regular_file(d / "e1_audio.xml", ec), e2 = fs::is_regular_file(d / "e2_audio.xml", ec);
    return e1 && !e2 ? 0 : e2 && !e1 ? 1 : -1;
  };
  if (const int e = at(dir); e >= 0) return e;
  for (const auto& sub : ListDir(dir)) {
    std::error_code ec;
    if (sub.is_directory(ec))
      if (const int e = at(sub.path()); e >= 0) return e;
  }
  return -1;
}

uint64_t FolderSize(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) total += it->file_size(ec);
  return total;
}

}  // namespace

std::vector<fs::directory_entry> ListDir(const fs::path& dir) {
  std::vector<fs::directory_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(*it);
  return out;
}

std::string Bytes(uint64_t n) {
  char buf[32];
  if (n >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", double(n) / double(1ull << 30));
  else if (n >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.0f MB", double(n) / double(1ull << 20));
  else std::snprintf(buf, sizeof(buf), "%.0f KB", double(n) / 1024.0);
  return buf;
}

fs::path InstallFolder(const fs::path& app) { return app / "install"; }
fs::path GameRoot(const fs::path& app) { return app / "UserData" / "LibertyRecomp" / "game"; }
namespace {
fs::path EpisodesRoot(const fs::path& app) { return app / "UserData" / "LibertyRecomp" / "dlc"; }
}  // namespace

const char* EpisodeName(int episode) { return episode == 1 ? "The Ballad of Gay Tony" : "The Lost and Damned"; }

State Look(const fs::path& app) {
  State s;
  s.installed = gta4::install::IsInstallReady(GameRoot(app), &s.reason);
  const Sources found = Find(app);
  s.iso = !found.disc.empty();
  s.update = !found.update.empty();
  for (int e = 0; e < kEpisodeCount; ++e) {
    s.episode[e] = gta4::install::IsEpisodeReady(EpisodesRoot(app), gta4::install::Episode(e));
    s.episode_found[e] = !found.episode[e].empty();
  }
  REXLOG_INFO("PS5 hub: installed {}, disc image {}, title update {}{}; episodes installed {}/{}, found {}/{}",
              s.installed, s.iso, s.update, s.installed ? "" : " (" + s.reason + ")", s.episode[0], s.episode[1],
              s.episode_found[0], s.episode_found[1]);
  return s;
}

Sources Find(const fs::path& app) {
  Sources s;
  std::error_code ec;
  auto entries = ListDir(InstallFolder(app));
  std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path() < b.path(); });
  fs::path package;
  uint64_t package_bytes = 0;
  for (const auto& e : entries) {
    const fs::path& p = e.path();
    const std::string name = Lower(p.filename().string());
    if (name.empty() || name[0] == '.') continue;
    if (e.is_directory(ec)) {
      // an extracted game: a folder with default.xex in it
      if (fs::is_regular_file(p / "default.xex", ec)) {
        if (s.disc.empty()) s.disc = p, s.disc_bytes = FolderSize(p);
        continue;
      }
      // an episode's files copied out of its package
      if (const int k = FolderEpisode(p); k >= 0 && s.episode[k].empty()) s.episode[k] = p, s.episode_bytes[k] = FolderSize(p);
      continue;
    }
    if (!e.is_regular_file(ec)) continue;
    const uint64_t size = e.file_size(ec);
    const std::string ext = Lower(p.extension().string());
    if (ext == ".iso") {
      if (s.disc.empty()) s.disc = p, s.disc_bytes = size;
    } else if (ext == ".xexp" || name.rfind("tu_", 0) == 0) {
      if (s.update.empty()) s.update = p, s.update_bytes = size;
    } else if (IsPackage(p)) {
      // (what it is, from its header)
      const PackageInfo info = Inspect(p);
      switch (info.kind) {
        case PackageKind::kEpisode:
          if (info.episode < 0) s.other_episodes.push_back(p);  // (opened when an episode is installed)
          else if (s.episode[info.episode].empty()) s.episode[info.episode] = p, s.episode_bytes[info.episode] = size;
          break;
        case PackageKind::kUpdate:
          if (s.update.empty()) s.update = p, s.update_bytes = size;
          break;
        case PackageKind::kOther:
          // (a package the header doesn't say: the smallest one, as the episodes
          // are far bigger)
          if (package.empty() || size < package_bytes) package = p, package_bytes = size;
          break;
      }
    }
  }
  if (s.update.empty() && !package.empty()) s.update = package, s.update_bytes = package_bytes;
  return s;
}

#if defined(__PROSPERO__)
namespace {
// The kernel's statfs for a path (the SDK's statvfs is a stand-in). A title
// can't open a folder, so for a folder a file is made in it for the moment.
bool StatFs(const fs::path& path, struct statfs& st) {
  std::error_code ec;
  int fd = -1;
  fs::path probe;
  if (fs::is_directory(path, ec)) {
    probe = path / ".drive-probe";
    fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  } else {
    fd = ::open(path.c_str(), O_RDONLY);
  }
  if (fd < 0) return false;
  const bool ok = _fstatfs(fd, &st) == 0;
  ::close(fd);
  if (!probe.empty()) ::unlink(probe.c_str());
  return ok;
}
}  // namespace
#endif

std::string Drive::Name() const {  // (shown in the hub: in its language)
  if (type.empty()) return hub::lang::T("the app's drive");
  if (type == "ufs") return hub::lang::T("internal storage");
  if (type.find("exfat") != std::string::npos) return hub::lang::T("an exFAT drive");
  if (type.find("msdos") != std::string::npos || type.find("fat") != std::string::npos) return hub::lang::T("a FAT drive");
  return hub::lang::F("a {} drive", {type});
}

Drive DriveOf(const fs::path& path) {
  Drive d;
#if defined(__PROSPERO__)
  std::error_code ec;
  if (!fs::exists(path, ec)) fs::create_directories(path, ec);
  struct statfs st {};
  if (!StatFs(path, st)) return d;
  d.type = st.f_fstypename;
  d.free = uint64_t(st.f_bavail) * uint64_t(st.f_bsize);
  d.folder = path.string();
  d.device = st.f_mntfromname;
  // (/app0 is a nullfs mount of the app's real folder: follow it to the drive)
  if (d.type == "nullfs") {
    const std::string on = st.f_mntonname, from = st.f_mntfromname, p = path.string();
    if (p.rfind(on, 0) == 0) d.folder = from + p.substr(on.size());
    d.type = "nullfs";
    struct statfs lower {};
    if (StatFs(d.folder, lower)) {
      d.type = lower.f_fstypename;
      d.free = uint64_t(lower.f_bavail) * uint64_t(lower.f_bsize);
      d.device = lower.f_mntfromname;
    }
  }
#else
  (void)path;
#endif
  return d;
}

uint64_t FreeSpace(const fs::path& install_root) {
  std::error_code ec;
  fs::create_directories(install_root, ec);
#if defined(__PROSPERO__)
  if (const Drive d = DriveOf(install_root); !d.type.empty()) return d.free;
#endif
  const auto space = fs::space(install_root, ec);
  // (on the PS5 a title can't learn the drive's free space: the SDK's
  // statvfs always answers 16 GiB, so that means "not known")
  if (ec || uint64_t(space.available) == (16ull << 30)) return 0;
  return uint64_t(space.available);
}

// ------------------------------------------------------------------ the installer payload

namespace {

using Clock = std::chrono::steady_clock;

// The installer payload, packaged in the app (ps5/payload/installer).
constexpr const char* kPayloadElf = "payloads/gta4-ps5-installer.elf";
// The install's job files, in the install root. The payload looks for them at
// <the app's real folder>/UserData/LibertyRecomp/.install-job.
constexpr const char* kJobDir = ".install-job";

// `path` as a path inside the app's folder ("install/x.iso"), or "" when it
// isn't in it.
std::string InsideApp(const fs::path& app, const fs::path& path) {
  const fs::path rel = path.lexically_normal().lexically_relative(app.lexically_normal());
  if (rel.empty() || rel.is_absolute()) return {};
  const std::string first = rel.begin()->string();
  if (first == ".." || first == ".") return {};
  return rel.generic_string();
}

bool ReadText(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Whole, at once: written beside, then renamed over (never read half done).
bool WriteText(const fs::path& path, const std::string& text) {
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.write(text.data(), std::streamsize(text.size()))) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  return !ec;
}

std::string Field(const std::string& text, const char* key) {
  const std::string k = std::string(key) + "=";
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    if (text.compare(at, k.size(), k) == 0) return text.substr(at + k.size(), end - at - k.size());
    at = end + 1;
  }
  return {};
}

uint64_t Number(const std::string& text, const char* key) {
  return std::strtoull(Field(text, key).c_str(), nullptr, 10);
}

// Seconds since `path` was written (a lot when it isn't there).
double Age(const fs::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 1e9;
  return std::difftime(std::time(nullptr), st.st_mtime);
}

// What the payload last wrote to status.txt.
struct PayloadStatus {
  std::string token, phase, error, file, folder, drive;
  int step = 0;
  uint64_t copy_done = 0, copy_total = 0, unpack_done = 0, unpack_total = 0, unpack_guess = 0;
  uint64_t file_size = 0, read_bytes = 0, read_ns = 0, write_bytes = 0, write_ns = 0, free = 0, beat = 0;
  float seconds = 0;
  // still at it (the phases before done, failed or stopped)
  bool Working() const { return phase != "done" && phase != "failed" && phase != "stopped"; }
};

bool ReadStatus(const fs::path& dir, PayloadStatus& s) {
  std::string text;
  if (!ReadText(dir / "status.txt", text)) return false;
  s.token = Field(text, "token");
  s.phase = Field(text, "phase");
  s.error = Field(text, "error");
  s.file = Field(text, "file");
  s.folder = Field(text, "folder");
  s.drive = Field(text, "drive");
  s.step = int(Number(text, "step"));
  s.copy_done = Number(text, "copy_done");
  s.copy_total = Number(text, "copy_total");
  s.unpack_done = Number(text, "unpack_done");
  s.unpack_total = Number(text, "unpack_total");
  s.unpack_guess = Number(text, "unpack_guess");
  s.file_size = Number(text, "file_size");
  s.read_bytes = Number(text, "read_bytes");
  s.read_ns = Number(text, "read_ns");
  s.write_bytes = Number(text, "write_bytes");
  s.write_ns = Number(text, "write_ns");
  s.free = Number(text, "free");
  s.beat = Number(text, "beat");
  s.seconds = std::strtof(Field(text, "seconds").c_str(), nullptr);
  return !s.token.empty() && !s.phase.empty();
}

// The payload to the console's payload loader (etaHEN, elfldr or any other)
// on 127.0.0.1:9021, which runs it as a process of its own.
bool SendToLoader(const std::string& elf, int& err) {
  err = 0;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = errno;
    return false;
  }
  timeval limit{};
  limit.tv_sec = 3;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
#ifdef SO_NOSIGPIPE
  const int one = 1;  // (a loader that hangs up mustn't end the app)
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(9021);
  sa.sin_addr.s_addr = htonl(0x7F000001);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    err = errno;
    ::close(fd);
    return false;
  }
  size_t at = 0;
  while (at < elf.size()) {
    const ssize_t n = ::send(fd, elf.data() + at, elf.size() - at, flags);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      err = errno;
      ::close(fd);
      return false;
    }
    at += size_t(n);
  }
  ::shutdown(fd, SHUT_WR);
  ::close(fd);
  return true;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : bytes) out += kDigits[b >> 4], out += kDigits[b & 15];
  return out;
}

std::string Seconds(uint64_t ns) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f s", double(ns) / 1e9);
  return buf;
}

}  // namespace

// ------------------------------------------------------------------ the job

struct Job::Impl {
  enum class Outcome { kUnavailable, kDone, kFailed, kStopped, kLeft };

  fs::path app, root;
  Sources found;
  mutable std::mutex m;
  std::condition_variable cv;
  Status st;
  bool go = false;
  std::atomic<bool> quit{false}, stop{false};
  gta4::install::Progress progress;
  Clock::time_point started;
  std::thread thread, monitor;
  std::atomic<bool> monitoring{false};
  Drive drive;
  std::string where;    // the install drive, for the screen (installing in the app)
  std::string failure;  // why the payload's install failed
  std::string token;    // the payload's job
  // an episode instead of the game: which (0, 1; -1 the game), its package or
  // folder and what's copied from it
  int episode = -1;
  fs::path episode_source;
  gta4::install::EpisodePlan episode_plan;

  fs::path JobDir() const { return root / kJobDir; }

  void Set(Phase phase, std::string problem = {}) {
    std::lock_guard lock(m);
    st.phase = phase;
    st.problem = std::move(problem);
    st.stopping = false;
  }

  // The job's files, once it's over ("running" stays: a payload's lock).
  void ClearJob() {
    std::error_code ec;
    for (const char* name : {"job.txt", "job.txt.tmp", "job.taken", "cancel", "cancel.tmp", "status.txt", "status.tmp"})
      fs::remove(JobDir() / name, ec);
  }

  // A job no payload has taken yet, taken back (job.txt deleted). False when one
  // took it first (it renames job.txt to job.taken), so exactly one of them has it.
  bool TakeBack() {
    std::error_code ec;
    return fs::remove(JobDir() / "job.txt", ec) && !ec;
  }

  // A payload at work right now (its status written in the last 15 s).
  bool PayloadWorking(PayloadStatus& s) const {
    return ReadStatus(JobDir(), s) && s.Working() && Age(JobDir() / "status.txt") <= 15;
  }

  // ---------------------------------------------- with the installer payload

  // How fast it's going, from the payload's numbers over the last second or so.
  struct Rates {
    Clock::time_point at{}, logged{};
    uint64_t written = 0, rb = 0, rns = 0, wb = 0, wns = 0;
    float rate = 0, read = 0, write = 0, shown = 0;
  };

  void Show(const PayloadStatus& s, Rates& r) {
    const auto now = Clock::now();
    const uint64_t written = s.copy_done + s.unpack_done;
    const uint64_t unpack = std::max(s.unpack_total, s.unpack_guess);
    // two steps: the disc image's files copied (45%), the archives unpacked
    // (their total grows as each one is opened, so it starts as a guess)
    float f = 0;
    if (episode >= 0) {
      // (an episode: copying, and that's all)
      f = s.copy_total ? float(double(s.copy_done) / double(s.copy_total)) : 0;
    } else if (s.step >= 2) {
      f = 0.45f + 0.55f * float(double(s.unpack_done) / double(std::max<uint64_t>(unpack, 1)));
    } else if (s.copy_total) {
      f = 0.45f * float(double(s.copy_done) / double(s.copy_total));
    }
    r.shown = std::max(r.shown, std::min(f, 0.99f));
    if (r.at == Clock::time_point{}) {
      r.at = now, r.written = written, r.rb = s.read_bytes, r.rns = s.read_ns, r.wb = s.write_bytes, r.wns = s.write_ns;
    }
    const float dt = std::chrono::duration<float>(now - r.at).count();
    if (dt >= 1.0f) {
      const float w = float(double(written - std::min(written, r.written)) / dt);
      r.rate = r.rate > 0 ? r.rate * 0.6f + w * 0.4f : w;
      // (bytes over the time spent at it: one thread's speed)
      if (s.read_ns > r.rns) r.read = float(double(s.read_bytes - std::min(s.read_bytes, r.rb)) / (double(s.read_ns - r.rns) / 1e9));
      if (s.write_ns > r.wns) r.write = float(double(s.write_bytes - std::min(s.write_bytes, r.wb)) / (double(s.write_ns - r.wns) / 1e9));
      r.at = now, r.written = written, r.rb = s.read_bytes, r.rns = s.read_ns, r.wb = s.write_bytes, r.wns = s.write_ns;
    }
    Drive d;
    d.type = s.drive.substr(0, s.drive.find(' '));
    std::lock_guard lock(m);
    st.step = s.step >= 2 ? 2 : 1;
    st.progress = r.shown;
    st.written = written;
    st.seconds = std::chrono::duration<float>(now - started).count();
    st.rate = r.rate;
    st.read_rate = r.read;
    st.write_rate = r.write;
    if (!d.type.empty()) st.drive = d.Name();
    // time left: what's still to write, at the speed of late
    st.eta = -1;
    if (s.copy_total && r.rate > 0 && s.seconds > 8) {
      const uint64_t expected = episode >= 0 ? s.copy_total : s.copy_total + (unpack ? unpack : s.copy_total * 23 / 20);
      st.eta = float(double(expected - std::min(expected, written)) / double(r.rate));
    }
    st.diag_file = s.file.empty() ? std::string() : "Writing " + s.file + " (" + Bytes(s.file_size) + ")";
    st.diag_time = "Installer payload  \xC2\xB7  " + s.phase + "  \xC2\xB7  reading " + Seconds(s.read_ns) +
                   ", writing " + Seconds(s.write_ns);
    st.diag_drive = s.folder + "  \xC2\xB7  " + (s.drive.empty() ? std::string("drive not known") : s.drive) +
                    "  \xC2\xB7  " + Bytes(s.free) + " free";
    if (now - r.logged >= std::chrono::seconds(10)) {
      r.logged = now;
      REXLOG_INFO("PS5 installer: payload {} step {} {:.1f}%: copied {} of {}, unpacked {} of {}, {:.0f} MB/s now "
                  "(read {:.0f} / write {:.0f} MB/s per thread); {}",
                  s.phase, st.step, st.progress * 100, Bytes(s.copy_done), Bytes(s.copy_total), Bytes(s.unpack_done),
                  Bytes(unpack), double(r.rate) / double(1 << 20), double(r.read) / double(1 << 20),
                  double(r.write) / double(1 << 20), st.diag_file);
    }
  }

  // Follows the payload's status until it's done, failed or stopped. Stopping
  // (Cancel) is passed on as the cancel file.
  Outcome Follow(const std::string& token, bool attached) {
    const auto sent = Clock::now();
    auto changed = sent;
    uint64_t beat = 0;
    bool running = attached, asked = false;
    Rates rates;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if (quit.load()) return Outcome::kLeft;  // (the app is closing: the payload carries on)
      const auto now = Clock::now();
      if (stop.load() && !asked) {
        asked = true;
        // (not taken yet: taken back, and that's the end of it)
        if (!running && TakeBack()) return Outcome::kStopped;
        WriteText(JobDir() / "cancel", "stop\n");
        REXLOG_INFO("PS5 installer: asked the payload to stop");
        std::lock_guard lock(m);
        st.stopping = true;
      }
      PayloadStatus s;
      if (ReadStatus(JobDir(), s) && s.token == token) {
        if (!running) {
          REXLOG_INFO("PS5 installer: the payload started after {:.1f} s",
                      std::chrono::duration<float>(now - sent).count());
        }
        running = true;
        if (s.beat != beat) beat = s.beat, changed = now;
        if (s.phase == "done") {
          Show(s, rates);
          return Outcome::kDone;
        }
        if (s.phase == "stopped") return Outcome::kStopped;
        if (s.phase == "failed") {
          failure = s.error.empty() ? std::string("The installer payload stopped with an error.") : s.error;
          return Outcome::kFailed;
        }
        if (s.phase == "cleaning") {
          std::lock_guard lock(m);
          st.stopping = true;
        } else if (!asked) {
          Show(s, rates);
        }
      }
      if (!running && now - sent > std::chrono::seconds(15)) {
        // Not started: the job taken back (one that starts late finds none), and the
        // app installs by itself. Taken but quiet: given longer.
        if (TakeBack()) return Outcome::kUnavailable;
        if (now - sent > std::chrono::seconds(45)) {
          WriteText(JobDir() / "cancel", "stop\n");
          failure = "The installer payload didn't start.";
          return Outcome::kFailed;
        }
      }
      if (running && now - changed > std::chrono::seconds(30)) {
        failure = "The installer payload stopped answering.";
        return Outcome::kFailed;
      }
    }
  }

  // The install, written by the payload. kUnavailable (and why) when it can't
  // be used: then the app installs by itself.
  // The job written and the payload sent to the loader (kUnavailable: no
  // loader, or no payload in the app).
  Outcome SendJob(const std::string& body, std::string& why_not) {
    std::string elf;
    if (!ReadText(app / kPayloadElf, elf) || elf.size() < 64 || elf.compare(0, 4, "\x7F" "ELF") != 0) {
      why_not = "The installer payload is missing from the app";
      return Outcome::kUnavailable;
    }
    // the job (an earlier one's files gone first; never while a payload is at it)
    const fs::path dir = JobDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (PayloadStatus busy; PayloadWorking(busy)) {
      failure = "An earlier install is still running. Wait for it to finish, then try again.";
      return Outcome::kFailed;
    }
    ClearJob();
    char made[48];
    std::snprintf(made, sizeof(made), "%lld-%lld", (long long)std::time(nullptr),
                  (long long)(Clock::now().time_since_epoch().count() % 1000000));
    token = made;
    if (!WriteText(dir / "job.txt", "token=" + token + "\n" + body)) {
      why_not = "The install job couldn't be written";
      return Outcome::kUnavailable;
    }
    int err = 0;
    if (!SendToLoader(elf, err)) {
      fs::remove(dir / "job.txt", ec);
      REXLOG_WARN("PS5 installer: no payload loader answered on port 9021 (errno {})", err);
      why_not = "No payload loader answered on port 9021";
      return Outcome::kUnavailable;
    }
    REXLOG_INFO("PS5 installer: sent the installer payload ({} KB) to the payload loader on port 9021, job {}",
                elf.size() / 1024, token);
    {
      std::lock_guard lock(m);
      st.diag_time = "Starting the installer payload";
    }
    const Outcome outcome = Follow(token, false);
    if (outcome == Outcome::kUnavailable) why_not = "The payload loader didn't start the installer";
    return outcome;
  }

  // An episode, written by the payload.
  Outcome EpisodeWithPayload(std::string& why_not) {
    if (root.lexically_normal() != (app / "UserData" / "LibertyRecomp").lexically_normal()) {
      why_not = "The install folder isn't where the installer payload looks for it";
      return Outcome::kUnavailable;
    }
    const std::string source = InsideApp(app, episode_source), staging = InsideApp(app, root / ".install-staging");
    if (source.empty() || staging.empty()) {
      why_not = "The episode isn't in the app's folder";
      return Outcome::kUnavailable;
    }
    return SendJob("kind=episode\nstaging=" + staging + "\nsource=" + source +
                       "\nsource_kind=" + (episode_plan.package ? "package" : "folder") + "\nroot=" +
                       episode_plan.root + "\nname=" + gta4::install::EpisodeDirectory(gta4::install::Episode(episode)) +
                       "\nneeded=" + std::to_string(EpisodeSpaceNeeded()) + "\n",
                   why_not);
  }

  uint64_t EpisodeSpaceNeeded() const { return episode_plan.copy_bytes + (256ull << 20); }

  Outcome WithPayload(std::string& why_not) {
    if (episode >= 0) return EpisodeWithPayload(why_not);
    // (the payload finds the job at <the app's real folder>/UserData/LibertyRecomp)
    if (root.lexically_normal() != (app / "UserData" / "LibertyRecomp").lexically_normal()) {
      why_not = "The install folder isn't where the installer payload looks for it";
      return Outcome::kUnavailable;
    }
    const std::string image = InsideApp(app, found.disc), update = InsideApp(app, found.update),
                      staging = InsideApp(app, root / ".install-staging");
    if (image.empty() || update.empty() || staging.empty()) {
      why_not = "The disc image isn't in the app's folder";
      return Outcome::kUnavailable;
    }
    gta4::install::Selection selection;
    selection.game_source = found.disc;
    selection.update_source = found.update;
    gta4::install::InstallPlan plan;
    if (!gta4::install::PlanInstall(selection, plan, failure)) return Outcome::kFailed;
    std::error_code kind_ec;
    return SendJob("kind=install\nstaging=" + staging + "\nimage=" + image + "\nimage_kind=" +
                       (fs::is_directory(found.disc, kind_ec) ? "folder" : "iso") + "\ndisc_root=" + plan.disc_root +
                       "\nupdate=" + update + "\nupdate_kind=" + (plan.update_package ? "package" : "xexp") +
                       "\nupdate_root=" + plan.update_root + "\nupdate_xexp=" + plan.update_xexp +
                       "\nkey=" + Hex(plan.aes_key) + "\nneeded=" + std::to_string(SpaceNeeded(found.disc_bytes)) +
                       "\nthreads=3\n",
                   why_not);
  }

  // The payload's work checked against the disc image, and put in place.
  void Finish(const std::string& token) {
    {
      std::lock_guard lock(m);
      st.step = 3;
      st.progress = std::max(st.progress, 0.99f);
      st.eta = -1;
      st.rate = 0;
      st.stopping = false;
      st.diag_file.clear();
    }
    gta4::install::Selection selection;
    selection.game_source = found.disc;
    selection.update_source = found.update;
    const auto t0 = Clock::now();
    // (the staging folder carries the token of the job that filled it)
    std::string mark;
    gta4::install::Result result;
    if (!ReadText(root / ".install-staging" / ".token", mark) || mark != token) {
      result.error = "The installed files aren't from this install. Press Cross to install again.";
    } else if (episode >= 0) {
      result = gta4::install::FinishEpisode(episode_source, gta4::install::Episode(episode), root);
    } else {
      result = gta4::install::FinishInstall(selection, root);
    }
    ClearJob();
    {
      std::lock_guard lock(m);
      st.seconds = std::chrono::duration<float>(Clock::now() - started).count();
    }
    if (result.success) {
      REXLOG_INFO("PS5 installer: done in {:.0f} s (checked and put in place in {:.1f} s)", st.seconds,
                  std::chrono::duration<float>(Clock::now() - t0).count());
      Set(Phase::kDone);
    } else {
      REXLOG_ERROR("PS5 installer: the installed game didn't check out: {}", result.error);
      Set(Phase::kFailed, result.error);
    }
  }

  void Conclude(Outcome outcome, const std::string& token) {
    switch (outcome) {
      case Outcome::kDone:
        Finish(token);
        break;
      case Outcome::kFailed:
        // (a payload that stopped answering may still be there: what it wrote is left
        // to the next install, which empties the staging folder first)
        REXLOG_ERROR("PS5 installer: failed: {}", failure);
        ClearJob();
        Set(Phase::kFailed, failure);
        break;
      case Outcome::kStopped:
        REXLOG_INFO("PS5 installer: stopped");
        ClearJob();
        Set(Phase::kCancelled);
        break;
      case Outcome::kLeft:
      case Outcome::kUnavailable:
        break;
    }
  }

  // An install from before: the app was closed while the payload worked. One
  // still going is followed again; one that finished is checked and put in
  // place. True when it took over.
  bool Resume() {
    const fs::path dir = JobDir();
    std::string job;
    PayloadStatus s;
    if (!ReadText(dir / "job.taken", job) || !ReadStatus(dir, s) || s.token != Field(job, "token")) return false;
    // (only this kind of install's: the game's, or this episode's)
    const std::string kind = Field(job, "kind");
    if (episode >= 0 ? kind != "episode" ||
                           Field(job, "name") != gta4::install::EpisodeDirectory(gta4::install::Episode(episode))
                     : kind == "episode")
      return false;
    token = s.token;
    std::error_code ec;
    if (s.phase == "done") {
      const fs::path staged = episode >= 0 ? root / ".install-staging" / "dlc" /
                                                 gta4::install::EpisodeDirectory(gta4::install::Episode(episode))
                                           : root / ".install-staging" / "game";
      if (!fs::is_directory(staged, ec)) return false;
      REXLOG_INFO("PS5 installer: the payload finished an install while the app was closed; finishing it");
      {
        std::lock_guard lock(m);
        st.phase = Phase::kInstalling;
        started = Clock::now();
      }
      Finish(token);
      return true;
    }
    if (!s.Working() || Age(dir / "status.txt") > 15) return false;
    if (s.phase == "cleaning") {
      // (one that was stopped, still removing its files: waited for)
      REXLOG_INFO("PS5 installer: waiting for an earlier install to finish removing its files");
      for (int i = 0; i < 1200 && !quit.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!ReadStatus(dir, s) || s.phase != "cleaning" || Age(dir / "status.txt") > 15) break;
      }
      return false;
    }
    REXLOG_INFO("PS5 installer: an install from before is still going ({}); following it", s.phase);
    {
      std::lock_guard lock(m);
      st.phase = Phase::kInstalling;
      started = Clock::now() - std::chrono::milliseconds(int64_t(s.seconds * 1000));
    }
    Conclude(Follow(token, true), token);
    return true;
  }

  // ---------------------------------------------- in the app (no payload loader)

  // While installing: once a second, the numbers for the screen; every 10 s,
  // a line in the log. On its own thread, so the screen never waits on the
  // drive.
  void Monitor() {
    uint64_t copy_total = 0, unpack_guess = 0;
    float shown = 0, rate = 0, read_rate = 0, write_rate = 0;
    Clock::time_point rate_at = Clock::now(), io_at = rate_at, logged_at{};
    uint64_t rate_bytes = 0, io_rb = 0, io_rns = 0, io_wb = 0, io_wns = 0;
    while (monitoring.load()) {
      for (int i = 0; i < 10 && monitoring.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!monitoring.load()) break;
      const auto now = Clock::now();
      const uint64_t done = progress.copied_bytes.load(), total = progress.total_bytes.load();
      // Two steps: the disc image's files are copied (the total known up
      // front), then the archives are unpacked (their total grows as each one
      // is opened, so it's guessed from the archives' sizes).
      if (!copy_total && total) copy_total = total;
      int step = 1;
      float f = 0;
      if (episode >= 0) {
        f = total ? float(double(done) / double(total)) : 0;  // (an episode: copying, and that's all)
      } else if (copy_total && total > copy_total) {
        if (!unpack_guess) {
          const fs::path staged = root / ".install-staging" / "game";
          std::error_code ec;
          for (const char* rpf : {"common.rpf", "xbox360.rpf", "audio.rpf"}) unpack_guess += fs::file_size(staged / rpf, ec);
          unpack_guess = unpack_guess * 23 / 20;
        }
        const uint64_t unpack_total = std::max(total - copy_total, unpack_guess);
        step = 2;
        f = 0.45f + 0.55f * float(double(done - std::min(done, copy_total)) / double(std::max<uint64_t>(unpack_total, 1)));
      } else if (copy_total) {
        f = 0.45f * float(double(done) / double(copy_total));
      }
      shown = std::max(shown, std::min(f, 0.995f));
      const float dt = std::chrono::duration<float>(now - rate_at).count();
      if (dt >= 1.0f) {
        const float r = float(double(done - std::min(done, rate_bytes)) / dt);
        rate = rate > 0 ? rate * 0.6f + r * 0.4f : r;
        rate_at = now, rate_bytes = done;
      }
      const auto& io = gta4::install::io::GetStats();
      const uint64_t rb = io.read_bytes.load(), rns = io.read_ns.load(), wb = io.write_bytes.load(),
                     wns = io.write_ns.load();
      if (now - io_at >= std::chrono::seconds(2)) {
        // (bytes over the time spent at it: one thread's speed)
        if (rns > io_rns) read_rate = float(double(rb - io_rb) / (double(rns - io_rns) / 1e9));
        if (wns > io_wns) write_rate = float(double(wb - io_wb) / (double(wns - io_wns) / 1e9));
        io_at = now, io_rb = rb, io_rns = rns, io_wb = wb, io_wns = wns;
      }
      size_t flexible = 0;
#if defined(__PROSPERO__)
      sceKernelAvailableFlexibleMemorySize(&flexible);
#endif
      std::string current;
      uint64_t current_size = 0;
      gta4::install::io::Current(current, current_size);
      const float seconds = std::chrono::duration<float>(now - started).count();
      char times[256];
      std::snprintf(times, sizeof(times),
                    "Time spent: reading %.0f s, writing %.0f s, flushing %.0f s (%llu), sizing files %.0f s  "
                    "\xC2\xB7  %s memory free",
                    double(rns) / 1e9, double(wns) / 1e9, double(io.sync_ns.load()) / 1e9,
                    (unsigned long long)io.syncs.load(), double(io.reserve_ns.load()) / 1e9, Bytes(flexible).c_str());
      {
        std::lock_guard lock(m);
        st.written = done;
        st.seconds = seconds;
        st.step = step;
        st.progress = shown;
        st.rate = rate;
        st.read_rate = read_rate;
        st.write_rate = write_rate;
        st.diag_drive = where;
        st.diag_time = times;
        st.diag_file = current.empty() ? std::string()
                                       : "Writing " + fs::path(current).filename().string() + " (" +
                                             Bytes(current_size) + ")";
        // time left: what's still to write, at the speed of late
        st.eta = -1;
        if (copy_total && rate > 0 && seconds > 10) {
          const uint64_t unpack = episode >= 0           ? 0
                                  : total > copy_total ? std::max(total - copy_total, unpack_guess)
                                                       : copy_total * 23 / 20;
          const uint64_t expected = copy_total + unpack;
          st.eta = float(double(expected - std::min(expected, done)) / double(rate));
        }
      }
      if (now - logged_at >= std::chrono::seconds(10)) {
        logged_at = now;
        REXLOG_INFO("PS5 installer: step {} {:.1f}% {} of {} in {:.0f} s ({:.0f} MB/s now, read {:.0f} / write {:.0f} "
                    "MB/s per thread); {}; {} ({})",
                    step, shown * 100, Bytes(done), Bytes(total), seconds, double(rate) / double(1 << 20),
                    double(read_rate) / double(1 << 20), double(write_rate) / double(1 << 20), times, current,
                    Bytes(current_size));
      }
    }
  }

  void InApp() {
    {
      // how to write, for the drive it writes to (gta4_install_io.h): the
      // internal storage (ufs) past the file cache, three files at a time;
      // any other drive (exFAT and the like) one file at a time, each made
      // bigger in 128 MB steps ahead of the writing
      auto& policy = gta4::install::io::Policy();
      const bool internal = drive.internal();
      policy.preallocate = !internal;
      policy.reserve_step = 128ull << 20;
      policy.direct = internal;
      policy.threads = internal ? 3 : 1;
      std::lock_guard lock(m);
      st.drive = drive.Name();
      REXLOG_INFO("PS5 installer: writing with {} thread(s), {} writes, {}", policy.threads,
                  policy.direct ? "direct" : "cached", policy.preallocate ? "files sized in 128 MB steps" : "no sizing");
    }
    monitoring = true;
    monitor = std::thread([this] { Monitor(); });
    gta4::install::Selection selection;
    if (episode >= 0) {
      // (an episode on its own: the installed game is checked, and kept)
      selection.dlc_sources.push_back({gta4::install::Episode(episode), episode_source});
    } else {
      selection.game_source = found.disc;
      selection.update_source = found.update;
    }
    const auto result = gta4::install::Install(selection, root, progress);
    monitoring = false;
    if (monitor.joinable()) monitor.join();
    if (result.success) {
      REXLOG_INFO("PS5 installer: done in {:.0f} s", std::chrono::duration<float>(Clock::now() - started).count());
      Set(Phase::kDone);
    } else if (progress.cancel_requested.load()) {
      REXLOG_INFO("PS5 installer: cancelled");
      Set(Phase::kCancelled);
    } else {
      REXLOG_ERROR("PS5 installer: failed: {}", result.error);
      Set(Phase::kFailed, result.error);
    }
  }

  // ---------------------------------------------- the job

  // An episode: its package checked, then installed as the game is.
  void RunEpisode() {
    const char* name = EpisodeName(episode);
    episode_source = found.episode[episode];
    // (a package whose name didn't say which episode: opened to see)
    for (size_t i = 0; episode_source.empty() && i < found.other_episodes.size(); ++i) {
      gta4::install::EpisodePlan plan;
      std::string why;
      if (gta4::install::PlanEpisode(found.other_episodes[i], plan, why) && int(plan.episode) == episode)
        episode_source = found.other_episodes[i];
    }
    if (!episode_source.empty()) {
      std::error_code ec;
      std::lock_guard lock(m);
      st.source = episode_source.filename().string();
      st.source_bytes = fs::is_directory(episode_source, ec) ? FolderSize(episode_source) : fs::file_size(episode_source, ec);
    }
    REXLOG_INFO("PS5 installer: episode {} from '{}'", name, episode_source.string());
    std::string reason;
    if (!gta4::install::IsInstallReady(GameRoot(app), &reason)) {
      Set(Phase::kProblem, hub::lang::T("Install GTA IV first. The episodes are added to the installed game."));
      return;
    }
    if (episode_source.empty()) {
      Set(Phase::kProblem,
          hub::lang::F("There's no package for {} in the install folder. Copy it from your Xbox 360 (on its drive "
                       "it's in Content/0000000000000000/545407F2/00000002) into /data/homebrew/PPSA94004/install/ "
                       "and press Triangle.",
                       {name}));
      return;
    }
    std::string why;
    if (!gta4::install::PlanEpisode(episode_source, episode_plan, why) || int(episode_plan.episode) != episode) {
      Set(Phase::kProblem, hub::lang::F("This package can't be used: {}", {why}));
      return;
    }
    {
      std::lock_guard lock(m);
      st.disc = std::string(hub::lang::T(episode_plan.package ? "Xbox 360 package" : "Folder")) + "  \xC2\xB7  " +
                hub::lang::Units(Bytes(episode_plan.copy_bytes));
    }
    drive = DriveOf(root);
    where = (drive.folder.empty() ? root.string() : drive.folder) + "  \xC2\xB7  " +
            (drive.type.empty() ? std::string("drive not known") : drive.type + " " + drive.device) + "  \xC2\xB7  " +
            Bytes(drive.free) + " free";
    if (Resume() || quit.load()) return;
    const uint64_t needed = EpisodeSpaceNeeded();
    if (drive.free && drive.free < needed && drive.free + FolderSize(root / ".install-staging") < needed) {
      Set(Phase::kProblem,
          hub::lang::F("There isn't enough free space. The install needs about {} and the drive has {} free.",
                       {hub::lang::Units(Bytes(needed)), hub::lang::Units(Bytes(drive.free))}));
      return;
    }
    Set(Phase::kReady);
    {
      std::unique_lock lock(m);
      cv.wait(lock, [this] { return go || quit.load(); });
      if (quit.load()) return;
      st.phase = Phase::kInstalling;
      started = Clock::now();
    }
    std::string why_not;
    const Outcome outcome = WithPayload(why_not);
    if (outcome != Outcome::kUnavailable) return Conclude(outcome, token);
    if (stop.load()) return Conclude(Outcome::kStopped, token);
    REXLOG_WARN("PS5 installer: {}; installing the episode in the app instead", why_not);
    {
      std::lock_guard lock(m);
      st.note = why_not + ", so the app is installing by itself. That's much slower.";
      st.diag_time.clear();
    }
    InApp();
  }

  void Run() {
    if (episode >= 0) return RunEpisode();
    REXLOG_INFO("PS5 installer: disc '{}', title update '{}'", found.disc.string(), found.update.string());
    if (found.disc.empty()) {
      Set(Phase::kProblem,
          hub::lang::T("There's no disc image in the install folder. Copy your GTA IV disc image (.iso) into "
                       "/data/homebrew/PPSA94004/install/ and press Triangle."));
      return;
    }
    const auto inspected = gta4::install::InspectGameSource(found.disc);
    if (!inspected.supported()) {
      Set(Phase::kProblem,
          hub::lang::F("This disc image can't be used: {}\nIt has to be the USA disc of GTA IV, the original release.",
                       {inspected.rejection_reason}));
      return;
    }
    {
      std::lock_guard lock(m);
      st.disc = gta4::install::FormatXexRegion(inspected.region) + "  \xC2\xB7  " + inspected.release_label;
    }
    if (found.update.empty()) {
      Set(Phase::kProblem,
          hub::lang::T("Title Update 8 is missing. Copy TU_1A581VI_000000K000000.0000000000205 into the install "
                       "folder, next to the disc image, and press Triangle."));
      return;
    }
    drive = DriveOf(root);
    const Drive disc_drive = DriveOf(found.disc);
    where = (drive.folder.empty() ? root.string() : drive.folder) + "  \xC2\xB7  " +
            (drive.type.empty() ? std::string("drive not known") : drive.type + " " + drive.device) + "  \xC2\xB7  " +
            Bytes(drive.free) + " free  \xC2\xB7  disc image on " +
            (disc_drive.type.empty() ? std::string("?") : disc_drive.type + " " + disc_drive.device);
    REXLOG_INFO("PS5 installer: installs to {}", where);
    // an install from before: still going, or finished while the app was closed
    if (Resume() || quit.load()) return;
    // (an earlier try's files in the staging folder are removed first: they count as free)
    const uint64_t needed = SpaceNeeded(found.disc_bytes);
    if (drive.free && drive.free < needed && drive.free + FolderSize(root / ".install-staging") < needed) {
      Set(Phase::kProblem,
          hub::lang::F("There isn't enough free space. The install needs about {} and the drive has {} free.",
                       {hub::lang::Units(Bytes(needed)), hub::lang::Units(Bytes(drive.free))}));
      return;
    }
    Set(Phase::kReady);
    {
      std::unique_lock lock(m);
      cv.wait(lock, [this] { return go || quit.load(); });
      if (quit.load()) return;
      st.phase = Phase::kInstalling;
      started = Clock::now();
    }
    std::string why_not;
    const Outcome outcome = WithPayload(why_not);
    if (outcome != Outcome::kUnavailable) return Conclude(outcome, token);
    if (stop.load()) return Conclude(Outcome::kStopped, token);
    REXLOG_WARN("PS5 installer: {}; installing in the app instead", why_not);
    {
      std::lock_guard lock(m);
      st.note = why_not + ", so the app is installing by itself. That's much slower.";
      st.diag_time.clear();
    }
    InApp();
  }
};

Job::Job(const fs::path& app, const fs::path& install_root, int episode) : impl_(std::make_unique<Impl>()) {
  impl_->app = app;
  impl_->root = install_root;
  impl_->episode = episode >= 0 && episode < kEpisodeCount ? episode : -1;
  impl_->found = Find(app);
  impl_->thread = std::thread([impl = impl_.get()] { impl->Run(); });
}

Job::~Job() {
  {
    std::lock_guard lock(impl_->m);
    impl_->quit = true;
  }
  impl_->progress.cancel_requested = true;
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->monitoring = false;
  if (impl_->monitor.joinable()) impl_->monitor.join();
}

const Sources& Job::Found() const { return impl_->found; }

void Job::Start() {
  {
    std::lock_guard lock(impl_->m);
    if (impl_->st.phase != Phase::kReady) return;
    impl_->go = true;
  }
  impl_->cv.notify_all();
}

void Job::Cancel() {
  impl_->stop = true;
  impl_->progress.cancel_requested = true;
}

// (only what was last worked out: the screen never touches the drive)
Job::Status Job::Get() const {
  std::lock_guard lock(impl_->m);
  Status s = impl_->st;
  if (s.phase == Phase::kDone) s.progress = 1;
  return s;
}

}  // namespace gta4ps5::hub::install
