#include "brain.h"
#include "json.h"
#include "http.h"
#include "singer.h"
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cmath>

static double nowS() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }

void Brain::start(const Options& o, Engine* e, Analysis* a) {
  opts = o; engine = e; analysis = a; earRing.assign(48000 * 12, 0.0f);
  { std::lock_guard<std::mutex> g(mu); st.ears = o.ears; st.voice = o.voice; }
  if (!opts.enabled) { std::lock_guard<std::mutex> g(mu); st.phase = "off (no servers)"; return; }
  run = true; lastAuto = nowS();
  planner = std::thread([this] { plannerLoop(); });
  speaker = std::thread([this] { speakerLoop(); });
}
void Brain::stop() { run = false; cv.notify_all(); scv.notify_all(); if (planner.joinable()) planner.join(); if (speaker.joinable()) speaker.join(); }

void Brain::hear(const float* mono, int n) {
  size_t w = earW.load(std::memory_order_relaxed);
  for (int i = 0; i < n; i++) earRing[w % earRing.size()] = mono[i], w++;
  earW.store(w, std::memory_order_release);
}

void Brain::steer(const std::string& what) {
  { std::lock_guard<std::mutex> g(jobMu); pendingSteer.push_back(what); steerDue = nowS() + 1.5; }
  { std::lock_guard<std::mutex> g(mu); st.steering.push_back(what); if (st.steering.size() > 6) st.steering.erase(st.steering.begin()); }
  cv.notify_all();
}
void Brain::requestPlan(bool full) {
  std::lock_guard<std::mutex> g(jobMu);
  Job j{full, pendingSteer}; pendingSteer.clear(); steerDue = 0;
  if (jobs.size() < 2) jobs.push_back(j);
  cv.notify_all();
}
void Brain::nextVoice(int delta) {
  std::lock_guard<std::mutex> g(mu);
  if (st.voices.empty()) return;
  st.voiceIdx = ((st.voiceIdx + delta) % (int)st.voices.size() + (int)st.voices.size()) % (int)st.voices.size();
  st.voice = st.voices[st.voiceIdx];
}
void Brain::speak(const std::string& text, const std::vector<int>& melody, bool sung, int verse, int of) {
  { std::lock_guard<std::mutex> g(jobMu); says.push_back({text, melody, sung, verse, of}); }
  { std::lock_guard<std::mutex> g(mu); st.ttsQueued = (int)says.size(); }
  scv.notify_all();
}

void Brain::health() {
  std::string body; bool llm = false, loading = false, tts = false, hearKnown = false, canHear = true; std::string model;
  http::Response r;
  if (http::Client::request(opts.llmUrl, "GET", "/health", "", r, 800)) { body = r.all(2000); llm = r.status == 200; loading = r.status == 503; }
  if (llm) {
    std::string m; if (http::Client::get(opts.llmUrl, "/v1/models", m, 2000)) { json::Value v; if (json::parse(m, v)) { auto& d = v.arrOf("data"); if (!d.empty()) { model = d[0].strOr("id", ""); size_t s = model.find_last_of('/'); if (s != std::string::npos) model = model.substr(s + 1); } } }
    std::string p; if (http::Client::get(opts.llmUrl, "/props", p, 2000)) { json::Value v; if (json::parse(p, v)) if (const json::Value* m = v.get("modalities")) { canHear = m->boolOr("audio", false); hearKnown = true; } }
  }
  http::Response t;
  if (http::Client::request(opts.ttsUrl, "GET", "/health", "", t, 800)) { t.all(2000); tts = t.status == 200; }
  std::lock_guard<std::mutex> g(mu);
  st.llmOk = llm; st.llmLoading = loading; st.ttsOk = tts; if (!model.empty()) st.model = model; if (hearKnown) st.canHear = canHear;
  if (!llm && st.phase == "idle") st.phase = loading ? "model loading" : "no model server";
  else if (llm && (st.phase == "model loading" || st.phase == "no model server")) st.phase = "idle";
}

