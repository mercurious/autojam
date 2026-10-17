#include "player.h"
#include <algorithm>
#include <cstring>
#include <cmath>

using namespace theory;

static bool is(const Plan& p, const char* s) { return p.style == s; }

void Engine::setup() {
  micRing.assign(1 << 15, 0.0f);
  bassV.wave = 0; bassV.detune = 0.0f; bassV.cutoff = 300; bassV.fAmt = 900; bassV.res = 0.2f; bassV.amp.set(0.004f, 0.3f, 0.6f, 0.08f); bassV.fenv.set(0.002f, 0.15f, 0.2f, 0.1f);
  bassTone.setHz(2500);
  for (auto& v : keysV) { v.wave = 0; v.detune = 0.006f; v.cutoff = 900; v.fAmt = 1200; v.res = 0.1f; v.amp.set(0.02f, 0.4f, 0.7f, 0.3f); v.fenv.set(0.02f, 0.5f, 0.3f, 0.3f); }
  leadV.wave = 1; leadV.detune = 0.003f; leadV.cutoff = 1500; leadV.fAmt = 2500; leadV.res = 0.25f; leadV.amp.set(0.01f, 0.2f, 0.7f, 0.15f); leadV.fenv.set(0.005f, 0.2f, 0.4f, 0.15f);
  leadDelay.fb = 0.32f; voiceDelay.fb = 0.25f;
  plan = Plan(); applyPlanNow(plan); havePlan = false;
}

void Engine::micIn(const float* mono, int n) {
  size_t w = micW.load(std::memory_order_relaxed);
  for (int i = 0; i < n; i++) { micRing[w & (micRing.size() - 1)] = mono[i]; w++; }
  micW.store(w, std::memory_order_release);
}
void Engine::queueLine(VoiceLine&& l) { std::lock_guard<std::mutex> g(lineMu); lines.push_back(std::move(l)); }
void Engine::speakNow(VoiceLine&& l) { std::lock_guard<std::mutex> g(lineMu); spoken.push_back(std::move(l)); }

std::string Engine::describePlaying() const {
  BandState s = state();
  if (!s.playing) return "the band is silent (stopped)";
  std::string d = "the band plays " + s.plan.style + " at " + std::to_string((int)lroundf(s.bpm)) + " BPM, drums " + s.plan.feel;
  const char* lv[] = {"off", "light", "medium", "busy"};
  for (int p = 0; p < 4; p++) d += std::string(", ") + PART_NAMES[p] + " " + lv[std::max(0, std::min(3, s.density[p]))] + (s.partMute[p] ? " (muted)" : "");
  d += s.following ? ", following the humans' chords" : ", on its own chords";
  if (s.chordRoot >= 0) d += ", now on " + chordName(s.chordRoot, s.chordType);
  if (s.linesQueued) d += ", " + std::to_string(s.linesQueued) + " lyric lines still to sing";
  return d;
}

