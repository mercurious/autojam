// autojam: the device around the app. PipeWire sources and sinks through pactl (the mic the humans are heard
// through, the speaker, headphones or a USB interface they are answered on), the headset jack switch, USB
// sound cards and raw MIDI ports, the battery. Everything here is a shell-out or a sysfs read; nothing is
// held open.
#pragma once
#include <string>
#include <vector>

namespace sysaudio {

struct Source { std::string name, desc; bool usb = false, bt = false; };
struct MicChoice { std::string source, desc; };   // source empty: no mic

std::vector<Source> listSources();                  // real capture sources (not monitors)
std::vector<Source> listSinks();
std::vector<std::string> sourcePorts(const std::string& name);
void setSourcePort(const std::string& name, const std::string& port);
void setDefaultSink(const std::string& name);
std::string defaultSink();
// Moves this process's capture streams onto `source` when PipeWire has them elsewhere (a stream created while its
// target was missing falls back to the default source, a monitor even, and stays there). Returns where it was.
std::string keepCaptureOn(const std::string& source);
int headsetMicInserted();                           // 1 yes, 0 no, -1 unknown (SW_MICROPHONE_INSERT on the jack's input device)
// Port of retro-voice-chat's pick_source: USB/Bluetooth first, then the Flip 2 card: 3.5 mm headset if one is in,
// else the built-in mic when the device tree powers it ("[In] Mic", the ETK path).
MicChoice pickMic(const std::string& forced = "");

struct Card { int index; std::string id, name; bool usb; };
std::vector<Card> soundCards();                     // /proc/asound/cards
std::vector<std::string> rawMidiPorts();            // /dev/snd/midiC*D*
std::string usbRole();                              // host / device / none / ""
bool usbGadgetAudioAvailable();                     // f_uac2 or f_midi in this kernel

struct Battery { int percent = -1; bool charging = false; };
Battery battery();
long memTotalMb();
std::string run(const std::string& cmd, int timeoutS = 5);   // stdout of a shell command (empty on failure)

} // namespace sysaudio
