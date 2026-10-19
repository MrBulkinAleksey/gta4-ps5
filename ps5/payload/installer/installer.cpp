/* GTA IV for PS5 - the installer payload: the install's writing, at the drive's full speed.
 *
 * The app sends this to the console's payload loader on port 9021 (etaHEN, elfldr or
 * any other) when the game is installed from its disc image, so the writing happens in
 * a process of its own. The PS5 lets an app write only about a gigabyte quickly before
 * slowing it to a few MB/s (the app's own install wrote at 2 MB/s); a payload writes at
 * the drive's full speed, as an FTP server does. This is Recomp Store's copier
 * (app/payload/copier in elripalda/RecompStore) made for GTA IV: the same job files,
 * the store's disc image and content package readers (store_disc, store_stfs).
 *
 * The app leaves the job in its own folder:
 *     <app>/UserData/LibertyRecomp/.install-job/job.txt
 * It sees that folder as /app0; this finds where it really is (/data/homebrew/...,
 * a drive under /mnt) by looking for that file. Paths in the job are inside the app's
 * folder:
 *     token=<number the app chose>
 *     staging=UserData/LibertyRecomp/.install-staging
 *     image=install/<the disc image, or a folder of the disc's files>
 *     image_kind=iso|folder
 *     disc_root=<the disc's folder with default.xex in it; empty: the top>
 *     update=install/<Title Update 8>
 *     update_kind=package|xexp
 *     update_root=<the package's folder with default.xexp in it>
 *     update_xexp=<default.xexp's name in the package>
 *     key=<the archives' AES key, 64 hex digits>
 *     needed=<bytes the install needs free>
 *     threads=<files unpacked at once>
 * It empties the staging folder, then
 *   1. copies the disc's files into <staging>/game, the title update's into
 *      <staging>/game/update (its default.xexp beside default.xex too) and writes
 *      aes_key.bin there;
 *   2. unpacks common.rpf, xbox360.rpf, audio.rpf and the archives inside them, with
 *      the code the app installs with (gta4_rpf_extractor.cpp).
 * The app then checks the game, writes its manifest and moves it into place.
 *
 * It takes the job by renaming job.txt to job.taken (the app takes back a job nobody
 * took by deleting job.txt, so only one of them ever has it), one installer at a time
 * (a lock on "running"). Progress goes to status.txt there twice a second (written beside
 * and renamed over, so the app never reads half of it). A file called "cancel" there stops
 * it, as does job.taken going away or becoming another job's. A stopped or failed
 * install's files are removed, but only its own: the staging folder carries the job's
 * token (.token). Nothing is written outside the staging folder but the job folder and
 * logs/installer.log (and /data/gta4-ps5-installer.log when no job can be found). No
 * notifications.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "store_disc.hpp"
#include "store_stfs.hpp"
#include "store_zip.hpp"

#include "gta4_install_io.h"
#include "gta4_rpf_extractor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__PROSPERO__) || defined(__FreeBSD__)
#define HAVE_MOUNTS 1
#include <sys/mount.h>
#include <sys/param.h>
#else
#define HAVE_MOUNTS 0
#include <sys/statvfs.h>
#endif

namespace
{
namespace fs = std::filesystem;
namespace io = gta4::install::io;

/* Where the app leaves the job, inside its folder, and the only folder this empties. */
const char *const kJobRel = "UserData/LibertyRecomp/.install-job";
const char *const kStagingRel = "UserData/LibertyRecomp/.install-staging";
/* A job.txt older than this (seconds) is an old one. */
constexpr double kFreshJob = 600;

std::string g_app, g_jobdir, g_log;
std::atomic<bool> g_cancel{false}, g_quit{false};
/* The job became another one (a newer install took it over): this one stops, quietly. */
std::atomic<bool> g_superseded{false};
std::mutex g_log_mutex, g_status_mutex;

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

void log_line(const char *fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (g_log.empty())
        return;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    std::lock_guard<std::mutex> l(g_log_mutex);
    if (std::FILE *f = std::fopen(g_log.c_str(), "a"))
    {
        std::fprintf(f, "%s  installer: %s\n", stamp, body);
        std::fclose(f);
    }
}

bool read_text(const std::string &path, std::string &out)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return true;
}

std::string field(const std::string &text, const char *key)
{
    const std::string k = std::string(key) + "=";
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        if (text.compare(at, k.size(), k) == 0)
            return text.substr(at + k.size(), end - at - k.size());
        at = end + 1;
    }
    return "";
}

