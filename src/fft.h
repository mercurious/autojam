// autojam: an in-place radix-2 complex FFT and a Hann window, for the analysis of what the room plays.
#pragma once
#include <vector>
#include <complex>
#include <cmath>

struct FFT {
  int n = 0; std::vector<std::complex<float>> tw; std::vector<int> rev;
  explicit FFT(int size = 0) { if (size) init(size); }
  void init(int size) {
    n = size; tw.resize(n / 2); rev.resize(n);
    for (int i = 0; i < n / 2; i++) tw[i] = std::polar(1.0f, -2.0f * (float)M_PI * i / n);
    int bits = 0; while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[i] = r; }
  }
  void run(std::vector<std::complex<float>>& x) const {
    for (int i = 0; i < n; i++) if (rev[i] > i) std::swap(x[i], x[rev[i]]);
    for (int len = 2; len <= n; len <<= 1) {
      int step = n / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < len / 2; j++) {
          std::complex<float> u = x[i + j], v = x[i + j + len / 2] * tw[j * step];
          x[i + j] = u + v; x[i + j + len / 2] = u - v;
        }
    }
  }
  // magnitude spectrum of a windowed real frame: out has n/2+1 bins
  void magnitude(const float* frame, const std::vector<float>& window, std::vector<float>& out, std::vector<std::complex<float>>& work) const {
    work.resize(n);
    for (int i = 0; i < n; i++) work[i] = std::complex<float>(frame[i] * window[i], 0.0f);
    run(work);
    out.resize(n / 2 + 1);
    for (int i = 0; i <= n / 2; i++) out[i] = std::abs(work[i]);
  }
};

inline std::vector<float> hann(int n) { std::vector<float> w(n); for (int i = 0; i < n; i++) w[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / n); return w; }
