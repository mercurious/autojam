#include "ui.h"
#include <cmath>
#include <algorithm>

static const int W = 1920, H = 1080;
static const Color BG{12, 13, 16}, FG{232, 232, 236}, MUTED{130, 134, 146}, HEAR{86, 196, 255}, PLAY{255, 168, 66}, STEER{150, 230, 120}, WARN{255, 96, 96}, VOICE{230, 120, 255}, OFF{38, 40, 48};

void Panel::init() {
  for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) { fprintf(stderr, "pad: %s\n", SDL_GameControllerName(pad)); break; } }
  build();
}

void Panel::build() {
  ctls.clear();
  // steering buttons: 3 x 4
  const int X0 = 1296, Y0 = 118, BW = 196, BH = 92, GX = 12, GY = 12;
  struct S { const char* label; const char* ask; } steers[12] = {
    {"Calmer", "play calmer and softer: lower the energy and every part by one"}, {"Busier", "play busier: raise the energy and the parts by one"}, {"Darker", "go darker: a minor or phrygian colour, lower register, sparser keys"},
    {"Brighter", "go brighter: major or lydian colour, higher keys, lighter drums"}, {"Sparser", "leave more space: drums and keys down, longer notes"}, {"Denser", "fill it up: busier drums, arpeggiated keys"},
    {"Lay back", "lay back: the lead rests, bass and drums only, follow the humans"}, {"Solo!", "take a lead solo for the next phrases: lead at 3 with a new motif, keys lighter"}, {"New lyrics", "write 3 or 4 new lyric lines about what you hear and sing them"},
    {"Change feel", "change the drum feel to something different from the current one"}, {"Own chords", "stop following for a while: give 4 chords of your own in this key, follow=false"}, {"Surprise me", "surprise us: a new style, a new motif, a new direction"},
  };
  for (int i = 0; i < 12; i++) {
    Ctl c; c.kind = Ctl::Button; c.id = std::string("steer") + std::to_string(i); c.label = steers[i].label;
    c.x = X0 + (i % 3) * (BW + GX); c.y = Y0 + (i / 3) * (BH + GY); c.w = BW; c.h = BH;
    std::string ask = steers[i].ask; c.act = [this, ask, i](int) {
      // some directions take effect at once, Gemma confirms them on its next plan
      if (i == 0) { Command k; k.type = Command::Energy; k.a = -1; engine.send(k); }
      if (i == 1) { Command k; k.type = Command::Energy; k.a = 1; engine.send(k); }
      if (i == 6) { Command k; k.type = Command::Density; k.a = P_LEAD; k.b = -3; engine.send(k); Command f; f.type = Command::Follow; f.a = 1; engine.send(f); }
      if (i == 7) { Command k; k.type = Command::Density; k.a = P_LEAD; k.b = 3; engine.send(k); }
      if (i == 9) { Command k; k.type = Command::Feel; k.a = 1; engine.send(k); }
      // lyrics only come with a full plan: steer first so that plan carries the request, not a quick one after it
      if (i == 8 || i == 11) { brain.steer(ask); brain.requestPlan(true); say(i == 8 ? "Asking Gemma for new lyrics" : "Asking Gemma to surprise us"); }
      else steer(ask);
    };
    ctls.push_back(c);
  }
  int y = Y0 + 4 * (BH + GY) + 6;   // 542
  auto toggle = [&](const char* id, const char* label, int col, std::function<void()> act, std::function<bool()> on) {
    Ctl c; c.kind = Ctl::Toggle; c.id = id; c.label = label; c.x = X0 + col * (BW + GX); c.y = y; c.w = BW; c.h = 64; c.act = [act](int) { act(); }; c.on = on; ctls.push_back(c);
  };
  toggle("play", "Band", 0, [this] { Command c; c.type = engine.state().playing ? Command::Stop : Command::Play; engine.send(c); }, [this] { return b.playing; });
  toggle("follow", "Follow room", 1, [this] { Command c; c.type = Command::Follow; c.a = !b.following; engine.send(c); }, [this] { return b.following; });
  toggle("auto", "Auto plans", 2, [this] { brain.setAuto(!s.auto_); }, [this] { return s.auto_; });
  y += 76;
  toggle("gvoice", "Gemma sings", 0, [this] { Command c; c.type = Command::Mute; c.a = P_VOICE; c.b = -1; engine.send(c); }, [this] { return !b.partMute[P_VOICE]; });
  toggle("mictune", "Tune my mic", 1, [this] { Command c; c.type = Command::MicTune; c.a = -1; engine.send(c); if (!b.micTune) say("Mic through the tuner: use headphones, speakers will feed back"); }, [this] { return b.micTune; });
  toggle("ears", "Gemma's ears", 2, [this] { toggleEars(); }, [this] { return s.ears && s.canHear; });
  y += 76;
  // sliders: autotune amount, speed, master
  auto slider = [&](const char* id, const char* label, int col, int span, std::function<void(int)> act, std::function<float()> val, std::function<std::string()> txt) {
    Ctl c; c.kind = Ctl::Slider; c.id = id; c.label = label; c.x = X0 + col * (BW + GX); c.y = y; c.w = BW * span + GX * (span - 1); c.h = 58; c.act = act; c.value = val; c.text = txt; ctls.push_back(c);
  };
  slider("amount", "Autotune amount", 0, 1, [this](int d) { Command c; c.type = Command::Autotune; c.f = d >= 1000 ? (d - 1000) / 100.0f - b.autotuneAmount : d * 0.1f; engine.send(c); }, [this] { return b.autotuneAmount; }, [this] { return std::to_string((int)(b.autotuneAmount * 100)) + "%"; });
  slider("speed", "Retune speed", 1, 1, [this](int d) { Command c; c.type = Command::Autotune; c.a = d >= 1000 ? (int)lroundf(((d - 1000) / 100.0f - b.autotuneSpeed) * 10) : d; engine.send(c); }, [this] { return b.autotuneSpeed; }, [this] { return b.autotuneSpeed > 0.8f ? "hard" : (b.autotuneSpeed < 0.25f ? "natural" : "medium"); });
  slider("master", "Band volume", 2, 1, [this](int d) { Command c; c.type = Command::Volume; c.a = -1; c.f = d >= 1000 ? (d - 1000) / 100.0f * 1.2f - b.master : d * 0.05f; engine.send(c); }, [this] { return b.master / 1.2f; }, [this] { return std::to_string((int)(b.master * 100)) + "%"; });
  y += 70;
  // row: voice prev/next, tap tempo, mic, output
  auto button = [&](const char* id, const char* label, int col, std::function<void()> act, std::function<std::string()> txt = nullptr) {
    Ctl c; c.kind = Ctl::Button; c.id = id; c.label = label; c.x = X0 + col * (BW + GX); c.y = y; c.w = BW; c.h = 64; c.act = [act](int) { act(); }; c.text = txt; ctls.push_back(c);
  };
  button("voice", "Voice ›", 0, [this] { brain.nextVoice(1); }, [this] { return s.voice.empty() ? "(none)" : s.voice; });
  button("tap", "Tap tempo", 1, [this] { Command c; c.type = Command::Tap; engine.send(c); }, [this] { return b.tempoSource; });
  button("ask", "Ask Gemma now", 2, [this] { brain.requestPlan(true); say("Asking Gemma for a full plan"); });
  y += 76;
  button("mic", "Mic ›", 0, [this] { if (onRepickMic) onRepickMic(); }, [this] { return micDesc; });
  button("out", "Output ›", 1, [this] { if (onNextSink) onNextSink(); }, [this] { return sink.empty() ? "default" : sink; });
  button("midi", "USB MIDI ›", 2, [this] { if (onRescanMidi) onRescanMidi(); }, [this] { return midiOutPort.empty() ? "none" : midiOutPort; });
  y += 76;
  button("transdn", "Voice -1", 0, [this] { Command c; c.type = Command::Transpose; c.a = -1; engine.send(c); }, [this] { return (b.transpose > 0 ? "+" : "") + std::to_string(b.transpose) + " st"; });
  button("transup", "Voice +1", 1, [this] { Command c; c.type = Command::Transpose; c.a = 1; engine.send(c); }, [this] { return (b.transpose > 0 ? "+" : "") + std::to_string(b.transpose) + " st"; });
  button("skip", "Skip line", 2, [this] { Command c; c.type = Command::SkipLine; engine.send(c); });
  // part density steppers inside the band card (middle column)
  for (int p = 0; p < 4; p++) {
    Ctl c; c.kind = Ctl::Slider; c.id = std::string("dens") + PART_NAMES[p]; c.label = PART_NAMES[p]; c.x = 1030; c.y = 390 + p * 46; c.w = 200; c.h = 34;
    c.act = [this, p](int d) { Command k; k.type = Command::Density; k.a = p; k.b = d >= 1000 ? (int)lroundf((d - 1000) / 100.0f * 3) - b.density[p] : d; engine.send(k); };
    c.value = [this, p] { return b.density[p] / 3.0f; }; c.text = [this, p] { static const char* lv[] = {"off", "light", "med", "busy"}; return std::string(lv[std::max(0, std::min(3, b.density[p]))]); };
    ctls.push_back(c);
  }
}