bool exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string mb(std::uint64_t bytes)
{
    char buf[32];
    if (bytes >= (1ull << 30))
        std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
    else
        std::snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(1ull << 20));
    return buf;
}

/* `path` inside the folder `root` ("" for everything): what's left of it. */
bool under(const std::string &path, const std::string &root, std::string &rel)
{
    if (root.empty())
    {
        rel = path;
        return true;
    }
    if (path.size() <= root.size() + 1 || path[root.size()] != '/' || lower(path.substr(0, root.size())) != lower(root))
        return false;
    rel = path.substr(root.size() + 1);
    return true;
}

bool write_all(int fd, const std::uint8_t *data, std::size_t n)
{
    while (n)
    {
        const ssize_t w = ::write(fd, data, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        data += w;
        n -= std::size_t(w);
    }
    return true;
}

/* ---- What the app reads: status.txt ---------------------------------------------- */

struct Status
{
    std::mutex m; /* the strings and the step */
    std::string token, phase = "starting", error, drive;
    int step = 0;
    std::uint64_t free = 0, unpack_guess = 0, beat = 0;
    std::atomic<std::uint64_t> copy_done{0}, copy_total{0}, unpack_done{0}, unpack_total{0};
    double t0 = 0;
};
Status g;

std::string one_line(std::string s)
{
    for (char &c : s)
        if (c == '\n' || c == '\r')
            c = ' ';
    return s;
}

/* Whole, at once: written beside, then renamed over. */
void write_status()
{
    std::lock_guard<std::mutex> w(g_status_mutex);
    if (g_superseded.load())
        return; /* (status.txt is the newer job's) */
    std::string file;
    std::uint64_t file_size = 0;
    io::Current(file, file_size);
    file = file.substr(file.find_last_of('/') + 1);
    const io::Stats &s = io::GetStats();
    std::string text;
    {
        std::lock_guard<std::mutex> l(g.m);
        char buf[1024];
        std::snprintf(buf, sizeof buf,
                      "token=%s\nphase=%s\nstep=%d\ncopy_done=%llu\ncopy_total=%llu\nunpack_done=%llu\nunpack_total=%llu\n"
                      "unpack_guess=%llu\nseconds=%.1f\nfile_size=%llu\nread_bytes=%llu\nread_ns=%llu\nwrite_bytes=%llu\n"
                      "write_ns=%llu\nfree=%llu\nbeat=%llu\n",
                      g.token.c_str(), g.phase.c_str(), g.step, (unsigned long long)g.copy_done.load(),
                      (unsigned long long)g.copy_total.load(), (unsigned long long)g.unpack_done.load(),
                      (unsigned long long)g.unpack_total.load(), (unsigned long long)g.unpack_guess, now_s() - g.t0,
                      (unsigned long long)file_size, (unsigned long long)s.read_bytes.load(),
                      (unsigned long long)s.read_ns.load(), (unsigned long long)s.write_bytes.load(),
                      (unsigned long long)s.write_ns.load(), (unsigned long long)g.free, (unsigned long long)++g.beat);
        text = buf;
        text += "file=" + one_line(file) + "\nfolder=" + one_line(g_app) + "\ndrive=" + one_line(g.drive) +
                "\nerror=" + one_line(g.error) + "\n";
    }
    const std::string tmp = g_jobdir + "/status.tmp", fin = g_jobdir + "/status.txt";
    static bool failing = false; /* (logged once each time it starts failing) */
    bool ok = false;
    if (std::FILE *f = std::fopen(tmp.c_str(), "wb"))
    {
        ok = std::fputs(text.c_str(), f) >= 0;
        ok = std::fclose(f) == 0 && ok;
        chmod(tmp.c_str(), 0666);
        ok = ok && std::rename(tmp.c_str(), fin.c_str()) == 0;
    }
    if (!ok && !failing)
        log_line("couldn't write status.txt (error %d)", errno);
    failing = !ok;
}

void set_phase(const char *phase, int step = -1)
{
    {
        std::lock_guard<std::mutex> l(g.m);
        g.phase = phase;
        if (step >= 0)
            g.step = step;
    }
    write_status();
}

/* Four times a second: is there a cancel file? Twice a second: the status. Every
 * 10 s: a line in the log. */
void status_main()
{
    int tick = 0;
    while (!g_quit.load())
    {
        usleep(250000);
        /* a cancel file; or job.taken gone (the app let the job go) or another job's */
        if (!g_cancel.load())
        {
            std::string taken;
            const bool asked = exists(g_jobdir + "/cancel");
            const bool ours = read_text(g_jobdir + "/job.taken", taken) && field(taken, "token") == g.token;
            if (!ours)
                g_superseded = true;
            if (asked || !ours)
            {
                g_cancel = true;
                log_line(asked ? "the app asked to stop" : "the job isn't this installer's any more; stopping");
            }
        }
        if (++tick % 2 == 0)
            write_status();
        if (tick % 40 == 0)
        {
            std::uint64_t guess;
            {
                std::lock_guard<std::mutex> l(g.m);
                guess = g.unpack_guess;
            }
            const io::Stats &s = io::GetStats();
            const double rs = double(s.read_ns.load()) / 1e9, ws = double(s.write_ns.load()) / 1e9;
            log_line("%.0f s: copied %s of %s, unpacked %s of %s; reading %.0f s (%.0f MB/s), writing %.0f s (%.0f MB/s)",
                     now_s() - g.t0, mb(g.copy_done).c_str(), mb(g.copy_total).c_str(), mb(g.unpack_done).c_str(),
                     mb(std::max<std::uint64_t>(g.unpack_total, guess)).c_str(), rs,
                     rs > 0 ? double(s.read_bytes.load()) / 1048576.0 / rs : 0.0, ws,
                     ws > 0 ? double(s.write_bytes.load()) / 1048576.0 / ws : 0.0);
        }
    }
}

/* The end of a stopped or failed install: its files removed (`staging`, when it got
 * that far), then phase=stopped or failed. */
int fail(std::string why, const std::string &staging = std::string())
{
    const bool stopped = g_cancel.load();
    if (stopped)
        why = "Stopped.";
    log_line("%s: %s", stopped ? "stopped" : "failed", why.c_str());
    /* Its files go, if they're still its own (the staging folder carries its token). */
    std::string mark;
    if (!staging.empty() && !g_superseded.load() && read_text(staging + "/.token", mark) && mark == g.token)
    {
        set_phase("cleaning");
        std::error_code ec;
        const auto removed = fs::remove_all(staging, ec);
        if (ec)
            log_line("couldn't remove %s: %s", staging.c_str(), ec.message().c_str());
        else
            log_line("removed %s (%llu files and folders)", staging.c_str(), (unsigned long long)removed);
    }
    {
        std::lock_guard<std::mutex> l(g.m);
        g.phase = stopped ? "stopped" : "failed";
        g.error = why;
    }
    write_status();
    return 1;
}

/* ---- Where the app is ------------------------------------------------------------- */

void add_place(std::vector<std::string> &places, std::string p)
{
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    if (p.size() < 2 || p[0] != '/')
        return;
    if (std::find(places.begin(), places.end(), p) == places.end())
        places.push_back(p);
}

void add_folders_in(std::vector<std::string> &places, const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d))
        if (e->d_name[0] != '.')
            add_place(places, dir + "/" + e->d_name);
    closedir(d);
}

