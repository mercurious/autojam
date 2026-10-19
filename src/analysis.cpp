// autojam: the listener (see analysis.h).
#include "analysis.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static const float KS_MAJOR[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
static const float KS_MINOR[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

Analysis::Analysis() : fft(NFFT), cfft(CFFT) {
  ring.assign(1 << 16, 0.0f);
  win = hann(NFFT); cwin = hann(CFFT);
  prevMag.assign(NFFT / 2 + 1, 0.0f);
  yinBuf.resize(NFFT);
  snap.history.clear();
}

void Analysis::feed(const float* mono, int n) {
  for (int i = 0; i < n; i++) {
    ring[ringPos] = mono[i]; ringPos = (ringPos + 1) & (ring.size() - 1);
    samples += 1;
    if (++hopFill == HOP) { hopFill = 0; hop(); }
  }
}

void Analysis::tap() {
  std::lock_guard<std::mutex> l(mu);
  double now = samples / RATE;
  if (!taps.empty() && now - taps.back() > 2.5) taps.clear();
  taps.push_back(now); while (taps.size() > 8) taps.pop_front();
  if (taps.size() >= 2) {
    double p = (taps.back() - taps.front()) / (double)(taps.size() - 1);
    if (p > 0.25 && p < 1.5) { periodS = p; bpmEst = (float)(60.0 / p); locked = true; beatConf = std::max(beatConf, 0.7f); bpmHist.clear(); }
  }
  beatAnchor = now; barOffset = (barOffset + 1) & 3; beatCount = 0;
}

void Analysis::link(const LinkInfo& l) {
  // the beacon's next downbeat, put on the audio timeline: `samples` lags the room by the capture path as the
  // measured beats do, and the player compensates for that, so the same offset holds here
  std::lock_guard<std::mutex> g(mu);
  double now = samples / RATE; lk = l; linkSeen = now; linkAnchor = now + l.nextBarIn - (JamLink::steadyNow() - l.at);
}

void Analysis::boostNotes(uint16_t pcMask, float w) { std::lock_guard<std::mutex> l(mu); midiBoost = pcMask; midiBoostW = w; }

// read the last `n` samples of the ring into dst
static void tail(const std::vector<float>& ring, size_t pos, int n, float* dst) {
  size_t mask = ring.size() - 1, start = (pos + ring.size() - n) & mask;
  for (int i = 0; i < n; i++) dst[i] = ring[(start + i) & mask];
}

void Analysis::hop() {
  // level of this hop
  float frame[HOP]; tail(ring, ringPos, HOP, frame);
  float sq = 0, pk = 0; for (float v : frame) { sq += v * v; pk = std::max(pk, fabsf(v)); }
  float rms = sqrtf(sq / HOP);
  rmsEma += 0.3f * (rms - rmsEma); peakHold = std::max(pk, peakHold * 0.995f);
  onsetFrame();
  pitch();
  if (++chopFill >= CHOP / HOP) { chopFill = 0; chromaFrame(); chordAndKey(); }
  if (++sinceTempo >= (RATE / HOP) / 2) { sinceTempo = 0; tempo(); }   // twice a second
  publish();
}

void Analysis::onsetFrame() {
  float frame[NFFT]; tail(ring, ringPos, NFFT, frame);
  fft.magnitude(frame, win, mag, work);
  float flux = 0, fluxLow = 0;
  int lowBins = (int)(200.0f / ((float)RATE / NFFT));   // kick band < 200 Hz
  for (int i = 1; i < (int)mag.size(); i++) {
    float d = logf(1.0f + 20.0f * mag[i]) - logf(1.0f + 20.0f * prevMag[i]);
    if (d > 0) { flux += d; if (i <= lowBins) fluxLow += d; }
    prevMag[i] = mag[i];
  }
  flux /= (float)mag.size(); fluxLow /= (float)std::max(1, lowBins);
  osf.push_back(flux); osfLow.push_back(fluxLow);
  size_t keep = (size_t)(8.0 * RATE / HOP);
  while (osf.size() > keep) { osf.pop_front(); osfLow.pop_front(); }
  osfMean += 0.02f * (flux - osfMean);
}

void Analysis::tempo() {
  if (osf.size() < (size_t)(3.0 * RATE / HOP)) return;
  const float fps = (float)RATE / HOP;
  int n = (int)osf.size();
  std::vector<float> x(osf.begin(), osf.end());
  float mean = 0; for (float v : x) mean += v; mean /= n;
  for (auto& v : x) v = std::max(0.0f, v - mean);
  float e0 = 0; for (float v : x) e0 += v * v; if (e0 < 1e-9f) { beatConf *= 0.9f; return; }
  int lagMin = (int)(fps * 60.0f / 220.0f), lagMax = (int)(fps * 60.0f / 50.0f), acMax = std::max(lagMax, n / 2);
  std::vector<float> ac(acMax + 1, 0.0f);
  for (int lag = lagMin; lag <= acMax; lag++) { float s = 0; for (int i = lag; i < n; i++) s += x[i] * x[i - lag]; ac[lag] = s / e0; }
  // a comb over the period, its double and the bar (four beats), with a preference for 80-160 BPM. The bar
  // matters for loop-built beats (trap, jerk): their hits are syncopated, but the pattern repeats every bar, and
  // a 4:3 impostor (triplet hats) gets no support there, its four "beats" being three of the real ones
  int bestLag = -1; float best = -1, second = -1;
  for (int lag = lagMin; lag <= lagMax; lag++) {
    float bpm = fps * 60.0f / lag;
    float pref = expf(-0.5f * powf(log2f(bpm / 115.0f) / 0.55f, 2.0f));
    // the multiples of an integer lag drift from the true ones (half a frame off is two at the bar): take the
    // peak near each, the onset peaks being only a frame or two wide
    auto near = [&](int c, int w) { float m = 0; for (int k = std::max(lagMin, c - w); k <= std::min(acMax, c + w); k++) m = std::max(m, ac[k]); return m; };
    float s = ac[lag]; if (lag * 2 <= acMax) s += 0.5f * near(lag * 2, 1); if (lag * 4 <= acMax) s += 0.25f * near(lag * 4, 2);
    s *= 0.6f + 0.4f * pref;
    if (s > best) { second = best; best = s; bestLag = lag; } else if (s > second) second = s;
  }
  if (bestLag < 0) return;
  // refine the lag by parabolic interpolation on the raw autocorrelation
  float lagF = (float)bestLag;
  if (bestLag > lagMin && bestLag < lagMax) { float a = ac[bestLag - 1], b = ac[bestLag], c = ac[bestLag + 1]; float den = a - 2 * b + c; if (fabsf(den) > 1e-6f) lagF += 0.5f * (a - c) / den; }
  float bpm = fps * 60.0f / lagF;
  float conf = std::min(1.0f, std::max(0.0f, (best - second) * 3.0f + best * 0.5f));
  std::lock_guard<std::mutex> l(mu);
  if (bpmOverride > 0) { bpm = bpmOverride; conf = 1.0f; }
  bpmHist.push_back(bpm); while (bpmHist.size() > 6) bpmHist.pop_front();
  std::vector<float> h(bpmHist.begin(), bpmHist.end()); std::sort(h.begin(), h.end());
  float med = h[h.size() / 2];
  // hysteresis: keep the locked tempo while the new one is a simple ratio of it (double, half, or the 4:3 and 3:2
  // of triplet figures), the usual ways a beat tracker slips; if the ratio holds for 3 s it was the lock that
  // was wrong, and the new tempo is taken
  bool slip = false;
  if (locked && bpmEst > 0) for (float r : {2.0f, 0.5f, 4.0f / 3, 0.75f, 1.5f, 2.0f / 3}) if (fabsf(med - bpmEst * r) < bpmEst * r * 0.05f) { slip = true; break; }
  slips = slip ? slips + 1 : 0;
  if (slip && slips < 6) med = bpmEst; else if (slip) slips = 0;
  if (!locked || fabsf(med - bpmEst) > bpmEst * 0.12f) { bpmEst = med; } else bpmEst += 0.3f * (med - bpmEst);
  beatConf += 0.4f * (conf - beatConf);
  locked = beatConf > 0.25f && bpmHist.size() >= 3;
  if (!taps.empty() && samples / RATE - taps.back() < 6.0) { /* a human tapped recently: keep their tempo */ bpmEst = (float)(60.0 / periodS); }
  else periodS = 60.0 / bpmEst;
  // phase: which alignment of beats onto the recent onsets scores best
  int P = std::max(1, (int)lroundf(fps * (float)periodS));
  double now = samples / RATE;
  int bestPhi = 0; float bestS = -1;
  for (int phi = 0; phi < P; phi++) {
    float s = 0, w = 1.0f;
    for (int k = 0; k < 8; k++) { int idx = n - 1 - phi - k * P; if (idx < 1) break; s += w * (x[idx] + 0.5f * x[idx - 1] + (idx + 1 < n ? 0.5f * x[idx + 1] : 0)); w *= 0.85f; }
    if (s > bestS) { bestS = s; bestPhi = phi; }
  }
  double estAnchor = now - (double)bestPhi / fps;
  if (!taps.empty() && now - taps.back() < 6.0) return;
  if (beatAnchor <= 0) beatAnchor = estAnchor;
  else {   // nudge towards the estimate by at most a fraction of a beat (a PLL, not a jump)
    double k = fmod(estAnchor - beatAnchor, periodS); if (k > periodS / 2) k -= periodS; if (k < -periodS / 2) k += periodS;
    beatAnchor += 0.35 * k;
  }
  // bar: the beat position where the low band hits hardest
  for (int b = 0; b < 4; b++) barScore[b] *= 0.97f;
  int lowN = (int)osfLow.size();
  for (int k = 0; k < 16; k++) {
    double t = beatAnchor - k * periodS; int idx = lowN - 1 - (int)lround((now - t) * fps);
    if (idx < 1 || idx >= lowN) continue;
    float v = std::max(osfLow[idx], std::max(osfLow[idx - 1], idx + 1 < lowN ? osfLow[idx + 1] : 0.0f));
    barScore[((-k) % 4 + 4) % 4] += v;
  }
}

void Analysis::chromaFrame() {
  std::vector<float> frame(CFFT); tail(ring, ringPos, CFFT, frame.data());
  cfft.magnitude(frame.data(), cwin, cmag, work);
  float c[12] = {};
  float binHz = (float)RATE / CFFT;
  int lo = (int)(55.0f / binHz), hi = std::min((int)cmag.size() - 1, (int)(4200.0f / binHz));
  // a magnitude floor so the room's hiss does not become a chord
  float floor = 0; for (int i = lo; i <= hi; i++) floor += cmag[i]; floor /= (float)(hi - lo + 1);
  for (int i = lo; i <= hi; i++) {
    // only spectral peaks count (a bin louder than both neighbours), weighted towards the fundamentals
    if (cmag[i] < floor * 2.0f || cmag[i] < cmag[i - 1] || cmag[i] < cmag[i + 1]) continue;
    float f = i * binHz; float midi = theory::hzToMidi(f);
    int pc = ((int)lroundf(midi) % 12 + 12) % 12;
    float w = cmag[i] * (f < 1000.0f ? 1.0f : 1000.0f / f);
    c[pc] += w;
  }
  if (midiBoostW > 0) for (int pc = 0; pc < 12; pc++) if (midiBoost & (1 << pc)) c[pc] += midiBoostW;
  float sum = 0; for (float v : c) sum += v;
  if (sum > 1e-6f) for (auto& v : c) v /= sum; else memset(c, 0, sizeof c);
  float aFast = 0.35f, aSlow = 0.003f;   // ~0.3 s and ~14 s at 23 frames/s: a key outlives its chords
  for (int i = 0; i < 12; i++) { chromaFast[i] += aFast * (c[i] - chromaFast[i]); if (sum > 1e-6f) chromaSlow[i] += aSlow * (c[i] - chromaSlow[i]); }
}

void Analysis::chordAndKey() {
  // key: Krumhansl-Schmuckler correlation against the long chroma
  float best = -2, second = -2; int bestK = -1; bool bestMinor = false;
  auto corr = [&](const float* prof, int tonic) {
    float mx = 0, my = 0; for (int i = 0; i < 12; i++) { mx += chromaSlow[i]; my += prof[i]; } mx /= 12; my /= 12;
    float sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < 12; i++) { float x = chromaSlow[i] - mx, y = prof[(i - tonic + 12) % 12] - my; sxy += x * y; sxx += x * x; syy += y * y; }
    return sxx > 1e-9f ? sxy / sqrtf(sxx * syy) : 0.0f;
  };
  for (int t = 0; t < 12; t++) {
    float cM = corr(KS_MAJOR, t), cm = corr(KS_MINOR, t);
    if (cM > best) { second = best; best = cM; bestK = t; bestMinor = false; } else if (cM > second) second = cM;
    if (cm > best) { second = best; best = cm; bestK = t; bestMinor = true; } else if (cm > second) second = cm;
  }
  float energy = 0; for (float v : chromaFast) energy += v;
  std::lock_guard<std::mutex> l(mu);
  if (energy > 0.5f && best > 0.3f) {
    float conf = std::min(1.0f, (best - second) * 4.0f + 0.2f);
    if (keyPc < 0 || conf > keyConf + 0.15f || (bestK == keyPc && bestMinor == keyMinor)) { keyPc = bestK; keyMinor = bestMinor; keyConf += 0.2f * (conf - keyConf); }
    else keyConf *= 0.98f;
  }
  // chord: cosine against each template, with a preference for triads and for the key's own chords
  const auto& types = theory::chordTypes();
  if (energy < 0.5f || rmsEma < 0.0015f) { candCount = 0; if (curRoot >= 0 && samples / RATE - quietSince > 2.0 && quietSince > 0) { /* fall silent */ } return; }
  uint16_t keyMask = keyPc >= 0 ? theory::scaleMask(keyPc, keyMinor ? 1 : 0) : 0xFFF;
  int bRoot = -1, bType = 0; float bScore = -1, triadBest = -1; int triadRoot = -1, triadType = 0;
  float cn = 0; for (float v : chromaFast) cn += v * v; cn = sqrtf(cn) + 1e-9f;
  for (int r = 0; r < 12; r++) for (size_t t = 0; t < types.size(); t++) {
    float dot = 0, tn = 0;
    for (int iv : types[t].iv) { float w = iv == 0 ? 1.3f : (iv == 7 ? 0.9f : 1.0f); dot += w * chromaFast[(r + iv) % 12]; tn += w * w; }
    float s = dot / (cn * sqrtf(tn));
    // non-chord tones cost
    float off = 0; uint16_t m = theory::chordMask(r, (int)t); for (int pc = 0; pc < 12; pc++) if (!(m & (1 << pc))) off += chromaFast[pc];
    s -= 0.35f * off;
    if (keyPc >= 0 && (keyMask & (1 << r)) && ((theory::chordMask(r, (int)t) & ~keyMask) == 0)) s += 0.04f * keyConf;
    if (t == theory::CH_POWER) s -= 0.08f;
    if (s > bScore) { bScore = s; bRoot = r; bType = (int)t; }
    if (types[t].triad && s > triadBest) { triadBest = s; triadRoot = r; triadType = (int)t; }
  }
  if (!types[bType].triad && triadBest > bScore - 0.03f) { bRoot = triadRoot; bType = triadType; bScore = triadBest; }
  double now = samples / RATE;
  if (bRoot == curRoot && bType == curType) { curScore = bScore; candCount = 0; return; }
  if (bRoot == candRoot && bType == candType) candCount++; else { candRoot = bRoot; candType = bType; candCount = 1; }
  // switch when the candidate has held for ~3 frames (130 ms) and clearly beats the current chord
  float curNow = -1;
  if (curRoot >= 0) { float dot = 0, tn = 0; for (int iv : types[curType].iv) { float w = iv == 0 ? 1.3f : 1.0f; dot += w * chromaFast[(curRoot + iv) % 12]; tn += w * w; } curNow = dot / (cn * sqrtf(tn)); }
  if (candCount >= 3 && (curRoot < 0 || bScore > curNow + 0.06f)) {
    if (curRoot >= 0) { snap.history.push_back({curRoot, curType, chordStart, now}); while (snap.history.size() > 16) snap.history.erase(snap.history.begin()); }
    curRoot = bRoot; curType = bType; curScore = bScore; chordStart = now; candCount = 0;
  }
}