void Engine::handle(const Command& c) {
  switch (c.type) {
    case Command::Play: playing = true; needBarAlign = true; if (midi) midi->start(); break;
    case Command::Stop: playing = false; for (auto& o : offs) noteOff(o.part, o.note); offs.clear(); bassV.noteOff(); leadV.noteOff(); for (auto& v : keysV) v.noteOff(); if (midi) midi->stop(); break;
    case Command::ApplyPlan: if (c.plan) { applyPlanNow(*c.plan); delete c.plan; } break;
    case Command::Density: if (c.a >= 0 && c.a < 4) { density[c.a] = std::max(0, std::min(3, density[c.a] + c.b)); } break;
    case Command::Mute: if (c.a >= 0 && c.a < NUM_PARTS) mute[c.a] = c.b < 0 ? !mute[c.a] : (c.b != 0); break;
    case Command::Volume: if (c.a >= 0 && c.a < NUM_PARTS) volume[c.a] = dsp::clamp(volume[c.a] + c.f, 0.0f, 1.2f); else master = dsp::clamp(master + c.f, 0.0f, 1.2f); break;
    case Command::Energy: energy = std::max(0, std::min(3, energy + c.a)); for (int p = 0; p < 4; p++) if (density[p] > 0 || c.a > 0) density[p] = std::max(p == P_LEAD ? 0 : 1, std::min(3, density[p] + c.a)); break;
    case Command::Follow: following = c.a != 0; tempoMode = following ? 1 : (tempoMode == 1 ? 0 : tempoMode); break;
    case Command::Resync: needBarAlign = true; following = true; tempoMode = 1; break;
    case Command::Tempo: if (c.f > 0) { freeBpm = c.f; tempoMode = 0; bpm = c.f; } else tempoMode = 1; break;
    case Command::MicTune: micTune = c.a < 0 ? !micTune : c.a != 0; break;
    case Command::Autotune: atAmount = dsp::clamp(atAmount + c.f, 0.0f, 1.0f); if (c.a) atSpeed = dsp::clamp(atSpeed + c.a * 0.1f, 0.0f, 1.0f); break;
    case Command::Transpose: transpose = std::max(-12, std::min(12, transpose + c.a)); break;
    case Command::SkipLine: curActive = false; break;
    case Command::ClearLines: { std::lock_guard<std::mutex> g(lineMu); lines.clear(); curActive = false; } break;
    case Command::Style: plan.style = STYLES[((plan.styleIndex() + c.a) % NUM_STYLES + NUM_STYLES) % NUM_STYLES]; applyPlanNow(plan); break;
    case Command::Feel: plan.feel = FEELS[((plan.feelIndex() + c.a) % NUM_FEELS + NUM_FEELS) % NUM_FEELS]; break;
    case Command::Swing: swing = std::max(0, std::min(40, swing + c.a)); break;
    case Command::Tap: if (analysis) analysis->tap(); tempoMode = 2; needBarAlign = true; break;
  }
}

void Engine::applyPlanNow(const Plan& p) {
  Plan keepLyrics = plan;
  plan = p; havePlan = true; plansApplied++;
  density[P_DRUMS] = p.drums; density[P_BASS] = p.bass; density[P_KEYS] = p.keys; density[P_LEAD] = p.lead; energy = p.energy;
  following = p.follow || p.chords.empty(); swing = p.swing; atAmount = p.autotuneAmount / 100.0f; atSpeed = p.autotuneSpeed / 100.0f;
  motifPos = 0; leadRest = 0;
  // timbres by style
  drums.soft = is(p, "jazz") || is(p, "folk") || is(p, "ambient") || p.feel == "brush";
  bool bright = is(p, "funk") || is(p, "house") || is(p, "synthwave") || is(p, "rock");
  bassV.wave = (is(p, "hiphop") || is(p, "reggae") || is(p, "ambient")) ? 3 : (is(p, "synthwave") || is(p, "house") ? 1 : 0);
  bassV.cutoff = bright ? 420 : 260; bassV.amp.set(0.004f, is(p, "hiphop") ? 0.8f : 0.3f, is(p, "hiphop") ? 0.8f : 0.5f, is(p, "funk") ? 0.05f : 0.12f);
  for (auto& v : keysV) {
    v.wave = (is(p, "synthwave") || is(p, "house")) ? 1 : (is(p, "jazz") || is(p, "folk") ? 2 : 0);
    v.cutoff = is(p, "ambient") || is(p, "pad") ? 500 : (bright ? 1600 : 900);
    bool padLike = is(p, "pad") || is(p, "ambient") || (p.keys <= 1 && !is(p, "funk") && !is(p, "reggae"));
    v.amp.set(padLike ? 0.25f : 0.01f, 0.4f, padLike ? 0.8f : 0.4f, padLike ? 0.6f : 0.2f);
  }
  leadV.wave = (is(p, "synthwave") || is(p, "hiphop") || is(p, "house")) ? 1 : (is(p, "ambient") || is(p, "jazz") ? 3 : (is(p, "folk") || is(p, "latin") ? 2 : 0));
  leadV.cutoff = bright ? 2200 : 1300;
  leadDelay.mix = is(p, "ambient") || is(p, "synthwave") || is(p, "reggae") ? 0.35f : 0.18f;
  if (p.lyrics.empty() && !p.quick) { /* no new lyrics: keep what is queued */ }
}

