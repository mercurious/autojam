// autojam: USB MIDI through ALSA raw MIDI (/dev/snd/midiC*D*; this kernel has snd-usb-audio and rawmidi built
// in, no sequencer). Out: the band's notes on channels 1-4 and 10, MIDI clock, start and stop, so a synth or
// drum machine on the USB port plays along. In: notes a human plays on a USB keyboard are heard as chord tones
// and shown; CC 1 (mod wheel) steers the band's energy.
#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>

class MidiOut {
public:
  bool open(const std::string& path);     // non-blocking writes; a full pipe drops bytes rather than stalling audio
  void close();
  bool isOpen() const { return fd >= 0; }
  void noteOn(int ch, int note, int vel) { uint8_t m[3] = {(uint8_t)(0x90 | (ch & 15)), (uint8_t)(note & 127), (uint8_t)(vel & 127)}; write(m, 3); }
  void noteOff(int ch, int note) { uint8_t m[3] = {(uint8_t)(0x80 | (ch & 15)), (uint8_t)(note & 127), 0}; write(m, 3); }
  void clock() { uint8_t m = 0xF8; write(&m, 1); }
  void start() { uint8_t m = 0xFA; write(&m, 1); }
  void stop() { uint8_t m = 0xFC; write(&m, 1); }
  void allNotesOff() { for (int ch = 0; ch < 16; ch++) { uint8_t m[3] = {(uint8_t)(0xB0 | ch), 123, 0}; write(m, 3); } }
  std::string path;
private:
  int fd = -1;
  void write(const uint8_t* b, int n);
};

class MidiIn {
public:
  using OnNote = std::function<void(int note, int vel, bool on)>;
  using OnCC = std::function<void(int cc, int value)>;
  bool open(const std::string& path, OnNote onNote, OnCC onCC);
  void close();
  bool isOpen() const { return fd >= 0; }
  std::string path;
private:
  int fd = -1; std::thread th; std::atomic<bool> run{false};
};
