#include "mic.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>

bool MicCapture::start(const std::string& src, Sink sink) {
  stop(); source = src; error.clear();
  pa_sample_spec ss{}; ss.format = PA_SAMPLE_S16LE; ss.rate = RATE; ss.channels = 2;
  pa_buffer_attr attr{}; attr.maxlength = (uint32_t)-1; attr.fragsize = RATE / 50 * 4;   // 20 ms
  int err = 0;
  pa_simple* s = pa_simple_new(nullptr, "autojam", PA_STREAM_RECORD, src.empty() ? nullptr : src.c_str(), "jam", &ss, nullptr, &attr, &err);
  if (!s) { error = std::string("capture: ") + pa_strerror(err); return false; }
  run = true;
  th = std::thread([this, s, sink]() {
    std::vector<int16_t> buf(RATE / 100 * 2);     // 10 ms stereo
    std::vector<float> mono(RATE / 100);
    int e = 0;
    while (run.load()) {
      if (pa_simple_read(s, buf.data(), buf.size() * sizeof(int16_t), &e) < 0) { error = std::string("capture read: ") + pa_strerror(e); break; }
      float pk = 0;
      for (size_t i = 0; i < mono.size(); i++) { float v = (buf[2 * i] + buf[2 * i + 1]) * (0.5f / 32768.0f); mono[i] = v; pk = std::max(pk, fabsf(v)); }
      lvl = pk; frames += (long long)mono.size();
      sink(mono.data(), (int)mono.size());
    }
    pa_simple_free(s);
    run = false;
  });
  return true;
}

bool MicCapture::startWav(const std::string& path, Sink sink, bool realtime, bool loop) {
  stop(); error.clear(); source = path;
  std::vector<float> data; std::string err;
  if (!readWavMono48k(path, data, err)) { error = err; return false; }
  if (data.empty()) { error = "empty wav"; return false; }
  run = true;
  th = std::thread([this, data = std::move(data), sink, realtime, loop]() {
    const int block = RATE / 100; size_t pos = 0;
    auto t0 = std::chrono::steady_clock::now(); long long sent = 0;
    while (run.load()) {
      int n = (int)std::min((size_t)block, data.size() - pos);
      if (n <= 0) { if (!loop) break; pos = 0; continue; }
      float pk = 0; for (int i = 0; i < n; i++) pk = std::max(pk, fabsf(data[pos + i]));
      lvl = pk; frames += n;
      sink(&data[pos], n); pos += (size_t)n; sent += n;
      if (realtime) { auto due = t0 + std::chrono::microseconds(sent * 1000000LL / RATE); std::this_thread::sleep_until(due); }
    }
    run = false;
  });
  return true;
}

void MicCapture::stop() { run = false; if (th.joinable()) th.join(); }

bool readWavMono48k(const std::string& path, std::vector<float>& out, std::string& err) {
  FILE* f = fopen(path.c_str(), "rb"); if (!f) { err = "cannot open " + path; return false; }
  char id[4]; uint32_t sz;
  if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) || fread(&sz, 4, 1, f) != 1 || fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) { fclose(f); err = "not a WAV file"; return false; }
  uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0; std::vector<unsigned char> data;
  while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
    if (!memcmp(id, "fmt ", 4)) {
      unsigned char b[64] = {}; size_t n = std::min<size_t>(sz, sizeof b); if (fread(b, 1, n, f) != n) break; if (sz > n) fseek(f, (long)(sz - n), SEEK_CUR);
      fmt = (uint16_t)(b[0] | b[1] << 8); ch = (uint16_t)(b[2] | b[3] << 8); rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24; bits = (uint16_t)(b[14] | b[15] << 8);
      if (fmt == 0xFFFE && sz >= 26) fmt = (uint16_t)(b[24] | b[25] << 8);
    } else if (!memcmp(id, "data", 4)) { data.resize(sz); size_t got = fread(data.data(), 1, sz, f); data.resize(got); break; }
    else fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
  }
  fclose(f);
  if (!ch || !rate || !bits || data.empty()) { err = "WAV without fmt/data"; return false; }
  int bps = bits / 8; size_t frames = data.size() / (bps * ch);
  std::vector<float> mono(frames);
  for (size_t i = 0; i < frames; i++) {
    float acc = 0;
    for (int c = 0; c < ch; c++) {
      const unsigned char* p = &data[(i * ch + c) * bps]; float v = 0;
      if (fmt == 3 && bits == 32) { float fv; memcpy(&fv, p, 4); v = fv; }
      else if (bits == 16) v = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
      else if (bits == 24) v = (int32_t)((p[0] << 8 | p[1] << 16 | p[2] << 24)) / 2147483648.0f;
      else if (bits == 32) v = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0f;
      else if (bits == 8) v = (p[0] - 128) / 128.0f;
      acc += v;
    }
    mono[i] = acc / ch;
  }
  if (rate == 48000) { out = std::move(mono); return true; }
  double ratio = (double)rate / 48000.0; size_t n = (size_t)(frames / ratio);
  out.resize(n);
  for (size_t i = 0; i < n; i++) { double s = i * ratio; size_t k = (size_t)s; float fr = (float)(s - k); out[i] = k + 1 < frames ? mono[k] * (1 - fr) + mono[k + 1] * fr : mono[k]; }
  return true;
}