void Engine::updateClock(int frames) {
  // tempo
  float target = bpm;
  if (tempoMode == 1 && hear.tempoLocked && hear.bpm > 40) target = hear.bpm;
  else if (tempoMode == 2 && hear.bpm > 40) target = hear.bpm;
  else if (tempoMode == 0) target = freeBpm;
  if (fabsf(target - bpm) > 8.0f) bpm = target; else bpm += 0.08f * (target - bpm);
  double beatsPerBlock = (double)frames / (60.0 / bpm) / dsp::RATE;
  // phase lock onto the humans' beat (their clock lags us by the capture path, ~40 ms)
  if ((tempoMode == 1 && hear.tempoLocked && hear.lastBeat >= 0) || tempoMode == 2) {
    double theirs = (hear.time + 0.04 - hear.lastBeat) / hear.period; theirs -= floor(theirs);
    double ours = beatPos - floor(beatPos);
    double diff = theirs - ours; if (diff > 0.5) diff -= 1.0; if (diff < -0.5) diff += 1.0;
    beatPos += diff * (needBarAlign ? 0.5 : 0.06);
    if (needBarAlign && (hear.barConf > 0.3f || tempoMode == 2) && fabs(diff) < 0.1) {
      int ourBeat = ((int)floor(beatPos)) & 3;
      int shift = (hear.beatInBar - ourBeat + 4) % 4;
      beatPos += shift; needBarAlign = false; bar = 0;
    }
  }
  beatPos += beatsPerBlock;
  // MIDI clock: 24 per beat
  if (midi && midi->isOpen()) { int tickNow = (int)floor(beatPos * 24.0); while (midiClockCount < tickNow) { midi->clock(); midiClockCount++; } }
}

void Engine::decideChord() {
  if (hear.key >= 0 && hear.keyConf > 0.2f) { keyPc = hear.key; mode = hear.minorKey ? 1 : 0; }
  int r = -1, t = 0;
  if (following) {
    if (hear.chordRoot >= 0 && !hear.silence) { r = hear.chordRoot; t = hear.chordType; lastHeardRoot = r; lastHeardType = t; }
    else if (!plan.chords.empty()) { const PlanChord& pc = plan.chords[(bar / 1) % plan.chords.size()]; r = degreeToMidi(keyPc, mode, pc.degree - 1) % 12; t = pc.quality == "min" ? CH_MIN : pc.quality == "7" ? CH_DOM7 : pc.quality == "maj7" ? CH_MAJ7 : pc.quality == "min7" ? CH_MIN7 : pc.quality == "dim" ? CH_DIM : pc.quality == "sus" ? CH_SUS4 : CH_MAJ; }
    else if (lastHeardRoot >= 0) { r = lastHeardRoot; t = lastHeardType; }
    else if (keyPc >= 0) { r = keyPc; t = mode == 1 ? CH_MIN : CH_MAJ; }
  } else if (!plan.chords.empty()) {
    const PlanChord& pc = plan.chords[bar % plan.chords.size()]; r = degreeToMidi(keyPc, mode, pc.degree - 1) % 12;
    t = pc.quality == "min" ? CH_MIN : pc.quality == "7" ? CH_DOM7 : pc.quality == "maj7" ? CH_MAJ7 : pc.quality == "min7" ? CH_MIN7 : pc.quality == "dim" ? CH_DIM : pc.quality == "sus" ? CH_SUS4 : CH_MAJ;
  } else if (keyPc >= 0) { r = keyPc; t = mode == 1 ? CH_MIN : CH_MAJ; }
  if (r != chordRoot || t != chordType) { chordRoot = r; chordType = t; chordChanged = true; }
}

std::vector<int> Engine::chordVoicing() const {
  std::vector<int> v; if (chordRoot < 0) return v;
  for (int iv : chordTypes()[chordType].iv) { int n = 60 + ((chordRoot + iv) % 12); if (n > 67 && iv != 0) n -= 12; if (iv == 0 && n > 64) n -= 12; v.push_back(n); }
  std::sort(v.begin(), v.end()); return v;
}