bool Brain::fetchVoices() {
  std::string body; if (!http::Client::get(opts.ttsUrl, "/v1/audio/voices", body, 3000)) return false;
  json::Value v; if (!json::parse(body, v)) return false;
  std::vector<std::string> names;
  auto add = [&](const json::Value& e) { if (e.type == json::Value::String) names.push_back(e.s); else if (e.type == json::Value::Object) { std::string n = e.strOr("name", e.strOr("id", e.strOr("voice", ""))); if (!n.empty()) names.push_back(n); } };
  if (v.type == json::Value::Array) for (auto& e : v.a) add(e);
  else { for (auto& e : v.arrOf("voices")) add(e); for (auto& e : v.arrOf("data")) add(e); }
  if (names.empty()) return false;
  std::sort(names.begin(), names.end());
  std::lock_guard<std::mutex> g(mu);
  st.voices = names;
  if (st.voice.empty() || std::find(names.begin(), names.end(), st.voice) == names.end()) { st.voiceIdx = (int)(nowS() * 7) % (int)names.size(); st.voice = names[st.voiceIdx]; }
  else st.voiceIdx = (int)(std::find(names.begin(), names.end(), st.voice) - names.begin());
  return true;
}

void Brain::plannerLoop() {
  double lastHealth = 0; bool haveVoices = false;
  while (run.load()) {
    double t = nowS();
    if (t - lastHealth > 4.0) { health(); lastHealth = t; if (!haveVoices) haveVoices = fetchVoices(); }
    Job job; bool have = false;
    {
      std::unique_lock<std::mutex> g(jobMu);
      cv.wait_for(g, std::chrono::milliseconds(500));
      if (!run.load()) break;
      if (!jobs.empty()) { job = jobs.front(); jobs.pop_front(); have = true; }
      else if (!pendingSteer.empty() && nowS() >= steerDue) { job = Job{false, pendingSteer}; pendingSteer.clear(); have = true; }
    }
    BrainState s = state(); BandState b = engine ? engine->state() : BandState();
    if (!have && autoMode.load() && s.llmOk && b.playing && opts.autoEverySec > 0 && t - lastAuto > opts.autoEverySec) { job = Job{true, {}}; have = true; }
    { std::lock_guard<std::mutex> g(mu); st.nextAutoIn = (autoMode.load() && opts.autoEverySec > 0) ? std::max(0.0, opts.autoEverySec - (t - lastAuto)) : -1; st.auto_ = autoMode.load(); st.ears = opts.ears; }
    if (!have) continue;
    if (!s.llmOk) { health(); if (!state().llmOk) { std::lock_guard<std::mutex> g(mu); st.error = "the model server is not ready"; continue; } }
    plan(job); lastAuto = nowS();
  }
}

std::string Brain::systemPrompt(bool full) const {
  std::string s =
    "You are Gemma, sitting in with a band of humans who are jamming in the room. You hear them through a microphone: "
    "autojam measures the key, the chord being played, the tempo and the energy, and tells you. You answer with ONE "
    "JSON plan for the next phrases, nothing else. A synth band (drums, bass, keys, lead) plays your plan in time with "
    "the humans, following the chords it hears unless you give chords and say follow=false.\n\n"
    "Fields:\n"
    "- style: the sound of the band (" ;
  for (int i = 0; i < NUM_STYLES; i++) s += std::string(i ? ", " : "") + STYLES[i];
  s += "). pad is sustained chords only; ambient is sparse and slow; funk, house and latin are busy; jazz comps; "
       "reggae skanks on the offbeats; folk strums.\n"
       "- feel: how the drums play (straight, halftime, shuffle, breakbeat, brush, none). Brush is quiet.\n"
       "- energy 0..3 and parts drums/bass/keys/lead 0..3: 0 is silent, 1 light, 2 medium, 3 busy. Leave room: a human "
       "jam needs space, so rarely put everything at 3. If the humans are quiet or hesitant, play less.\n"
       "- motif: 3 to 8 scale degrees for the lead to riff on, 0 the tonic, 2 the third, 4 the fifth, 7 the octave, "
       "negative below. Make it singable and repeatable.\n"
       "- chords: up to 4 chords as degree 1..7 of the key with a quality, one per bar, for when the humans fall silent "
       "or when follow is false. Usually follow=true: the humans lead.\n"
       "- swing 0..30 percent. direction: one short sentence the band shows on screen, what you are going for.\n";
  if (full) s +=
       "- hear: in a few words each, the genre, the mood and the instruments you believe you hear (from the measurements"
       " and the audio if given).\n"
       "- lyrics: the verse you sing over the jam, 4 short lines (4 to 9 words, one breath each), each with a melody of "
       "one scale degree per syllable, sung on the beat with the last syllable held. The verse repeats until you write new lyrics, so make it a song: "
       "a hook the humans can sing along to, light rhymes, the mood of the room, original words only, nothing hateful. "
       "Write a new verse when none is being sung, when the humans ask for new lyrics or when the music has moved on; "
       "otherwise give none and your verse goes on.\n"
       "- voice: the name of the Pocket TTS voice that sings, from the list given.\n"
       "- say: one short friendly sentence spoken to the humans about what you hear or are about to do (or empty).\n"
       "- title: a name for this phrase of the jam.\n";
  s += "The humans' steering requests, when present, come first: honour them literally in the numbers. "
       "Write the JSON compact, on one line.";
  return s;
}

