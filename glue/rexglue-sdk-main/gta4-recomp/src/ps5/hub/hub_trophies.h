// GTA IV PS5 - its Xbox 360 achievements as PS5-style trophies: the
// hub's Trophies page (hub_trophies_page.cpp) and the popup in the game
// (ps5/trophies_game.cpp).
//
// The achievements are GTA IV's (in its default.xex), and every one of them
// counts toward the platinum. The game unlocks them as on the Xbox 360 and
// the runtime keeps them in user_data/achievements/545407F2.toml. When the
// game starts, the port also writes their list and pictures there
// (WriteCatalog), so the hub, which opens before the game, can show them.
//
// Grades by points: up to 15 bronze, up to 30 silver, more gold.
//
// Plain C++ and ImGui (the hub's preview builds it on a PC).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"

namespace gta4ps5::trophies {

constexpr uint32_t kTitleId = 0x545407F2;
// (the achievement's flags: its description shows while it's locked; without it, it's a hidden one)
constexpr uint32_t kShowUnachieved = 0x8;

enum class Grade { kBronze, kSilver, kGold, kPlatinum };

Grade GradeFor(uint32_t gamerscore);
const char* GradeName(Grade grade);  // "Bronze"...
// The achievements that count (all of GTA IV's).
bool Here(uint32_t id);

struct Trophy {
  uint32_t id = 0, gamerscore = 0, flags = 0, image_id = 0;
  std::string name, description, locked_description;
  int64_t earned = 0;  // unix time; 0: not earned
  Grade grade() const { return GradeFor(gamerscore); }
  bool hidden() const { return !(flags & kShowUnachieved); }
};

struct List {
  bool loaded = false;           // the list was there (the game has started once)
  std::vector<Trophy> trophies;  // the game's order
  int earnable = 0, earned = 0;
  int grade_total[3] = {}, grade_earned[3] = {};
  uint32_t points = 0, points_earned = 0;
  int64_t last_earned = 0;
  // The progress, 0-100 (by points, as the game weighs them).
  int Percent() const;
  bool Platinum() const { return earnable > 0 && earned == earnable; }
};

// UserData/achievements and its files.
std::filesystem::path Folder(const std::filesystem::path& user_data);
std::filesystem::path IconFile(const std::filesystem::path& user_data, uint32_t image_id);
std::filesystem::path TitleIconFile(const std::filesystem::path& user_data);

// The list with what's been earned (the runtime's unlocks file).
List Load(const std::filesystem::path& user_data);
// The list's file, written when it changed (the game, as it starts).
bool WriteCatalog(const std::filesystem::path& user_data, const std::vector<Trophy>& trophies);

int64_t UnixTime(uint64_t filetime);

// The popup's card (top right), `age` seconds into its showing: in from the
// right, then away. `display`: the screen's size.
struct Card {
  ImTextureID picture = 0;  // the achievement's (the platinum: the game's); 0: a cup
  std::string name;
  Grade grade = Grade::kBronze;
  bool platinum = false;
  float Seconds() const { return platinum ? 7.0f : 5.5f; }
};
void DrawCard(ImDrawList* dl, ImVec2 display, const Card& card, float age, ImFont* body, ImFont* bold);

// A trophy cup, `height` tall, centred on `c`: in its grade's colours (not
// lit: greyed, for one not earned yet); `on_light`: on a light background.
void DrawCup(ImDrawList* dl, ImVec2 c, float height, Grade grade, float alpha = 1, bool lit = true,
             bool on_light = false);
ImU32 GradeColor(Grade grade, float alpha = 1);

}  // namespace gta4ps5::trophies