void Engine::noteOn(int part, int note, float vel, float lengthSteps) {
  if (note < 0 || mute[part]) return;
  vel *= 0.75f + 0.25f * energy / 3.0f;
  int ch = part == P_DRUMS ? 9 : part - 1;   // bass ch1, keys ch2, lead ch3 (0-based 0,1,2), drums ch10
  if (part == P_BASS) { bassV.noteOn(note, vel, is(plan, "reggae") || is(plan, "hiphop") ? 0.0f : 0.012f); }
  else if (part == P_LEAD) { leadV.noteOn(note, vel, is(plan, "synthwave") || is(plan, "blues") || is(plan, "ambient") ? 0.03f : 0.0f); }
  else if (part == P_KEYS) {
    int slot = -1; for (int i = 0; i < 5; i++) if (!keysV[i].active()) { slot = i; break; }
    if (slot < 0) { slot = 0; for (int i = 1; i < 5; i++) if (keysV[i].age > keysV[slot].age) slot = i; }
    keysV[slot].noteOn(note, vel, 0.0f); keysNotes[slot] = note;
  }
  if (midi && midi->isOpen()) { midi->noteOn(ch, note, (int)(vel * 110) + 10); midiNotes++; }
  if (part != P_DRUMS) offs.push_back({part, note, ch, (int)(lengthSteps * stepSeconds() * dsp::RATE)});
}
void Engine::noteOff(int part, int note) {
  if (part == P_BASS) { if (bassV.note == note) bassV.noteOff(); }
  else if (part == P_LEAD) { if (leadV.note == note) leadV.noteOff(); }
  else if (part == P_KEYS) { for (int i = 0; i < 5; i++) if (keysNotes[i] == note && keysV[i].active()) { keysV[i].noteOff(); keysNotes[i] = -1; } }
  if (midi && midi->isOpen() && part != P_DRUMS) midi->noteOff(part - 1, note);
}

void Engine::keysChord(int step, float vel, float lengthSteps, bool arpOne) {
  std::vector<int> v = chordVoicing(); if (v.empty()) return;
  if (arpOne) { int n = v[arpPos % v.size()]; if ((arpPos / v.size()) & 1) n += 12; arpPos++; noteOn(P_KEYS, n, vel, lengthSteps); return; }
  for (size_t i = 0; i < v.size(); i++) noteOn(P_KEYS, v[i] + (is(plan, "jazz") && i == 0 ? 12 : 0), vel * (0.9f + 0.1f * (i == 0)), lengthSteps);
}

int Engine::bassNote(int which) {   // 0 root, 1 fifth, 2 octave, 3 approach (semitone under the root), 4 third, 5 seventh
  if (chordRoot < 0) return -1;
  int root = 36 + chordRoot; if (root > 45) root -= 12;
  const auto& iv = chordTypes()[chordType].iv;
  switch (which) { case 1: return root + 7; case 2: return root + 12; case 3: return root - 1; case 4: return root + (iv.size() > 1 ? iv[1] : 4); case 5: return root + (iv.size() > 3 ? iv[3] : 10); default: return root; }
}

int Engine::leadNote(bool strong) {
  if (plan.motif.empty() || keyPc < 0) return -1;
  int deg = plan.motif[motifPos % plan.motif.size()]; motifPos++;
  int tonic = 72 + keyPc; if (tonic > 77) tonic -= 12;
  int n = degreeToMidi(tonic, mode, deg);
  if (strong && chordRoot >= 0) { uint16_t cm = chordMask(chordRoot, chordType); if (!(cm & (1 << (n % 12)))) n = nearestInMask((float)n, cm); }
  return n;
}

// a 16-step pattern string: x hit, X accent, . rest
static bool at(const char* pat, int step) { return pat[step] == 'x' || pat[step] == 'X'; }
static float acc(const char* pat, int step) { return pat[step] == 'X' ? 1.0f : 0.75f; }

