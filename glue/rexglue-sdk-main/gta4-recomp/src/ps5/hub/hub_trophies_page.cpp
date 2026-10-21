// GTA IV PS5 - the hub's Trophies page (see hub_ui.h, hub_trophies.h):
// how far along you are, then the trophies - earned (newest first), then not
// yet - with the focused one's details.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <future>
#include <string>

#include "hub_style.h"
#include "hub_ui.h"

namespace gta4ps5::hub {

namespace {

constexpr float kRowH = 62;

std::string When(int64_t t) { return lang::Date(t); }

const char* kGroupNames[] = {"Earned", "Not earned yet"};

}  // namespace

void Hub::LoadTrophies() {
  if (trophies_load_.valid()) return;  // (one at a time)
  trophies_load_ =
      std::async(std::launch::async, [user_data = paths_.user_data] { return trophies::Load(user_data); });
}

void Hub::TakeTrophies(trophies::List list) {
  trophies_ = std::move(list);
  trophy_items_.clear();
  if (!trophies_.loaded) return;
  const auto& all = trophies_.trophies;
  // earned: the platinum first, then the newest
  std::vector<TrophyItem> earned, rest;
  for (int i = 0; i < int(all.size()); ++i) {
    const auto& t = all[size_t(i)];
    if (t.earned) earned.push_back({i, 0});
    else rest.push_back({i, 1});
  }
  std::stable_sort(earned.begin(), earned.end(), [&](const TrophyItem& a, const TrophyItem& b) {
    return all[size_t(a.index)].earned > all[size_t(b.index)].earned;
  });
  const bool plat = trophies_.Platinum();
  if (plat) trophy_items_.push_back({-1, 0});
  trophy_items_.insert(trophy_items_.end(), earned.begin(), earned.end());
  if (!plat) trophy_items_.push_back({-1, 1});
  trophy_items_.insert(trophy_items_.end(), rest.begin(), rest.end());
  item_[kTrophiesTab] = std::clamp(item_[kTrophiesTab], 0, int(trophy_items_.size()) - 1);
}

// A trophy's picture (the platinum's: the game's own), loaded the first time it's drawn.
Image Hub::TrophyPicture(int index) {
  const uint32_t key = index < 0 ? 0 : std::max(1u, trophies_.trophies[size_t(index)].image_id);
  if (auto it = trophy_pictures_.find(key); it != trophy_pictures_.end()) return it->second;
  // (two a frame at most: a page of them at once held the hub up when the
  // page first came by; the rest show the frame after)
  if (pictures_this_frame_ >= 2) return Image{};
  ++pictures_this_frame_;
  Image img;
  if (load_image_)
    img = load_image_(index < 0 ? trophies::TitleIconFile(paths_.user_data)
                                : trophies::IconFile(paths_.user_data, trophies_.trophies[size_t(index)].image_id));
  trophy_pictures_[key] = img;
  return img;
}

// The trophy's picture in a rounded square: dimmed until it's earned, and a
// question mark for a hidden one.
void Hub::TrophyIcon(int index, float x, float y, float size, float over) {
  const bool plat = index < 0;
  const trophies::Trophy* t = plat ? nullptr : &trophies_.trophies[size_t(index)];
  const bool earned = plat ? trophies_.Platinum() : t->earned != 0;
  const float r = size * 0.06f;  // (square, as the menu's boxes)
  if (t && !earned && t->hidden()) {
    Rect(x, y, size, size, Mix(Rgba(38, 40, 46), Rgba(206, 208, 214), over), r);
    TextMid(Bold(), size * 0.6f, x + size * 0.5f, y + size * 0.5f, Mix(White(0.5f), Black(0.45f), over), "?", 0.5f);
    return;
  }
  const Image img = TrophyPicture(index);
  if (img) {
    const ImU32 tint = earned ? White(1) : Mix(White(0.34f), White(0.55f), over);
    dl_->AddImageRounded(img.id, P(x, y), P(x + size, y + size), ImVec2(0, 0), ImVec2(1, 1), tint, r * s_);
  } else {
    Rect(x, y, size, size, Mix(Rgba(38, 40, 46), Rgba(206, 208, 214), over), r);
    trophies::DrawCup(dl_, P(x + size * 0.5f, y + size * 0.5f), size * 0.56f * s_,
                      plat ? trophies::Grade::kPlatinum : t->grade(), 1, earned);
  }
  if (!earned) {  // a padlock in the corner
    const float lx = x + size - size * 0.2f, ly = y + size - size * 0.2f, k = size / 64.0f;
    dl_->AddCircleFilled(P(lx, ly), 13 * k * s_, Black(0.72f), 24);
    dl_->AddRectFilled(P(lx - 5.5f * k, ly - 1.5f * k), P(lx + 5.5f * k, ly + 6.5f * k), White(0.85f), 1.5f * k * s_);
    dl_->PathClear();
    dl_->PathArcTo(P(lx, ly - 2 * k), 3.8f * k * s_, 3.14159f, 6.28318f, 12);
    dl_->PathStroke(White(0.85f), 0, 1.8f * k * s_);
    dl_->AddLine(P(lx - 3.8f * k, ly - 2 * k), P(lx - 3.8f * k, ly - 1.5f * k), White(0.85f), 1.8f * k * s_);
    dl_->AddLine(P(lx + 3.8f * k, ly - 2 * k), P(lx + 3.8f * k, ly - 1.5f * k), White(0.85f), 1.8f * k * s_);
  }
}

// How far along (the details' top): the percentage, earned of all, and the
// bar under it in the menu's yellow.
float Hub::TrophySummary() {
  const float x = kPanelX, w = kPanelW, y = kPanelY;
  const bool plat = trophies_.Platinum();
  const std::string pct = std::to_string(trophies_.Percent()) + "%";
  TextMid(Bold(), 64, x, y + 30, kText, pct);
  const float pw = TextWidth(Bold(), 64, pct);
  TextMid(Regular(), 28, x + pw + 26, y + 38, kMenuGray,
          lang::F("{} of {} earned", {std::to_string(trophies_.earned), std::to_string(trophies_.earnable)}));
  // each grade's cup and its count, at the right
  float rx = x + w;
  for (int g = 0; g < 3; ++g) {  // (bronze at the right, gold at the left)
    const std::string n = std::to_string(trophies_.grade_earned[g]) + "/" + std::to_string(trophies_.grade_total[g]);
    const float tw = TextWidth(Bold(), 26, n);
    TextMid(Bold(), 26, rx - tw, y + 38, trophies_.grade_earned[g] ? kText : kDimGray, n);
    trophies::DrawCup(dl_, P(rx - tw - 20, y + 38), 28 * s_, trophies::Grade(g), 1, trophies_.grade_earned[g] > 0);
    rx -= tw + 60;
  }
  const float by = y + 82;
  Rect(x, by, w, 10, kBarBack);
  const float f = std::clamp(trophies_.points ? float(trophies_.points_earned) / float(trophies_.points) : 0.0f, 0.0f, 1.0f);
  if (f > 0) Rect(x, by, std::max(4.0f, w * f), 10, plat ? trophies::GradeColor(trophies::Grade::kPlatinum) : pal_.title);
  return by + 10;
}

// A page with nothing to show yet: what to do.
void Hub::Empty(const char* title, const std::string& text) {
  TextMid(Bold(), 40, kListX, kListY + 30, kText, title);
  Wrapped(Regular(), 29, kListX, kListY + 76, 900, kMenuGray, text, 1.38f);
}

void Hub::DrawTrophies() {
  if (!trophies_.loaded) {
    Empty(lang::T("Start the game once"),
          lang::T("The trophy list comes from the game itself: start it once, and the next time you open the hub "
                  "your trophies are here, with the ones you've earned."));
    return;
  }
  const int n = int(trophy_items_.size());
  if (n == 0) return;
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  int counts[2] = {};
  for (const TrophyItem& it : trophy_items_) ++counts[it.group];
  for (int i = 0; i < n; ++i) {
    const int g = trophy_items_[size_t(i)].group;
    if (i == 0 || trophy_items_[size_t(i) - 1].group != g)
      rows.push_back({-1, std::string(lang::T(kGroupNames[g])) + "  " + std::to_string(counts[g])});
    rows.push_back({i});
  }
  auto name_of = [&](const TrophyItem& it) -> std::string {
    if (it.index < 0) return lang::T("Grand Theft Auto IV Platinum");
    const auto& t = trophies_.trophies[size_t(it.index)];
    return (!t.earned && t.hidden()) ? lang::T("Hidden trophy") : t.name;
  };
  auto text_of = [&](const TrophyItem& it) -> std::string {
    if (it.index < 0) return lang::F("Earn every other GTA IV trophy ({}).", {std::to_string(trophies_.earnable)});
    const auto& t = trophies_.trophies[size_t(it.index)];
    if (!t.earned && t.hidden()) return lang::T("Keep playing to find out what this one is.");
    return t.earned || t.locked_description.empty() ? t.description : t.locked_description;
  };
  const float x = kListX, right = kPanelX - 90;
  List(rows, sel, kRowH, [&](int i, float ry, float over) {
    const TrophyItem& it = trophy_items_[size_t(i)];
    const bool plat = it.index < 0;
    const bool earned = plat ? trophies_.Platinum() : trophies_.trophies[size_t(it.index)].earned != 0;
    const float cy = ry + kRowH * 0.5f;
    TrophyIcon(it.index, x, cy - 24, 48, 0);
    const ImU32 col = Mix(earned ? pal_.row : kDimGray, pal_.focus, over);
    TextMid(Bold(), 31, x + 70, cy, col, Fit(Bold(), 31, name_of(it), right - x - 70 - 50));
    const trophies::Grade g = plat ? trophies::Grade::kPlatinum : trophies_.trophies[size_t(it.index)].grade();
    trophies::DrawCup(dl_, P(right - 14, cy), 30 * s_, g, 1, earned);
  });

  // how far along, then the focused trophy
  const float top = TrophySummary() + 40;
  const TrophyItem& it = trophy_items_[size_t(sel)];
  const int v0 = dl_->VtxBuffer.Size;
  {
    const float px = kPanelX + (1 - Smooth(panel_in_)) * 12, pw = kPanelW;
    float py = top;
    const bool plat = it.index < 0;
    const trophies::Trophy* t = plat ? nullptr : &trophies_.trophies[size_t(it.index)];
    const bool earned = plat ? trophies_.Platinum() : t->earned != 0;
    const trophies::Grade g = plat ? trophies::Grade::kPlatinum : t->grade();
    TrophyIcon(it.index, px, py, 112, 0);
    const float nx = px + 138, nw = pw - 138;
    TextMid(Bold(), 21, nx, py + 14, trophies::GradeColor(g),
            Upper(lang::T((std::string(trophies::GradeName(g)) + " Trophy").c_str())), 0, 1.6f);
    const float nh = Wrapped(Bold(), 36, nx, py + 34, nw, kText, name_of(it), 1.1f);
    py += std::max(112.0f, 34 + nh) + 24;
    py += Wrapped(Regular(), 28, px, py, pw, Rgba(196, 196, 196), text_of(it), 1.38f) + 22;
    std::vector<std::pair<std::string, std::string>> facts;
    if (earned) facts.push_back({lang::T("Earned|date"), When(plat ? trophies_.last_earned : t->earned)});
    else if (plat)
      facts.push_back({lang::T("Status"), lang::F("{} to go", {std::to_string(trophies_.earnable - trophies_.earned)})});
    else facts.push_back({lang::T("Status"), lang::T("Not earned yet|status")});
    if (t) facts.push_back({"Gamerscore", std::to_string(t->gamerscore) + "G"});
    Facts(px, py, pw, facts);
  }
  Fade(v0, Smooth(panel_in_));
}

}  // namespace gta4ps5::hub
