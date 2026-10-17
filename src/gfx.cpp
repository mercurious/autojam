#include "gfx.h"
#include <cmath>
#include <vector>

bool Gfx::init(SDL_Renderer* r, const std::string& fontDir) {
  ren = r;
  if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return false; }
  std::string reg = fontDir + "/LiberationSans-Regular.ttf", bold = fontDir + "/LiberationSans-Bold.ttf";
  int sizes[NUM_FONTS] = {15, 19, 22, 28, 46, 110, 40, 18};
  for (int i = 0; i < NUM_FONTS; i++) {
    bool b = i == F_LABEL || i == F_BOLD || i == F_BIG || i == F_HUGE || i == F_LOGO;
    fonts[i] = TTF_OpenFont((b ? bold : reg).c_str(), sizes[i]);
    if (!fonts[i]) { fprintf(stderr, "font load failed in %s: %s\n", fontDir.c_str(), TTF_GetError()); return false; }
  }
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  return true;
}
void Gfx::shutdown() { for (auto& kv : cache) SDL_DestroyTexture(kv.second.t); cache.clear(); for (auto& f : fonts) if (f) TTF_CloseFont(f); TTF_Quit(); }
void Gfx::newFrame() {
  frameNo++;
  if (frameNo % 120 == 0) for (auto it = cache.begin(); it != cache.end();) { if (frameNo - it->second.age > 600) { SDL_DestroyTexture(it->second.t); it = cache.erase(it); } else ++it; }
}
Gfx::Entry& Gfx::get(const std::string& s, Font f, Color c) {
  Key k{s, f, (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | c.a};
  auto it = cache.find(k);
  if (it != cache.end()) { it->second.age = frameNo; return it->second; }
  SDL_Color col{c.r, c.g, c.b, 255};
  SDL_Surface* surf = TTF_RenderUTF8_Blended(fonts[f], s.empty() ? " " : s.c_str(), col);
  Entry e{nullptr, 0, 0, frameNo};
  if (surf) { e.t = SDL_CreateTextureFromSurface(ren, surf); e.w = surf->w; e.h = surf->h; SDL_FreeSurface(surf); if (e.t) { SDL_SetTextureBlendMode(e.t, SDL_BLENDMODE_BLEND); SDL_SetTextureAlphaMod(e.t, c.a); } }
  return cache[k] = e;
}
void Gfx::text(const std::string& s, int x, int y, Color c, Font f, int align) {
  Entry& e = get(s, f, c); if (!e.t) return;
  SDL_Rect d{align == 1 ? x - e.w / 2 : (align == 2 ? x - e.w : x), y, e.w, e.h};
  SDL_RenderCopy(ren, e.t, nullptr, &d);
}
int Gfx::textWidth(const std::string& s, Font f) { int w = 0, h = 0; TTF_SizeUTF8(fonts[f], s.c_str(), &w, &h); return w; }
int Gfx::textHeight(Font f) { return TTF_FontHeight(fonts[f]); }
int Gfx::wrapped(const std::string& s, int x, int y, int w, Color c, Font f, int maxLines) {
  std::vector<std::string> words; std::string cur;
  for (char ch : s) { if (ch == ' ' || ch == '\n') { if (!cur.empty()) words.push_back(cur); cur.clear(); if (ch == '\n') words.push_back("\n"); } else cur += ch; }
  if (!cur.empty()) words.push_back(cur);
  int lines = 0, lh = textHeight(f); std::string line;
  auto flush = [&]() { if (lines < maxLines) text(line, x, y + lines * lh, c, f); lines++; line.clear(); };
  for (auto& wd : words) {
    if (wd == "\n") { flush(); continue; }
    std::string t = line.empty() ? wd : line + " " + wd;
    if (textWidth(t, f) > w && !line.empty()) { flush(); line = wd; } else line = t;
  }
  if (!line.empty()) flush();
  return std::min(lines, maxLines);
}
void Gfx::rect(int x, int y, int w, int h, Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); SDL_Rect r{x, y, w, h}; SDL_RenderFillRect(ren, &r); }
void Gfx::frame(int x, int y, int w, int h, Color c, int t) { for (int i = 0; i < t; i++) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); SDL_Rect r{x + i, y + i, w - 2 * i, h - 2 * i}; SDL_RenderDrawRect(ren, &r); } }
void Gfx::roundRect(int x, int y, int w, int h, int rad, Color c) {
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
  rad = std::min(rad, std::min(w, h) / 2);
  for (int i = 0; i < h; i++) {
    int inset = 0;
    if (i < rad) { float dy = rad - i - 0.5f; inset = rad - (int)sqrtf(std::max(0.0f, rad * rad - dy * dy)); }
    else if (i >= h - rad) { float dy = i - (h - rad) + 0.5f; inset = rad - (int)sqrtf(std::max(0.0f, rad * rad - dy * dy)); }
    SDL_RenderDrawLine(ren, x + inset, y + i, x + w - 1 - inset, y + i);
  }
}
void Gfx::circle(int cx, int cy, int r, Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); for (int dy = -r; dy <= r; dy++) { int dx = (int)sqrtf((float)(r * r - dy * dy)); SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy); } }
void Gfx::ring(int cx, int cy, int r, int t, Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); for (int k = 0; k < t; k++) { int rr = r - k; int segs = std::max(24, rr * 2); float px = cx + rr, py = (float)cy; for (int i = 1; i <= segs; i++) { float a = 6.2831853f * i / segs; float x = cx + rr * cosf(a), y = cy + rr * sinf(a); SDL_RenderDrawLine(ren, (int)px, (int)py, (int)x, (int)y); px = x; py = y; } } }
void Gfx::line(int x0, int y0, int x1, int y1, Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); SDL_RenderDrawLine(ren, x0, y0, x1, y1); }
void Gfx::meter(int x, int y, int w, int h, float v, Color on, Color off, bool vertical) {
  v = v < 0 ? 0 : (v > 1 ? 1 : v);
  roundRect(x, y, w, h, 3, off);
  if (vertical) { int hh = (int)(h * v); if (hh > 0) roundRect(x, y + h - hh, w, hh, 3, on); }
  else { int ww = (int)(w * v); if (ww > 0) roundRect(x, y, ww, h, 3, on); }
}
void Gfx::led(int cx, int cy, int r, bool on, Color c) { if (on) { circle(cx, cy, r + 4, withA(c, 60)); circle(cx, cy, r, c); } else { circle(cx, cy, r, dim(c, 0.25f)); } }
void Gfx::card(int x, int y, int w, int h, const std::string& title, Color accent) {
  roundRect(x, y, w, h, 12, {22, 24, 30, 255});
  rect(x, y + 12, 4, h - 24, accent);
  if (!title.empty()) text(title, x + 18, y + 10, dim(accent, 0.95f), F_SMALL);
}