void Engine::tick(int step) {
  if (step == 0) { bar++; chordBar++; }
  decideChord();
  // nothing heard and nothing from Gemma: the band waits for the room rather than leading it
  if (chordRoot < 0 && hear.silence) return;
  const int d = density[P_DRUMS];
  const std::string& f = plan.feel;
  bool house = is(plan, "house"), reggae = is(plan, "reggae"), latin = is(plan, "latin"), hiphop = is(plan, "hiphop"), funk = is(plan, "funk"), jazz = is(plan, "jazz");
  // ---- drums
  if (d > 0 && f != "none") {
    const char *K = "x.......x.......", *S = "....x.......x...", *H = "x.x.x.x.x.x.x.x.", *O = "................", *R = "................", *C = "................";
    if (f == "halftime") { K = "x.........x....."; S = "........x......."; H = d >= 2 ? "x.x.x.x.x.x.x.x." : "x...x...x...x..."; }
    else if (f == "shuffle") { K = "x...x...x...x..."; S = "....x.......x..."; H = "x.x.x.x.x.x.x.x."; }
    else if (f == "breakbeat") { K = "x.....x...x....."; S = "....x......x..x."; H = "x.x.x.x.x.x.x.x."; }
    else if (f == "brush") { K = d >= 2 ? "x..............." : "................"; S = "................"; H = "x.x.x.x.x.x.x.x."; R = "....x.......x..."; }
    if (house) { K = "x...x...x...x..."; O = "..x...x...x...x."; C = d >= 2 ? "....x.......x..." : "................"; }
    if (reggae) { K = "........x......."; S = "........x......."; H = "x.x.x.x.x.x.x.x."; R = d >= 2 ? "..x...x...x...x." : "................"; }
    if (latin) { K = "x......x..x....."; R = "x..x..x...x.x..."; S = "................"; H = "x.x.x.x.x.x.x.x."; }
    if (hiphop && f == "straight") { K = "x.....x...x....."; S = "....x.......x..."; }
    if (funk) { K = "x..x....x.x....."; S = "....x.......x.x."; }
    if (d >= 3 && f != "brush") H = "xXxXxXxXxXxXxXxX";
    if (d >= 2 && !house && !reggae && f == "straight" && !hiphop) K = "x......x..x.....";
    if (d >= 3 && (f == "straight" || f == "breakbeat")) S = funk ? "....x..x....x.x." : "....x.......x..x";
    if (d <= 1 && f != "brush") H = jazz ? "x.x.x.x.x.x.x.x." : "x...x...x...x...";
    float v = 0.6f + 0.13f * energy;
    if (at(K, step)) drums.kick(v * acc(K, step));
    if (at(S, step)) { if (drums.soft) drums.rim(v * 0.8f); else drums.snare(v * acc(S, step)); }
    if (at(O, step)) drums.hat(v * 0.8f, true); else if (at(H, step)) drums.hat((step % 4 == 0 ? 1.0f : 0.65f) * v * acc(H, step), false);
    if (at(R, step)) drums.rim(v * 0.8f);
    if (at(C, step)) drums.clap(v);
    if (midi && midi->isOpen() && !mute[P_DRUMS]) { if (at(K, step)) midi->noteOn(9, 36, 100); if (at(S, step)) midi->noteOn(9, 38, 100); if (at(H, step)) midi->noteOn(9, 42, 80); if (at(O, step)) midi->noteOn(9, 46, 80); if (at(R, step)) midi->noteOn(9, 37, 80); if (at(C, step)) midi->noteOn(9, 39, 90); }
    level[P_DRUMS] = std::max(level[P_DRUMS], (at(K, step) || at(S, step)) ? 1.0f : 0.0f);
  }
  // ---- bass
  int bd = density[P_BASS];
  if (bd > 0 && chordRoot >= 0) {
    int which = -1; float len = 2.0f;
    if (reggae) { if (step == 0) which = 0; else if (step == 10) which = 1; else if (bd >= 2 && step == 6) which = 2; len = 3.0f; }
    else if (hiphop) { if (step == 0) { which = 0; len = 6.0f; } else if (step == 7) { which = 0; len = 2.0f; } else if (bd >= 2 && step == 10) { which = 1; len = 4.0f; } }
    else if (latin) { if (step == 6) { which = 1; len = 5.0f; } else if (step == 12) { which = 0; len = 4.0f; } else if (bd >= 2 && step == 0) { which = 0; len = 2.0f; } }
    else if (funk) { static const int pat[16] = {0, -1, -1, 2, -1, -1, 0, -1, -1, -1, 1, 5, -1, -1, 0, -1}; which = pat[step]; if (bd < 2 && (step == 3 || step == 11)) which = -1; len = 1.0f; }
    else if (jazz) { if (step % 4 == 0) { static const int walk[4] = {0, 4, 1, 3}; which = walk[(step / 4) & 3]; len = 3.5f; } }
    else if (house || is(plan, "synthwave")) { if (step % 2 == 0) { which = (step % 4 == 2) ? 2 : 0; if (bd < 2 && step % 4 == 2) which = -1; len = 1.5f; } }
    else {   // rock, pad, folk, blues, ambient
      if (bd == 1) { if (step == 0 || step == 8) which = 0; len = 7.0f; }
      else if (bd == 2) { if (step == 0) which = 0; else if (step == 6) which = 0; else if (step == 8) which = 1; else if (step == 14) which = 3; len = 1.8f; }
      else { if (step % 2 == 0) { static const int r8[8] = {0, 0, 1, 0, 2, 0, 1, 3}; which = r8[step / 2]; } len = 1.8f; }
      if (is(plan, "ambient") || is(plan, "pad")) { if (step == 0) which = 0; else which = -1; len = 15.0f; }
    }
    if (which >= 0) noteOn(P_BASS, bassNote(which), 0.85f, len);
  }
  // ---- keys
  int kd = density[P_KEYS];
  if (kd > 0 && chordRoot >= 0) {
    bool padLike = is(plan, "pad") || is(plan, "ambient") || (kd == 1 && !funk && !reggae && !latin);
    if (padLike) { if (step == 0 || (chordChanged && step % 2 == 0)) keysChord(step, 0.55f, 15.5f, false); }
    else if (kd == 3 && (house || is(plan, "synthwave") || is(plan, "ambient") || is(plan, "pad"))) { if (step % 2 == 0) keysChord(step, 0.6f, 1.7f, true); }
    else if (reggae) { if (step == 2 || step == 6 || step == 10 || step == 14) keysChord(step, 0.7f, 1.2f, false); }
    else if (funk) { if (step == 2 || step == 7 || step == 10 || (kd >= 3 && step == 13)) keysChord(step, 0.75f, 1.5f, false); }
    else if (jazz) { if (step == 3 || step == 10 || (kd >= 3 && step == 14)) keysChord(step, 0.6f, 2.5f, false); }
    else if (latin) { if (step == 0 || step == 3 || step == 6 || step == 10 || step == 12) keysChord(step, 0.65f, 1.2f, false); }
    else if (is(plan, "folk")) { if (step % 2 == 0) keysChord(step, step % 4 == 0 ? 0.7f : 0.45f, 1.8f, kd < 3 && step % 4 == 2); }
    else { if (step == 0 || step == 8 || (kd >= 3 && (step == 4 || step == 12))) keysChord(step, 0.6f, kd >= 3 ? 3.5f : 7.5f, false); }
  }
  // ---- lead: phrases of two bars, rests scale with the inverse of density
  int ld = density[P_LEAD];
  if (ld > 0 && keyPc >= 0) {
    if (step == 0) { if (leadRest > 0) leadRest--; else if (ld == 1 && (bar % 4) == 2) leadRest = 1; else if (ld == 2 && (bar % 4) == 3) leadRest = 1; }
    if (leadRest == 0) {
      bool hit = false;
      if (ld == 1) hit = step == 0 || step == 6 || step == 8 || step == 11;
      else if (ld == 2) hit = step % 2 == 0 && step != 14;
      else hit = step % 2 == 0 || step == 3 || step == 11;
      if (is(plan, "ambient")) hit = step == 0 || step == 8;
      if (hit) noteOn(P_LEAD, leadNote(step % 4 == 0), 0.7f + 0.1f * (step % 4 == 0), ld == 3 ? 1.6f : (ld == 2 ? 1.9f : 3.5f));
    }
  }
  chordChanged = false;
}