/* Through the console's own mounts the app's folder may be seen at several paths (the
 * app sees /app0, the console mounts it from where it really is): its real place first. */
int rank_of(const std::string &p)
{
    if (p.rfind("/mnt/sandbox/", 0) == 0)
        return 2;
    if (p.rfind("/system_ex/", 0) == 0)
        return 1;
    return 0;
}

/* The app's folder: the one holding the newest fresh job. */
bool find_app(std::string &app)
{
    std::vector<std::string> places;
#ifdef GTA4_TEST_ROOT
    add_folders_in(places, GTA4_TEST_ROOT);
#else
#if HAVE_MOUNTS
    /* every mount: a homebrew app's folder is mounted (nullfs) where the app is run from */
    struct statfs *mounts = nullptr;
    const int n = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < n; ++i)
    {
        add_place(places, mounts[i].f_mntfromname);
        add_place(places, mounts[i].f_mntonname);
    }
#endif
    /* and where homebrew apps usually are */
    add_folders_in(places, "/data/homebrew");
    add_folders_in(places, "/user/data/homebrew");
    for (const char *kind : {"usb", "ext"})
        for (int i = 0; i < 8; ++i)
        {
            const std::string drive = "/mnt/" + std::string(kind) + std::to_string(i);
            add_folders_in(places, drive + "/homebrew");
            add_folders_in(places, drive);
        }
    add_folders_in(places, "/data");
    add_folders_in(places, "/system_ex/app");
