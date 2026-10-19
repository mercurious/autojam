#include "singer.h"
#include "theory.h"
#include "synth.h"
#include <cmath>
#include <algorithm>

namespace {
const int SR = 24000, HOP = 120, W = 600;            // analysis at the TTS rate: 25 ms frames every 5 ms
const int OUT = 48000;

struct Syllable { int cs, vs, ve, ns = 0, ne = 0; };  // consonant start, voiced start and end, its loud nucleus, in frames

// YIN on one frame (as in the autotune): the f0 in Hz, 0 when unvoiced
float yin(const float* x, int tauMin, int tauMax, std::vector<float>& d) {
  for (int tau = tauMin; tau <= tauMax; tau++) { float s = 0; for (int i = 0; i < W; i += 2) { float dd = x[i] - x[i + tau]; s += dd * dd; } d[tau] = s; }
  float run = 0; for (int tau = tauMin; tau <= tauMax; tau++) { run += d[tau]; d[tau] = d[tau] * (tau - tauMin + 1) / (run + 1e-9f); }
  for (int tau = tauMin + 1; tau < tauMax; tau++)
    if (d[tau] < 0.25f && d[tau] <= d[tau + 1] && d[tau] <= d[tau - 1]) {
      float a = d[tau - 1], b = d[tau], c = d[tau + 1], den = a - 2 * b + c;
      return (float)SR / (tau + (fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f));
    }
  return 0;
}

float smoothstep(float t) { t = std::max(0.0f, std::min(1.0f, t)); return t * t * (3 - 2 * t); }
}