std::string Brain::userPrompt(const Job& j, const Hearing& h) const {
  char buf[512]; std::string u;
  if (!j.steering.empty()) { u += "The humans ask: "; for (size_t i = 0; i < j.steering.size(); i++) u += (i ? "; " : "") + j.steering[i]; u += ".\n"; }
  if (h.linked) u += "Rap Machine, a beat machine on another handheld, is linked over the network, so these are exact: it plays in " + h.keyName() + " at " + std::to_string((int)lroundf(h.bpm)) + " BPM, its chord loop (one chord a bar) is " + h.linkLoop + ".\n";
  if (h.silence) u += "The room is quiet right now (no one is playing).\n";
  else {
    snprintf(buf, sizeof buf, "Measured in the room: key %s (confidence %d%%), tempo %d BPM (%s, confidence %d%%), energy %d%%, chord now %s%s.\n",
             h.keyName().c_str(), (int)(h.keyConf * 100), (int)lroundf(h.bpm), h.tempoLocked ? "locked" : "unsure", (int)(h.beatConf * 100), (int)(h.energy * 100),
             h.chordName().c_str(), h.key >= 0 && h.chordRoot >= 0 ? (" (" + theory::roman(h.chordRoot, h.chordType, h.key, h.minorKey) + ")").c_str() : "");
    u += buf;
    if (!h.history.empty()) {
      u += "Chords heard lately (oldest first, with beats held): ";
      for (size_t i = std::max<size_t>(0, h.history.size() > 8 ? h.history.size() - 8 : 0); i < h.history.size(); i++) {
        const ChordEvent& e = h.history[i]; int beats = h.period > 0 ? (int)lround((e.end - e.start) / h.period) : 0;
        u += theory::chordName(e.root, e.type) + (beats > 0 ? " x" + std::to_string(beats) : "") + (i + 1 < h.history.size() ? ", " : ".\n");
      }
    }
    if (h.pitchConf > 0.5f) u += "A melody is being played around " + theory::midiName((int)lroundf(h.pitchMidi)) + ".\n";
  }
  if (engine) u += "Right now " + engine->describePlaying() + ".\n";
  BrainState s = state();
  if (!s.voices.empty()) { u += "Voices available: "; for (size_t i = 0; i < std::min<size_t>(12, s.voices.size()); i++) u += (i ? ", " : "") + s.voices[i]; u += ".\n"; }
  if (!titles.empty()) { u += "Earlier phrases this session: "; for (size_t i = 0; i < titles.size(); i++) u += (i ? "; " : "") + titles[i]; u += ". Move on from those.\n"; }
  u += j.full ? "Answer with the JSON plan." : "Answer with the short JSON plan (no lyrics).";
  return u;
}

