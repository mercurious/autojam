// autojam: DSP bricks for the band's instruments: oscillators, a resonant low-pass, envelopes, drums, a tempo
// delay and a small reverb. Everything runs per sample at 48 kHz inside the audio callback; nothing allocates.
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace dsp {
constexpr float RATE = 48000.0f;
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float fastTanh(float x) { float x2 = x * x; return x * (27.0f + x2) / (27.0f + 9.0f * x2); }

struct Rng { uint32_t s = 0x9E3779B9u; float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f; } };

// polyBLEP saw / square, sine, triangle
struct Osc {
  float phase = 0, inc = 0;
  void setHz(float hz) { inc = hz / RATE; }
  static float blep(float t, float dt) { if (t < dt) { t /= dt; return t + t - t * t - 1.0f; } if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; } return 0.0f; }
  float saw() { float v = 2.0f * phase - 1.0f - blep(phase, inc); step(); return v; }
  float square(float pw = 0.5f) { float v = phase < pw ? 1.0f : -1.0f; v += blep(phase, inc); float p2 = phase + 1.0f - pw; p2 -= floorf(p2); v -= blep(p2, inc); step(); return v; }
  float sine() { float v = sinf(phase * 6.2831853f); step(); return v; }
  float tri() { float v = 4.0f * fabsf(phase - 0.5f) - 1.0f; step(); return v; }
  void step() { phase += inc; if (phase >= 1.0f) phase -= 1.0f; }
};

// a 2-pole resonant low-pass (Chamberlin state variable), stable to ~RATE/6
struct SVF {
  float lp = 0, bp = 0, f = 0.2f, q = 0.5f;
  void set(float hz, float res) { f = 2.0f * sinf(3.14159265f * clamp(hz, 20.0f, 7000.0f) / RATE); q = 1.0f - clamp(res, 0.0f, 0.95f); }
  float process(float in) { lp += f * bp; float hp = in - lp - q * bp; bp += f * hp; return lp; }
  float band(float in) { process(in); return bp; }
  float high(float in) { lp += f * bp; float hp = in - lp - q * bp; bp += f * hp; return hp; }
};
struct OnePole { float z = 0, a = 0.1f; void setHz(float hz) { a = 1.0f - expf(-6.2831853f * hz / RATE); } float lp(float x) { z += a * (x - z); return z; } float hp(float x) { return x - lp(x); } };

struct ADSR {
  float a = 0.005f, d = 0.2f, s = 0.7f, r = 0.2f; float level = 0; int stage = 0;   // 0 idle 1 attack 2 decay 3 sustain 4 release
  void set(float at, float de, float su, float re) { a = std::max(at, 0.0005f); d = std::max(de, 0.001f); s = su; r = std::max(re, 0.002f); }
  void on() { stage = 1; }
  void off() { if (stage) stage = 4; }
  bool active() const { return stage != 0; }
  float next() {
    switch (stage) {
      case 1: level += 1.0f / (a * RATE); if (level >= 1.0f) { level = 1.0f; stage = 2; } break;
      case 2: level += (s - level) * (1.0f - expf(-1.0f / (d * RATE * 0.3f))); if (level - s < 0.001f) stage = 3; break;
      case 3: level = s; break;
      case 4: level *= expf(-1.0f / (r * RATE * 0.25f)); if (level < 0.0005f) { level = 0; stage = 0; } break;
      default: level = 0;
    }
    return level;
  }
};

// one synth voice: two detuned oscillators into the filter with its own envelope
struct Voice {
  Osc o1, o2, sub; SVF flt; ADSR amp, fenv; float hz = 0, targetHz = 0, glide = 0; int note = -1; float vel = 0, detune = 0.004f, cutoff = 1200, res = 0.2f, fAmt = 2000; int wave = 0; // 0 saw 1 square 2 tri 3 sine
  float age = 0;
  void noteOn(int n, float v, float glideSeconds) {
    note = n; vel = v; targetHz = 440.0f * powf(2.0f, (n - 69) / 12.0f);
    if (hz <= 0 || glideSeconds <= 0) hz = targetHz; glide = glideSeconds > 0 ? 1.0f - expf(-1.0f / (glideSeconds * RATE)) : 1.0f;
    amp.on(); fenv.on(); age = 0;
  }
  void noteOff() { amp.off(); fenv.off(); }
  bool active() const { return amp.active(); }
  float next() {
    if (!amp.active()) return 0.0f;
    hz += (targetHz - hz) * glide;
    o1.setHz(hz); o2.setHz(hz * (1.0f + detune)); sub.setHz(hz * 0.5f);
    float s;
    switch (wave) { case 1: s = 0.5f * (o1.square() + o2.square(0.45f)); break; case 2: s = 0.5f * (o1.tri() + o2.tri()); break; case 3: s = o1.sine(); o2.step(); break; default: s = 0.5f * (o1.saw() + o2.saw()); }
    float fe = fenv.next(); flt.set(cutoff + fAmt * fe * vel, res);
    float out = flt.process(s) * amp.next() * vel;
    age += 1.0f / RATE; return out;
  }
};

