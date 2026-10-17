#include "sysaudio.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace sysaudio {

std::string run(const std::string& cmd, int timeoutS) {
  std::string full = "timeout " + std::to_string(timeoutS) + " " + cmd + " 2>/dev/null";
  FILE* f = popen(full.c_str(), "r"); if (!f) return "";
  std::string out; char buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  pclose(f); return out;
}
static std::string readFile(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string shq(const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; }

static std::vector<Source> listShort(const char* what, bool skipMonitors) {
  std::vector<Source> out;
  std::istringstream in(run(std::string("pactl list short ") + what));
  std::string line;
  while (std::getline(in, line)) {
    size_t t1 = line.find('\t'); if (t1 == std::string::npos) continue;
    size_t t2 = line.find('\t', t1 + 1);
    Source s; s.name = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
    if (skipMonitors && s.name.size() > 8 && s.name.compare(s.name.size() - 8, 8, ".monitor") == 0) continue;
    std::string l = lower(s.name); s.usb = l.find("usb") != std::string::npos; s.bt = l.find("bluez") != std::string::npos;
    s.desc = s.usb ? "USB" : (s.bt ? "Bluetooth" : (l.find("sound_card0") != std::string::npos ? "Flip 2" : s.name));
    if (l.find("speaker") != std::string::npos) s.desc = "Speaker"; else if (l.find("headphones") != std::string::npos) s.desc = "Headphones";
    out.push_back(s);
  }
  return out;
}
std::vector<Source> listSources() { return listShort("sources", true); }
std::vector<Source> listSinks() { return listShort("sinks", false); }

std::vector<std::string> sourcePorts(const std::string& name) {
  std::vector<std::string> ports;
  std::string all = run("pactl list sources");
  size_t pos = all.find("Name: " + name + "\n"); if (pos == std::string::npos) return ports;
  size_t endBlock = all.find("Source #", pos); std::string block = all.substr(pos, endBlock == std::string::npos ? std::string::npos : endBlock - pos);
  std::istringstream in(block); std::string line;
  while (std::getline(in, line)) { size_t a = line.find("[In] "); if (a == std::string::npos) continue; size_t b = line.find(':', a); ports.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); }
  return ports;
}
void setSourcePort(const std::string& name, const std::string& port) { run("pactl set-source-port " + shq(name) + " " + shq(port)); }
void setDefaultSink(const std::string& name) { run("pactl set-default-sink " + shq(name)); }
std::string defaultSink() { std::string s = run("pactl get-default-sink"); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }

int headsetMicInserted() {
  DIR* d = opendir("/sys/class/input"); if (!d) return -1;
  int result = -1;
  while (dirent* e = readdir(d)) {
    if (strncmp(e->d_name, "event", 5)) continue;
    std::string name = readFile(std::string("/sys/class/input/") + e->d_name + "/device/name");
    if (name.find("Headset Jack") == std::string::npos || name.find("DualSense") != std::string::npos) continue;
    int fd = open((std::string("/dev/input/") + e->d_name).c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    unsigned char sw[8] = {};
    if (ioctl(fd, EVIOCGSW(sizeof sw), sw) >= 0) result = (sw[0] >> SW_MICROPHONE_INSERT) & 1;
    close(fd); break;
  }
  closedir(d); return result;
}

MicChoice pickMic(const std::string& forced) {
  if (!forced.empty()) return {forced, forced};
  auto srcs = listSources();
  if (srcs.empty()) return {"", "no microphone found"};
  for (auto& s : srcs) if (s.usb || s.bt) return {s.name, s.usb ? "USB mic" : "Bluetooth mic"};
  for (auto& s : srcs) {
    if (s.name.find("sound_card0") == std::string::npos) continue;
    auto ports = sourcePorts(s.name); int ins = headsetMicInserted();
    bool hasMic = std::find(ports.begin(), ports.end(), "[In] Mic") != ports.end();
    bool hasHeadset = std::find(ports.begin(), ports.end(), "[In] Headset") != ports.end();
    if (hasMic && ins != 1) { setSourcePort(s.name, "[In] Mic"); return {s.name, "built-in mic"}; }
    if (ins == 0) return {"", "no mic: plug a headset (TRRS) or USB mic"};
    if (hasHeadset) setSourcePort(s.name, "[In] Headset");
    return {s.name, "headset mic"};
  }
  return {srcs[0].name, srcs[0].name};
}

std::vector<Card> soundCards() {
  std::vector<Card> out; std::istringstream in(readFile("/proc/asound/cards")); std::string line;
  while (std::getline(in, line)) {
    // " 1 [Device         ]: USB-Audio - USB Audio Device"
    size_t lb = line.find('['), rb = line.find(']'), colon = line.find(':');
    if (lb == std::string::npos || rb == std::string::npos || colon == std::string::npos) continue;
    Card c; c.index = atoi(line.c_str()); c.id = line.substr(lb + 1, rb - lb - 1); while (!c.id.empty() && c.id.back() == ' ') c.id.pop_back();
    c.name = line.substr(colon + 2); c.usb = lower(c.name).find("usb") != std::string::npos;
    std::string extra; if (std::getline(in, extra)) { size_t a = extra.find_first_not_of(' '); if (a != std::string::npos) c.name = extra.substr(a); }
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> rawMidiPorts() {
  std::vector<std::string> out; DIR* d = opendir("/dev/snd"); if (!d) return out;
  while (dirent* e = readdir(d)) if (!strncmp(e->d_name, "midiC", 5)) out.push_back(std::string("/dev/snd/") + e->d_name);
  closedir(d); std::sort(out.begin(), out.end()); return out;
}

std::string usbRole() {
  DIR* d = opendir("/sys/class/usb_role"); if (!d) return "";
  std::string role;
  while (dirent* e = readdir(d)) { if (e->d_name[0] == '.') continue; role = readFile(std::string("/sys/class/usb_role/") + e->d_name + "/role"); break; }
  closedir(d); while (!role.empty() && (role.back() == '\n')) role.pop_back(); return role;
}
bool usbGadgetAudioAvailable() {
  // configfs lists a function type when its module is built in or loaded: try to see the usb_f_uac2 / usb_f_midi aliases
  std::string mods = readFile("/proc/modules"); std::string cfg = run("zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_USB_CONFIGFS_F_(UAC2|MIDI)=y'");
  return !cfg.empty() || mods.find("usb_f_uac2") != std::string::npos || mods.find("usb_f_midi") != std::string::npos;
}

Battery battery() {
  Battery b; DIR* d = opendir("/sys/class/power_supply"); if (!d) return b;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string base = std::string("/sys/class/power_supply/") + e->d_name;
    std::string type = readFile(base + "/type"); if (type.find("Battery") == std::string::npos) continue;
    std::string cap = readFile(base + "/capacity"); if (!cap.empty()) b.percent = atoi(cap.c_str());
    std::string st = readFile(base + "/status"); b.charging = st.find("Charging") != std::string::npos || st.find("Full") != std::string::npos;
    break;
  }
  closedir(d); return b;
}
long memTotalMb() { std::istringstream in(readFile("/proc/meminfo")); std::string k; long v = 0; while (in >> k >> v) { if (k == "MemTotal:") return v / 1024; std::string unit; in >> unit; } return 0; }

} // namespace sysaudio