std::string Brain::schema(bool full) const {
  using json::Value;
  auto intRange = [](int lo, int hi) { Value v = Value::obj(); v["type"] = Value::str("integer"); v["minimum"] = Value::num(lo); v["maximum"] = Value::num(hi); return v; };
  auto str = [](int maxLen) { Value v = Value::obj(); v["type"] = Value::str("string"); v["maxLength"] = Value::num(maxLen); return v; };
  auto enumOf = [](const char* const* names, int n) { Value v = Value::obj(); Value e = Value::arr(); for (int i = 0; i < n; i++) e.push(Value::str(names[i])); v["enum"] = e; return v; };
  Value root = Value::obj(); root["type"] = Value::str("object");
  Value props = Value::obj(); Value req = Value::arr();
  auto add = [&](const char* k, Value v) { props[k] = v; req.push(Value::str(k)); };
  add("style", enumOf(STYLES, NUM_STYLES)); add("feel", enumOf(FEELS, NUM_FEELS)); add("energy", intRange(0, 3));
  Value parts = Value::obj(); parts["type"] = Value::str("object"); Value pp = Value::obj(); Value pr = Value::arr();
  for (const char* p : {"drums", "bass", "keys", "lead"}) { pp[p] = intRange(0, 3); pr.push(Value::str(p)); }
  parts["properties"] = pp; parts["required"] = pr; add("parts", parts);
  Value motif = Value::obj(); motif["type"] = Value::str("array"); motif["minItems"] = Value::num(3); motif["maxItems"] = Value::num(8); motif["items"] = intRange(-3, 9); add("motif", motif);
  Value chords = Value::obj(); chords["type"] = Value::str("array"); chords["maxItems"] = Value::num(4);
  Value ch = Value::obj(); ch["type"] = Value::str("object"); Value cp = Value::obj(); cp["degree"] = intRange(1, 7);
  static const char* const QUAL[] = {"maj", "min", "7", "maj7", "min7", "dim", "sus"}; cp["quality"] = enumOf(QUAL, 7);
  ch["properties"] = cp; Value cr = Value::arr(); cr.push(Value::str("degree")); cr.push(Value::str("quality")); ch["required"] = cr; chords["items"] = ch; add("chords", chords);
  Value fol = Value::obj(); fol["type"] = Value::str("boolean"); add("follow", fol);
  add("swing", intRange(0, 30)); add("direction", str(90));
  if (full) {
    Value hear = Value::obj(); hear["type"] = Value::str("object"); Value hp = Value::obj(); hp["genre"] = str(24); hp["mood"] = str(24); hp["instruments"] = str(48);
    hear["properties"] = hp; Value hr = Value::arr(); for (const char* k : {"genre", "mood", "instruments"}) hr.push(Value::str(k)); hear["required"] = hr; add("hear", hear);
    Value lyr = Value::obj(); lyr["type"] = Value::str("array"); lyr["maxItems"] = Value::num(4);
    Value line = Value::obj(); line["type"] = Value::str("object"); Value lp = Value::obj(); lp["text"] = str(64);
    Value mel = Value::obj(); mel["type"] = Value::str("array"); mel["minItems"] = Value::num(2); mel["maxItems"] = Value::num(12); mel["items"] = intRange(-3, 9); lp["melody"] = mel;
    line["properties"] = lp; Value lr = Value::arr(); lr.push(Value::str("text")); lr.push(Value::str("melody")); line["required"] = lr; lyr["items"] = line; add("lyrics", lyr);
    add("voice", str(24)); add("say", str(100)); add("title", str(32));
  }
  root["properties"] = props; root["required"] = req;
  return json::dump(root);
}

// The plan schema as a compact GBNF grammar: llama.cpp's json_schema grammar lets the model indent, and with the
// audio clip in the prompt Gemma pretty-prints (one number a line, 400 to 700+ tokens, cut off at max_tokens).
// Here there is no whitespace at all and the keys come in the schema's order.
static std::string gbnfLit(const std::string& t) { std::string o = "\""; for (char c : t) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\""; }
static std::string gbnf(const json::Value& s) {
  if (const json::Value* e = s.get("enum")) { std::string o = "("; for (size_t i = 0; i < e->a.size(); i++) o += (i ? " | " : "") + gbnfLit("\"" + e->a[i].s + "\""); return o + ")"; }
  std::string t = s.strOr("type", "");
  if (t == "integer") { int lo = s.intOr("minimum", 0), hi = s.intOr("maximum", 9); std::string o = "("; for (int i = lo; i <= hi; i++) o += (i > lo ? " | " : "") + gbnfLit(std::to_string(i)); return o + ")"; }
  if (t == "boolean") return "(\"true\" | \"false\")";
  if (t == "string") return "\"\\\"\" char{0," + std::to_string(s.intOr("maxLength", 64)) + "} \"\\\"\"";
  if (t == "array") {
    int lo = s.intOr("minItems", 0), hi = s.intOr("maxItems", 8); std::string it = s.get("items") ? gbnf(*s.get("items")) : "\"null\"";
    std::string items = it + " (\",\" " + it + "){" + std::to_string(std::max(0, lo - 1)) + "," + std::to_string(std::max(0, hi - 1)) + "}";
    return "\"[\" (" + items + ")" + (lo > 0 ? "" : "?") + " \"]\"";
  }
  if (t == "object") {
    std::string o = "\"{\""; const json::Value* props = s.get("properties"); const auto& req = s.arrOf("required");
    for (size_t i = 0; i < req.size(); i++) { const json::Value* v = props ? props->get(req[i].s.c_str()) : nullptr; o += " " + gbnfLit(std::string(i ? "," : "") + "\"" + req[i].s + "\":") + " " + (v ? gbnf(*v) : "\"null\""); }
    return o + " \"}\"";
  }
  return "\"null\"";
}

