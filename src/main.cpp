// autojam: Gemma sits in with a human jam on the Retroid Pocket Flip 2 (ROCKNIX). Entry point.
//   autojam                          full screen on the device
//   autojam --window                 a 1920x1080 window (desktop testing)
//   autojam --wav FILE               a WAV stands in for the microphone (looped, real time)
//   autojam --analyze FILE           no window, no audio device: print what the listener hears in FILE and exit
//   autojam --screenshot out.bmp     draw one frame offscreen and save it (with --wav: after hearing the file)
//   autojam --wav in.wav --render out.wav --seconds N    no window, no audio device: the band plays along to in.wav
//   autojam --headless N             no window: run N seconds (mic or --wav), ask Gemma, print what happens
//   autojam --sing "TEXT" --out F.wav [--melody 0,2,4 --key Am --bpm 96 --voice NAME]   Pocket TTS speaks it, the
//                                    singer sings it: F.wav sung, F.wav.speech.wav as spoken (no window, no audio)
//   --no-llm                         no model and voice servers (the band follows the room on its own)
//   --llm URL --tts URL --source PULSE_SOURCE --sink PULSE_SINK --voice NAME --ears --midi PATH|auto|none --auto SECONDS
//   --link PORT|off                  follow Rap Machine's jam-link beacon on the LAN (default UDP 47800)
// Env: AUTOJAM_ASSETS, AUTOJAM_DATA (settings), AUTOJAM_LLM, AUTOJAM_TTS, AUTOJAM_SOURCE, AUTOJAM_EARS=1, AUTOJAM_MIDI, AUTOJAM_AUTO, AUTOJAM_LINK
#include <SDL.h>
#include "analysis.h"
#include "player.h"
#include "brain.h"
#include "mic.h"
#include "gfx.h"
#include "ui.h"
#include "midi.h"
#include "sysaudio.h"
#include "json.h"
#include "http.h"
#include "singer.h"
#include "link.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static Engine* gEngine = nullptr;
static bool gAudioThreadSet = false;
static void audioCallback(void*, Uint8* stream, int len) {
  if (!gAudioThreadSet) {
    gAudioThreadSet = true;
    sched_param sp{}; sp.sched_priority = 5; pthread_setschedparam(pthread_self(), SCHED_RR, &sp);   // allowed as root (ROCKNIX); ignored elsewhere
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n >= 8) { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(7, &cs); pthread_setaffinity_np(pthread_self(), sizeof cs, &cs); }   // SM8250: the prime core
  }
  gEngine->render((float*)stream, len / (int)(sizeof(float) * 2));
}

static std::string envOr(const char* k, const std::string& d) { const char* v = getenv(k); return v && *v ? v : d; }
static std::string dataDir() { std::string d = envOr("AUTOJAM_DATA", ""); if (d.empty()) d = envOr("HOME", ".") + "/.config/autojam"; return d; }
static std::string assetDir() { std::string d = envOr("AUTOJAM_ASSETS", ""); if (!d.empty()) return d; char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1); if (n > 0) { buf[n] = 0; std::string p(buf); size_t s = p.find_last_of('/'); if (s != std::string::npos) { std::string base = p.substr(0, s); std::ifstream t(base + "/assets/fonts/LiberationSans-Regular.ttf"); if (t.good()) return base + "/assets"; std::ifstream t2(base + "/../assets/fonts/LiberationSans-Regular.ttf"); if (t2.good()) return base + "/../assets"; } } return "assets"; }

static bool writeWavStereo(const std::string& path, const std::vector<float>& inter, int rate) {
  FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
  uint32_t n = (uint32_t)inter.size(); uint32_t dataBytes = n * 2; uint32_t riff = 36 + dataBytes;
  uint16_t ch = 2, bits = 16, blockAlign = 4; uint32_t byteRate = rate * blockAlign; uint32_t fmtLen = 16; uint16_t fmt = 1;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
  for (float v : inter) { int16_t s = (int16_t)lroundf(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f); fwrite(&s, 2, 1, f); }
  fclose(f); return true;
}

