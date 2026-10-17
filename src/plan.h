// autojam: a Plan is what the language model hands the band: a direction for the next phrases. The player
// reads it at the next bar; the UI shows it so the humans see what Gemma is contributing.
#pragma once
#include <string>
#include <vector>

struct LyricLine { std::string text; std::vector<int> melody; };   // melody: scale degrees, 0 = tonic
struct PlanChord { int degree = 1; std::string quality = "maj"; };   // degree 1..7 of the key

static const char* const STYLES[] = {"pad", "funk", "rock", "jazz", "ambient", "hiphop", "house", "folk", "latin", "reggae", "blues", "synthwave"};
static const int NUM_STYLES = 12;
static const char* const FEELS[] = {"straight", "halftime", "shuffle", "breakbeat", "brush", "none"};
static const int NUM_FEELS = 6;

struct Plan {
  std::string title, direction, say, voice;
  std::string hearGenre, hearMood, hearInstruments;
  std::string style = "pad", feel = "straight";
  int energy = 1;                              // 0 .. 3
  int drums = 1, bass = 1, keys = 1, lead = 0;  // density per part, 0 off .. 3 busy
  std::vector<int> motif = {0, 2, 4, 2};
  std::vector<PlanChord> chords;               // empty: follow the humans' chords
  bool follow = true;                          // follow the humans' chords (chords is then a suggestion only)
  int swing = 0;                               // percent, 0 .. 30
  std::vector<LyricLine> lyrics;
  int autotuneAmount = 90, autotuneSpeed = 35;
  double created = 0; int tokens = 0; float seconds = 0; bool quick = false;
  int styleIndex() const { for (int i = 0; i < NUM_STYLES; i++) if (style == STYLES[i]) return i; return 0; }
  int feelIndex() const { for (int i = 0; i < NUM_FEELS; i++) if (feel == FEELS[i]) return i; return 0; }
};