std::string Brain::grammar(bool full) const {
  json::Value sch; json::parse(schema(full), sch);
  return "root ::= " + gbnf(sch) + "\n" R"(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))" "\n";
}

static std::string base64(const std::string& in) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out; size_t i = 0; unsigned char a3[3]; unsigned char a4[4];
  while (i + 3 <= in.size()) { a3[0] = in[i]; a3[1] = in[i + 1]; a3[2] = in[i + 2]; i += 3; a4[0] = a3[0] >> 2; a4[1] = ((a3[0] & 3) << 4) | (a3[1] >> 4); a4[2] = ((a3[1] & 15) << 2) | (a3[2] >> 6); a4[3] = a3[2] & 63; for (unsigned char c : a4) out += T[c]; }
  size_t rem = in.size() - i;
  if (rem) { a3[0] = in[i]; a3[1] = rem > 1 ? in[i + 1] : 0; a3[2] = 0; out += T[a3[0] >> 2]; out += T[((a3[0] & 3) << 4) | (a3[1] >> 4)]; out += rem > 1 ? T[((a3[1] & 15) << 2)] : '='; out += '='; }
  return out;
}

std::string Brain::clipWavBase64() const {
  // the last 8 s of the room at 16 kHz mono s16 (3:1 decimation with a box filter)
  const int secs = 8, rate = 16000; size_t need = (size_t)secs * 48000;
  size_t w = earW.load(std::memory_order_acquire); if (w < need) need = w;
  size_t start = w - need; size_t n = need / 3;
  std::string pcm; pcm.resize(n * 2);
  float gain = 1.0f, pk = 0; for (size_t i = 0; i < need; i++) pk = std::max(pk, fabsf(earRing[(start + i) % earRing.size()])); if (pk > 0.01f) gain = std::min(8.0f, 0.8f / pk);
  for (size_t i = 0; i < n; i++) { float s = 0; for (int k = 0; k < 3; k++) s += earRing[(start + i * 3 + k) % earRing.size()]; s = s / 3 * gain; int16_t v = (int16_t)lroundf(dsp::clamp(s, -1.0f, 1.0f) * 32767); pcm[2 * i] = (char)(v & 255); pcm[2 * i + 1] = (char)((v >> 8) & 255); }
  std::string wav; uint32_t dataBytes = (uint32_t)pcm.size(), riff = 36 + dataBytes, byteRate = rate * 2, fmtLen = 16, rate32 = rate; uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) wav += (char)((v >> (8 * i)) & 255); }; auto put16 = [&](uint16_t v) { wav += (char)(v & 255); wav += (char)(v >> 8); };
  wav += "RIFF"; put32(riff); wav += "WAVEfmt "; put32(fmtLen); put16(fmt); put16(ch); put32(rate32); put32(byteRate); put16(align); put16(bits); wav += "data"; put32(dataBytes); wav += pcm;
  return base64(wav);
}

