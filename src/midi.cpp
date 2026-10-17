#include "midi.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

bool MidiOut::open(const std::string& p) { close(); fd = ::open(p.c_str(), O_WRONLY | O_NONBLOCK); path = p; return fd >= 0; }
void MidiOut::close() { if (fd >= 0) { allNotesOff(); ::close(fd); fd = -1; } }
void MidiOut::write(const uint8_t* b, int n) { if (fd >= 0) { ssize_t r = ::write(fd, b, (size_t)n); (void)r; } }

bool MidiIn::open(const std::string& p, OnNote onNote, OnCC onCC) {
  close(); fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK); path = p;
  if (fd < 0) return false;
  run = true;
  th = std::thread([this, onNote, onCC]() {
    uint8_t status = 0, data[2]; int need = 0, have = 0;
    while (run.load()) {
      pollfd pf{fd, POLLIN, 0}; if (poll(&pf, 1, 200) <= 0) continue;
      uint8_t buf[64]; ssize_t n = ::read(fd, buf, sizeof buf); if (n <= 0) continue;
      for (ssize_t i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (b >= 0xF8) continue;                       // realtime
        if (b & 0x80) { status = b; have = 0; uint8_t hi = b & 0xF0; need = (hi == 0xC0 || hi == 0xD0) ? 1 : (b >= 0xF0 ? 0 : 2); continue; }
        if (!need) continue;
        data[have++] = b;
        if (have < need) continue;
        have = 0; uint8_t hi = status & 0xF0;
        if (hi == 0x90 && onNote) onNote(data[0], data[1], data[1] > 0);
        else if (hi == 0x80 && onNote) onNote(data[0], 0, false);
        else if (hi == 0xB0 && onCC) onCC(data[0], data[1]);
      }
    }
  });
  return true;
}
void MidiIn::close() { run = false; if (th.joinable()) th.join(); if (fd >= 0) { ::close(fd); fd = -1; } }