static int singTest(const std::string& tts, const std::string& voice, const std::string& text, const std::string& melody, const std::string& key, float bpm, const std::string& outPath) {
  json::Value body = json::Value::obj(); body["input"] = json::Value::str(text); if (!voice.empty()) body["voice"] = json::Value::str(voice); body["response_format"] = json::Value::str("pcm");
  http::Response r;
  if (!http::Client::request(tts, "POST", "/v1/audio/speech", json::dump(body), r, 3000) || r.status != 200) { fprintf(stderr, "tts: %s\n", r.error.empty() ? ("http " + std::to_string(r.status)).c_str() : r.error.c_str()); return 1; }
  std::string pcm; for (;;) { std::string p = r.read(60000); if (p.empty()) break; pcm += p; }
  size_t n = pcm.size() / 2; std::vector<int16_t> s16(n); memcpy(s16.data(), pcm.data(), n * 2);
  size_t a = 0, b = n; while (a < n && abs(s16[a]) < 180) a++; while (b > a && abs(s16[b - 1]) < 180) b--;   // as the speaker thread trims
  a = a > 1440 ? a - 1440 : 0; b = std::min(n, b + 1440);
  SingRequest rq; rq.bpm = bpm; size_t best = 0;
  for (int i = 0; i < 12; i++) { size_t l = strlen(theory::NOTE_NAMES[i]); if (key.compare(0, l, theory::NOTE_NAMES[i]) == 0 && l > best) { best = l; rq.tonicPc = i; } }
  if (key.size() > best && key[best] == 'm') rq.mode = std::max(0, theory::modeIndex("aeolian"));
  for (size_t p = 0; p < melody.size();) { size_t q = melody.find(',', p); if (q == std::string::npos) q = melody.size(); rq.melody.push_back(atoi(melody.substr(p, q - p).c_str())); p = q + 1; }
  Sung sung; if (b <= a || !sing(&s16[a], (int)(b - a), rq, sung)) { fprintf(stderr, "no syllables found\n"); return 1; }
  auto stereo = [](const std::vector<float>& m) { std::vector<float> o(m.size() * 2); for (size_t i = 0; i < m.size(); i++) o[2 * i] = o[2 * i + 1] = m[i]; return o; };
  std::vector<float> spoken; dsp::upsample2x(&s16[a], (int)(b - a), spoken);
  bool ok = writeWavStereo(outPath, stereo(sung.pcm), 48000) && writeWavStereo(outPath + ".speech.wav", stereo(spoken), 48000);
  printf("%s: %.2f s from TTS, %d syllables, %.2f s spoken -> %.2f s sung at %.0f BPM, notes", outPath.c_str(), n / 24000.0, sung.syllables, spoken.size() / 48000.0, sung.pcm.size() / 48000.0, bpm);
  for (auto& nt : sung.notes) printf(" %s@%.2f", theory::midiName(nt.second).c_str(), nt.first / 48000.0);
  printf("\n"); return ok ? 0 : 1;
}

