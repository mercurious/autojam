// autojam: Gemma. A planner thread asks llama-server for a Plan (schema-constrained JSON, streamed so the UI
// can show it being written), hands it to the band and its lyrics to a speaker thread that fetches Pocket TTS
// audio (tts-server, streamed s16 24 kHz) and queues it as sung lines. The planner can also send Gemma the
// last seconds of the room as audio (the model's own ears, through its audio projector) when that is on.
#pragma once
#include "plan.h"
#include "player.h"
#include "json.h"
#include "analysis.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <string>
#include <vector>

struct BrainState {
  bool llmOk = false, ttsOk = false, llmLoading = false, ears = false, auto_ = true;
  bool canHear = true;                       // llama-server has an audio projector (/props modalities.audio)
  std::string phase = "idle", error, model, lastTitle; int tokens = 0; float seconds = 0; int plans = 0, failures = 0;
  std::vector<std::string> voices; int voiceIdx = 0; std::string voice;
  std::vector<std::string> steering;        // what the humans asked for since the last plan
  std::string lastSay, lastHear; double nextAutoIn = 0; int ttsQueued = 0; std::string lastRaw;
};

class Brain {
public:
  struct Options { std::string llmUrl = "http://127.0.0.1:8103", ttsUrl = "http://127.0.0.1:8104"; bool ears = false; int autoEverySec = 120; bool enabled = true; std::string voice; };
  void start(const Options& o, Engine* e, Analysis* a);
  void stop();
  void hear(const float* mono, int n);                       // capture thread: the room, kept for the audio clips
  void steer(const std::string& what);                       // a direction from the humans; a quick plan follows
  void requestPlan(bool full);                               // ask now
  void setAuto(bool on) { autoMode = on; }
  void toggleEars() { opts.ears = !opts.ears; }
  void nextVoice(int delta);
  void speak(const std::string& text, const std::vector<int>& melody, bool sung, int verse = 0, int of = 1);   // queue for TTS
  BrainState state() const { std::lock_guard<std::mutex> g(mu); return st; }
  std::string lastPlanJson() const { std::lock_guard<std::mutex> g(mu); return st.lastRaw; }
private:
  Options opts; Engine* engine = nullptr; Analysis* analysis = nullptr;
  mutable std::mutex mu; BrainState st; std::atomic<bool> run{false}, autoMode{true};
  std::thread planner, speaker; std::condition_variable cv, scv; std::mutex jobMu;
  struct Job { bool full; std::vector<std::string> steering; }; std::deque<Job> jobs; std::vector<std::string> pendingSteer; double steerDue = 0;
  struct Say { std::string text; std::vector<int> melody; bool sung; int verse, of; }; std::deque<Say> says; int verses = 0;
  std::vector<float> earRing; std::atomic<size_t> earW{0};
  double lastAuto = 0; std::deque<std::string> titles;
  void plannerLoop(); void speakerLoop();
  void health();
  bool fetchVoices();
  bool plan(const Job& j);
  std::string systemPrompt(bool full) const;
  std::string userPrompt(const Job& j, const Hearing& h) const;
  std::string schema(bool full) const;
  std::string clipWavBase64() const;
  static bool parsePlan(const json::Value& v, Plan& p, bool full);
};