#endif
    const std::time_t now = std::time(nullptr);
    bool found = false;
    struct stat best{};
    int best_rank = 0;
    /* (no job anywhere: what was looked at, for whoever looks into it) */
    struct Note
    {
        const std::vector<std::string> &places;
        const bool &found;
        ~Note()
        {
            if (found)
                return;
#ifdef GTA4_TEST_ROOT
            std::FILE *f = std::fopen(GTA4_TEST_ROOT "/installer-no-job.log", "a");
#else
            std::FILE *f = std::fopen("/data/gta4-ps5-installer.log", "a");
#endif
            if (!f)
                return;
            std::fprintf(f, "%ld  installer: no fresh %s/job.txt in any of %zu folders:", long(std::time(nullptr)), kJobRel,
                         places.size());
            for (const std::string &p : places)
                if (p.find("homebrew") != std::string::npos || p.find("PPSA") != std::string::npos)
                    std::fprintf(f, " %s", p.c_str());
            std::fprintf(f, "\n");
            std::fclose(f);
        }
    } note{places, found};
    for (const std::string &p : places)
    {
        struct stat st, eboot;
        if (stat((p + "/" + kJobRel + "/job.txt").c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
            std::difftime(now, st.st_mtime) > kFreshJob || stat((p + "/eboot.bin").c_str(), &eboot) != 0)
            continue;
        const int rank = rank_of(p);
        const bool same = found && st.st_ino == best.st_ino && st.st_mtime == best.st_mtime;
        if (!found || (same ? rank < best_rank : st.st_mtime > best.st_mtime))
        {
            app = p;
            best = st;
            best_rank = rank;
            found = true;
        }
    }
    return found;
}

/* The drive the app's folder is on: its kind and what's mounted, and its free space. */
void drive_of(const std::string &path)
{
    std::lock_guard<std::mutex> l(g.m);
#if HAVE_MOUNTS
    struct statfs sf;
    if (statfs(path.c_str(), &sf) == 0)
    {
        g.drive = std::string(sf.f_fstypename) + " " + sf.f_mntfromname;
        g.free = std::uint64_t(sf.f_bavail) * std::uint64_t(sf.f_bsize);
    }
#else
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) == 0)
    {
        g.drive = "local";
        g.free = std::uint64_t(sv.f_bavail) * std::uint64_t(sv.f_frsize);
    }
#endif
}

/* ---- 1. The disc's files ----------------------------------------------------------- */

struct DiscItem
{
    const recomp::DiscFile *file;
    std::string rel; /* where it goes, inside the game's folder */
};

constexpr std::size_t kBlock = 16u << 20;
constexpr int kBlocks = 3;

/* A reader thread fills kBlock pieces in disc order, the writer (the caller) writes them. */
struct Pipe
{
    std::mutex m;
    std::condition_variable cv;
    std::uint8_t *data[kBlocks] = {};
    std::size_t size[kBlocks] = {};
    std::deque<int> full; /* oldest first */
    std::vector<int> free;
    bool stop = false, reader_done = false, read_failed = false;
    std::string bad;
};

void read_files(Pipe &p, recomp::Disc &disc, const std::vector<DiscItem> &items)
{
    for (const DiscItem &it : items)
    {
        std::uint64_t off = std::uint64_t(it.file->sector) * 2048, left = it.file->size;
        while (left)
        {
            int bi;
            {
                std::unique_lock<std::mutex> l(p.m);
                p.cv.wait(l, [&] { return !p.free.empty() || p.stop; });
                if (p.stop)
                    return;
                bi = p.free.back();
                p.free.pop_back();
            }
            const std::size_t n = std::size_t(std::min<std::uint64_t>(left, kBlock));
            const std::uint64_t t0 = io::detail::NowNs();
            const bool ok = disc.read(off, p.data[bi], n);
            io::GetStats().read_ns += io::detail::NowNs() - t0;
            {
                std::lock_guard<std::mutex> l(p.m);
                if (!ok)
                {
                    p.read_failed = true;
                    p.bad = it.rel;
                    p.free.push_back(bi);
                    p.cv.notify_all();
                    return;
                }
                io::GetStats().read_bytes += n;
                p.size[bi] = n;
                p.full.push_back(bi);
            }
            p.cv.notify_all();
            off += n;
            left -= n;
        }
    }
}

