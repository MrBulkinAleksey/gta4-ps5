// GTA IV PS5 - the trophy chime, made here: a struck bell or two (see
// hub_sfx.h).
#include "hub_sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gta4ps5::hub {
namespace {

constexpr float kRate = 48000;
constexpr float kPi = 3.14159265f;

struct Buffer {
  std::vector<float> mono;
  explicit Buffer(float seconds) : mono(size_t(seconds * kRate), 0.0f) {}
  // A tone from `start` s: frequency gliding f0 -> f1, attack, then an exponential decay.
  void Tone(float start, float length, float f0, float f1, float amp, float attack, float decay, float bright = 0.25f) {
    const size_t a = size_t(start * kRate), n = size_t(length * kRate);
    float phase = 0;
    for (size_t i = 0; i < n && a + i < mono.size(); ++i) {
      const float t = float(i) / kRate;
      const float f = f0 + (f1 - f0) * std::min(1.0f, t / length);
      phase += 2 * kPi * f / kRate;
      const float env = std::min(1.0f, t / attack) * std::exp(-t / decay);
      // (a touch of the 2nd and 3rd harmonics: rounder than a bare sine)
      const float s = std::sin(phase) + bright * 0.5f * std::sin(2 * phase) + bright * 0.25f * std::sin(3 * phase);
      mono[a + i] += amp * env * s;
    }
  }
  // Noise through a band (two one-pole filters), shaped by rise / fall.
  void Noise(float start, float length, float low_hz, float high_hz, float amp, float rise, float fall) {
    uint32_t seed = 0x9E3779B9u;
    float lp = 0, hp_in = 0, hp = 0;
    const float a_lp = 1 - std::exp(-2 * kPi * high_hz / kRate), a_hp = std::exp(-2 * kPi * low_hz / kRate);
    const size_t a = size_t(start * kRate), n = size_t(length * kRate);
    for (size_t i = 0; i < n && a + i < mono.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float w = float(int32_t(seed)) / 2147483648.0f;
      lp += a_lp * (w - lp);
      hp = a_hp * (hp + lp - hp_in);
      hp_in = lp;
      const float t = float(i) / kRate;
      const float env = t < rise ? t / rise : std::exp(-(t - rise) / fall);
      mono[a + i] += amp * env * hp;
    }
  }
  // A little room: two short echoes, quiet.
  void Room() {
    const size_t d1 = size_t(0.031f * kRate), d2 = size_t(0.047f * kRate);
    std::vector<float> dry = mono;
    for (size_t i = 0; i < mono.size(); ++i) {
      float wet = 0;
      if (i >= d1) wet += 0.16f * mono[i - d1];
      if (i >= d2) wet += 0.10f * mono[i - d2];
      mono[i] = dry[i] + wet;
    }
  }
  std::vector<float> Stereo() const {
    // (the end faded over 5 ms: no click)
    std::vector<float> lr(mono.size() * 2);
    const size_t tail = size_t(0.005f * kRate);
    for (size_t i = 0; i < mono.size(); ++i) {
      float s = mono[i] * 1.6f;  // (made quiet above, brought to sit just under the music)
      if (i + tail > mono.size()) s *= float(mono.size() - i) / float(tail);
      lr[i * 2] = lr[i * 2 + 1] = s;
    }
    return lr;
  }
};

}  // namespace

std::vector<float> TrophyChime(bool platinum) {
  // A bell: each note a struck tone with a quieter, quicker partial above it (2.76x: a bell's, not a string's).
  auto bell = [](Buffer& b, float at, float f, float amp, float decay) {
    b.Tone(at, decay * 5, f, f, amp, 0.002f, decay, 0.12f);
    b.Tone(at, decay * 2.5f, f * 2.76f, f * 2.76f, amp * 0.28f, 0.001f, decay * 0.35f, 0.0f);
    b.Tone(at, decay * 4, f * 0.5f, f * 0.5f, amp * 0.18f, 0.004f, decay * 0.8f, 0.0f);
  };
  if (!platinum) {  // two notes up a fourth, then a soft shimmer
    Buffer b(1.6f);
    bell(b, 0.00f, 1174.66f, 0.13f, 0.20f);  // D6
    bell(b, 0.09f, 1567.98f, 0.15f, 0.32f);  // G6
    b.Noise(0.09f, 0.7f, 5000, 12000, 0.025f, 0.12f, 0.22f);
    b.Room();
    return b.Stereo();
  }
  // the platinum: up a major chord, a low warm note under it
  Buffer b(2.6f);
  const float notes[] = {1046.50f, 1318.51f, 1567.98f, 2093.00f};  // C6 E6 G6 C7
  for (int k = 0; k < 4; ++k) bell(b, k * 0.085f, notes[k], 0.11f + 0.015f * k, 0.22f + 0.12f * k);
  b.Tone(0.0f, 2.2f, 261.63f, 261.63f, 0.07f, 0.03f, 0.6f, 0.3f);  // C4
  b.Noise(0.25f, 1.4f, 5000, 13000, 0.03f, 0.25f, 0.4f);
  b.Room();
  return b.Stereo();
}

}  // namespace gta4ps5::hub
