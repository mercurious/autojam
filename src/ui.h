// autojam: the screen (1920x1080): left, what Gemma hears; middle, what Gemma plays and sings; right, the
// controls to steer it. Touch and the gamepad drive the same controls.
#pragma once
#include "gfx.h"
#include "analysis.h"
#include "player.h"
#include "brain.h"
#include "sysaudio.h"
#include <vector>
#include <string>
#include <functional>

struct Ctl {
  enum Kind { Button, Toggle, Slider } kind; std::string id, label;
  int x, y, w, h; std::function<void(int)> act;   // Button/Toggle: act(0) press; Slider: act(delta) in steps, act(1000+pct) absolute
  std::function<float()> value; std::function<bool()> on; std::function<std::string()> text;
};

class Panel {
public:
  Panel(Gfx& g, Analysis& a, Engine& e, Brain& b) : gfx(g), analysis(a), engine(e), brain(b) {}
  void init();
  void handleEvent(const SDL_Event& ev);
  void update(float dt);
  void draw();
  bool wantsQuit() const { return quit; }
  std::string micDesc = "no mic", micSource; std::string sink; std::string midiOutPort; std::string hint;
  std::function<void()> onRepickMic; std::function<void()> onNextSink; std::function<void()> onRescanMidi;
  bool micOk = false; bool noServers = false;
private:
  Gfx& gfx; Analysis& analysis; Engine& engine; Brain& brain;
  std::vector<Ctl> ctls; int focus = 0; bool quit = false; float t = 0, flash = 0; std::string flashMsg;
  bool selectDown = false, startDown = false; float stickLY = 0, stickRY = 0, stickRX = 0; float volAccum = 0;
  Hearing h; BandState b; BrainState s; sysaudio::Battery bat; float batTimer = 0;
  std::map<int64_t, int> drags;
  SDL_GameController* pad = nullptr;
  void build();
  int hit(int x, int y) const;
  void press(int i);
  void moveFocus(int dx, int dy);
  void say(const std::string& m) { flashMsg = m; flash = 2.5f; }
  void drawHearing(); void drawBand(); void drawSteer(); void drawTop(); void drawBottom();
  void drawCtl(const Ctl& c, bool focused);
  void steer(const std::string& what) { brain.steer(what); say("Gemma: " + what); }
  void toggleEars() {
    if (!s.canHear) { say("No ears: the model server runs without the audio projector (mmproj, 10 GB devices)"); return; }
    brain.toggleEars(); say(s.ears ? "Gemma reads the measurements only" : "Gemma also gets 8 s of audio with each plan");
  }
};
