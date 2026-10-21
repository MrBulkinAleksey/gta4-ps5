// GTA IV PS5 - renders the hub (gta4-recomp/src/ps5/hub/hub_ui.cpp) to a picture on
// a PC, with a tiny software rasterizer: previews without the console.
//   hub_preview <out.rgb> <width> <height> <app dir> <fonts dir> <logo.png> <hero.jpg> [presses...]
// Presses: one per frame, e.g. "r1 down down right". HUB_BUTTONS=<dir>: the
// button pictures (<dir>/Cross.png...). HUB_TIME=<seconds>: how long the hub
// has been open (the opening animations), default 3.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"
#include "hub_ui.h"

namespace {
struct Tex {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

std::vector<float> g_px;  // RGB, linear 0..1
int g_w, g_h;

void Sample(const Tex* t, float u, float v, float out[4]) {
  if (!t) {
    out[0] = out[1] = out[2] = out[3] = 1;
    return;
  }
  float x = u * t->w - 0.5f, y = v * t->h - 0.5f;
  int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  float fx = x - x0, fy = y - y0;
  auto px = [&](int xx, int yy, int c) {
    xx = std::clamp(xx, 0, t->w - 1);
    yy = std::clamp(yy, 0, t->h - 1);
    return t->rgba[(size_t(yy) * t->w + xx) * 4 + c] / 255.0f;
  };
  for (int c = 0; c < 4; ++c) {
    float a = px(x0, y0, c) * (1 - fx) + px(x0 + 1, y0, c) * fx;
    float b = px(x0, y0 + 1, c) * (1 - fx) + px(x0 + 1, y0 + 1, c) * fx;
    out[c] = a * (1 - fy) + b * fy;
  }
}

void Raster(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const Tex* tex, const ImVec4& clip) {
  float minx = std::max(clip.x, std::min({a.pos.x, b.pos.x, c.pos.x}));
  float maxx = std::min(clip.z, std::max({a.pos.x, b.pos.x, c.pos.x}));
  float miny = std::max(clip.y, std::min({a.pos.y, b.pos.y, c.pos.y}));
  float maxy = std::min(clip.w, std::max({a.pos.y, b.pos.y, c.pos.y}));
  float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
  if (std::fabs(area) < 1e-8f) return;
  auto col = [](ImU32 c, int i) { return ((c >> (i * 8)) & 0xFF) / 255.0f; };
  for (int y = std::max(0, int(miny)); y < std::min(g_h, int(std::ceil(maxy))); ++y) {
    for (int x = std::max(0, int(minx)); x < std::min(g_w, int(std::ceil(maxx))); ++x) {
      float px = x + 0.5f, py = y + 0.5f;
      float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
      float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
      float w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      float u = a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2;
      float v = a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2;
      float t[4];
      Sample(tex, u, v, t);
      float cr = (col(a.col, 0) * w0 + col(b.col, 0) * w1 + col(c.col, 0) * w2) * t[0];
      float cg = (col(a.col, 1) * w0 + col(b.col, 1) * w1 + col(c.col, 1) * w2) * t[1];
      float cb = (col(a.col, 2) * w0 + col(b.col, 2) * w1 + col(c.col, 2) * w2) * t[2];
      float ca = (col(a.col, 3) * w0 + col(b.col, 3) * w1 + col(c.col, 3) * w2) * t[3];
      float* d = &g_px[(size_t(y) * g_w + x) * 3];
      d[0] = d[0] * (1 - ca) + cr * ca;
      d[1] = d[1] * (1 - ca) + cg * ca;
      d[2] = d[2] * (1 - ca) + cb * ca;
    }
  }
}

Tex* LoadPng(const std::string& path) {
  int w, h, n;
  unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!p) return nullptr;
  auto* t = new Tex{w, h, std::vector<uint8_t>(p, p + size_t(w) * h * 4)};
  stbi_image_free(p);
  return t;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 8) {
    std::fprintf(stderr, "usage: hub_preview <out.rgb> <w> <h> <app dir> <fonts dir> <logo> <hero> [presses...]\n");
    return 1;
  }
  using namespace gta4ps5::hub;
  g_w = std::atoi(argv[2]);
  g_h = std::atoi(argv[3]);
  const std::string app = argv[4], fonts = argv[5];
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(g_w), float(g_h));
  // (the console's sizes: made for 4K, drawn scaled)
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  Assets a;
  a.display = io.Fonts->AddFontFromFileTTF((fonts + "/oswald-latin-700-normal.ttf").c_str(), 72.0f * 2, &cfg);
  a.nav = io.Fonts->AddFontFromFileTTF((fonts + "/oswald-latin-600-normal.ttf").c_str(), 34.0f * 2, &cfg);
  a.body = io.Fonts->AddFontFromFileTTF((fonts + "/inter-latin-400-normal.ttf").c_str(), 26.0f * 2, &cfg);
  a.body_bold = io.Fonts->AddFontFromFileTTF((fonts + "/inter-latin-600-normal.ttf").c_str(), 26.0f * 2, &cfg);
  {  // the menu type, at the hub's sizes (the bytes kept for the atlas's life)
    auto read = [](const std::string& f) {
      std::ifstream in(f, std::ios::binary);
      return new std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const auto* bold = read(fonts + "/texgyreheroscn-bold.otf");
    const auto* regular = read(fonts + "/texgyreheroscn-regular.otf");
    AddMenuFonts(io.Fonts, bold->data(), int(bold->size()), regular->data(), int(regular->size()), a);
  }
  unsigned char* px;
  int fw, fh;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  if (std::getenv("HUB_ATLAS")) std::fprintf(stderr, "font atlas %dx%d\n", fw, fh);
  Tex font{fw, fh, std::vector<uint8_t>(px, px + size_t(fw) * fh * 4)};
  io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(&font));
  if (Tex* t = LoadPng(argv[6])) a.logo = {reinterpret_cast<ImTextureID>(t), float(t->w), float(t->h)};
  if (Tex* t = LoadPng(argv[7])) a.hero = {reinterpret_cast<ImTextureID>(t), float(t->w), float(t->h)};
  if (const char* dir = std::getenv("HUB_BUTTONS")) {
    const std::pair<uint32_t, const char*> names[] = {{kCross, "Cross"},       {kCircle, "Circle"}, {kSquare, "Square"},
                                                      {kTriangle, "Triangle"}, {kL1, "L1"},         {kR1, "R1"},
                                                      {kOptions, "Options"},   {kLeft | kRight, "D-Pad"}, {kR3, "R3"}};
    for (const auto& [b, n] : names)
      if (Tex* t = LoadPng(std::string(dir) + "/" + n + ".png")) {
        Image img{reinterpret_cast<ImTextureID>(t), float(t->w), float(t->h)};
        MeasureFill(img, t->rgba.data(), t->w, t->h);
        a.buttons[b] = img;
      }
  }
  // Home's panels (HUB_HOME: a folder as hub/home)
  auto tex = [](const Pixels& p) {
    auto* t = new Tex{p.w, p.h, p.rgba};
    return Image{reinterpret_cast<ImTextureID>(t), float(p.w), float(p.h)};
  };
  if (const char* dir = std::getenv("HUB_HOME")) {
    const char* games[] = {"gta4", "tlad", "tbogt"};
    for (int g = 0; g < 3; ++g) {
      for (const char* ext : {".jpg", ".png"}) {
        int w, h, n;
        if (a.home[g].picture) break;
        unsigned char* p = stbi_load((std::string(dir) + "/" + games[g] + ext).c_str(), &w, &h, &n, 4);
        if (!p) continue;
        const Pixels px = Shrink(p, w, h, kHomePictureHeight);
        stbi_image_free(p);
        a.home[g].picture = tex(px);
        a.home[g].grey = tex(Grey(px));
        a.home[g].color = Accent(px);
      }
      if (Tex* t = LoadPng(std::string(dir) + "/" + games[g] + "-logo.png"))
        a.home[g].logo = {reinterpret_cast<ImTextureID>(t), float(t->w), float(t->h)};
    }
  }
  if (!a.home[0].logo) a.home[0].logo = a.logo;
  Hub hub;
  Paths paths{app, app + "/native.toml", app + "/UserData"};
  hub.SetImageLoader([](const std::filesystem::path& file) {
    Tex* t = LoadPng(file.string());
    return t ? Image{reinterpret_cast<ImTextureID>(t), float(t->w), float(t->h)} : Image{};
  });
  // The Refresh Rate's switch, as on the PS5 (HUB_DISPLAY: "ok" takes 120 Hz, "refuse" doesn't).
  static bool high_refresh = false;
  if (const char* d = std::getenv("HUB_DISPLAY"))
    hub.SetDisplay({[] { return high_refresh; },
                    [ok = std::string(d) == "ok"](bool high) { return high_refresh = high && ok, !high || ok; }});
  hub.Init(paths, a, std::getenv("HUB_VERSION") ? std::getenv("HUB_VERSION") : "");
  hub.SetHasMusic(true);
  std::vector<uint32_t> script;
  for (int i = 8; i < argc; ++i) {
    const std::string s = argv[i];
    const uint32_t b = s == "up" ? kUp : s == "down" ? kDown : s == "left" ? kLeft : s == "right" ? kRight
                     : s == "cross" ? kCross : s == "circle" ? kCircle : s == "l1" ? kL1 : s == "r1" ? kR1
                     : s == "options" ? kOptions : s == "square" ? kSquare : s == "triangle" ? kTriangle
                     : s == "r3" ? kR3 : 0;
    script.push_back(b);
  }
  // then time passes, so the animations settle
  const float open = std::getenv("HUB_TIME") ? float(std::atof(std::getenv("HUB_TIME"))) : 3.0f;
  const float step = 0.05f;
  for (float t = 0; t < open; t += step) script.push_back(0);
  for (size_t f = 0; f < script.size(); ++f) {
    io.DeltaTime = step;
    ImGui::NewFrame();
    Pad pad;
    pad.pressed = pad.held = script[f];
    hub.Draw(pad, step);
    ImGui::Render();
  }
  g_px.assign(size_t(g_w) * g_h * 3, 0);
  ImDrawData* dd = ImGui::GetDrawData();
  for (int l = 0; l < dd->CmdListsCount; ++l) {
    const ImDrawList* list = dd->CmdLists[l];
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      const Tex* tex = reinterpret_cast<const Tex*>(cmd.GetTexID());
      for (unsigned i = 0; i < cmd.ElemCount; i += 3) {
        const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset + i;
        Raster(list->VtxBuffer[cmd.VtxOffset + idx[0]], list->VtxBuffer[cmd.VtxOffset + idx[1]],
               list->VtxBuffer[cmd.VtxOffset + idx[2]], tex, cmd.ClipRect);
      }
    }
  }
  std::vector<uint8_t> out(size_t(g_w) * g_h * 3);
  for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(std::clamp(g_px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
  std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
  return 0;
}
