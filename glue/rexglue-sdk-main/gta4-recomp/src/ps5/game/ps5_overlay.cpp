// GTA IV PS5 - the frame rate counter and the frame time capture (see
// ps5_overlay.h).
#include "ps5_overlay.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/frame_stats.h>
#include <rex/logging.h>

#include "../hub/ps5_hub.h"

REXCVAR_DEFINE_BOOL(ps5_fps_counter, false, "PS5",
                    "Show the frame rate counter at the left edge of the screen while playing");
REXCVAR_DEFINE_BOOL(ps5_frametime_capture, false, "PS5",
                    "Record every frame's time to a CSV file in /app0/logs");
REXCVAR_DEFINE_BOOL(ps5_overlay_every_vblank, false, "PS5",
                    "Repaint the game's overlay every vblank (the old way: each new frame waits "
                    "behind repeats of the last one)");

namespace gta4ps5 {

const char* BuildVersion() { return GTA4PS5_VERSION; }

GameOverlay::GameOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
  window_start_ns_ = rex::frame_stats::NowNs();
  window_frames_ = rex::frame_stats::g_presents.load(std::memory_order_acquire);
  if (REXCVAR_GET(ps5_frametime_capture)) {
    std::error_code ec;
    std::filesystem::create_directories("/app0/logs", ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char name[96];
    std::strftime(name, sizeof(name), "/app0/logs/frametimes-%Y-%m-%d-%H-%M-%S.csv", &tm);
    csv_ = std::fopen(name, "w");
    if (csv_) {
      std::fputs("frame,seconds,frame_ms\n", csv_);
      REXLOG_INFO("PS5: frame time capture to {}", name);
    } else {
      REXLOG_WARN("PS5: couldn't start the frame time capture ({})", name);
    }
  }
}

GameOverlay::~GameOverlay() {
  if (csv_) std::fclose(csv_);
}

void GameOverlay::Sample() {
  const int64_t now = rex::frame_stats::NowNs();
  const uint64_t frames = rex::frame_stats::g_presents.load(std::memory_order_acquire);
  const int64_t span = now - window_start_ns_;
  if (span < 500'000'000) return;
  const uint64_t n = frames - window_frames_;
  fps_ = float(double(n) * 1e9 / double(span));
  ms_ = n ? float(double(span) / 1e6 / double(n)) : 0.0f;
  window_start_ns_ = now;
  window_frames_ = frames;
  // the slowest frames of the last few seconds: 1% low and the worst one
  const uint32_t count = uint32_t(std::min<uint64_t>(frames > 1 ? frames - 1 : 0, rex::frame_stats::kHistory));
  std::vector<uint32_t> times;
  times.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t us = rex::frame_stats::g_history_us[(frames - 1 - i) % rex::frame_stats::kHistory].load(
        std::memory_order_relaxed);
    if (us) times.push_back(us);
  }
  if (times.empty()) {
    low_fps_ = worst_ms_ = 0;
    return;
  }
  std::sort(times.begin(), times.end());
  const uint32_t p99 = times[size_t(double(times.size() - 1) * 0.99)];
  low_fps_ = p99 ? float(1e6 / double(p99)) : 0.0f;
  worst_ms_ = float(times.back()) / 1000.0f;
}

void GameOverlay::Capture() {
  if (!csv_) return;
  const uint64_t frames = rex::frame_stats::g_presents.load(std::memory_order_acquire);
  if (frames == captured_) return;
  // (one line for the newest frame; frames between two draws of the overlay
  // share its time, which is rare at the rates the game runs)
  const int64_t now = rex::frame_stats::g_last_present_ns.load(std::memory_order_acquire);
  if (!capture_start_ns_) capture_start_ns_ = now;
  const double ms = rex::frame_stats::g_last_frame_us.load(std::memory_order_relaxed) / 1000.0;
  for (uint64_t f = captured_ + 1; f <= frames; ++f)
    std::fprintf(csv_, "%llu,%.4f,%.3f\n", (unsigned long long)f, double(now - capture_start_ns_) / 1e9, ms);
  captured_ = frames;
  if ((frames & 255) == 0) std::fflush(csv_);
}

namespace {
// green at 60 or better, white at 30, then yellow, then red
ImU32 RateColor(float ms) {
  return ms <= 17.5f ? IM_COL32(120, 230, 140, 255)
         : ms <= 34.5f ? IM_COL32(244, 245, 247, 255)
         : ms <= 50.5f ? IM_COL32(245, 200, 80, 255)
                       : IM_COL32(240, 90, 80, 255);
}
}  // namespace

bool GameOverlay::WantsContinuousRepaint() const { return REXCVAR_GET(ps5_overlay_every_vblank); }