void Engine::voiceSample(float& l, float& r) {
  float v = 0;
  // sung lines: one at a time, each starting on a bar
  if (!curActive) {
    int step = ((int)floor(beatPos * 4.0)) & 15;
    if (step == 0 && lastStep != 0) { std::lock_guard<std::mutex> g(lineMu); if (!lines.empty() && cur.text != lines.front().text) { } if (!lines.empty()) { cur = std::move(lines.front()); lines.pop_front(); curPos = 0; curActive = true; lyricBar = bar; } }
  }
  if (curActive) {
    if (curPos < cur.pcm.size()) {
      float s = cur.pcm[curPos++];
      float forced = -1;
      if (!cur.melody.empty() && cur.sung) {
        size_t idx = std::min(cur.melody.size() - 1, curPos * cur.melody.size() / std::max<size_t>(1, cur.pcm.size()));
        int tonic = (keyPc >= 0 ? keyPc : 0) + 60; if (tonic > 66) tonic -= 12;
        lyricNote = degreeToMidi(tonic, mode, cur.melody[idx]) + transpose; forced = (float)lyricNote;
      }
      v += tuneLine.process(s, keyMask(), forced, cur.sung ? atAmount : 0.35f, cur.sung ? atSpeed : 0.2f, 0) * 1.3f;
    } else { curActive = false; lyricNote = -1; }
  }
  // spoken comments (not sung), as soon as they arrive
  if (!sayActive) { std::lock_guard<std::mutex> g(lineMu); if (!spoken.empty()) { say = std::move(spoken.front()); spoken.pop_front(); sayPos = 0; sayActive = true; } }
  if (sayActive) { if (sayPos < say.pcm.size()) v += say.pcm[sayPos++] * 1.1f; else sayActive = false; }
  // the humans' own voice through the tuner, when they ask for it (headphones!)
  if (micTune) {
    size_t w = micW.load(std::memory_order_acquire);
    if (micR + 2400 < w) micR = w - 1200;            // fell behind: jump close to live
    if (micR < w) { float s = micRing[micR & (micRing.size() - 1)]; micR++; v += tuneMic.process(s, keyMask(), -1, atAmount, atSpeed, (float)transpose) * 1.2f; }
  }
  float d = voiceDelay.process(v) * 0.18f;
  float rl, rr; verb.process(v * 0.25f, rl, rr);
  l = v + d + rl; r = v + d * 0.8f + rr;
  level[P_VOICE] = std::max(level[P_VOICE], fabsf(v));
}