bool sing(const int16_t* speech, int n, const SingRequest& rq, Sung& out) {
  out = Sung();
  if (n < SR / 5) return false;
  const int tauMin = SR / 500, tauMax = SR / 70, nf = n / HOP;
  std::vector<float> x(n + W + tauMax + 1, 0.0f); for (int i = 0; i < n; i++) x[i] = speech[i] / 32768.0f;
  // 1. frames: level and pitch
  std::vector<float> rms(nf), f0(nf); std::vector<float> d(tauMax + 2); float peak = 0;
  for (int f = 0; f < nf; f++) {
    const float* p = &x[(size_t)f * HOP]; float e = 0; for (int i = 0; i < W; i++) e += p[i] * p[i];
    rms[f] = sqrtf(e / W); peak = std::max(peak, rms[f]); f0[f] = rms[f] > 1e-3f ? yin(p, tauMin, tauMax, d) : 0;
  }
  if (peak <= 0) return false;
  std::vector<char> voiced(nf); for (int f = 0; f < nf; f++) voiced[f] = f0[f] > 0 && rms[f] > 0.06f * peak;
  for (int f = 1; f + 2 < nf; f++) if (!voiced[f] && voiced[f - 1] && (voiced[f + 1] || voiced[f + 2])) voiced[f] = 1;   // close 1-2 frame gaps
  // 2. syllables: voiced runs of 20 ms or more, split where the level dips well below both sides
  std::vector<Syllable> syl; int prevEnd = 0;
  for (int f = 0; f < nf;) {
    if (!voiced[f]) { f++; continue; }
    int a = f; while (f < nf && voiced[f]) f++; int b = f;
    if (b - a < 4) continue;
    int a0 = a;
    for (int m = a + 8; m < b - 8; m++) {
      bool dip = true; for (int k = -2; k <= 2; k++) if (rms[m + k] < rms[m]) dip = false;
      if (!dip) continue;
      float left = *std::max_element(&rms[a0], &rms[m]), right = *std::max_element(&rms[m], &rms[b - 1] + 1);
      if (m - a0 >= 8 && rms[m] < 0.6f * left && rms[m] < 0.6f * right) {
        syl.push_back({std::max(prevEnd, a0 - 30), a0, m}); prevEnd = m; a0 = m;
      }
    }
    syl.push_back({std::max(prevEnd, a0 - 30), a0, b}); prevEnd = b;   // a consonant is at most 150 ms
  }
  if (syl.empty()) return false;
  for (auto& s : syl) {   // the nucleus: around the loudest frame, down to half its level; only this part is held
    int p = s.vs; for (int f = s.vs; f < s.ve; f++) if (rms[f] > rms[p]) p = f;
    s.ns = p; while (s.ns > s.vs && rms[s.ns - 1] > 0.5f * rms[p]) s.ns--;
    s.ne = p + 1; while (s.ne < s.ve && rms[s.ne] > 0.5f * rms[p]) s.ne++;
  }
  syl[0].cs = std::max(0, std::min(syl[0].cs, syl[0].vs - 1)); if (syl[0].vs - syl[0].cs > 30) syl[0].cs = syl[0].vs - 30;
  const int S = (int)syl.size(); out.syllables = S;
  int tailEnd = std::min(nf, syl.back().ve + 20);       // a final consonant, up to 100 ms
  // 3. the score: vowel onsets on quarters (or eighths, or sixteenths for long lines), the last syllable held
  float eighth = 60.0f / std::max(40.0f, rq.bpm) / 2 * OUT; int usable = 8 * std::max(1, rq.bars) - 2;
  float unit = S * 2 <= usable - 2 ? 2.0f : (S <= usable - 1 ? 1.0f : std::max(0.25f, (float)(usable - 1) / S));
  // pitch: the tonic in the octave that puts the melody nearest the voice's own register
  std::vector<float> vf; for (int f = 0; f < nf; f++) if (voiced[f]) vf.push_back(theory::hzToMidi(f0[f]));
  std::nth_element(vf.begin(), vf.begin() + vf.size() / 2, vf.end()); float voiceMidi = vf.empty() ? 57 : vf[vf.size() / 2];
  std::vector<int> deg(S); const std::vector<int>& mel = rq.melody;
  for (int k = 0; k < S; k++) deg[k] = mel.empty() ? 0 : mel[std::min(mel.size() - 1, (size_t)k * mel.size() / S)];
  int tonic = rq.tonicPc % 12 + 24; float bestErr = 1e9f;
  for (int t = rq.tonicPc % 12 + 24; t < 96; t += 12) {
    float m = 0; for (int k = 0; k < S; k++) m += theory::degreeToMidi(t, rq.mode, deg[k]); m /= S;
    if (fabsf(m - voiceMidi) < bestErr) { bestErr = fabsf(m - voiceMidi); tonic = t; }
  }
  std::vector<int> note(S); for (int k = 0; k < S; k++) note[k] = theory::degreeToMidi(tonic, rq.mode, deg[k]) + rq.transpose;
  // output times (48 kHz): consonants before the beat at natural speed (squeezed when the note is short)
  std::vector<float> cons(S), T(S), vend(S);
  for (int k = 0; k < S; k++) cons[k] = (float)(syl[k].vs - syl[k].cs) * HOP * 2;
  for (int k = 1; k < S; k++) cons[k] = std::min(cons[k], 0.45f * unit * eighth);
  float lead = cons[0];
  for (int k = 0; k < S; k++) T[k] = lead + k * unit * eighth;
  for (int k = 0; k + 1 < S; k++) vend[k] = T[k + 1] - cons[k + 1];
  vend[S - 1] = lead + (usable - 1) * eighth; if (vend[S - 1] < T[S - 1] + unit * eighth) vend[S - 1] = T[S - 1] + unit * eighth;
  float tailLen = (float)(tailEnd - syl.back().ve) * HOP * 2;
  // the time map, output sample -> source sample (both at 48 kHz): consonant, vowel, consonant ...
  std::vector<std::pair<float, float>> knots;
  for (int k = 0; k < S; k++) {
    // onset and decay of the vowel at natural speed (squeezed if the note is short), the nucleus stretched
    float pre = (float)(syl[k].ns - syl[k].vs) * HOP * 2, post = (float)(syl[k].ve - syl[k].ne) * HOP * 2, avail = vend[k] - T[k];
    if (pre + post > 0.8f * avail) { float g = 0.8f * avail / std::max(1.0f, pre + post); pre *= g; post *= g; }
    knots.push_back({T[k] - cons[k], (float)syl[k].cs * HOP * 2}); knots.push_back({T[k], (float)syl[k].vs * HOP * 2});
    knots.push_back({T[k] + pre, (float)syl[k].ns * HOP * 2}); knots.push_back({vend[k] - post, (float)syl[k].ne * HOP * 2});
    knots.push_back({vend[k], (float)syl[k].ve * HOP * 2});
  }
  knots.push_back({vend[S - 1] + tailLen, (float)tailEnd * HOP * 2});
  auto srcAt = [&](float t) {
    if (t <= knots.front().first) return knots.front().second;
    for (size_t i = 1; i < knots.size(); i++) if (t < knots[i].first) { auto& a = knots[i - 1]; auto& b = knots[i]; float u = (t - a.first) / std::max(1.0f, b.first - a.first); return a.second + u * (b.second - a.second); }
    return knots.back().second;
  };
  // 4. resynthesis
  std::vector<float> src; dsp::upsample2x(speech, n, src); src.resize(src.size() + 4096, 0.0f);
  auto f0At = [&](float s48) { int f = std::max(0, std::min(nf - 1, (int)(s48 / 2 / HOP))); return f0[f]; };
  size_t total = (size_t)(vend[S - 1] + tailLen + 4800); std::vector<float> y(total + 4096, 0.0f);
  // consonants (and the tail): plain overlap-add of 10 ms Hann frames along the map
  const int CF = 480, CH = 240; std::vector<float> win(CF); for (int i = 0; i < CF; i++) win[i] = 0.5f - 0.5f * cosf(6.2831853f * i / CF);
  auto copyRange = [&](float t0, float t1) {
    for (float t = t0 - CH; t < t1; t += CH) {
      if (t < 0) continue; int s = (int)srcAt(t + CH) - CH; if (s < 0) continue;
      for (int i = 0; i < CF; i++) y[(size_t)t + i] += src[(size_t)s + i] * win[i];
    }
  };
  for (int k = 0; k < S; k++) copyRange(T[k] - cons[k], T[k]);
  copyRange(vend[S - 1], vend[S - 1] + tailLen);
  // vowels: pitch marks on the source, grains of two source periods placed at the target period
  for (int k = 0; k < S; k++) {
    int v0 = syl[k].vs * HOP * 2, v1 = syl[k].ve * HOP * 2; std::vector<int> marks;
    float P = OUT / std::max(70.0f, f0At((float)v0 + 1)); int m = v0;
    for (int i = v0; i < std::min(v1, v0 + (int)P); i++) if (src[i] > src[m]) m = i;
    while (m < v1) {
      marks.push_back(m); float f = f0At((float)m); if (f > 0) P = OUT / f;
      int c = m + (int)P, best = c; for (int i = c - (int)(P / 4); i <= c + (int)(P / 4); i++) if (i > m && i < (int)src.size() && src[i] > src[best]) best = i;
      m = best;
    }
    if (marks.empty()) continue;
    int prevNote = k > 0 ? note[k - 1] : note[k];
    for (float t = T[k]; t < vend[k];) {
      float age = (t - T[k]) / OUT, len = (vend[k] - T[k]) / OUT;
      float midi = prevNote + (note[k] - prevNote) * smoothstep(age / 0.06f);
      midi += 0.3f * smoothstep((age - 0.18f) / 0.2f) * sinf(6.2831853f * 5.5f * age) * (len > 0.3f ? 1.0f : 0.0f);
      float Pout = OUT / theory::midiToHz(midi);
      float s = srcAt(t); size_t i = std::lower_bound(marks.begin(), marks.end(), (int)s) - marks.begin();
      if (i == marks.size() || (i > 0 && s - marks[i - 1] < marks[i] - s)) i = i > 0 ? i - 1 : 0;
      int c = marks[i]; float f = f0At((float)c); int h = (int)(OUT / (f > 0 ? f : 120.0f)); h = std::max(48, std::min(h, 700));
      float gain = std::min(2.0f, Pout / h), fade = std::min(1.0f, (vend[k] - t) / (0.03f * OUT));   // a short release
      for (int j = -h; j < h; j++) {
        long o = (long)t + j, si = (long)c + j; if (o < 0 || si < 0 || (size_t)si >= src.size()) continue;
        float w = 0.5f + 0.5f * cosf(3.14159265f * j / h);
        y[(size_t)o] += src[(size_t)si] * w * gain * fade;
      }
      t += Pout;
    }
    out.notes.push_back({(int)T[k], note[k]});
  }
  // level: the speech's peak, a 20 ms fade at the end
  y.resize(total); float py = 0, ps = 0; for (float v : y) py = std::max(py, fabsf(v)); for (int i = 0; i < n; i++) ps = std::max(ps, fabsf(speech[i] / 32768.0f));
  float g = py > 1e-6f ? ps / py : 1.0f; for (float& v : y) v *= g;
  for (int i = 0; i < 960 && i < (int)y.size(); i++) y[y.size() - 1 - i] *= i / 960.0f;
  out.pcm = std::move(y);
  return true;
}