bool Brain::parsePlan(const json::Value& v, Plan& p, bool full) {
  if (v.type != json::Value::Object) return false;
  p.style = v.strOr("style", p.style); p.feel = v.strOr("feel", p.feel); p.energy = v.intOr("energy", p.energy);
  if (const json::Value* parts = v.get("parts")) { p.drums = parts->intOr("drums", p.drums); p.bass = parts->intOr("bass", p.bass); p.keys = parts->intOr("keys", p.keys); p.lead = parts->intOr("lead", p.lead); }
  auto& m = v.arrOf("motif"); if (!m.empty()) { p.motif.clear(); for (auto& e : m) if (e.type == json::Value::Number) p.motif.push_back((int)e.n); }
  p.chords.clear(); for (auto& c : v.arrOf("chords")) { PlanChord pc; pc.degree = c.intOr("degree", 1); pc.quality = c.strOr("quality", "maj"); p.chords.push_back(pc); }
  p.follow = v.boolOr("follow", true); p.swing = v.intOr("swing", 0); p.direction = v.strOr("direction", "");
  if (full) {
    if (const json::Value* h = v.get("hear")) { p.hearGenre = h->strOr("genre", ""); p.hearMood = h->strOr("mood", ""); p.hearInstruments = h->strOr("instruments", ""); }
    p.lyrics.clear();
    for (auto& l : v.arrOf("lyrics")) { LyricLine ll; ll.text = l.strOr("text", ""); for (auto& d : l.arrOf("melody")) if (d.type == json::Value::Number) ll.melody.push_back((int)d.n); if (!ll.text.empty()) p.lyrics.push_back(ll); }
    p.voice = v.strOr("voice", ""); p.say = v.strOr("say", ""); p.title = v.strOr("title", "");
  }
  p.quick = !full;
  for (int* d : {&p.drums, &p.bass, &p.keys, &p.lead, &p.energy}) *d = std::max(0, std::min(3, *d));
  return true;
}

bool Brain::plan(const Job& j) {
  Hearing h = analysis ? analysis->get() : Hearing();
  BandState band = engine ? engine->state() : BandState();
  using json::Value;
  Value body = Value::obj(); Value msgs = Value::arr();
  Value sys = Value::obj(); sys["role"] = Value::str("system"); sys["content"] = Value::str(systemPrompt(j.full)); msgs.push(sys);
  Value usr = Value::obj(); usr["role"] = Value::str("user");
  bool ears = opts.ears && state().canHear && j.full && !h.silence;   // without the mmproj llama-server answers 500
  if (ears) {
    Value parts = Value::arr();
    Value audio = Value::obj(); audio["type"] = Value::str("input_audio"); Value ia = Value::obj(); ia["data"] = Value::str(clipWavBase64()); ia["format"] = Value::str("wav"); audio["input_audio"] = ia; parts.push(audio);
    Value text = Value::obj(); text["type"] = Value::str("text"); text["text"] = Value::str("That is the last 8 seconds of the room.\n" + userPrompt(j, h)); parts.push(text);
    usr["content"] = parts;
  } else usr["content"] = Value::str(userPrompt(j, h));
  msgs.push(usr);
  body["messages"] = msgs; body["stream"] = Value::boolean(true); body["temperature"] = Value::num(0.8); body["top_p"] = Value::num(0.95);
  body["max_tokens"] = Value::num(j.full ? 700 : 260);
  body["grammar"] = Value::str(grammar(j.full));
  Value kw = Value::obj(); kw["enable_thinking"] = Value::boolean(false); body["chat_template_kwargs"] = kw;
  double t0 = nowS();
  { std::lock_guard<std::mutex> g(mu); st.phase = ears ? "listening (audio to Gemma)" : "prompt"; st.tokens = 0; st.seconds = 0; st.error.clear(); }
  http::Response r;
  if (!http::Client::request(opts.llmUrl, "POST", "/v1/chat/completions", json::dump(body), r, 3000)) { std::lock_guard<std::mutex> g(mu); st.phase = "idle"; st.error = "llm: " + r.error; st.failures++; return false; }
  if (r.status != 200) { std::string e = r.all(5000); std::lock_guard<std::mutex> g(mu); st.phase = "idle"; st.error = "llm http " + std::to_string(r.status) + ": " + e.substr(0, 120); st.failures++; return false; }
  std::string text, line; int tokens = 0;
  while (run.load() && r.line(line, 600000)) {
    if (line.compare(0, 5, "data:") != 0) continue;
    std::string data = line.substr(5); size_t a = data.find_first_not_of(' '); data = a == std::string::npos ? "" : data.substr(a);
    if (data == "[DONE]") break;
    Value ev; if (!json::parse(data, ev)) continue;
    for (auto& ch : ev.arrOf("choices")) { if (const Value* d = ch.get("delta")) { std::string piece = d->strOr("content", ""); if (!piece.empty()) { text += piece; tokens++; } } }
    std::lock_guard<std::mutex> g(mu); st.phase = "writing"; st.tokens = tokens; st.seconds = (float)(nowS() - t0);
  }
  Value v; Plan p = band.plan; p.quick = false;
  if (!json::parseEmbedded(text, v) || !parsePlan(v, p, j.full)) { std::lock_guard<std::mutex> g(mu); st.phase = "idle"; st.error = "bad JSON from the model"; st.lastRaw = text.substr(0, 2000); st.failures++; return false; }
  p.created = nowS(); p.tokens = tokens; p.seconds = (float)(nowS() - t0);
  if (!j.full) { p.title = band.plan.title; p.hearGenre = band.plan.hearGenre; p.hearMood = band.plan.hearMood; p.hearInstruments = band.plan.hearInstruments; p.voice = band.plan.voice; }
  if (engine) engine->applyPlan(p);
  BrainState s = state();
  if (j.full && !p.voice.empty() && std::find(s.voices.begin(), s.voices.end(), p.voice) != s.voices.end()) { std::lock_guard<std::mutex> g(mu); st.voice = p.voice; st.voiceIdx = (int)(std::find(st.voices.begin(), st.voices.end(), p.voice) - st.voices.begin()); }
  if (j.full) { if (!p.say.empty()) speak(p.say, {}, false); int verse = ++verses; for (auto& l : p.lyrics) speak(l.text, l.melody, true, verse, (int)p.lyrics.size()); if (!p.title.empty()) { titles.push_back(p.title); while (titles.size() > 6) titles.pop_front(); } }
  std::lock_guard<std::mutex> g(mu);
  st.phase = "idle"; st.tokens = tokens; st.seconds = p.seconds; st.plans++; st.lastRaw = text; st.steering.clear();
  if (j.full) { st.lastTitle = p.title; st.lastSay = p.say; st.lastHear = p.hearGenre + (p.hearMood.empty() ? "" : " · " + p.hearMood) + (p.hearInstruments.empty() ? "" : " · " + p.hearInstruments); }
  return true;
}