void Analysis::pitch() {
  // YIN on the last NFFT samples, 60..1200 Hz
  const int N = NFFT, W = N / 2;
  tail(ring, ringPos, N, yinBuf.data());
  const float* x = yinBuf.data();
  int tauMin = RATE / 1200, tauMax = std::min(W - 1, RATE / 60);
  static std::vector<float> d; d.assign(tauMax + 1, 0.0f);
  float e = 0; for (int i = 0; i < W; i++) e += x[i] * x[i];
  if (e < 1e-4f) { std::lock_guard<std::mutex> l(mu); snap.pitchConf *= 0.8f; return; }
  for (int tau = tauMin; tau <= tauMax; tau++) { float s = 0; for (int i = 0; i < W; i += 2) { float dd = x[i] - x[i + tau]; s += dd * dd; } d[tau] = s * 2; }
  // cumulative mean normalised difference
  float run = 0; for (int tau = 1; tau <= tauMax; tau++) { if (tau >= tauMin) { run += d[tau]; d[tau] = d[tau] * tau / (run + 1e-9f); } }
  int best = -1;
  for (int tau = tauMin + 1; tau < tauMax; tau++) if (d[tau] < 0.15f && d[tau] <= d[tau + 1] && d[tau] <= d[tau - 1]) { best = tau; break; }
  float conf = 0, hz = 0;
  if (best > 0) {
    float a = d[best - 1], b = d[best], c = d[best + 1]; float den = a - 2 * b + c; float tauF = best + (fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0);
    hz = (float)RATE / tauF; conf = 1.0f - b;
  }
  std::lock_guard<std::mutex> l(mu);
  if (best > 0) { snap.pitchHz = hz; snap.pitchMidi = theory::hzToMidi(hz); snap.pitchConf += 0.5f * (conf - snap.pitchConf); }
  else snap.pitchConf *= 0.8f;
}

