#include "link.h"
#include "json.h"
#include "theory.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>

double JamLink::steadyNow() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }

int JamLink::chordTypeOf(const std::vector<int>& tones) {
  // the theory type whose notes best cover the sender's (ninths and altered sevenths fall back to the nearest)
  uint16_t have = 0; for (int t : tones) have |= 1 << (((t % 12) + 12) % 12);
  const auto& types = theory::chordTypes(); int best = 0; float bestS = -1e9f;
  for (size_t i = 0; i < types.size(); i++) {
    uint16_t m = 0; for (int iv : types[i].iv) m |= 1 << (iv % 12);
    float s = __builtin_popcount(have & m) - 1.0f * __builtin_popcount(m & ~have) - 0.3f * __builtin_popcount(have & ~m);
    if (s > bestS) { bestS = s; best = (int)i; }
  }
  return best;
}

bool JamLink::start(int port, Sink sink) {
  stop();
  fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return false;
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  timeval tv{0, 500000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (sockaddr*)&a, sizeof a) < 0) { close(fd); fd = -1; return false; }
  run = true;
  th = std::thread([this, sink]() {
    char buf[8192]; double anchor = 0, anchorBpm = 0; std::string anchorFrom;
    while (run.load()) {
      sockaddr_in src{}; socklen_t sl = sizeof src;
      ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (sockaddr*)&src, &sl);
      if (n <= 0) continue;
      double now = steadyNow(); buf[n] = 0;
      json::Value v; if (!json::parse(std::string(buf, (size_t)n), v) || v.strOr("app", "") != "rap-machine") continue;
      LinkInfo L; L.at = now; char ip[INET_ADDRSTRLEN] = {}; inet_ntop(AF_INET, &src.sin_addr, ip, sizeof ip); L.from = ip;
      L.host = v.strOr("host", ""); L.lane = v.strOr("lane", ""); L.playing = v.boolOr("playing", false); L.bpm = (float)v.numOr("bpm", 0);
      L.root = v.intOr("root", -1); L.modeName = v.strOr("mode", "");
      L.mode = theory::modeIndex(L.modeName == "harmonic" ? "harmonic minor" : L.modeName);
      for (auto& c : v.arrOf("chords")) {
        std::vector<int> t; for (auto& x : c.arrOf("tones")) if (x.type == json::Value::Number) t.push_back((int)x.n);
        L.chordRoot.push_back(c.intOr("root", 0) % 12); L.chordType.push_back(chordTypeOf(t)); L.chordName.push_back(c.strOr("name", ""));
      }
      L.nextBar = v.intOr("bar", 0); L.nextBarIn = v.numOr("nextBarIn", 0) / 1000.0;
      // the downbeat, smoothed: Wi-Fi delivers each beacon a few to tens of ms late, so follow a phase estimate
      // that moves a little per beacon and only jumps on a new sender, tempo or a large error (a re-sync)
      if (L.playing && L.bpm > 0) {
        double bar = 240.0 / L.bpm, est = now + L.nextBarIn;
        if (anchor <= 0 || anchorFrom != L.from || fabs(anchorBpm - L.bpm) > 0.01) anchor = est;
        else {
          double d = fmod(est - anchor, bar); if (d > bar / 2) d -= bar; if (d < -bar / 2) d += bar;
          if (fabs(d) > 0.08) anchor = est; else anchor += 0.15 * d;
        }
        anchorBpm = L.bpm; anchorFrom = L.from;
        double k = round((est - anchor) / bar); L.nextBarIn = anchor + k * bar - now;   // the smoothed one nearest the beacon's (keeps its bar index)
      } else anchor = 0;
      sink(L);
    }
  });
  return true;
}

void JamLink::stop() { run = false; if (th.joinable()) th.join(); if (fd >= 0) { close(fd); fd = -1; } }
