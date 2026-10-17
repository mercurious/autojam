// autojam: where the room's sound comes from. A PipeWire (Pulse protocol) capture of one source at 48 kHz
// S16 stereo (the format the Flip 2's q6 capture path is pinned to; see extras/), downmixed to mono and handed
// to a callback on the capture thread. A WAV file can stand in for the mic (desktop testing, --wav), replayed
// at real time or as fast as possible (--analyze).
#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <cstdint>

class MicCapture {
public:
  using Sink = std::function<void(const float* mono, int n)>;
  static constexpr int RATE = 48000;
  bool start(const std::string& source, Sink sink);      // source: a pactl source name, or "" for the default
  bool startWav(const std::string& path, Sink sink, bool realtime, bool loop);
  void stop();
  bool running() const { return run.load(); }
  std::string error;
  std::string source;
  double seconds() const { return (double)frames.load() / RATE; }   // audio delivered so far
  float level() const { return lvl.load(); }
  ~MicCapture() { stop(); }
private:
  std::thread th; std::atomic<bool> run{false}; std::atomic<long long> frames{0}; std::atomic<float> lvl{0};
};

// WAV reading (PCM 16/24/32-bit int and 32-bit float, any channels), resampled to 48 kHz mono by linear interpolation
bool readWavMono48k(const std::string& path, std::vector<float>& out, std::string& err);
