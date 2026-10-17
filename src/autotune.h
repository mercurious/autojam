// autojam: the pitch corrector for the voice bus. A YIN detector finds the pitch of the incoming voice (Gemma's
// Pocket TTS lines, or the humans' mic when they ask for it); the target is a forced note (a lyric's melody) or
// the nearest note of the key; a two-grain delay-line shifter moves the voice there. Formants are not preserved
// (that is the hard, tuned sound). Per-sample, no allocation after construction.
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>

class PitchShifter {
public:
  PitchShifter() : buf(8192, 0.0f) {}
  // ratio > 1 raises the pitch
  float process(float in, float ratio) {
    buf[w] = in; w = (w + 1) & (buf.size() - 1);
    // two read heads moving at (1 - ratio) through a grain window, crossfaded by a raised cosine
    float out = 0;
    for (int g = 0; g < 2; g++) {
      float ph = grainPhase + 0.5f * g; if (ph >= 1.0f) ph -= 1.0f;
      float delay = 32.0f + ph * grainLen;                       // samples behind the write head
      float rp = (float)w - delay; while (rp < 0) rp += (float)buf.size();
      size_t i0 = (size_t)rp & (buf.size() - 1); size_t i1 = (i0 + 1) & (buf.size() - 1); float fr = rp - floorf(rp);
      float s = buf[i0] * (1 - fr) + buf[i1] * fr;
      float win = 0.5f - 0.5f * cosf(6.2831853f * ph);
      out += s * win;
    }
    grainPhase += (ratio - 1.0f) / grainLen; while (grainPhase >= 1.0f) grainPhase -= 1.0f; while (grainPhase < 0.0f) grainPhase += 1.0f;
    return out;
  }
private:
  std::vector<float> buf; size_t w = 0; float grainPhase = 0; float grainLen = 1800.0f;   // ~37 ms grains
};

class Autotune {
public:
  static constexpr int RATE = 48000, N = 2048, HOP = 256;
  Autotune();
  // key: pitch-class mask the voice may land on (0xFFF: chromatic). forcedMidi >= 0 pins the target note.
  // amount 0..1 how far towards the target, speed 0..1 how fast (1 = hard snap), transpose in semitones.
  float process(float in, uint16_t keyMask, float forcedMidi, float amount, float speed, float transpose);
  float detectedMidi = 0, targetMidi = 0, confidence = 0;
private:
  std::vector<float> ring; size_t pos = 0; int hopFill = 0; std::vector<float> d;
  float ratio = 1.0f, ratioTarget = 1.0f; PitchShifter shifter;
  void detect();
};
