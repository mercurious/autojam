// autojam: the listener. Mono audio at 48 kHz in, a Hearing out: tempo and beat phase, key, the chord being
// played, the chroma, a melody pitch and the level. Runs on the capture thread; the UI and the player take
// snapshots. Nothing here depends on the language model: this is what the humans play, measured.
#pragma once
#include "fft.h"
#include "theory.h"
#include "link.h"
#include <vector>
#include <deque>
#include <mutex>
#include <string>
#include <cstdint>

struct ChordEvent { int root, type; double start, end; };  // seconds

struct Hearing {
  double time = 0;              // seconds of audio heard
  // level
  float rmsDb = -100, peakDb = -100; bool silence = true; float energy = 0;   // energy 0..1 (loudness, smoothed)
  // beat
  float bpm = 0, beatConf = 0; double lastBeat = -1, period = 0.5;   // seconds; period = 60/bpm
  int beatInBar = 0; float barConf = 0; bool tempoLocked = false; float onset = 0;
  // tonality
  float chroma[12] = {}; float chromaLong[12] = {};
  int key = -1; bool minorKey = false; float keyConf = 0; int mode = -1;   // mode: theory::modes() index, when known (the link)
  int chordRoot = -1, chordType = 0; float chordConf = 0; double chordSince = 0;
  std::vector<ChordEvent> history;    // the last chords, oldest first
  // melody
  float pitchHz = 0, pitchMidi = 0, pitchConf = 0;
  // the jam link: tempo, phase, key and chords come from Rap Machine on the LAN while this is true
  bool linked = false; std::string linkFrom, linkLoop;
  std::string chordName() const { return theory::chordName(chordRoot, chordType); }
  std::string keyName() const { return key < 0 ? "?" : std::string(theory::NOTE_NAMES[key]) + " " + (mode >= 0 ? theory::modes()[mode].name : (minorKey ? "minor" : "major")); }
  // the other name of the same seven notes: a minor key's relative major, a major key's relative minor
  std::string relativeName() const { return key < 0 || mode >= 0 ? "" : std::string(theory::NOTE_NAMES[(key + (minorKey ? 3 : 9)) % 12]) + (minorKey ? " major" : " minor"); }
};

class Analysis {
public:
  static constexpr int RATE = 48000, HOP = 512, NFFT = 2048, CFFT = 8192, CHOP = 2048;
  Analysis();
  void feed(const float* mono, int n);      // any block size
  Hearing get() const { std::lock_guard<std::mutex> l(mu); return snap; }
  void tap();                               // a human tap: tempo and phase from taps
  void nudgePhase(double seconds) { std::lock_guard<std::mutex> l(mu); beatAnchor += seconds; }
  void setBpmOverride(float bpm) { std::lock_guard<std::mutex> l(mu); bpmOverride = bpm; }
  void link(const LinkInfo& l);             // a jam-link beacon (any thread): followed for 3 s
  void boostNotes(uint16_t pcMask, float w);  // MIDI-in notes: the keys someone presses count as heard
private:
  mutable std::mutex mu; Hearing snap;
  // input ring (power of two)
  std::vector<float> ring; size_t ringPos = 0, hopFill = 0; double samples = 0;
  // onset
  FFT fft; std::vector<float> win, mag, prevMag; std::vector<std::complex<float>> work;
  std::deque<float> osf, osfLow;            // onset strength per hop, ~8 s
  float osfMean = 0; int sinceTempo = 0;
  // tempo / beat
  double periodS = 0.5, beatAnchor = 0; float bpmEst = 0, beatConf = 0; std::deque<float> bpmHist; bool locked = false; int slips = 0;
  float barScore[4] = {}; int barOffset = 0; int beatCount = 0; double lastBeatOut = -1;
  float bpmOverride = 0; std::deque<double> taps;
  // chroma
  FFT cfft; std::vector<float> cwin, cmag; int chopFill = 0;
  float chromaFast[12] = {}, chromaSlow[12] = {}; uint16_t midiBoost = 0; float midiBoostW = 0;
  int curRoot = -1, curType = 0; float curScore = 0; int candRoot = -1, candType = 0, candCount = 0; double chordStart = 0;
  int keyPc = -1; bool keyMinor = false; float keyConf = 0;
  // level
  float rmsEma = 0, peakHold = 0; double quietSince = 0;
  // pitch
  std::vector<float> yinBuf;
  // the jam link: the last beacon, the downbeat of its next bar on the audio timeline, when it came
  LinkInfo lk; double linkAnchor = 0, linkSeen = -1e9; int linkR = -1, linkT = 0; double linkSince = 0; std::vector<ChordEvent> linkHist;
  void hop();
  void onsetFrame();
  void chromaFrame();
  void tempo();
  void chordAndKey();
  void pitch();
  void publish();
};
