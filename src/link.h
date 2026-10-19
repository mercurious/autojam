// autojam: the jam link. Underground Rap Machine (another handheld on the LAN) beacons what it plays four times a
// second on UDP (its server.py, port 47800): tempo, key and mode, its chord loop, which bar comes next and in how
// many milliseconds that downbeat is heard. Through the air a Flip 2 speaker loses the kick and the 808, so the
// listener cannot lock onto it; over the link the band follows exactly. The listener still measures the room.
#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

struct LinkInfo {
  std::string from, host, lane;               // sender address, its hostname, its lane (glitter, jerk...)
  bool playing = false; float bpm = 0;
  int root = -1, mode = -1; std::string modeName;    // mode: theory::modes() index
  std::vector<int> chordRoot, chordType; std::vector<std::string> chordName;   // the loop, one chord a bar
  int nextBar = 0; double nextBarIn = 0;      // the bar that starts next (loop index), seconds from `at`
  double at = 0;                              // steady-clock seconds when this was received
};

class JamLink {
public:
  using Sink = std::function<void(const LinkInfo&)>;
  bool start(int port, Sink sink);            // listens on 0.0.0.0:port; false if the port cannot be bound
  void stop();
  ~JamLink() { stop(); }
  static int chordTypeOf(const std::vector<int>& tones);   // intervals from the root -> theory chord type
  static double steadyNow();
private:
  std::thread th; std::atomic<bool> run{false}; int fd = -1;
};