void Engine::render(float* out, int frames) {
  { std::unique_lock<std::mutex> g(cmdMu, std::try_to_lock); if (g.owns_lock()) { for (auto& c : cmds) handle(c); cmds.clear(); } }
  if (analysis) hear = analysis->get();
  if (playing) updateClock(frames);
  double stepLen = stepSeconds() * dsp::RATE;
  leadDelay.setSeconds((float)(stepSeconds() * 3)); voiceDelay.setSeconds((float)(stepSeconds() * 4));
  int stepsPerBlock = std::max(1, (int)(frames / stepLen) + 1);
  for (int i = 0; i < frames; i++) {
    // step ticks (swing delays the odd 16ths)
    if (playing) {
      double bp = beatPos - (double)(frames - i) / dsp::RATE / (60.0 / bpm);
      int step = ((int)floor(bp * 4.0)) & 15;
      if (step != lastStep) { lastStep = step; if ((step & 1) && swing > 0) { pendingStep = step; pendingDelay = (int)(stepLen * swing / 100.0); } else tick(step); }
      if (pendingStep >= 0 && --pendingDelay <= 0) { tick(pendingStep); pendingStep = -1; }
      for (size_t k = 0; k < offs.size();) { if (--offs[k].offAt <= 0) { noteOff(offs[k].part, offs[k].note); offs[k] = offs.back(); offs.pop_back(); } else k++; }
    }
    float dr = drums.next() * volume[P_DRUMS] * (mute[P_DRUMS] ? 0 : 1);
    float bs = bassTone.lp(dsp::fastTanh(bassV.next() * 1.6f)) * volume[P_BASS] * (mute[P_BASS] ? 0 : 1);
    float ky = 0; for (auto& v : keysV) ky += v.next(); ky *= 0.35f * volume[P_KEYS] * (mute[P_KEYS] ? 0 : 1);
    float ld = leadV.next() * 0.6f; ld = (ld + leadDelay.process(ld) * leadDelay.mix) * volume[P_LEAD] * (mute[P_LEAD] ? 0 : 1);
    float vl = 0, vr = 0; voiceSample(vl, vr); vl *= volume[P_VOICE] * (mute[P_VOICE] ? 0 : 1); vr *= volume[P_VOICE] * (mute[P_VOICE] ? 0 : 1);
    float kl, kr; verb.process(ky * 0.15f, kl, kr);
    float L = dr * 0.95f + bs + ky * 0.9f + kl + ld * 0.8f + vl, R = dr * 0.95f + bs + ky * 1.1f + kr + ld * 1.0f + vr;
    L = dsp::fastTanh(L * master * 0.9f); R = dsp::fastTanh(R * master * 0.9f);
    out[2 * i] = L; out[2 * i + 1] = R;
    outPeak = std::max(outPeak * 0.9998f, std::max(fabsf(L), fabsf(R)));
    level[P_BASS] = std::max(level[P_BASS] * 0.9995f, fabsf(bs)); level[P_KEYS] = std::max(level[P_KEYS] * 0.9995f, fabsf(ky)); level[P_LEAD] = std::max(level[P_LEAD] * 0.9995f, fabsf(ld));
    level[P_DRUMS] *= 0.9993f; level[P_VOICE] *= 0.9995f;
  }
  (void)stepsPerBlock;
  publish();
}

