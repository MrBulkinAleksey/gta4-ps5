// [windows port] See gta4_debug_server.h. One client at a time; each request
// is a JSON object on one line ({"id": 1, "cmd": "read", ...}) and gets one
// line back ({"id": 1, "ok": true, ...}). Bytes travel as hex, in guest
// (big-endian) order: decoding them is the tool's job.
#include "devtools/gta4_debug_server.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_UINT32(gta4_debug_server_port, 0, "GTA IV/Debug",
                      "Port of the debug server on 127.0.0.1 for tools/gta4-live-mcp (0 = off)")
    .range(0, 65535)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gta4_debug_server_allow_write, false, "GTA IV/Debug",
                    "Let the debug server write to guest memory");

namespace gta4::debug_server {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket socket) { close(socket); }
#endif
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;  // (a closed client is not a SIGPIPE)
#else
constexpr int kSendFlags = 0;
#endif

using nlohmann::json;

constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kMaxReadBytes = 1u << 20;
constexpr size_t kMaxRequestBytes = 4u << 20;
constexpr size_t kMaxFindResults = 1000;
constexpr size_t kMaxLogLines = 5000;

std::filesystem::path g_logs_dir;

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

bool PageAccessible(rex::memory::Memory* memory, uint32_t page, bool write) {
  auto* heap = memory->LookupHeap(page);
  if (!heap) return false;
  using rex::memory::PageAccess;
  const PageAccess access = heap->QueryRangeAccess(page, page + kPageSize - 1);
  if (write) return access == PageAccess::kReadWrite || access == PageAccess::kExecuteReadWrite;
  return access == PageAccess::kReadOnly || access == PageAccess::kReadWrite ||
         access == PageAccess::kExecuteReadOnly || access == PageAccess::kExecuteReadWrite;
}

// Whether every page of [address, address + bytes) is mapped for the access:
// reading an unmapped guest page would take the game down.
bool RangeAccessible(rex::memory::Memory* memory, uint32_t address, uint32_t bytes, bool write) {
  if (!bytes || uint64_t(address) + bytes > (uint64_t(1) << 32)) return false;
  for (uint64_t page = address & ~uint64_t(kPageSize - 1); page < uint64_t(address) + bytes;
       page += kPageSize) {
    if (!PageAccessible(memory, uint32_t(page), write)) return false;
  }
  return true;
}

uint32_t ParseAddress(const json& value) {
  if (value.is_number_unsigned()) return value.get<uint32_t>();
  if (value.is_string()) return uint32_t(std::stoull(value.get<std::string>(), nullptr, 0));
  throw std::runtime_error("an address is a number or a \"0x...\" string");
}

std::string ToHex(const uint8_t* bytes, size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    text[i * 2] = kDigits[bytes[i] >> 4];
    text[i * 2 + 1] = kDigits[bytes[i] & 0xF];
  }
  return text;
}

std::vector<uint8_t> FromHex(const std::string& text) {
  if (text.size() % 2) throw std::runtime_error("hex has an odd number of digits");
  auto digit = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
    throw std::runtime_error("not a hex digit");
  };
  std::vector<uint8_t> bytes(text.size() / 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = uint8_t(digit(text[i * 2]) << 4 | digit(text[i * 2 + 1]));
  }
  return bytes;
}

json Info(rex::memory::Memory* memory) {
  return {
      {"title", "GTA IV"},
      {"virtual_membase", fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(memory->virtual_membase()))},
      {"physical_membase", fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(memory->physical_membase()))},
      {"allow_write", REXCVAR_GET(gta4_debug_server_allow_write)},
      {"logs_dir", g_logs_dir.string()},
  };
}

json Read(rex::memory::Memory* memory, const json& request) {
  const uint32_t address = ParseAddress(request.at("addr"));
  const uint32_t size = request.at("size").get<uint32_t>();
  if (!size || size > kMaxReadBytes) throw std::runtime_error("size is 1..1048576 bytes");
  if (!RangeAccessible(memory, address, size, false)) {
    throw std::runtime_error(fmt::format("{:08X}+{:X} is not all mapped readable", address, size));
  }
  return {{"hex", ToHex(memory->virtual_membase() + address, size)}};
}