bool copy_disc(recomp::Disc &disc, const std::vector<DiscItem> &items, const std::string &game, std::string &error)
{
    Pipe p;
    for (int i = 0; i < kBlocks; ++i)
    {
        p.data[i] = static_cast<std::uint8_t *>(std::malloc(kBlock));
        if (!p.data[i])
        {
            for (int j = 0; j < i; ++j)
                std::free(p.data[j]);
            error = "The installer couldn't get memory to copy with.";
            return false;
        }
        p.free.push_back(i);
    }
    std::thread reader([&] {
        read_files(p, disc, items);
        std::lock_guard<std::mutex> l(p.m);
        p.reader_done = true;
        p.cv.notify_all();
    });

    std::set<std::string> made;
    for (const DiscItem &it : items)
    {
        const std::string dst = game + "/" + it.rel;
        const std::string dir = dst.substr(0, dst.find_last_of('/'));
        if (made.insert(dir).second)
            recomp::zip::make_dirs(dir);
        const int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
        {
            error = "Couldn't create " + it.rel + " (error " + std::to_string(errno) + ").";
            break;
        }
        io::SetCurrent(dst, it.file->size);
        std::uint64_t left = it.file->size;
        bool ok = true;
        while (left && ok)
        {
            int bi;
            {
                std::unique_lock<std::mutex> l(p.m);
                p.cv.wait(l, [&] { return !p.full.empty() || p.read_failed || p.reader_done; });
                if (p.full.empty())
                {
                    error = "The disc image couldn't be read at " + (p.bad.empty() ? it.rel : p.bad) +
                            ". It may be incomplete.";
                    ok = false;
                    break;
                }
                bi = p.full.front();
                p.full.pop_front();
            }
            const std::size_t n = p.size[bi];
            const std::uint64_t t0 = io::detail::NowNs();
#ifdef GTA4_TEST_SLOW
            usleep(GTA4_TEST_SLOW); /* the desktop test only: a slow drive */
#endif
            const bool wrote = write_all(fd, p.data[bi], n);
            const int why = errno;
            io::GetStats().write_ns += io::detail::NowNs() - t0;
            if (wrote)
                io::GetStats().write_bytes += n;
            {
                std::lock_guard<std::mutex> l(p.m);
                p.free.push_back(bi);
            }
            p.cv.notify_all();
            if (!wrote)
            {
                error = "Writing " + it.rel + " failed (error " + std::to_string(why) + "). Is the drive full?";
                ok = false;
                break;
            }
            left -= n;
            g.copy_done += n;
            if (g_cancel.load())
            {
                error = "Stopped.";
                ok = false;
            }
        }
        if (::close(fd) != 0 && ok)
        {
            error = "Writing " + it.rel + " failed. Is the drive full?";
            ok = false;
        }
        if (!ok)
            break;
    }
    {
        std::lock_guard<std::mutex> l(p.m);
        p.stop = true;
    }
    p.cv.notify_all();
    reader.join();
    for (std::uint8_t *d : p.data)
        std::free(d);
    if (!error.empty())
        return false;
    /* every file there, at its size */
    for (const DiscItem &it : items)
    {
        struct stat st;
        if (stat((game + "/" + it.rel).c_str(), &st) != 0 || std::uint64_t(st.st_size) != it.file->size)
        {
            error = it.rel + " didn't copy whole.";
            return false;
        }
    }
    return true;
}

std::string leaf(const std::string &path) { return path.substr(path.find_last_of('/') + 1); }

std::uint64_t size_of(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? std::uint64_t(st.st_size) : 0;
}

/* One file into another, counted as copied. */
bool copy_file(const std::string &from, const std::string &to, std::string &error)
{
    const int in = ::open(from.c_str(), O_RDONLY);
    if (in < 0)
    {
        error = "Couldn't read " + leaf(from) + ".";
        return false;
    }
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
    {
        ::close(in);
        error = "Couldn't create " + leaf(to) + ".";
        return false;
    }
    io::SetCurrent(to, size_of(from));
    std::vector<std::uint8_t> buf(8u << 20);
    bool read_ok = true, write_ok = true;
    for (;;)
    {
        if (g_cancel.load())
        {
            error = "Stopped.";
            break;
        }
        const std::uint64_t r0 = io::detail::NowNs();
        const ssize_t n = ::read(in, buf.data(), buf.size());
        io::GetStats().read_ns += io::detail::NowNs() - r0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            read_ok = false;
        if (n <= 0)
            break;
        io::GetStats().read_bytes += std::uint64_t(n);
        const std::uint64_t w0 = io::detail::NowNs();
        write_ok = write_all(out, buf.data(), std::size_t(n));
        io::GetStats().write_ns += io::detail::NowNs() - w0;
        if (!write_ok)
            break;
        io::GetStats().write_bytes += std::uint64_t(n);
        g.copy_done += std::uint64_t(n);
    }
    ::close(in);
    if (::close(out) != 0)
        write_ok = false;
    if (!error.empty())
        return false;
    if (!read_ok)
    {
        error = leaf(from) + " couldn't be read. It may be incomplete.";
        return false;
    }
    if (!write_ok)
    {
        error = "Writing " + leaf(to) + " failed. Is the drive full?";
        return false;
    }
    return true;
}

