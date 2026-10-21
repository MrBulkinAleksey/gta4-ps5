// GTA IV PS5 - the PS5 hub's colours and small helpers
// (hub_ui.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "hub_lang.h"
#include "imgui.h"

namespace gta4ps5::hub {

constexpr float kW = 1920, kH = 1080;
// The frame, after GTA IV's pause menu: the tabs across the top, a rule under
// them and one over the footer, the page between them on black.
constexpr float kTabsY = 156;        // the tabs' capitals, centred on this
constexpr float kRuleTop = 206;      // the rule under the tabs
constexpr float kRuleBottom = 873;   // the rule over the footer
constexpr float kHintsY = 913;       // the footer's first line of hints (the second: + 40)
constexpr float kPageL = 128, kPageR = 1792;  // the page's left and right edges
// the page's list on the left (a row: label, then its value from kValueX),
// the details at the right
constexpr float kListX = kPageL, kListW = 900, kListY = 224, kListView = kRuleBottom - 26 - kListY;
constexpr float kValueX = 560;
constexpr float kPanelX = 1100, kPanelW = kPageR - kPanelX, kPanelY = 230;

inline ImU32 Rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline ImU32 White(float a) { return IM_COL32(255, 255, 255, int(std::clamp(a, 0.0f, 1.0f) * 255)); }
inline ImU32 Black(float a) { return IM_COL32(0, 0, 0, int(std::clamp(a, 0.0f, 1.0f) * 255)); }
inline ImU32 WithAlpha(ImU32 c, float a) {
  const int base = int((c >> IM_COL32_A_SHIFT) & 0xFF);
  return (c & ~IM_COL32_A_MASK) | (ImU32(std::clamp(int(base * a), 0, 255)) << IM_COL32_A_SHIFT);
}
inline float Smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
constexpr ImU32 Mix(ImU32 a, ImU32 b, float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  ImU32 out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    const float ca = float((a >> sh) & 0xFF), cb = float((b >> sh) & 0xFF);
    out |= ImU32(ca + (cb - ca) * t + 0.5f) << sh;
  }
  return out;
}

const ImU32 kInk = Rgba(12, 13, 16);      // text on a focused (white) row
const ImU32 kAccent = Rgba(221, 155, 17);  // the menu's gold (#DD9B11): the page, the row, what's on
const ImU32 kText = Rgba(236, 236, 236);
const ImU32 kMenuGray = Rgba(170, 170, 170);  // a row, a tab not chosen
const ImU32 kDimGray = Rgba(112, 112, 112);   // headings, what's not on
const ImU32 kRuleGray = Rgba(96, 96, 96);     // the rules
const ImU32 kBarBack = Rgba(58, 58, 58);      // a level's bar, behind
const ImU32 kBad = Rgba(232, 58, 46);         // something wrong (the installer)

// Each game's menu, as its own pause menu: the hub takes on the look of the
// game chosen on Home (0 GTA IV, 1 The Lost and Damned, 2 The Ballad of Gay
// Tony), easing from one to the next. GTA IV's is the gold and grey above; The
// Lost and Damned's a dark red over grey, the tabs scuffed; The Ballad of Gay
// Tony's a rainbow (each tab its colour, the rules a gradient), pink rows, sky
// blue hints, white for what's chosen.
struct Palette {
  ImU32 tab_off, tab_on;        // a tab, and the chosen one
  ImU32 row, focus;             // a row of a page, and the focused one
  ImU32 title;                  // titles and paths
  ImU32 hint;                   // the footer's labels
  ImU32 toast;                  // the message at the footer's left
  ImU32 rule_top, rule_bottom;  // the rules (The Ballad of Gay Tony: its gradient over them)
};
inline const Palette& GamePalette(int game) {
  static const Palette kPalettes[3] = {
      {Rgba(148, 148, 148), kAccent, kMenuGray, kAccent, kAccent, kMenuGray, kAccent, kRuleGray, kRuleGray},
      {Rgba(146, 146, 146), Rgba(104, 14, 14), Rgba(128, 128, 128), Rgba(156, 26, 26), Rgba(156, 26, 26),
       Rgba(112, 112, 112), Rgba(156, 26, 26), Rgba(52, 52, 52), Rgba(40, 40, 40)},
      {Rgba(192, 18, 96), Rgba(244, 244, 244), Rgba(192, 18, 96), Rgba(244, 244, 244), Rgba(208, 28, 108),
       Rgba(88, 116, 168), Rgba(206, 162, 46), Rgba(140, 70, 110), Rgba(90, 44, 70)},
  };
  return kPalettes[std::clamp(game, 0, 2)];
}
// The Ballad of Gay Tony's colours, left to right (its tabs, from the game's
// Map to Game) and its rule under the tabs (the one over the footer is darker).
constexpr ImU32 kTbogtTabs[] = {IM_COL32(192, 150, 36, 255), IM_COL32(204, 120, 42, 255), IM_COL32(198, 72, 66, 255),
                                IM_COL32(192, 18, 96, 255),  IM_COL32(150, 30, 120, 255), IM_COL32(120, 60, 132, 255),
                                IM_COL32(78, 102, 150, 255)};
constexpr ImU32 kTbogtRule[] = {IM_COL32(202, 190, 58, 255), IM_COL32(213, 154, 54, 255), IM_COL32(208, 99, 71, 255),
                                IM_COL32(193, 34, 101, 255), IM_COL32(163, 33, 121, 255), IM_COL32(128, 69, 137, 255),
                                IM_COL32(94, 106, 157, 255), IM_COL32(72, 131, 165, 255)};
// A colour along one of those, 0..1.
template <size_t N>
inline ImU32 Along(const ImU32 (&stops)[N], float t) {
  const float f = std::clamp(t, 0.0f, 1.0f) * float(N - 1);
  const size_t i = std::min(N - 2, size_t(f));
  return Mix(stops[i], stops[i + 1], f - float(i));
}

// Capitals, accented letters too (UTF-8: à..þ to À..Þ, œ to Œ, ß to SS).
inline std::string Upper(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const unsigned char n = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
    if (c == 0xC3 && n >= 0xA0 && n <= 0xBE && n != 0xB7) {
      out += char(0xC3), out += char(n - 0x20), ++i;
    } else if (c == 0xC3 && n == 0x9F) {
      out += "SS", ++i;
    } else if (c == 0xC5 && n == 0x93) {
      out += char(0xC5), out += char(0x92), ++i;
    } else if (c >= 0xC0) {  // (another letter: as it is)
      out += char(c);
      while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) out += s[++i];
    } else {
      out += char(std::toupper(c));
    }
  }
  return out;
}

inline std::string Size(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / double(1ull << 20));
  else std::snprintf(buf, sizeof(buf), "%.0f KB", double(bytes) / 1024.0);
  return lang::Units(buf);
}

// A folder's files: total size and count.
inline uint64_t FolderBytes(const std::filesystem::path& dir, uint32_t* files = nullptr) {
  uint64_t total = 0;
  uint32_t n = 0;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) total += it->file_size(ec), ++n;
  }
  if (files) *files = n;
  return total;
}

}  // namespace gta4ps5::hub
