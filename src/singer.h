// autojam: the singer. Pocket TTS speaks; this turns a spoken line into a sung one, offline on the speaker thread.
// It finds the syllables (voiced stretches, split at energy dips), lays them on the beat grid (each vowel onset on
// a quarter or an eighth, the last one held), stretches the vowels to the note lengths and resynthesises them by
// TD-PSOLA at the melody's pitch, with a glide into each note and vibrato on long ones. Consonants keep their
// natural speed. The grains are the voice's own, so its formants stay and it still sounds like the chosen voice.
#pragma once
#include <vector>
#include <cstdint>

struct SingRequest {
  int tonicPc = 0, mode = 0;          // the key of the jam (theory::modes() index)
  float bpm = 100; int bars = 2;      // the line fills this many bars of 4/4 (a beat of breath at the end)
  std::vector<int> melody;            // scale degrees, spread over the syllables
  int transpose = 0;                  // semitones
};
struct Sung {
  std::vector<float> pcm;             // 48 kHz mono
  std::vector<std::pair<int, int>> notes;   // (first sample, MIDI note) of each sung syllable
  int syllables = 0;
};
// speech: Pocket TTS output, 24 kHz s16 with the silence at both ends trimmed. False when no syllable is found.
bool sing(const int16_t* speech, int n, const SingRequest& rq, Sung& out);
