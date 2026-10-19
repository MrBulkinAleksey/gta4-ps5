// The hub preview's stand-in for hub_install.cpp (no game, no runtime).
#include "hub_install.h"
namespace gta4ps5::hub::install {
std::vector<fs::directory_entry> ListDir(const fs::path& dir) {
  std::vector<fs::directory_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(*it);
  return out;
}
std::string Bytes(uint64_t n) { return std::to_string(n >> 20) + " MB"; }
fs::path InstallFolder(const fs::path& app) { return app / "install"; }
fs::path GameRoot(const fs::path& app) { return app / "UserData" / "LibertyRecomp" / "game"; }
State Look(const fs::path&) { State s; s.installed = true; return s; }
}  // namespace gta4ps5::hub::install