/* A game folder (a disc's files copied out already): every file in it, with its path. */
struct FolderItem
{
    std::string from, rel;
    std::uint64_t size;
};

bool list_folder(const std::string &dir, std::vector<FolderItem> &out, std::string &error)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec))
            continue;
        out.push_back({it->path().string(), it->path().lexically_relative(dir).generic_string(),
                       std::uint64_t(it->file_size(type_ec))});
    }
    if (ec)
    {
        error = "The game folder couldn't be read (" + ec.message() + ").";
        return false;
    }
    return true;
}

/* The title update's package: its files under `root` into <game>/update, and its
 * default.xexp (`xexp`, its name in the package) also beside default.xex. */
bool copy_package(recomp::stfs::Package &pkg, const std::string &root, const std::string &xexp, const std::string &game,
                  std::string &error)
{
    const std::string target = game + "/update";
    std::string xexp_copy;
    for (const recomp::stfs::Entry &e : pkg.entries())
    {
        std::string rel;
        if (!under(e.name, root, rel))
            continue;
        if (!recomp::zip::safe_name(rel))
        {
            error = "The title update has a file with an unsafe name.";
            return false;
        }
        const std::string dst = target + "/" + rel;
        recomp::zip::make_dirs(dst.substr(0, dst.find_last_of('/')));
        const int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
        {
            error = "Couldn't create update/" + rel + ".";
            return false;
        }
        io::SetCurrent(dst, e.size);
        bool wrote = true;
        const bool read = pkg.read(e, [&](const std::uint8_t *data, std::size_t n) {
            if (g_cancel.load())
                return false;
            const std::uint64_t t0 = io::detail::NowNs();
            wrote = write_all(fd, data, n);
            io::GetStats().write_ns += io::detail::NowNs() - t0;
            if (!wrote)
                return false;
            io::GetStats().write_bytes += n;
            g.copy_done += n;
            return true;
        });
        const bool closed = ::close(fd) == 0;
        struct stat st;
        if (g_cancel.load())
        {
            error = "Stopped.";
            return false;
        }
        if (!wrote || !closed)
        {
            error = "Writing update/" + rel + " failed. Is the drive full?";
            return false;
        }
        if (!read || stat(dst.c_str(), &st) != 0 || std::uint64_t(st.st_size) != e.size)
        {
            error = "The title update couldn't be read at " + e.name + ". It may be incomplete.";
            return false;
        }
        if (lower(e.name) == lower(xexp))
            xexp_copy = dst;
    }
    if (xexp_copy.empty())
    {
        error = "The title update has no default.xexp.";
        return false;
    }
    return copy_file(xexp_copy, game + "/default.xexp", error);
}

bool parse_key(const std::string &hex, std::uint8_t key[32])
{
    if (hex.size() != 64)
        return false;
    for (int i = 0; i < 32; ++i)
    {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k)
        {
            const char c = char(std::tolower(static_cast<unsigned char>(hex[std::size_t(i * 2 + k)])));
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0)
                return false;
            v = v * 16 + unsigned(d);
        }
        key[i] = std::uint8_t(v);
    }
    return true;
}

/* ---- The install ------------------------------------------------------------------- */

