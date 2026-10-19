// autojam: the band. Runs inside the audio callback: a clock that locks onto the humans' beat, drums, bass,
// keys and a lead that follow the chord being heard (or the plan's chords), and a voice bus where Gemma's
// lines are sung through the pitch corrector. The UI steers it through Commands; the language model through
// Plans. Everything the UI shows about the band comes from a BandState snapshot.
#pragma once
#include "plan.h"
#include "synth.h"
#include "autotune.h"
#include "analysis.h"
#include "midi.h"
#include <mutex>
#include <vector>
#include <deque>
#include <string>
#include <atomic>
#include <memory>

enum Part { P_DRUMS = 0, P_BASS, P_KEYS, P_LEAD, P_VOICE, NUM_PARTS };
static const char* const PART_NAMES[NUM_PARTS] = {"drums", "bass", "keys", "lead", "voice"};

struct VoiceLine { std::vector<float> pcm; std::vector<int> melody; std::string text; bool sung = true; int verse = 0, of = 1;   // verse: one per plan, of: its lines
  std::vector<std::pair<int, int>> notes; };   // (sample, MIDI) when the singer already pitched it (no autotune then)

struct Command {
  enum Type { Play, Stop, ApplyPlan, Density, Mute, Volume, Energy, Follow, Resync, Tempo, MicTune, Autotune, Transpose, SkipLine, ClearLines, Style, Feel, Swing, Tap } type;
  int a = 0, b = 0; float f = 0; Plan* plan = nullptr;
};

struct BandState {
  bool playing = false, following = true, locked = false, micTune = false, planPending = false;
  float bpm = 100; double beatPos = 0; int step = 0, bar = 0, beatInBar = 0;
  int chordRoot = -1, chordType = 0; int key = -1, mode = 0;
  float partLevel[NUM_PARTS] = {}; bool partMute[NUM_PARTS] = {}; int density[4] = {}; float volume[NUM_PARTS] = {};
  std::string lyricNow, lyricNext; float lyricProgress = 0; int linesQueued = 0, verseLines = 0; int lyricNote = -1;
  float voiceDetected = 0, voiceTarget = 0, autotuneAmount = 0.9f, autotuneSpeed = 0.35f; int transpose = 0;
  Plan plan; int plansApplied = 0; float master = 0.8f; float outPeak = 0; int swing = 0; std::string tempoSource = "free";
  bool midiOut = false; int midiNotes = 0;
};

class Engine {
public:
  void init(Analysis* a) { analysis = a; setup(); }
  void render(float* stereo, int frames);
  void micIn(const float* mono, int n);                  // capture thread
  void queueLine(VoiceLine&& l);                         // brain thread: a sung line, starts on a bar; its verse repeats
  void speakNow(VoiceLine&& l);                          // brain thread: a spoken comment, starts at once
  void send(const Command& c) { std::lock_guard<std::mutex> g(cmdMu); cmds.push_back(c); }
  void applyPlan(const Plan& p) { Command c; c.type = Command::ApplyPlan; c.plan = new Plan(p); send(c); }
  BandState state() const { std::lock_guard<std::mutex> g(stateMu); return snap; }
  void setMidiOut(MidiOut* m) { midi = m; }
  std::string describePlaying() const;                   // one line for the model: what the band is doing

private:
  Analysis* analysis = nullptr; MidiOut* midi = nullptr;
  std::mutex cmdMu; std::vector<Command> cmds;
  mutable std::mutex stateMu; BandState snap;
  std::mutex lineMu; std::deque<std::shared_ptr<const VoiceLine>> lines; std::deque<VoiceLine> spoken;
  std::vector<std::shared_ptr<const VoiceLine>> verse; int verseId = -1; size_t versePos = 0;   // the verse on repeat
  // clock
  bool playing = false, following = true, needBarAlign = true; double beatPos = 0; float bpm = 100, freeBpm = 100; int lastStep = -1; int bar = 0; int pendingStep = -1, pendingDelay = 0;
  double lastClockTick = 0; int tempoMode = 1;           // 0 free, 1 humans, 2 tap
  Hearing hear;
  // music
  Plan plan; bool havePlan = false; int chordRoot = -1, chordType = 0, keyPc = -1, mode = 0; int lastHeardRoot = -1, lastHeardType = 0; bool chordChanged = false; int chordBar = 0;
  int density[4] = {1, 1, 1, 0}; bool mute[NUM_PARTS] = {}; float volume[NUM_PARTS] = {0.8f, 0.8f, 0.7f, 0.7f, 0.9f}; float master = 0.8f; int swing = 0; int energy = 1;
  int motifPos = 0, leadRest = 0, arpPos = 0, lyricBar = 0, lyricEndBar = 0;
  // instruments
  dsp::Drums drums; dsp::Voice bassV; dsp::Voice keysV[5]; int keysActive = 0; dsp::Voice leadV; dsp::Delay leadDelay, voiceDelay; dsp::Reverb verb; dsp::OnePole bassTone;
  struct Pending { int part, note, ch; int offAt; };   // sample countdown to note off
  std::vector<Pending> offs;
  int keysNotes[5] = {-1, -1, -1, -1, -1};
  // voice
  Autotune tuneLine, tuneMic; std::vector<float> micRing; std::atomic<size_t> micW{0}; size_t micR = 0; bool micTune = false; int transpose = 0; float atAmount = 0.9f, atSpeed = 0.35f;
  std::shared_ptr<const VoiceLine> cur; size_t curPos = 0, curNote = 0; bool curActive = false; VoiceLine say; size_t sayPos = 0; bool sayActive = false; int lyricNote = -1;
  float level[NUM_PARTS] = {}; float outPeak = 0; int plansApplied = 0; int midiNotes = 0; int midiClockCount = 0;
  bool wantPlay = false;

  void setup();
  void handle(const Command& c);
  void applyPlanNow(const Plan& p);
  void updateClock(int frames);
  void decideChord();
  void tick(int step);
  void noteOn(int part, int note, float vel, float lengthSteps);
  void noteOff(int part, int note);
  void keysChord(int baseStep, float vel, float lengthSteps, bool arpOne);
  int bassNote(int pattern);
  int leadNote(bool strong);
  void voiceSample(float& l, float& r);
  void publish();
  double stepSeconds() const { return 15.0 / bpm; }
  uint16_t keyMask() const { return keyPc >= 0 ? theory::scaleMask(keyPc, mode) : 0xFFF; }
  std::vector<int> chordVoicing() const;
};