int Panel::hit(int x, int y) const { for (size_t i = 0; i < ctls.size(); i++) { const Ctl& c = ctls[i]; if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) return (int)i; } return -1; }
void Panel::press(int i) { if (i < 0 || i >= (int)ctls.size()) return; Ctl& c = ctls[i]; if (c.kind == Ctl::Slider) { c.act(1); } else c.act(0); }

void Panel::moveFocus(int dx, int dy) {
  const Ctl& f = ctls[focus]; int fx = f.x + f.w / 2, fy = f.y + f.h / 2; int best = -1; float bestD = 1e9f;
  for (size_t i = 0; i < ctls.size(); i++) {
    if ((int)i == focus) continue; const Ctl& c = ctls[i]; int cx = c.x + c.w / 2, cy = c.y + c.h / 2; int ddx = cx - fx, ddy = cy - fy;
    if (dx && (ddx * dx <= 0 || abs(ddy) > abs(ddx) * 1.2f + 40)) continue;
    if (dy && (ddy * dy <= 0 || abs(ddx) > abs(ddy) * 1.2f + 40)) continue;
    float d = (float)(ddx * ddx + ddy * ddy); if (d < bestD) { bestD = d; best = (int)i; }
  }
  if (best >= 0) focus = best;
}

void Panel::handleEvent(const SDL_Event& ev) {
  switch (ev.type) {
    case SDL_QUIT: quit = true; break;
    case SDL_FINGERDOWN: { int i = hit((int)(ev.tfinger.x * W), (int)(ev.tfinger.y * H)); if (i >= 0) { focus = i; drags[(int64_t)ev.tfinger.fingerId] = i; if (ctls[i].kind == Ctl::Slider) ctls[i].act(1000 + (int)(100.0f * ((ev.tfinger.x * W - ctls[i].x) / ctls[i].w))); else press(i); } break; }
    case SDL_FINGERMOTION: { auto it = drags.find((int64_t)ev.tfinger.fingerId); if (it != drags.end() && ctls[it->second].kind == Ctl::Slider) { const Ctl& c = ctls[it->second]; float v = (ev.tfinger.x * W - c.x) / c.w; c.act(1000 + (int)(100.0f * std::max(0.0f, std::min(1.0f, v)))); } break; }
    case SDL_FINGERUP: drags.erase((int64_t)ev.tfinger.fingerId); break;
    case SDL_MOUSEBUTTONDOWN: if (ev.button.which != SDL_TOUCH_MOUSEID) { int i = hit(ev.button.x, ev.button.y); if (i >= 0) { focus = i; drags[-1] = i; if (ctls[i].kind == Ctl::Slider) ctls[i].act(1000 + (int)(100.0f * (ev.button.x - ctls[i].x) / (float)ctls[i].w)); else press(i); } } break;
    case SDL_MOUSEMOTION: if (ev.motion.which != SDL_TOUCH_MOUSEID && (ev.motion.state & SDL_BUTTON_LMASK)) { auto it = drags.find(-1); if (it != drags.end() && ctls[it->second].kind == Ctl::Slider) { const Ctl& c = ctls[it->second]; c.act(1000 + (int)(100.0f * std::max(0.0f, std::min(1.0f, (ev.motion.x - c.x) / (float)c.w)))); } } break;
    case SDL_MOUSEBUTTONUP: drags.erase(-1); break;
    case SDL_CONTROLLERDEVICEADDED: if (!pad) { pad = SDL_GameControllerOpen(ev.cdevice.which); if (pad) fprintf(stderr, "pad: %s\n", SDL_GameControllerName(pad)); } break;
    case SDL_CONTROLLERDEVICEREMOVED: if (pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) { SDL_GameControllerClose(pad); pad = nullptr; } break;
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
      bool down = ev.type == SDL_CONTROLLERBUTTONDOWN;
      switch (ev.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: if (down) press(focus); break;
        case SDL_CONTROLLER_BUTTON_B: if (down) { Command c; c.type = Command::Tap; engine.send(c); } break;
        case SDL_CONTROLLER_BUTTON_X: if (down) { brain.requestPlan(true); say("Asking Gemma for a full plan"); } break;
        case SDL_CONTROLLER_BUTTON_Y: if (down) { Command c; c.type = Command::Mute; c.a = P_VOICE; c.b = -1; engine.send(c); } break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: if (down) brain.nextVoice(-1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) brain.nextVoice(1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: if (down) { Command c; c.type = Command::MicTune; c.a = -1; engine.send(c); } break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: if (down) toggleEars(); break;
        case SDL_CONTROLLER_BUTTON_START: startDown = down; if (down) { if (selectDown) quit = true; else { Command c; c.type = b.playing ? Command::Stop : Command::Play; engine.send(c); } } break;
        case SDL_CONTROLLER_BUTTON_BACK: selectDown = down; if (down) { if (startDown) quit = true; else if (onRepickMic) onRepickMic(); } break;
        case SDL_CONTROLLER_BUTTON_GUIDE: if (down) quit = true; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (down) { if (ctls[focus].kind == Ctl::Slider) ctls[focus].act(-1); else moveFocus(-1, 0); } break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (down) { if (ctls[focus].kind == Ctl::Slider) ctls[focus].act(1); else moveFocus(1, 0); } break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (down) moveFocus(0, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (down) moveFocus(0, 1); break;
      }
      break;
    }
    case SDL_CONTROLLERAXISMOTION: {
      float v = ev.caxis.value / 32767.0f; if (fabsf(v) < 0.15f) v = 0;
      switch (ev.caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTY: stickLY = -v; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: stickRY = -v; break;
        case SDL_CONTROLLER_AXIS_RIGHTX: stickRX = v; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: { static bool was = false; bool now = v > 0.5f; if (now && !was) { Command c; c.type = Command::Energy; c.a = -1; engine.send(c); } was = now; break; }
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: { static bool was = false; bool now = v > 0.5f; if (now && !was) { Command c; c.type = Command::Energy; c.a = 1; engine.send(c); } was = now; break; }
      }
      break;
    }
    case SDL_KEYDOWN: {
      SDL_Keycode k = ev.key.keysym.sym;
      if (k == SDLK_ESCAPE) quit = true; else if (k == SDLK_SPACE) { Command c; c.type = b.playing ? Command::Stop : Command::Play; engine.send(c); }
      else if (k == SDLK_t) { Command c; c.type = Command::Tap; engine.send(c); } else if (k == SDLK_g) { brain.requestPlan(true); say("Asking Gemma for a full plan"); }
      else if (k == SDLK_LEFT) moveFocus(-1, 0); else if (k == SDLK_RIGHT) moveFocus(1, 0); else if (k == SDLK_UP) moveFocus(0, -1); else if (k == SDLK_DOWN) moveFocus(0, 1);
      else if (k == SDLK_RETURN) press(focus); else if (k == SDLK_v) brain.nextVoice(1); else if (k == SDLK_m) { Command c; c.type = Command::MicTune; c.a = -1; engine.send(c); }
      else if (k == SDLK_MINUS) { Command c; c.type = Command::Energy; c.a = -1; engine.send(c); } else if (k == SDLK_EQUALS) { Command c; c.type = Command::Energy; c.a = 1; engine.send(c); }
      break;
    }
  }
}

void Panel::update(float dt) {
  t += dt; if (flash > 0) flash -= dt;
  h = analysis.get(); b = engine.state(); s = brain.state();
  batTimer -= dt; if (batTimer <= 0) { bat = sysaudio::battery(); batTimer = 20.0f; }
  if (stickLY != 0) { volAccum += stickLY * dt; if (fabsf(volAccum) > 0.1f) { Command c; c.type = Command::Volume; c.a = -1; c.f = volAccum > 0 ? 0.05f : -0.05f; engine.send(c); volAccum = 0; } }
  if (stickRY != 0) { Command c; c.type = Command::Volume; c.a = P_VOICE; c.f = stickRY * dt * 0.6f; engine.send(c); }
  if (stickRX != 0) { Command c; c.type = Command::Autotune; c.f = stickRX * dt * 0.5f; engine.send(c); }
}

void Panel::drawCtl(const Ctl& c, bool focused) {
  bool on = c.on ? c.on() : false;
  Color base = c.kind == Ctl::Toggle ? (on ? dim(STEER, 0.35f) : OFF) : (c.id.rfind("steer", 0) == 0 ? Color{30, 44, 30, 255} : OFF);
  gfx.roundRect(c.x, c.y, c.w, c.h, 10, base);
  if (focused) gfx.frame(c.x, c.y, c.w, c.h, STEER, 3);
  if (c.kind == Ctl::Slider && c.h < 44) {   // a compact stepper: label | meter | value on one line
    gfx.text(c.label, c.x + 8, c.y + 9, MUTED, Gfx::F_TINY);
    gfx.meter(c.x + 64, c.y + 12, c.w - 120, 10, c.value ? c.value() : 0, PLAY, {50, 52, 62, 255});
    if (c.text) gfx.text(c.text(), c.x + c.w - 8, c.y + 9, FG, Gfx::F_TINY, 2);
  } else if (c.kind == Ctl::Slider) {
    gfx.text(c.label, c.x + 12, c.y + 6, MUTED, Gfx::F_TINY);
    if (c.text) gfx.text(c.text(), c.x + c.w - 12, c.y + 6, FG, Gfx::F_TINY, 2);
    gfx.meter(c.x + 12, c.y + c.h - 18, c.w - 24, 10, c.value ? c.value() : 0, STEER, {50, 52, 62, 255});
  } else if (c.kind == Ctl::Toggle) {
    gfx.led(c.x + 24, c.y + c.h / 2, 7, on, STEER);
    gfx.text(c.label, c.x + 44, c.y + (c.h - gfx.textHeight(Gfx::F_LABEL)) / 2, on ? FG : MUTED, Gfx::F_LABEL);
  } else {
    bool big = c.h > 80;
    gfx.text(c.label, c.x + c.w / 2, c.y + (c.text ? 8 : (c.h - gfx.textHeight(big ? Gfx::F_BOLD : Gfx::F_LABEL)) / 2), FG, big ? Gfx::F_BOLD : Gfx::F_LABEL, 1);
    if (c.text) { std::string tx = c.text(); if (gfx.textWidth(tx, Gfx::F_TINY) > c.w - 16) tx = tx.substr(0, 22) + "…"; gfx.text(tx, c.x + c.w / 2, c.y + c.h - 26, MUTED, Gfx::F_TINY, 1); }
  }
}

void Panel::drawTop() {
  gfx.rect(0, 0, W, 72, {18, 19, 24, 255});
  gfx.text("AUTOJAM", 28, 14, PLAY, Gfx::F_LOGO);
  gfx.text("Gemma sits in with the band", 240, 30, MUTED, Gfx::F_SMALL);
  // mic
  Color mc = micOk ? HEAR : WARN;
  gfx.text(std::string("mic: ") + micDesc, 560, 16, mc, Gfx::F_SMALL);
  gfx.meter(560, 44, 220, 10, std::min(1.0f, std::max(0.0f, (h.rmsDb + 60) / 60)), mc, OFF);
  // servers
  std::string llm = noServers ? "model: off (--no-llm)" : (s.llmOk ? "model: " + (s.model.empty() ? std::string("ready") : s.model) : (s.llmLoading ? "model: loading…" : "model: not running"));
  gfx.text(llm, 820, 16, s.llmOk ? STEER : MUTED, Gfx::F_SMALL);
  std::string ph = s.phase; if (ph == "writing") { char bb[64]; snprintf(bb, sizeof bb, "Gemma is writing… %d tokens, %.0f s", s.tokens, s.seconds); ph = bb; } else if (ph == "idle" && s.plans) { char bb[96]; snprintf(bb, sizeof bb, "%d plan%s · last %d tokens in %.0f s", s.plans, s.plans == 1 ? "" : "s", s.tokens, s.seconds); ph = bb; }
  gfx.text(ph, 820, 42, ph.rfind("Gemma is", 0) == 0 ? PLAY : MUTED, Gfx::F_TINY);
  gfx.text(std::string("voice: ") + (s.ttsOk ? (s.voice.empty() ? "ready" : s.voice) : "not running"), 1250, 16, s.ttsOk ? VOICE : MUTED, Gfx::F_SMALL);
  gfx.text(std::string("MIDI out: ") + (b.midiOut ? midiOutPort.substr(midiOutPort.find_last_of('/') + 1) : "none"), 1250, 42, b.midiOut ? STEER : MUTED, Gfx::F_TINY);
  // battery
  if (bat.percent >= 0) { gfx.frame(1810, 22, 60, 26, MUTED, 2); gfx.rect(1870, 30, 5, 10, MUTED); gfx.rect(1814, 26, (int)(52 * bat.percent / 100.0f), 18, bat.percent < 20 ? WARN : (bat.charging ? STEER : FG)); gfx.text(std::to_string(bat.percent) + "%", 1800, 28, MUTED, Gfx::F_TINY, 2); }
  if (!s.error.empty()) gfx.text(s.error.substr(0, 90), 1240, 56, WARN, Gfx::F_TINY);
}

void Panel::drawHearing() {
  const int X = 24, CW = 616;
  // KEY
  gfx.card(X, 88, CW, 150, "WHAT GEMMA HEARS · KEY", HEAR);
  gfx.text(h.key >= 0 ? h.keyName() : "listening…", X + 20, 118, h.key >= 0 ? FG : MUTED, Gfx::F_BIG);
  gfx.text("confidence", X + 20, 196, MUTED, Gfx::F_TINY); gfx.meter(X + 110, 200, 220, 10, h.keyConf, HEAR, OFF);
  if (h.key >= 0 && h.chordRoot >= 0) gfx.text(theory::roman(h.chordRoot, h.chordType, h.key, h.minorKey), X + CW - 24, 118, HEAR, Gfx::F_BIG, 2);
  gfx.text(h.silence ? "the room is quiet" : (h.energy > 0.7f ? "loud" : (h.energy > 0.35f ? "playing" : "soft")), X + CW - 24, 196, MUTED, Gfx::F_TINY, 2);
  // CHORD
  gfx.card(X, 250, CW, 220, "CHORD", HEAR);
  std::string cn = h.chordRoot >= 0 ? h.chordName() : "";
  if (h.chordRoot >= 0) gfx.text(cn, X + 20, 268, FG, Gfx::F_HUGE); else gfx.text(h.silence ? "waiting for the room" : "listening…", X + 20, 300, MUTED, Gfx::F_BIG);
  gfx.text(h.chordRoot >= 0 ? theory::chordTypes()[h.chordType].name : "", X + CW - 24, 290, MUTED, Gfx::F_SMALL, 2);
  gfx.meter(X + CW - 244, 322, 220, 10, h.chordConf, HEAR, OFF);
  // history strip
  int hx = X + 20, hy = 412;
  size_t from = h.history.size() > 7 ? h.history.size() - 7 : 0;
  for (size_t i = from; i < h.history.size(); i++) {
    const ChordEvent& e = h.history[i]; std::string nm = theory::chordName(e.root, e.type);
    int w = std::max(56, gfx.textWidth(nm, Gfx::F_SMALL) + 20);
    gfx.roundRect(hx, hy, w, 40, 8, OFF); gfx.text(nm, hx + w / 2, hy + 8, MUTED, Gfx::F_SMALL, 1);
    int beats = h.period > 0 ? (int)lround((e.end - e.start) / h.period) : 0; if (beats > 0) gfx.text(std::to_string(beats), hx + w - 6, hy + 26, dim(MUTED, 0.7f), Gfx::F_TINY, 2);
    hx += w + 8; if (hx > X + CW - 80) break;
  }
  if (h.chordRoot >= 0) { int w = std::max(56, gfx.textWidth(cn, Gfx::F_SMALL) + 20); if (hx + w < X + CW - 10) { gfx.roundRect(hx, hy, w, 40, 8, dim(HEAR, 0.35f)); gfx.text(cn, hx + w / 2, hy + 8, FG, Gfx::F_SMALL, 1); } }
  // BPM
  gfx.card(X, 482, CW, 160, "TEMPO", HEAR);
  char bb[32]; snprintf(bb, sizeof bb, "%d", (int)lroundf(h.bpm)); gfx.text(h.bpm > 0 ? bb : "—", X + 20, 500, h.tempoLocked ? FG : MUTED, Gfx::F_HUGE);
  gfx.text("BPM", X + 20 + gfx.textWidth(h.bpm > 0 ? bb : "—", Gfx::F_HUGE) + 12, 560, MUTED, Gfx::F_LABEL);
  for (int i = 0; i < 4; i++) { bool on = h.lastBeat >= 0 && h.beatInBar == i && (h.time - h.lastBeat) < 0.12; gfx.led(X + 400 + i * 48, 540, 14, on, i == 0 ? PLAY : HEAR); }
  gfx.text(h.tempoLocked ? "locked onto the room" : "listening for a beat", X + 376, 578, h.tempoLocked ? HEAR : MUTED, Gfx::F_TINY);
  gfx.meter(X + 376, 604, 220, 10, h.beatConf, HEAR, OFF);
  gfx.text("bar " + std::to_string(b.bar) + " · band beat " + std::to_string(b.beatInBar + 1) + " · " + b.tempoSource, X + 20, 612, MUTED, Gfx::F_TINY);
  // CHROMA
  gfx.card(X, 654, CW, 190, "PITCH CLASSES", HEAR);
  uint16_t cm = h.chordRoot >= 0 ? theory::chordMask(h.chordRoot, h.chordType) : 0;
  float mx = 0; for (float v : h.chroma) mx = std::max(mx, v); if (mx < 1e-6f) mx = 1;
  for (int i = 0; i < 12; i++) {
    int bx = X + 24 + i * 48; float v = h.chroma[i] / mx; bool tone = cm & (1 << i);
    gfx.meter(bx, 690, 32, 110, v, tone ? HEAR : dim(HEAR, 0.5f), OFF, true);
    gfx.text(theory::NOTE_NAMES[i], bx + 16, 806, tone ? FG : MUTED, Gfx::F_TINY, 1);
    if (h.key == i) gfx.rect(bx, 824, 32, 3, PLAY);
  }
  // LEVEL + MELODY
  gfx.card(X, 856, CW, 140, "INPUT · MELODY", HEAR);
  gfx.text("level", X + 20, 886, MUTED, Gfx::F_TINY); gfx.meter(X + 70, 888, 300, 12, std::min(1.0f, std::max(0.0f, (h.rmsDb + 60) / 60)), h.peakDb > -3 ? WARN : HEAR, OFF);
  snprintf(bb, sizeof bb, "%.0f dBFS", h.rmsDb); gfx.text(bb, X + 380, 884, MUTED, Gfx::F_TINY);
  std::string mel = h.pitchConf > 0.5f ? theory::midiName((int)lroundf(h.pitchMidi)) : "—";
  gfx.text("melody heard", X + 20, 924, MUTED, Gfx::F_TINY); gfx.text(mel, X + 140, 914, h.pitchConf > 0.5f ? FG : MUTED, Gfx::F_BOLD);
  if (h.pitchConf > 0.5f) { float cents = (h.pitchMidi - lroundf(h.pitchMidi)) * 100; snprintf(bb, sizeof bb, "%+.0f cents", cents); gfx.text(bb, X + 230, 924, MUTED, Gfx::F_TINY); }
  gfx.text(micSource.empty() ? "" : micSource.substr(0, 46), X + CW - 24, 964, dim(MUTED, 0.8f), Gfx::F_TINY, 2);
}

void Panel::drawBand() {
  const int X = 656, CW = 616;
  // PLAN
  gfx.card(X, 88, CW, 236, "WHAT GEMMA PLAYS · PLAN", PLAY);
  const Plan& p = b.plan;
  std::string title = p.title.empty() ? (b.plansApplied ? "untitled phrase" : "no plan yet: the band follows the room") : p.title;
  gfx.text(title, X + 20, 112, FG, Gfx::F_BOLD);
  int ly = 150;
  if (!p.direction.empty()) ly += gfx.wrapped("“" + p.direction + "”", X + 20, ly, CW - 40, PLAY, Gfx::F_SMALL, 2) * gfx.textHeight(Gfx::F_SMALL) + 6;
  std::string hear = s.lastHear.empty() ? (p.hearGenre.empty() ? "" : p.hearGenre + " · " + p.hearMood + " · " + p.hearInstruments) : s.lastHear;
  if (!hear.empty()) { gfx.text("Gemma hears:", X + 20, ly, MUTED, Gfx::F_TINY); gfx.wrapped(hear, X + 120, ly, CW - 140, FG, Gfx::F_TINY, 2); ly += 44; }
  // chips
  int cx = X + 20; auto chip = [&](const std::string& t, Color c) { int w = gfx.textWidth(t, Gfx::F_TINY) + 20; gfx.roundRect(cx, 278, w, 30, 8, dim(c, 0.3f)); gfx.text(t, cx + 10, 284, FG, Gfx::F_TINY); cx += w + 8; };
  chip(p.style, PLAY); chip("drums: " + p.feel, PLAY); chip("energy " + std::to_string(p.energy), PLAY); if (p.swing) chip("swing " + std::to_string(p.swing) + "%", PLAY); chip(b.following ? "follows the room" : "own chords", HEAR);
  if (!p.motif.empty()) { std::string m = "motif"; for (int d : p.motif) m += " " + std::to_string(d); gfx.text(m, X + CW - 24, 284, MUTED, Gfx::F_TINY, 2); }
  if (s.phase == "writing" || s.phase.rfind("listening", 0) == 0) { gfx.rect(X + 4, 88 + 12, 4, 212, PLAY); char bb[64]; snprintf(bb, sizeof bb, "%s · %d tokens · %.0f s", s.phase.c_str(), s.tokens, s.seconds); gfx.text(bb, X + CW - 24, 112, PLAY, Gfx::F_TINY, 2); }
  else if (s.nextAutoIn >= 0 && b.playing && s.llmOk) { char bb[48]; snprintf(bb, sizeof bb, "next plan in %d s", (int)s.nextAutoIn); gfx.text(bb, X + CW - 24, 112, MUTED, Gfx::F_TINY, 2); }
  // BAND
  gfx.card(X, 336, CW, 290, "THE BAND", PLAY);
  // step LEDs
  for (int i = 0; i < 16; i++) { bool on = b.playing && b.step == i; gfx.led(X + 24 + i * 36, 376, 7, on, i % 4 == 0 ? PLAY : dim(PLAY, 0.8f)); }
  gfx.text(b.playing ? "playing" : "stopped (Start)", X + CW - 24, 364, b.playing ? PLAY : MUTED, Gfx::F_TINY, 2);
  for (int pi = 0; pi < NUM_PARTS; pi++) {
    int y = 390 + pi * 46;
    gfx.text(PART_NAMES[pi], X + 24, y + 6, b.partMute[pi] ? dim(MUTED, 0.6f) : FG, Gfx::F_LABEL);
    gfx.meter(X + 120, y + 10, 220, 14, b.partLevel[pi], pi == P_VOICE ? VOICE : PLAY, OFF);
    if (b.partMute[pi]) gfx.text("muted", X + 350, y + 8, WARN, Gfx::F_TINY);
  }
  std::string chord = b.chordRoot >= 0 ? theory::chordName(b.chordRoot, b.chordType) : "—";
  gfx.text(b.chordRoot >= 0 ? "band plays " + chord + (b.key >= 0 ? " in " + std::string(theory::NOTE_NAMES[b.key]) + (b.mode == 1 ? " minor" : " major") : "") : (b.playing ? "band waits for the room (or for chords from Gemma)" : "band stopped"), X + 24, 604, PLAY, Gfx::F_TINY);
  char bb[48]; snprintf(bb, sizeof bb, "%d BPM", (int)lroundf(b.bpm)); gfx.text(bb, X + CW - 24, 602, FG, Gfx::F_SMALL, 2);
  // LYRICS
  gfx.card(X, 638, CW, 300, "LYRICS · VOICE", VOICE);
  if (!b.lyricNow.empty()) {
    int lines = gfx.wrapped(b.lyricNow, X + 20, 672, CW - 40, FG, Gfx::F_BOLD, 2);
    int uy = 672 + lines * gfx.textHeight(Gfx::F_BOLD) + 6; gfx.meter(X + 20, uy, CW - 40, 6, b.lyricProgress, VOICE, OFF);
    if (b.lyricNote >= 0) gfx.text("singing " + theory::midiName(b.lyricNote), X + CW - 24, 646, VOICE, Gfx::F_TINY, 2);
  } else gfx.text(s.ttsOk ? (b.linesQueued ? "next line starts on the bar…" : "no lyrics queued · New lyrics asks Gemma") : "Pocket TTS is not running", X + 20, 672, MUTED, Gfx::F_SMALL);
  if (!b.lyricNext.empty()) { gfx.text("next:", X + 20, 770, MUTED, Gfx::F_TINY); gfx.wrapped(b.lyricNext, X + 70, 768, CW - 90, dim(FG, 0.7f), Gfx::F_SMALL, 2); }
  if (b.linesQueued > 1) gfx.text("+" + std::to_string(b.linesQueued - 1) + " more", X + CW - 24, 770, MUTED, Gfx::F_TINY, 2);
  // melody of the current plan's lyrics, as dots
  if (!p.lyrics.empty()) {
    int my = 836; gfx.text("melodies", X + 20, my, MUTED, Gfx::F_TINY); int mx = X + 100;
    for (auto& l : p.lyrics) { for (size_t i = 0; i < l.melody.size(); i++) { int d = l.melody[i]; gfx.circle(mx, my + 20 - d * 2, 3, VOICE); mx += 10; } mx += 18; if (mx > X + CW - 40) break; }
  }
  if (!s.lastSay.empty()) { gfx.text("Gemma said:", X + 20, 876, MUTED, Gfx::F_TINY); gfx.wrapped("“" + s.lastSay + "”", X + 110, 874, CW - 130, dim(VOICE, 0.9f), Gfx::F_TINY, 2); }
  snprintf(bb, sizeof bb, "tuner %s → %s", b.voiceDetected > 0 ? theory::midiName((int)lroundf(b.voiceDetected)).c_str() : "—", b.voiceTarget > 0 ? theory::midiName((int)lroundf(b.voiceTarget)).c_str() : "—");
  gfx.text(bb, X + CW - 24, 912, MUTED, Gfx::F_TINY, 2);
  // small status
  gfx.text(std::string("out: ") + (sink.empty() ? "default" : sink) + (b.micTune ? "  ·  mic through the tuner (headphones!)" : ""), X + 4, 952, MUTED, Gfx::F_TINY);
  if (!s.steering.empty()) { std::string st = "asked: "; for (size_t i = 0; i < s.steering.size(); i++) st += (i ? "; " : "") + s.steering[i].substr(0, 28); gfx.text(st.substr(0, 90), X + 4, 976, dim(STEER, 0.9f), Gfx::F_TINY); }
}

void Panel::drawSteer() {
  gfx.text("STEER GEMMA", 1296, 92, STEER, Gfx::F_SMALL);
  for (size_t i = 0; i < ctls.size(); i++) drawCtl(ctls[i], (int)i == focus);
}

void Panel::drawBottom() {
  gfx.rect(0, H - 56, W, 56, {18, 19, 24, 255});
  std::string hints = "A press · B tap tempo · X ask Gemma · Y Gemma's voice · L1/R1 voice · L2/R2 energy · L3 tune mic · R3 ears · Start band · Select mic · Select+Start quit";
  gfx.text(hints, W / 2, H - 38, MUTED, Gfx::F_TINY, 1);
  if (flash > 0 && !flashMsg.empty()) { int w = gfx.textWidth(flashMsg, Gfx::F_LABEL) + 40; gfx.roundRect(W / 2 - w / 2, H - 120, w, 48, 12, {40, 60, 40, 240}); gfx.text(flashMsg, W / 2, H - 110, FG, Gfx::F_LABEL, 1); }
  else if (!hint.empty()) gfx.text(hint, W / 2, H - 100, PLAY, Gfx::F_SMALL, 1);
}

void Panel::draw() {
  gfx.newFrame();
  gfx.rect(0, 0, W, H, BG);
  drawTop(); drawHearing(); drawBand(); drawSteer(); drawBottom();
}