int install(const std::string &job)
{
    const std::string staging_rel = field(job, "staging"), image_rel = field(job, "image"),
                      update_rel = field(job, "update"), update_kind = field(job, "update_kind"),
                      disc_root = field(job, "disc_root"), update_root = field(job, "update_root"),
                      update_xexp = field(job, "update_xexp");
    const std::uint64_t needed = std::strtoull(field(job, "needed").c_str(), nullptr, 10);
    int threads = std::atoi(field(job, "threads").c_str());
    threads = threads < 1 ? 3 : std::min(threads, 8);

    /* Only inside the app's folder, and only its staging folder is emptied. */
    const bool plain = staging_rel == kStagingRel && recomp::zip::safe_name(image_rel) && recomp::zip::safe_name(update_rel) &&
                       (disc_root.empty() || recomp::zip::safe_name(disc_root)) &&
                       (update_root.empty() || recomp::zip::safe_name(update_root)) &&
                       (update_kind == "xexp" || (update_kind == "package" && recomp::zip::safe_name(update_xexp)));
    std::uint8_t key[32];
    if (!plain || !parse_key(field(job, "key"), key))
        return fail("The install job is damaged. Start the install again.");
    const std::string staging = g_app + "/" + staging_rel, game = staging + "/game";
    const std::string image = g_app + "/" + image_rel, update = g_app + "/" + update_rel;
    drive_of(g_app);
    log_line("started (pid %d): %s, %s, into %s (%s, %s free)", int(getpid()), image_rel.c_str(), update_rel.c_str(),
             staging.c_str(), g.drive.c_str(), mb(g.free).c_str());

    /* What's copied: the disc's files (from the disc image, or a folder they were copied
     * out to)... */
    set_phase("reading", 1);
    const bool folder = field(job, "image_kind") == "folder";
    recomp::Disc disc;
    std::string error;
    std::vector<DiscItem> items;
    std::vector<FolderItem> folder_items;
    std::set<std::string> names;
    std::uint64_t total = 0;
    bool xex = false;
    if (folder)
    {
        if (!list_folder(disc_root.empty() ? image : image + "/" + disc_root, folder_items, error))
            return fail(error);
        for (const FolderItem &f : folder_items)
        {
            if (!recomp::zip::safe_name(f.rel) || !names.insert(lower(f.rel)).second)
                return fail("The game folder has a file the installer can't write: " + f.rel + ".");
            total += f.size;
            xex = xex || lower(f.rel) == "default.xex";
        }
    }
    else
    {
        if (!disc.open(image, error))
            return fail(error);
        for (const recomp::DiscFile &f : disc.files())
        {
            std::string rel;
            if (f.is_dir || !under(f.path, disc_root, rel))
                continue;
            if (!recomp::zip::safe_name(rel) || !names.insert(lower(rel)).second)
                return fail("The disc image has a file the installer can't write: " + f.path + ".");
            items.push_back({&f, rel});
            total += f.size;
            xex = xex || lower(rel) == "default.xex";
        }
        std::sort(items.begin(), items.end(),
                  [](const DiscItem &a, const DiscItem &b) { return a.file->sector < b.file->sector; });
    }
    if (!xex)
        return fail("This disc image has no default.xex where the app found it. Check the disc image again.");
    /* ...and the title update's */
    recomp::stfs::Package pkg;
    if (update_kind == "package")
    {
        if (!pkg.open(update, error))
            return fail("Title Update 8: " + error);
        std::uint64_t xexp_size = 0;
        for (const recomp::stfs::Entry &e : pkg.entries())
        {
            std::string rel;
            if (!under(e.name, update_root, rel))
                continue;
            total += e.size;
            if (lower(e.name) == lower(update_xexp))
                xexp_size = e.size;
        }
        if (!xexp_size)
            return fail("The title update has no default.xexp.");
        total += xexp_size;
    }
    else
    {
        if (!exists(update))
            return fail("Title Update 8 couldn't be found.");
        total += size_of(update);
    }
    g.copy_total = total + 32;

    /* An empty staging folder (an earlier try's files go), marked as this job's, and room
     * for the install */
    if (g_cancel.load())
        return fail("Stopped.");
    set_phase("clearing", 1);
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (ec)
        return fail("The files of an earlier install couldn't be removed (" + ec.message() + ").");
    recomp::zip::make_dirs(game);
    {
        const std::string mark = staging + "/.token";
        const int fd = ::open(mark.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        const bool ok = fd >= 0 && write_all(fd, reinterpret_cast<const std::uint8_t *>(g.token.data()), g.token.size());
        if (fd >= 0)
            ::close(fd);
        if (!ok)
            return fail("Couldn't write in the app's folder. Is the drive full?");
    }
    drive_of(g_app);
    if (needed && g.free && g.free < needed)
        return fail("There isn't enough free space. The install needs about " + mb(needed) + " and the drive has " +
                        mb(g.free) + " free.",
                    staging);

    /* 1. Copying */
    const double t1 = now_s();
    set_phase("copying", 1);
    log_line("copying %zu files from the %s, %s", folder ? folder_items.size() : items.size(),
             folder ? "game folder" : "disc image", mb(total).c_str());
    if (folder)
    {
        for (const FolderItem &f : folder_items)
        {
            const std::string dst = game + "/" + f.rel;
            recomp::zip::make_dirs(dst.substr(0, dst.find_last_of('/')));
            if (!copy_file(f.from, dst, error))
                return fail(error, staging);
        }
    }
    else if (!copy_disc(disc, items, game, error))
        return fail(error, staging);
    disc.close();
    if (update_kind == "package" ? !copy_package(pkg, update_root, update_xexp, game, error)
                                 : !copy_file(update, game + "/default.xexp", error))
        return fail(error, staging);
    pkg.close();
    {
        const std::string path = game + "/aes_key.bin";
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        const bool ok = fd >= 0 && write_all(fd, key, sizeof key);
        if (fd >= 0 && ::close(fd) != 0)
            return fail("Couldn't write aes_key.bin. Is the drive full?", staging);
        if (!ok)
            return fail("Couldn't write aes_key.bin.", staging);
        g.copy_done += sizeof key;
    }
    const double copy_s = now_s() - t1;
    log_line("copied %s in %.1f s (%.0f MB/s)", mb(g.copy_done).c_str(), copy_s,
             copy_s > 0 ? double(g.copy_done) / 1048576.0 / copy_s : 0.0);
    if (g_cancel.load())
        return fail("Stopped.", staging);

    /* 2. Unpacking: the app's own unpacker, a few files at once */
    const double t2 = now_s();
    {
        std::lock_guard<std::mutex> l(g.m);
        g.unpack_guess =
            (size_of(game + "/common.rpf") + size_of(game + "/xbox360.rpf") + size_of(game + "/audio.rpf")) * 23 / 20;
    }
    set_phase("unpacking", 2);
    io::Policy().threads = threads;
    io::Policy().preallocate = false;
    io::Policy().direct = false;
    const gta4::install::RpfExtractionProgress progress{&g.unpack_done, &g.unpack_total, &g_cancel};
    if (!gta4::install::ExtractGameArchives(game, game + "/aes_key.bin", progress, error))
        return fail(error, staging);
    const double unpack_s = now_s() - t2;
    log_line("unpacked %s in %.1f s (%.0f MB/s, %d at once)", mb(g.unpack_done).c_str(), unpack_s,
             unpack_s > 0 ? double(g.unpack_done) / 1048576.0 / unpack_s : 0.0, threads);

    /* On the drive, then done */
    set_phase("syncing", 2);
    const double t3 = now_s();
    sync();
    if (g_cancel.load())
        return fail("Stopped.", staging);
    {
        std::lock_guard<std::mutex> l(g.m);
        g.phase = "done";
    }
    write_status();
    log_line("done in %.0f s (flushing %.1f s)", now_s() - g.t0, now_s() - t3);
    return 0;
}
} // namespace