void Brain::speakerLoop() {
  while (run.load()) {
    Say s; bool have = false;
    { std::unique_lock<std::mutex> g(jobMu); scv.wait_for(g, std::chrono::milliseconds(500)); if (!run.load()) break; if (!says.empty()) { s = says.front(); says.pop_front(); have = true; } }
    { std::lock_guard<std::mutex> g(mu); st.ttsQueued = (int)says.size(); }
    if (!have) continue;
    BrainState bs = state();
    if (!bs.ttsOk) continue;
    using json::Value; Value body = Value::obj(); body["input"] = Value::str(s.text); if (!bs.voice.empty()) body["voice"] = Value::str(bs.voice); body["response_format"] = Value::str("pcm");
    http::Response r;
    if (!http::Client::request(opts.ttsUrl, "POST", "/v1/audio/speech", json::dump(body), r, 3000) || r.status != 200) { std::lock_guard<std::mutex> g(mu); st.error = "tts: " + (r.error.empty() ? "http " + std::to_string(r.status) : r.error); continue; }
    std::string pcm; for (;;) { std::string p = r.read(60000); if (p.empty()) break; pcm += p; }
    size_t n = pcm.size() / 2; std::vector<int16_t> s16(n); memcpy(s16.data(), pcm.data(), n * 2);
    // trim silence at both ends (threshold -45 dBFS), keep 60 ms
    size_t a = 0, b = n; const int thr = 180; while (a < n && abs(s16[a]) < thr) a++; while (b > a && abs(s16[b - 1]) < thr) b--;
    a = a > 1440 ? a - 1440 : 0; b = std::min(n, b + 1440);
    if (b <= a) continue;
    VoiceLine line; line.text = s.text; line.melody = s.melody; line.sung = s.sung; line.verse = s.verse; line.of = s.of;
    Sung sung;
    if (s.sung && engine) {   // sing it in the jam's key and tempo; if no syllables are found, the autotune does what it can
      BandState band = engine->state(); SingRequest rq; rq.tonicPc = band.key >= 0 ? band.key : 0; rq.mode = band.key >= 0 ? band.mode : 0;
      rq.bpm = band.bpm > 40 ? band.bpm : 100; rq.melody = s.melody; rq.transpose = band.transpose;
      if (sing(&s16[a], (int)(b - a), rq, sung)) { line.pcm = std::move(sung.pcm); line.notes = std::move(sung.notes); }
    }
    if (line.pcm.empty()) dsp::upsample2x(&s16[a], (int)(b - a), line.pcm);
    if (engine) { if (s.sung) engine->queueLine(std::move(line)); else engine->speakNow(std::move(line)); }
  }
}