void GameOverlay::OnDraw(ImGuiIO& io) {
  Capture();
  // (the overlay stays up with the counter off, drawing nothing: the game's
  // frames reach the TV the same way either way)
  if (!REXCVAR_GET(ps5_fps_counter)) return;
  Sample();
  const ImVec2 size = io.DisplaySize;
  const float s = std::min(size.x / 1920.0f, size.y / 1080.0f);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const auto& fonts = hub::Fonts();
  ImFont* bold = fonts.body_bold ? fonts.body_bold : ImGui::GetFont();
  ImFont* body = fonts.body ? fonts.body : ImGui::GetFont();
  auto P = [&](float x, float y) { return ImVec2(x * s, y * s); };

  // ---- the counter, on the left edge halfway down (the game's own HUD uses
  // the corners): frames a second, frame time, the slow frames and a graph
  const float w = 264, pad = 12;
  const float graph_h = 48, h = pad + 36 + 20 + 10 + graph_h + pad;
  const float x = 12, y = (size.y / s - h) * 0.5f;
  dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(0, 0, 0, 170), 8 * s);
  char big[16], unit[] = "FPS", ms[32], small[96];
  if (fps_ > 0) {
    std::snprintf(big, sizeof(big), "%.0f", fps_);
    std::snprintf(ms, sizeof(ms), "%.1f ms", ms_);
    std::snprintf(small, sizeof(small), "1%% low %.0f  \xC2\xB7  worst frame %.0f ms", low_fps_, worst_ms_);
  } else {
    std::snprintf(big, sizeof(big), "--");
    ms[0] = small[0] = 0;
  }
  const ImU32 col = fps_ > 0 ? RateColor(ms_) : IM_COL32(244, 245, 247, 255);
  const float big_size = 36 * s;
  const ImVec2 bsz = bold->CalcTextSizeA(big_size, 1e9f, 0, big);
  dl->AddText(bold, big_size, P(x + pad, y + pad - 5), col, big);
  dl->AddText(bold, 15 * s, ImVec2((x + pad) * s + bsz.x + 6 * s, (y + pad + 13) * s), IM_COL32(255, 255, 255, 170), unit);
  const ImVec2 msz = bold->CalcTextSizeA(20 * s, 1e9f, 0, ms);
  dl->AddText(bold, 20 * s, ImVec2((x + w - pad) * s - msz.x, (y + pad + 8) * s), IM_COL32(244, 245, 247, 255), ms);
  dl->AddText(body, 14 * s, P(x + pad, y + pad + 37), IM_COL32(255, 255, 255, 175), small);

  // the graph: one bar a frame, newest on the right, 0 to 66 ms (15 FPS) tall
  const float gx = x + pad, gy = y + pad + 36 + 20 + 10, gw = w - pad * 2;
  dl->AddRectFilled(P(gx, gy), P(gx + gw, gy + graph_h), IM_COL32(255, 255, 255, 18), 4 * s);
  constexpr float kTopMs = 66.7f;
  const uint64_t frames = rex::frame_stats::g_presents.load(std::memory_order_acquire);
  constexpr int kBars = 120;
  const float bar = gw / float(kBars);
  for (int i = 0; i < kBars && uint64_t(i) + 1 < frames; ++i) {
    const uint32_t us =
        rex::frame_stats::g_history_us[(frames - 1 - uint64_t(i)) % rex::frame_stats::kHistory].load(
            std::memory_order_relaxed);
    if (!us) continue;
    const float f_ms = float(us) / 1000.0f;
    const float bh = graph_h * std::min(f_ms, kTopMs) / kTopMs;
    const float bx = gx + gw - bar * float(i + 1);
    dl->AddRectFilled(P(bx, gy + graph_h - bh), P(bx + bar * 0.8f, gy + graph_h), RateColor(f_ms));
  }
  for (float line_ms : {16.7f, 33.3f}) {  // the 60 and 30 FPS marks, over the bars
    const float ly = gy + graph_h - graph_h * line_ms / kTopMs;
    dl->AddLine(P(gx, ly), P(gx + gw, ly), IM_COL32(0, 0, 0, 140), 2 * s);
    dl->AddLine(P(gx, ly), P(gx + gw, ly), IM_COL32(255, 255, 255, 90), 1 * s);
    const char* label = line_ms < 20 ? "60" : "30";
    dl->AddText(bold, 11 * s, P(gx + 3 + 1, ly - 13 + 1), IM_COL32(0, 0, 0, 200), label);
    dl->AddText(bold, 11 * s, P(gx + 3, ly - 13), IM_COL32(255, 255, 255, 200), label);
  }
}

}  // namespace gta4ps5
