#include "autotune.h"
#include "theory.h"
#include <algorithm>

Autotune::Autotune() : ring(4096, 0.0f), d(N / 2 + 1, 0.0f) {}

void Autotune::detect() {
  const int W = N / 2; float x[N];
  size_t start = (pos + ring.size() - N) & (ring.size() - 1);
  for (int i = 0; i < N; i++) x[i] = ring[(start + i) & (ring.size() - 1)];
  int tauMin = RATE / 1000, tauMax = std::min(W - 1, RATE / 70);
  float e = 0; for (int i = 0; i < W; i++) e += x[i] * x[i];
  if (e < 2e-4f) { confidence *= 0.7f; return; }
  for (int tau = tauMin; tau <= tauMax; tau++) { float s = 0; for (int i = 0; i < W; i += 2) { float dd = x[i] - x[i + tau]; s += dd * dd; } d[tau] = s; }
  float run = 0; for (int tau = tauMin; tau <= tauMax; tau++) { run += d[tau]; d[tau] = d[tau] * (tau - tauMin + 1) / (run + 1e-9f); }
  int best = -1;
  for (int tau = tauMin + 1; tau < tauMax; tau++) if (d[tau] < 0.2f && d[tau] <= d[tau + 1] && d[tau] <= d[tau - 1]) { best = tau; break; }
  if (best < 0) { confidence *= 0.7f; return; }
  float a = d[best - 1], b = d[best], c = d[best + 1]; float den = a - 2 * b + c; float tauF = best + (fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0);
  float midi = theory::hzToMidi((float)RATE / tauF);
  detectedMidi = confidence > 0.3f ? detectedMidi + 0.5f * (midi - detectedMidi) : midi;
  confidence = 1.0f - b;
}

float Autotune::process(float in, uint16_t keyMask, float forcedMidi, float amount, float speed, float transpose) {
  ring[pos] = in; pos = (pos + 1) & (ring.size() - 1);
  if (++hopFill >= HOP) {
    hopFill = 0; detect();
    if (confidence > 0.35f) {
      float target = forcedMidi >= 0 ? forcedMidi : (float)theory::nearestInMask(detectedMidi + transpose, keyMask ? keyMask : 0xFFF);
      targetMidi = target;
      float shift = (target - detectedMidi) * amount + (forcedMidi >= 0 ? 0.0f : 0.0f);
      // keep the shift within an octave: a forced note far from the voice is taken at the nearest octave
      while (shift > 7.0f) shift -= 12.0f; while (shift < -7.0f) shift += 12.0f;
      if (forcedMidi < 0) shift += transpose * (1.0f - amount);
      ratioTarget = powf(2.0f, shift / 12.0f);
    }
  }
  float k = 0.0005f + speed * speed * 0.02f;      // per-sample glide towards the target ratio
  ratio += (ratioTarget - ratio) * k;
  if (fabsf(ratio - 1.0f) < 0.0007f) return in;    // nothing to do: pass the voice straight through
  return shifter.process(in, ratio);
}