static int analyze(const std::string& path) {
  Analysis an; std::vector<float> data; std::string err;
  if (!readWavMono48k(path, data, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  printf("%s: %.1f s\n  time   bpm conf lock  key          chord  conf  level\n", path.c_str(), data.size() / 48000.0);
  size_t pos = 0; int sec = 0; std::map<std::string, int> keys; std::vector<float> bpms; std::vector<std::string> chords; std::string lastChord;
  while (pos < data.size()) {
    size_t n = std::min<size_t>(4800, data.size() - pos); an.feed(&data[pos], (int)n); pos += n;
    if (pos / 48000 > (size_t)sec) {
      sec = (int)(pos / 48000); Hearing h = an.get();
      printf("  %4d  %5.1f %4.2f %s  %-12s %-6s %4.2f  %5.1f dB\n", sec, h.bpm, h.beatConf, h.tempoLocked ? "lock" : "    ", h.keyName().c_str(), h.chordName().c_str(), h.chordConf, h.rmsDb);
      if (sec > 3) { if (h.key >= 0) keys[h.keyName()]++; if (h.tempoLocked) bpms.push_back(h.bpm); }
      if (h.chordRoot >= 0 && h.chordName() != lastChord) { lastChord = h.chordName(); chords.push_back(lastChord); }
    }
  }
  std::string bestKey; int bestN = 0; for (auto& kv : keys) if (kv.second > bestN) { bestN = kv.second; bestKey = kv.first; }
  std::sort(bpms.begin(), bpms.end()); float med = bpms.empty() ? 0 : bpms[bpms.size() / 2];
  printf("summary: key %s (%d of %d s), tempo %.1f BPM (median of locked seconds), chords:", bestKey.c_str(), bestN, std::max(0, sec - 3), med);
  for (auto& c : chords) printf(" %s", c.c_str()); printf("\n");
  return 0;
}

int main(int argc, char** argv) {
  std::string wav, analyzePath, shot, llm = envOr("AUTOJAM_LLM", "http://127.0.0.1:8103"), tts = envOr("AUTOJAM_TTS", "http://127.0.0.1:8104");
  std::string source = envOr("AUTOJAM_SOURCE", ""), sinkName, voice = envOr("AUTOJAM_VOICE", ""), midiPath = envOr("AUTOJAM_MIDI", "auto"), renderPath;
  bool windowed = false, noLlm = false, ears = envOr("AUTOJAM_EARS", "0") == "1"; int autoSec = atoi(envOr("AUTOJAM_AUTO", "120").c_str()); float seconds = 24; int headless = 0;
  std::string linkPort = envOr("AUTOJAM_LINK", "47800");
  std::string singText, singOut = "sung.wav", singMelody = "0,2,4,2,0", singKey = "C"; float singBpm = 96;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i]; auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--window") windowed = true; else if (a == "--wav") wav = next(); else if (a == "--analyze") analyzePath = next(); else if (a == "--screenshot") shot = next();
    else if (a == "--no-llm") noLlm = true; else if (a == "--llm") llm = next(); else if (a == "--tts") tts = next(); else if (a == "--source") source = next(); else if (a == "--sink") sinkName = next();
    else if (a == "--render") renderPath = next(); else if (a == "--seconds") seconds = (float)atof(next().c_str()); else if (a == "--headless") headless = atoi(next().c_str());
    else if (a == "--voice") voice = next(); else if (a == "--ears") ears = true; else if (a == "--midi") midiPath = next(); else if (a == "--auto") autoSec = atoi(next().c_str());
    else if (a == "--link") linkPort = next();
    else if (a == "--sing") singText = next(); else if (a == "--out") singOut = next(); else if (a == "--melody") singMelody = next(); else if (a == "--key") singKey = next(); else if (a == "--bpm") singBpm = (float)atof(next().c_str());
    else if (a == "--help" || a == "-h") { printf("autojam [--window] [--wav FILE] [--analyze FILE] [--screenshot out.bmp] [--no-llm] [--llm URL] [--tts URL] [--source NAME] [--sink NAME] [--voice NAME] [--ears] [--midi PATH|auto|none] [--auto SECONDS]\n"); return 0; }
  }
  if (!analyzePath.empty()) return analyze(analyzePath);
  if (!singText.empty()) return singTest(tts, voice, singText, singMelody, singKey, singBpm, singOut);

  // settings
  json::Value settings; { std::ifstream f(dataDir() + "/settings.json"); std::stringstream ss; ss << f.rdbuf(); json::parse(ss.str(), settings); }
  if (voice.empty()) voice = settings.strOr("voice", "");

  Analysis analysis; Engine engine; engine.init(&analysis); gEngine = &engine;
  if (!renderPath.empty()) {   // offline: the band plays along to the file, output to a stereo WAV
    if (wav.empty()) { fprintf(stderr, "--render needs --wav FILE (the room)\n"); return 1; }
    std::vector<float> in; std::string err; if (!readWavMono48k(wav, in, err) || in.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    int frames = (int)(seconds * 48000); std::vector<float> out((size_t)frames * 2); float blk[256];
    Command c; c.type = Command::Play; engine.send(c);
    float pk = 0;
    for (int done = 0; done < frames; done += 256) {
      int n = std::min(256, frames - done);
      for (int i = 0; i < n; i++) blk[i] = in[(size_t)(done + i) % in.size()];
      analysis.feed(blk, n); engine.micIn(blk, n); engine.render(&out[(size_t)done * 2], n);
      for (int i = 0; i < n * 2; i++) pk = std::max(pk, fabsf(out[(size_t)done * 2 + i]));
    }
    bool ok = writeWavStereo(renderPath, out, 48000); BandState b = engine.state();
    printf("%s %s (%.1f s, peak %.1f dBFS, band ended on %s at %d BPM, bar %d)\n", ok ? "wrote" : "FAILED", renderPath.c_str(), seconds, 20 * log10f(pk + 1e-9f), b.chordRoot >= 0 ? theory::chordName(b.chordRoot, b.chordType).c_str() : "nothing", (int)lroundf(b.bpm), b.bar);
    return ok ? 0 : 1;
  }
  JamLink jamLink;
  if (linkPort != "off" && atoi(linkPort.c_str()) > 0) {
    if (jamLink.start(atoi(linkPort.c_str()), [&](const LinkInfo& l) { analysis.link(l); })) fprintf(stderr, "jam link: listening on UDP %s\n", linkPort.c_str());
    else fprintf(stderr, "jam link: UDP %s is taken, not following Rap Machine\n", linkPort.c_str());
  }
  Brain brain; Brain::Options bo; bo.llmUrl = llm; bo.ttsUrl = tts; bo.ears = ears; bo.autoEverySec = autoSec; bo.enabled = !noLlm; bo.voice = voice;
  brain.start(bo, &engine, &analysis);
  MidiOut midiOut; MidiIn midiIn;
  auto openMidi = [&]() {
    midiOut.close(); midiIn.close();
    if (midiPath == "none") return;
    std::string p = midiPath;
    if (p == "auto") { auto ports = sysaudio::rawMidiPorts(); p = ports.empty() ? "" : ports[0]; }
    if (p.empty()) return;
    if (midiOut.open(p)) fprintf(stderr, "midi out: %s\n", p.c_str());
    midiIn.open(p, [&](int note, int vel, bool on) { static uint16_t held = 0; if (on) held |= 1 << (note % 12); else held &= ~(1 << (note % 12)); analysis.boostNotes(held, held ? 0.6f : 0.0f); (void)vel; },
                [&](int cc, int v) { if (cc == 1) { static int last = -1; int e = v / 32; if (last >= 0 && e != last) { Command c; c.type = Command::Energy; c.a = e > last ? 1 : -1; engine.send(c); } last = e; } });
  };
  openMidi(); engine.setMidiOut(&midiOut);

  if (headless > 0) {   // no screen: the band and Gemma for N seconds, a log on stdout
    MicCapture mic; auto sink = [&](const float* mono, int n) { analysis.feed(mono, n); engine.micIn(mono, n); brain.hear(mono, n); };
    if (!wav.empty()) { if (!mic.startWav(wav, sink, true, true)) fprintf(stderr, "%s\n", mic.error.c_str()); }
    else { sysaudio::MicChoice m = sysaudio::pickMic(source); if (m.source.empty()) printf("mic: %s\n", m.desc.c_str()); else if (!mic.start(m.source, sink)) fprintf(stderr, "%s\n", mic.error.c_str()); else printf("mic: %s (%s)\n", m.desc.c_str(), m.source.c_str()); }
    Command c; c.type = Command::Play; engine.send(c);
    std::vector<float> buf(512 * 2); auto t0 = std::chrono::steady_clock::now(); long long rendered = 0; int lastPlans = 0, lastPrint = -1; std::string lastPhase, lastLyric; bool asked = false; float pk = 0;
    for (;;) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (el >= headless) break;
      engine.render(buf.data(), 512); rendered += 512; for (float v : buf) pk = std::max(pk, fabsf(v));
      std::this_thread::sleep_until(t0 + std::chrono::microseconds(rendered * 1000000LL / 48000));
      BrainState s = brain.state(); BandState b = engine.state(); Hearing h = analysis.get();
      if (!asked && el > 6 && !noLlm) { brain.requestPlan(true); asked = true; printf("[%5.1f] asked Gemma for a full plan (model %s, llm %s, tts %s, %zu voices)\n", el, s.model.c_str(), s.llmOk ? "ok" : "down", s.ttsOk ? "ok" : "down", s.voices.size()); }
      if (s.phase != lastPhase) { printf("[%5.1f] gemma: %s%s\n", el, s.phase.c_str(), s.error.empty() ? "" : (" (" + s.error + ")").c_str()); lastPhase = s.phase; }
      if ((int)el / 5 != lastPrint) { lastPrint = (int)el / 5; printf("[%5.1f] hear %s %s %d BPM %s | band %s | gemma %s %d tok %.0f s | tts queue %d, peak %.1f dB\n", el, h.keyName().c_str(), h.chordName().c_str(), (int)lroundf(h.bpm), h.linked ? ("linked to " + h.linkFrom).c_str() : (h.tempoLocked ? "locked" : ""), engine.describePlaying().c_str(), s.phase.c_str(), s.tokens, s.seconds, s.ttsQueued, 20 * log10f(pk + 1e-9f)); pk = 0; }
      if (b.plansApplied != lastPlans) { lastPlans = b.plansApplied; const Plan& p = b.plan; printf("[%5.1f] PLAN \"%s\": %s | style %s, feel %s, energy %d, parts %d/%d/%d/%d, motif", el, p.title.c_str(), p.direction.c_str(), p.style.c_str(), p.feel.c_str(), p.energy, p.drums, p.bass, p.keys, p.lead); for (int d : p.motif) printf(" %d", d); printf(", %zu chords, follow %d, swing %d, voice %s\n", p.chords.size(), p.follow, p.swing, p.voice.c_str()); printf("        hears: %s · %s · %s\n        say: %s\n", p.hearGenre.c_str(), p.hearMood.c_str(), p.hearInstruments.c_str(), p.say.c_str()); for (auto& l : p.lyrics) { printf("        lyric: \"%s\" melody", l.text.c_str()); for (int d : l.melody) printf(" %d", d); printf("\n"); } }
      if (b.lyricNow != lastLyric) { lastLyric = b.lyricNow; if (!lastLyric.empty()) printf("[%5.1f] singing: %s\n", el, lastLyric.c_str()); }
    }
    mic.stop(); brain.stop(); midiIn.close(); midiOut.close();
    std::string raw = brain.lastPlanJson(); if (!raw.empty()) printf("last plan JSON: %s\n", raw.c_str());
    return 0;
  }

  bool offscreen = !shot.empty();
  if (offscreen) SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0"); SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | (offscreen ? 0 : SDL_INIT_AUDIO)) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  Uint32 flags = offscreen ? SDL_WINDOW_HIDDEN : (windowed ? SDL_WINDOW_SHOWN : SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_Window* win = SDL_CreateWindow("autojam", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920, 1080, flags);
  if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, offscreen ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
  SDL_RenderSetLogicalSize(ren, 1920, 1080); SDL_ShowCursor(windowed ? SDL_ENABLE : SDL_DISABLE);
  Gfx gfx; if (!gfx.init(ren, assetDir() + "/fonts")) return 1;
  Panel panel(gfx, analysis, engine, brain); panel.init(); panel.noServers = noLlm;

  // the microphone (or the WAV standing in for it)
  MicCapture mic;
  auto sink = [&](const float* mono, int n) { analysis.feed(mono, n); engine.micIn(mono, n); brain.hear(mono, n); };
  auto pickMic = [&]() {
    if (!wav.empty()) { panel.micOk = mic.startWav(wav, sink, true, true); panel.micDesc = panel.micOk ? "WAV file" : mic.error; panel.micSource = wav; return; }
    sysaudio::MicChoice m = sysaudio::pickMic(source);
    if (m.source.empty()) { mic.stop(); panel.micOk = false; panel.micDesc = m.desc; panel.micSource = ""; return; }
    panel.micOk = mic.start(m.source, sink); panel.micDesc = panel.micOk ? m.desc : mic.error; panel.micSource = m.source;
    if (!panel.micOk) fprintf(stderr, "%s\n", mic.error.c_str());
  };
  if (!offscreen) pickMic();
  else if (!wav.empty()) {   // a screenshot of what the listener and the band make of a file: feed it all first
    std::vector<float> data; std::string err;
    if (readWavMono48k(wav, data, err)) { for (size_t p = 0; p < data.size(); p += 4800) sink(&data[p], (int)std::min<size_t>(4800, data.size() - p)); panel.micOk = true; panel.micDesc = "WAV file"; panel.micSource = wav; }
    else fprintf(stderr, "%s\n", err.c_str());
  }
  panel.onRepickMic = [&]() { source.clear(); pickMic(); };
  std::vector<sysaudio::Source> sinks; int sinkIdx = -1;
  auto showSink = [&]() { std::string d = sysaudio::defaultSink(); panel.sink = d.empty() ? "" : (d.find("Speaker") != std::string::npos ? "Speaker" : d.find("Headphones") != std::string::npos ? "Headphones" : d.find("usb") != std::string::npos || d.find("USB") != std::string::npos ? "USB" : d.substr(0, 24)); };
  panel.onNextSink = [&]() { sinks = sysaudio::listSinks(); if (sinks.empty()) return; sinkIdx = (sinkIdx + 1) % (int)sinks.size(); sysaudio::setDefaultSink(sinks[sinkIdx].name); showSink(); };
  if (!sinkName.empty()) sysaudio::setDefaultSink(sinkName);
  if (!offscreen) showSink();
  panel.onRescanMidi = [&]() { auto ports = sysaudio::rawMidiPorts(); if (ports.empty()) { midiPath = "auto"; openMidi(); panel.midiOutPort = ""; return; } int cur = -1; for (size_t i = 0; i < ports.size(); i++) if (ports[i] == midiOut.path) cur = (int)i; midiPath = ports[(cur + 1) % ports.size()]; openMidi(); panel.midiOutPort = midiOut.isOpen() ? midiOut.path : ""; };
  panel.midiOutPort = midiOut.isOpen() ? midiOut.path : "";

  SDL_AudioDeviceID dev = 0;
  if (!offscreen) {
    SDL_AudioSpec want{}, have{}; want.freq = 48000; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 256; want.callback = audioCallback;
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return 1; }
    fprintf(stderr, "audio: %d Hz, %d frames per callback, driver %s\n", have.freq, have.samples, SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(dev, 0);
  }
  { Command c; c.type = Command::Volume; c.a = -1; c.f = (float)settings.numOr("master", 0.8) - 0.8f; engine.send(c); }
  if (settings.boolOr("play", true)) { Command c; c.type = Command::Play; engine.send(c); }

  Uint64 last = SDL_GetPerformanceCounter(); bool running = true;
  while (running) {
    SDL_Event ev; while (SDL_PollEvent(&ev)) panel.handleEvent(ev);
    Uint64 now = SDL_GetPerformanceCounter(); float dt = (float)(now - last) / (float)SDL_GetPerformanceFrequency(); last = now; if (dt > 0.1f) dt = 0.1f;
    if (offscreen) { static float dummy[512 * 2]; for (int i = 0; i < 8; i++) engine.render(dummy, 512); }
    panel.update(dt);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    panel.draw();
    if (offscreen) { SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1080, 32, SDL_PIXELFORMAT_ARGB8888); SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch); SDL_SaveBMP(s, shot.c_str()); SDL_FreeSurface(s); printf("wrote %s\n", shot.c_str()); running = false; }
    else SDL_RenderPresent(ren);
    if (panel.wantsQuit()) running = false;
  }
  if (dev) { SDL_PauseAudioDevice(dev, 1); SDL_CloseAudioDevice(dev); }
  mic.stop(); brain.stop(); midiIn.close(); midiOut.close();
  { BandState b = engine.state(); BrainState s = brain.state(); json::Value out = json::Value::obj(); out["master"] = json::Value::num(b.master); out["voice"] = json::Value::str(s.voice); out["play"] = json::Value::boolean(true);
    std::string d = dataDir(); std::string cmd = "mkdir -p '" + d + "'"; if (system(cmd.c_str()) == 0) { std::ofstream f(d + "/settings.json"); f << json::dump(out); } }
  gfx.shutdown(); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