void Engine::publish() {
  std::unique_lock<std::mutex> g(stateMu, std::try_to_lock); if (!g.owns_lock()) return;
  snap.playing = playing; snap.following = following; snap.locked = hear.tempoLocked; snap.micTune = micTune;
  snap.bpm = bpm; snap.beatPos = beatPos; snap.step = lastStep < 0 ? 0 : lastStep; snap.bar = bar; snap.beatInBar = ((int)floor(beatPos)) & 3;
  snap.chordRoot = chordRoot; snap.chordType = chordType; snap.key = keyPc; snap.mode = mode;
  for (int p = 0; p < NUM_PARTS; p++) { snap.partLevel[p] = std::min(1.0f, level[p] * 2.0f); snap.partMute[p] = mute[p]; snap.volume[p] = volume[p]; }
  for (int p = 0; p < 4; p++) snap.density[p] = density[p];
  snap.lyricNow = curActive ? cur.text : ""; snap.lyricProgress = curActive && !cur.pcm.empty() ? (float)curPos / cur.pcm.size() : 0; snap.lyricNote = curActive ? lyricNote : -1;
  { std::unique_lock<std::mutex> l(lineMu, std::try_to_lock); if (l.owns_lock()) { snap.linesQueued = (int)lines.size(); snap.lyricNext = lines.empty() ? "" : lines.front().text; } }
  snap.voiceDetected = tuneLine.detectedMidi; snap.voiceTarget = tuneLine.targetMidi; snap.autotuneAmount = atAmount; snap.autotuneSpeed = atSpeed; snap.transpose = transpose;
  snap.plan = plan; snap.plansApplied = plansApplied; snap.master = master; snap.outPeak = outPeak; snap.swing = swing;
  snap.tempoSource = tempoMode == 0 ? "free" : (tempoMode == 2 ? "tapped" : (hear.tempoLocked ? "the room" : "listening"));
  snap.midiOut = midi && midi->isOpen(); snap.midiNotes = midiNotes;
}