void Analysis::publish() {
  std::lock_guard<std::mutex> l(mu);
  double now = samples / RATE;
  snap.time = now;
  snap.rmsDb = 20.0f * log10f(rmsEma + 1e-9f); snap.peakDb = 20.0f * log10f(peakHold + 1e-9f);
  if (rmsEma < 0.0015f) { if (quietSince <= 0) quietSince = now; } else quietSince = 0;
  snap.silence = quietSince > 0 && now - quietSince > 1.5;
  float target = std::min(1.0f, std::max(0.0f, (snap.rmsDb + 50.0f) / 40.0f));
  snap.energy += 0.05f * (target - snap.energy);
  snap.onset = osf.empty() ? 0 : std::min(1.0f, osf.back() / (osfMean * 3.0f + 1e-6f));
  snap.bpm = bpmEst; snap.beatConf = beatConf; snap.period = periodS; snap.tempoLocked = locked;
  if (beatAnchor > 0 && periodS > 0) {
    double k = floor((now - beatAnchor) / periodS);
    double lb = beatAnchor + k * periodS;
    if (lb != snap.lastBeat) { if (snap.lastBeat >= 0) beatCount += (int)lround((lb - snap.lastBeat) / periodS); snap.lastBeat = lb; }
    int bestB = 0; for (int b = 1; b < 4; b++) if (barScore[b] > barScore[bestB]) bestB = b;
    float tot = barScore[0] + barScore[1] + barScore[2] + barScore[3];
    snap.barConf = tot > 0 ? std::min(1.0f, (barScore[bestB] / tot - 0.25f) * 2.5f) : 0;
    snap.beatInBar = ((beatCount - bestB) % 4 + 4) % 4;
  }
  memcpy(snap.chroma, chromaFast, sizeof chromaFast); memcpy(snap.chromaLong, chromaSlow, sizeof chromaSlow);
  snap.key = keyPc; snap.minorKey = keyMinor; snap.keyConf = keyConf;
  if (snap.silence && curRoot >= 0) { snap.history.push_back({curRoot, curType, chordStart, now}); while (snap.history.size() > 16) snap.history.erase(snap.history.begin()); curRoot = -1; candCount = 0; }
  snap.chordRoot = curRoot; snap.chordType = curType; snap.chordConf = std::min(1.0f, std::max(0.0f, curScore)); snap.chordSince = chordStart;
  snap.mode = -1; snap.linked = false;
  // the jam link, while it is fresh: Rap Machine's own tempo, downbeat, key, mode and chord, not the air's
  if (lk.playing && lk.bpm > 0 && now - linkSeen < 3.0) {
    double period = 60.0 / lk.bpm, bar = 4 * period, since = now - linkAnchor, beats = floor(since / period);
    snap.linked = true; snap.bpm = lk.bpm; snap.period = period; snap.tempoLocked = true; snap.beatConf = 1;
    snap.lastBeat = linkAnchor + beats * period; snap.beatInBar = (int)(((long long)beats % 4 + 4) % 4); snap.barConf = 1;
    snap.key = lk.root; snap.mode = lk.mode; snap.minorKey = lk.mode >= 0 && theory::modes()[lk.mode].minorish; snap.keyConf = 1;
    int nb = (int)lk.chordRoot.size();
    if (nb > 0) {
      long long bars = (long long)floor(since / bar); int b = (int)(((lk.nextBar + bars) % nb + nb) % nb);
      if (lk.chordRoot[b] != linkR || lk.chordType[b] != linkT) {
        if (linkR >= 0) { linkHist.push_back({linkR, linkT, linkSince, now}); while (linkHist.size() > 16) linkHist.erase(linkHist.begin()); }
        linkR = lk.chordRoot[b]; linkT = lk.chordType[b]; linkSince = linkAnchor + bars * bar;
      }
      snap.chordRoot = linkR; snap.chordType = linkT; snap.chordConf = 1; snap.chordSince = linkSince; snap.history = linkHist;
    }
    snap.silence = false;
    snap.linkFrom = (lk.host.empty() ? lk.from : lk.host) + (lk.lane.empty() ? "" : " · " + lk.lane);
    snap.linkLoop.clear(); for (size_t i = 0; i < lk.chordName.size(); i++) snap.linkLoop += (i ? " " : "") + lk.chordName[i];
  }
}