int main()
{
    umask(0);
    g.t0 = now_s();
    std::string app;
    if (!find_app(app))
        return 1; /* (no job: the app falls back to installing by itself) */
    g_app = app;
    g_jobdir = app + "/" + kJobRel;
    recomp::zip::make_dirs(app + "/logs");
    g_log = app + "/logs/installer.log";
    if (size_of(g_log) > (1u << 20))
        std::rename(g_log.c_str(), (g_log + ".old").c_str());
    /* One at a time: a lock on "running" (let go when this ends, however it ends); a
     * second installer leaves the job to the first. */
    const int lock = ::open((g_jobdir + "/running").c_str(), O_RDWR | O_CREAT, 0666);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
    {
        log_line("another installer is already working; this one leaves it to that");
        return 1;
    }
    /* The job taken: job.txt becomes job.taken. If it's gone, the app took it back (it
     * stopped waiting for an installer to start) and installs by itself, or stopped. */
    std::string job;
    if (std::rename((g_jobdir + "/job.txt").c_str(), (g_jobdir + "/job.taken").c_str()) != 0 ||
        !read_text(g_jobdir + "/job.taken", job))
    {
        log_line("the app took the job back before this installer started; leaving it");
        return 0;
    }
    g.token = field(job, "token");
    if (exists(g_jobdir + "/cancel"))
    {
        g_cancel = true;
        return fail("Stopped.");
    }
    write_status();
    std::thread status(status_main);
    const int rc = install(job);
    g_quit = true;
    status.join();
    write_status();
    ::close(lock);
    return rc;
}