json Write(rex::memory::Memory* memory, const json& request) {
  if (!REXCVAR_GET(gta4_debug_server_allow_write)) {
    throw std::runtime_error("writes are off (gta4_debug_server_allow_write)");
  }
  const uint32_t address = ParseAddress(request.at("addr"));
  const std::vector<uint8_t> bytes = FromHex(request.at("hex").get<std::string>());
  if (bytes.empty() || bytes.size() > kMaxReadBytes) throw std::runtime_error("1..1048576 bytes");
  if (!RangeAccessible(memory, address, uint32_t(bytes.size()), true)) {
    throw std::runtime_error(fmt::format("{:08X}+{:X} is not all mapped writable", address,
                                         bytes.size()));
  }
  std::memcpy(memory->virtual_membase() + address, bytes.data(), bytes.size());
  REXLOG_INFO("gta4-debug-server: wrote {} bytes at {:08X}", bytes.size(), address);
  return {{"written", bytes.size()}};
}

// The pattern in every readable run of pages in [start, end), each run searched
// as one block so a match may span pages.
json Find(rex::memory::Memory* memory, const json& request) {
  const std::vector<uint8_t> pattern = FromHex(request.at("hex").get<std::string>());
  if (pattern.empty()) throw std::runtime_error("an empty pattern");
  const uint64_t start = request.contains("start") ? ParseAddress(request["start"]) : 0x10000;
  const uint64_t end = request.contains("end") ? ParseAddress(request["end"]) : 0x100000000ull;
  const size_t limit =
      std::min<size_t>(request.value("max_results", size_t(100)), kMaxFindResults);
  const uint8_t* membase = memory->virtual_membase();
  json matches = json::array();
  auto search = [&](uint64_t run_start, uint64_t run_end) {
    const uint8_t* first = membase + run_start;
    const uint8_t* last = membase + run_end;
    for (const uint8_t* at = first; matches.size() < limit;) {
      at = std::search(at, last, pattern.begin(), pattern.end());
      if (at == last) break;
      matches.push_back(fmt::format("0x{:08X}", uint32_t(at - membase)));
      ++at;
    }
  };
  uint64_t run_start = 0;
  bool in_run = false;
  for (uint64_t page = start & ~uint64_t(kPageSize - 1); page < end && matches.size() < limit;
       page += kPageSize) {
    if (PageAccessible(memory, uint32_t(page), false)) {
      if (!in_run) run_start = std::max(page, start);
      in_run = true;
    } else if (in_run) {
      search(run_start, page);
      in_run = false;
    }
  }
  if (in_run && matches.size() < limit) search(run_start, std::min(end, uint64_t(1) << 32));
  return {{"matches", matches}, {"truncated", matches.size() >= limit}};
}

json CvarDescription(const std::string& name) {
  json entry = {{"name", name}, {"value", rex::cvar::GetFlagByName(name)}};
  if (const auto* info = rex::cvar::GetFlagInfo(name)) {
    entry["category"] = info->category;
    entry["description"] = info->description;
    entry["default"] = info->default_value;
    entry["requires_restart"] = info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
  }
  return entry;
}

json CvarList(const json& request) {
  const std::string filter = request.value("filter", std::string());
  json flags = json::array();
  for (const std::string& name : rex::cvar::ListFlags()) {
    if (!filter.empty() && name.find(filter) == std::string::npos) continue;
    flags.push_back(CvarDescription(name));
    if (flags.size() >= 500) break;
  }
  return {{"cvars", flags}};
}

json CvarSet(const json& request) {
  const std::string name = request.at("name").get<std::string>();
  const std::string value = request.at("value").get<std::string>();
  if (!rex::cvar::SetFlagByName(name, value)) {
    throw std::runtime_error(fmt::format("{} rejected '{}'", name, value));
  }
  REXLOG_INFO("gta4-debug-server: {} = {}", name, value);
  return CvarDescription(name);
}

