// autojam: note names, scales and chord templates shared by the listener (analysis), the player and the UI.
#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>

namespace theory {

static const char* const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* const NOTE_NAMES_FLAT[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

// Chord types the listener recognises and the player voices. Intervals in semitones from the root.
struct ChordType { const char* name; const char* suffix; std::vector<int> iv; bool triad; };
inline const std::vector<ChordType>& chordTypes() {
  static const std::vector<ChordType> t = {
    {"major", "", {0, 4, 7}, true}, {"minor", "m", {0, 3, 7}, true}, {"diminished", "dim", {0, 3, 6}, true},
    {"augmented", "aug", {0, 4, 8}, true}, {"sus2", "sus2", {0, 2, 7}, true}, {"sus4", "sus4", {0, 5, 7}, true},
    {"power", "5", {0, 7}, true},
    {"dominant 7", "7", {0, 4, 7, 10}, false}, {"major 7", "maj7", {0, 4, 7, 11}, false}, {"minor 7", "m7", {0, 3, 7, 10}, false},
    {"half-dim", "m7b5", {0, 3, 6, 10}, false}, {"6", "6", {0, 4, 7, 9}, false}, {"minor 6", "m6", {0, 3, 7, 9}, false},
    {"add9", "add9", {0, 2, 4, 7}, false}, {"minor add9", "madd9", {0, 2, 3, 7}, false},
  };
  return t;
}
enum { CH_MAJ = 0, CH_MIN = 1, CH_DIM = 2, CH_AUG = 3, CH_SUS2 = 4, CH_SUS4 = 5, CH_POWER = 6, CH_DOM7 = 7, CH_MAJ7 = 8, CH_MIN7 = 9, CH_HDIM = 10, CH_6 = 11, CH_M6 = 12, CH_ADD9 = 13, CH_MADD9 = 14 };
inline bool chordIsMinorish(int type) { return type == CH_MIN || type == CH_DIM || type == CH_MIN7 || type == CH_HDIM || type == CH_M6 || type == CH_MADD9; }
inline std::string chordName(int root, int type) { if (root < 0) return "N.C."; return std::string(NOTE_NAMES[root % 12]) + chordTypes()[type].suffix; }

// Modes as semitone sets from the tonic; the player spells degrees with these.
struct Mode { const char* name; std::vector<int> iv; bool minorish; };
inline const std::vector<Mode>& modes() {
  static const std::vector<Mode> m = {
    {"major", {0, 2, 4, 5, 7, 9, 11}, false}, {"minor", {0, 2, 3, 5, 7, 8, 10}, true},
    {"dorian", {0, 2, 3, 5, 7, 9, 10}, true}, {"mixolydian", {0, 2, 4, 5, 7, 9, 10}, false},
    {"lydian", {0, 2, 4, 6, 7, 9, 11}, false}, {"phrygian", {0, 1, 3, 5, 7, 8, 10}, true},
    {"harmonic minor", {0, 2, 3, 5, 7, 8, 11}, true}, {"blues", {0, 3, 5, 6, 7, 10}, true},
    {"pentatonic", {0, 2, 4, 7, 9}, false}, {"minor pentatonic", {0, 3, 5, 7, 10}, true},
  };
  return m;
}
inline int modeIndex(const std::string& name) { auto& m = modes(); for (size_t i = 0; i < m.size(); i++) if (name == m[i].name) return (int)i; return name == "ionian" ? 0 : (name == "aeolian" ? 1 : -1); }

// scale degree (0 = tonic, 7 = octave, negative below) -> MIDI note, around a base octave (MIDI of tonic)
inline int degreeToMidi(int tonicMidi, int mode, int degree) {
  const auto& iv = modes()[mode].iv; int n = (int)iv.size();
  int oct = degree >= 0 ? degree / n : -((-degree + n - 1) / n);
  int d = degree - oct * n;
  return tonicMidi + oct * 12 + iv[d];
}
// nearest note of the scale (12-bit mask of pitch classes relative to C) to a MIDI float
inline int nearestInMask(float midi, uint16_t mask) {
  int m = (int)lroundf(midi); int best = m; float bestD = 1e9f;
  for (int d = -6; d <= 6; d++) { int c = m + d; if (c < 0) continue; if (mask & (1 << (c % 12))) { float dist = fabsf(midi - c); if (dist < bestD) { bestD = dist; best = c; } } }
  return best;
}
inline uint16_t scaleMask(int tonic, int mode) { uint16_t m = 0; for (int iv : modes()[mode].iv) m |= 1 << ((tonic + iv) % 12); return m; }
inline uint16_t chordMask(int root, int type) { uint16_t m = 0; for (int iv : chordTypes()[type].iv) m |= 1 << ((root + iv) % 12); return m; }

inline float midiToHz(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }
inline float hzToMidi(float hz) { return 69.0f + 12.0f * log2f(hz / 440.0f); }
inline std::string midiName(int m) { if (m < 0) return "-"; return std::string(NOTE_NAMES[m % 12]) + std::to_string(m / 12 - 1); }

// Roman numeral of a chord in a key, for the display ("IV", "vi", "V7", "bVII")
inline std::string roman(int root, int type, int tonic, bool minorKey) {
  if (root < 0) return "";
  static const char* MAJ[12] = {"I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII"};
  static const char* MIN[12] = {"I", "bII", "II", "III", "#III", "IV", "#IV", "V", "VI", "#VI", "VII", "#VII"};
  int d = ((root - tonic) % 12 + 12) % 12;
  std::string r = minorKey ? MIN[d] : MAJ[d];
  if (chordIsMinorish(type)) for (auto& c : r) c = (char)tolower((unsigned char)c);
  const char* suf = chordTypes()[type].suffix;
  if (type == CH_DOM7 || type == CH_MAJ7 || type == CH_MIN7) r += (type == CH_MIN7) ? "7" : suf;
  else if (type == CH_DIM || type == CH_HDIM) r += "°";
  else if (type == CH_AUG) r += "+";
  else if (type == CH_SUS2 || type == CH_SUS4) r += suf;
  return r;
}

} // namespace theory