struct Drums {
  Rng rng; float kickPh = 0, kickEnv = 0, kickPitch = 0; float snEnv = 0, snTone = 0, snPh = 0; float hatEnv = 0, hatLen = 0.04f; float rimEnv = 0, rimPh = 0; float clapEnv = 0; int clapCount = 0, clapTimer = 0;
  SVF snBand, hatHp, clapBand; float kVel = 0, sVel = 0, hVel = 0, rVel = 0, cVel = 0; bool soft = false;
  Drums() { snBand.set(1800, 0.3f); hatHp.set(6500, 0.1f); clapBand.set(1200, 0.5f); }
  void kick(float v) { kVel = v; kickEnv = 1.0f; kickPitch = 1.0f; kickPh = 0; }
  void snare(float v) { sVel = v; snEnv = 1.0f; snTone = 1.0f; snPh = 0; }
  void hat(float v, bool open) { hVel = v; hatEnv = 1.0f; hatLen = open ? 0.25f : 0.045f; }
  void rim(float v) { rVel = v; rimEnv = 1.0f; rimPh = 0; }
  void clap(float v) { cVel = v; clapEnv = 1.0f; clapCount = 3; clapTimer = 0; }
  float next() {
    float out = 0;
    if (kickEnv > 0.0005f) { float hz = soft ? 45.0f + 60.0f * kickPitch : 50.0f + 110.0f * kickPitch; kickPh += hz / RATE; if (kickPh >= 1) kickPh -= 1; out += fastTanh(sinf(kickPh * 6.2831853f) * (1.5f + kickEnv)) * kickEnv * kVel * 0.9f; kickEnv *= soft ? 0.9994f : 0.9996f; kickPitch *= 0.9990f; }
    if (snEnv > 0.0005f) { float n = snBand.band(rng.next()) * 2.5f; snPh += 185.0f / RATE; if (snPh >= 1) snPh -= 1; float tone = sinf(snPh * 6.2831853f) * snTone; out += (n * snEnv + tone * snTone * 0.6f) * sVel * 0.6f; snEnv *= 0.9993f; snTone *= 0.9985f; }
    if (hatEnv > 0.0005f) { float n = hatHp.high(rng.next()); out += n * hatEnv * hVel * (soft ? 0.18f : 0.28f); hatEnv *= expf(-1.0f / (hatLen * RATE * 0.35f)); }
    if (rimEnv > 0.0005f) { rimPh += 620.0f / RATE; if (rimPh >= 1) rimPh -= 1; out += (sinf(rimPh * 6.2831853f) * 0.6f + rng.next() * 0.3f) * rimEnv * rVel * 0.35f; rimEnv *= 0.9975f; }
    if (clapCount > 0 || clapEnv > 0.0005f) { if (clapCount > 0 && --clapTimer <= 0) { clapEnv = 1.0f; clapCount--; clapTimer = (int)(0.011f * RATE); } float n = clapBand.band(rng.next()) * 3.0f; out += n * clapEnv * cVel * 0.4f; clapEnv *= clapCount > 0 ? 0.998f : 0.9994f; }
    return out;
  }
};

struct Delay {
  std::vector<float> buf; size_t pos = 0; float fb = 0.3f, mix = 0.25f; size_t len = 24000; OnePole tone;
  Delay() { buf.assign(1 << 17, 0.0f); tone.setHz(3500); }
  void setSeconds(float s) { len = (size_t)clamp(s * RATE, 1.0f, (float)(buf.size() - 1)); }
  float process(float in) { size_t rd = (pos + buf.size() - len) % buf.size(); float d = buf[rd]; buf[pos] = tone.lp(in + d * fb); pos = (pos + 1) % buf.size(); return d; }
};

// a small Schroeder reverb: 4 combs + 2 allpasses, mono in, stereo-ish out by two taps
struct Reverb {
  struct Comb { std::vector<float> b; size_t p = 0; float fb, damp, z = 0; Comb(size_t n, float f, float d) : b(n, 0.0f), fb(f), damp(d) {} float go(float x) { float y = b[p]; z += damp * (y - z); b[p] = x + z * fb; p = (p + 1) % b.size(); return y; } };
  struct AP { std::vector<float> b; size_t p = 0; AP(size_t n) : b(n, 0.0f) {} float go(float x) { float y = b[p]; float o = -x + y; b[p] = x + y * 0.5f; p = (p + 1) % b.size(); return o; } };
  Comb c[4] = {Comb(1557, 0.84f, 0.2f), Comb(1617, 0.84f, 0.2f), Comb(1491, 0.84f, 0.2f), Comb(1422, 0.84f, 0.2f)}; AP a[2] = {AP(225), AP(556)};
  void process(float in, float& l, float& r) { float s = 0; for (auto& k : c) s += k.go(in); s *= 0.25f; s = a[1].go(a[0].go(s)); l = s; r = -s * 0.9f; }
};

// linear resampler state for 24 kHz voice lines to 48 kHz
inline void upsample2x(const int16_t* in, int n, std::vector<float>& out) {
  out.resize((size_t)n * 2);
  for (int i = 0; i < n; i++) { float a = in[i] / 32768.0f, b = (i + 1 < n ? in[i + 1] : in[i]) / 32768.0f; out[2 * i] = a; out[2 * i + 1] = 0.5f * (a + b); }
}

} // namespace dsp