// The last lines of the newest log file.
json LogTail(const json& request) {
  const size_t wanted = std::min<size_t>(request.value("lines", size_t(200)), kMaxLogLines);
  std::filesystem::path newest;
  std::filesystem::file_time_type newest_time{};
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(g_logs_dir, error)) {
    if (entry.path().extension() != ".log") continue;
    const auto time = entry.last_write_time(error);
    if (!error && (newest.empty() || time > newest_time)) {
      newest = entry.path();
      newest_time = time;
    }
  }
  if (newest.empty()) throw std::runtime_error("no log in " + g_logs_dir.string());
  std::ifstream file(newest, std::ios::binary | std::ios::ate);
  const std::streamoff size = file.tellg();
  const std::streamoff from = std::max<std::streamoff>(0, size - (8 << 20));
  file.seekg(from);
  std::string text(size_t(size - from), '\0');
  file.read(text.data(), std::streamsize(text.size()));
  std::vector<std::string> lines;
  for (size_t begin = 0; begin < text.size();) {
    size_t newline = text.find('\n', begin);
    if (newline == std::string::npos) newline = text.size();
    lines.emplace_back(text, begin, newline - begin);
    begin = newline + 1;
  }
  if (lines.size() > wanted) lines.erase(lines.begin(), lines.end() - wanted);
  return {{"file", newest.string()}, {"lines", lines}};
}

json Handle(const json& request) {
  const std::string command = request.at("cmd").get<std::string>();
  if (command == "ping") return {{"pong", true}};
  if (command == "cvar_list") return CvarList(request);
  if (command == "cvar_get") return CvarDescription(request.at("name").get<std::string>());
  if (command == "cvar_set") return CvarSet(request);
  if (command == "log_tail") return LogTail(request);
  auto* memory = GuestMemory();
  if (!memory) throw std::runtime_error("the runtime is not up yet");
  if (command == "info") return Info(memory);
  if (command == "read") return Read(memory, request);
  if (command == "write") return Write(memory, request);
  if (command == "find") return Find(memory, request);
  throw std::runtime_error("unknown command '" + command + "'");
}

bool SendLine(Socket client, std::string line) {
  line.push_back('\n');
  for (size_t sent = 0; sent < line.size();) {
    const auto result =
        send(client, line.data() + sent, int(std::min<size_t>(line.size() - sent, 1 << 20)), kSendFlags);
    if (result <= 0) return false;
    sent += size_t(result);
  }
  return true;
}

void Serve(Socket client) {
  std::string pending;
  char buffer[64 << 10];
  for (;;) {
    const auto received = recv(client, buffer, int(sizeof(buffer)), 0);
    if (received <= 0) return;
    pending.append(buffer, size_t(received));
    if (pending.size() > kMaxRequestBytes) return;
    for (size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
      const std::string line = pending.substr(0, newline);
      pending.erase(0, newline + 1);
      json reply;
      try {
        const json request = json::parse(line);
        reply = Handle(request);
        reply["ok"] = true;
        if (request.contains("id")) reply["id"] = request["id"];
      } catch (const std::exception& exception) {
        reply = {{"ok", false}, {"error", exception.what()}};
      }
      if (!SendLine(client, reply.dump(-1, ' ', false, json::error_handler_t::replace))) return;
    }
  }
}

}  // namespace

void Start(const std::filesystem::path& logs_dir) {
  static std::atomic<bool> started{false};
  const uint32_t port = REXCVAR_GET(gta4_debug_server_port);
  if (!port || started.exchange(true)) return;
  g_logs_dir = logs_dir;
#if defined(_WIN32)
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    REXLOG_ERROR("gta4-debug-server: WSAStartup failed");
    return;
  }
#endif
  const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kNoSocket) {
    REXLOG_ERROR("gta4-debug-server: no socket");
    return;
  }
  const int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(uint16_t(port));
  // Loopback only: the tool reaches it through an SSH tunnel, never the network.
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 1) != 0) {
    REXLOG_ERROR("gta4-debug-server: cannot listen on 127.0.0.1:{}", port);
    CloseSocket(listener);
    return;
  }
  REXLOG_INFO("gta4-debug-server: listening on 127.0.0.1:{} (writes {})", port,
              REXCVAR_GET(gta4_debug_server_allow_write) ? "on" : "off");
  std::thread([listener] {
    for (;;) {
      const Socket client = accept(listener, nullptr, nullptr);
      if (client == kNoSocket) continue;
      REXLOG_INFO("gta4-debug-server: client connected");
      Serve(client);
      CloseSocket(client);
      REXLOG_INFO("gta4-debug-server: client disconnected");
    }
  }).detach();
}

}  // namespace gta4::debug_server
