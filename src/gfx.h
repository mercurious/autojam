// autojam: drawing on an SDL_Renderer: text through a texture cache, rectangles, meters, LEDs.
#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <map>
#include <cstdint>

struct Color { uint8_t r, g, b, a = 255; };
inline Color dim(Color c, float k) { return {(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k), c.a}; }
inline Color withA(Color c, uint8_t a) { c.a = a; return c; }

class Gfx {
public:
  enum Font { F_TINY, F_SMALL, F_LABEL, F_BOLD, F_BIG, F_HUGE, F_LOGO, F_MONO, NUM_FONTS };
  bool init(SDL_Renderer* r, const std::string& fontDir);
  void shutdown();
  SDL_Renderer* ren = nullptr;
  void text(const std::string& s, int x, int y, Color c, Font f = F_LABEL, int align = 0);   // 0 left 1 centre 2 right
  int textWidth(const std::string& s, Font f = F_LABEL);
  int textHeight(Font f);
  int wrapped(const std::string& s, int x, int y, int w, Color c, Font f, int maxLines = 4);    // returns lines drawn
  void rect(int x, int y, int w, int h, Color c);
  void frame(int x, int y, int w, int h, Color c, int t = 1);
  void roundRect(int x, int y, int w, int h, int rad, Color c);
  void circle(int cx, int cy, int r, Color c);
  void ring(int cx, int cy, int r, int t, Color c);
  void line(int x0, int y0, int x1, int y1, Color c);
  void meter(int x, int y, int w, int h, float v, Color on, Color off, bool vertical = false);
  void led(int cx, int cy, int r, bool on, Color c);
  void card(int x, int y, int w, int h, const std::string& title, Color accent);
private:
  TTF_Font* fonts[NUM_FONTS] = {};
  struct Key { std::string s; int f; uint32_t c; bool operator<(const Key& o) const { return c != o.c ? c < o.c : (f != o.f ? f < o.f : s < o.s); } };
  struct Entry { SDL_Texture* t; int w, h; unsigned age; };
  std::map<Key, Entry> cache; unsigned frameNo = 0;
  Entry& get(const std::string& s, Font f, Color c);
public:
  void newFrame();   // ages the cache
};
