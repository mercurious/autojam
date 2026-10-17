// autojam: a small JSON reader and writer (RFC 8259 subset: full strings with escapes and \uXXXX, numbers,
// nested arrays and objects). Used for the model's plans, the servers' answers and our settings file.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace json {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> a;
  std::map<std::string, Value> o;

  static Value num(double v) { Value x; x.type = Number; x.n = v; return x; }
  static Value boolean(bool v) { Value x; x.type = Bool; x.b = v; return x; }
  static Value str(const std::string& v) { Value x; x.type = String; x.s = v; return x; }
  static Value arr() { Value x; x.type = Array; return x; }
  static Value obj() { Value x; x.type = Object; return x; }
  bool isNull() const { return type == Null; }
  const Value* get(const char* k) const { if (type != Object) return nullptr; auto it = o.find(k); return it == o.end() ? nullptr : &it->second; }
  double numOr(const char* k, double d) const { const Value* v = get(k); return (v && v->type == Number) ? v->n : d; }
  int intOr(const char* k, int d) const { return (int)std::lround(numOr(k, d)); }
  bool boolOr(const char* k, bool d) const { const Value* v = get(k); return (v && v->type == Bool) ? v->b : d; }
  std::string strOr(const char* k, const std::string& d) const { const Value* v = get(k); return (v && v->type == String) ? v->s : d; }
  const std::vector<Value>& arrOf(const char* k) const { static const std::vector<Value> none; const Value* v = get(k); return (v && v->type == Array) ? v->a : none; }
  Value& operator[](const char* k) { type = Object; return o[k]; }
  void push(Value v) { type = Array; a.push_back(std::move(v)); }
};

inline void writeStr(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; } else out += (char)c;
    }
  }
  out += '"';
}

inline void write(std::string& out, const Value& v) {
  switch (v.type) {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.b ? "true" : "false"; break;
    case Value::Number: {
      char b[32];
      if (std::isfinite(v.n) && v.n == (double)(long long)v.n && std::fabs(v.n) < 1e15) snprintf(b, sizeof b, "%lld", (long long)v.n);
      else snprintf(b, sizeof b, "%.6g", v.n);
      out += b; break;
    }
    case Value::String: writeStr(out, v.s); break;
    case Value::Array:
      out += '[';
      for (size_t i = 0; i < v.a.size(); i++) { if (i) out += ','; write(out, v.a[i]); }
      out += ']'; break;
    case Value::Object: {
      out += '{'; bool first = true;
      for (auto& kv : v.o) { if (!first) out += ','; first = false; writeStr(out, kv.first); out += ':'; write(out, kv.second); }
      out += '}'; break;
    }
  }
}
inline std::string dump(const Value& v) { std::string s; write(s, v); return s; }

struct Reader {
  const char* p; const char* end;
  Reader(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}
  void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool lit(const char* s) { size_t n = strlen(s); if ((size_t)(end - p) >= n && !strncmp(p, s, n)) { p += n; return true; } return false; }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  }
  bool hex4(unsigned& v) {
    if (end - p < 4) return false; v = 0;
    for (int i = 0; i < 4; i++) { char c = *p++; v <<= 4; if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false; }
    return true;
  }
  bool string(std::string& out) {
    if (p >= end || *p != '"') return false; p++;
    while (p < end) {
      char c = *p++;
      if (c == '"') return true;
      if (c != '\\') { out += c; continue; }
      if (p >= end) return false;
      char e = *p++;
      switch (e) {
        case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
        case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
        case 'r': out += '\r'; break; case 't': out += '\t'; break;
        case 'u': {
          unsigned cp; if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') { p += 2; unsigned lo; if (!hex4(lo)) return false; cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
          utf8(out, cp); break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(Value& v, int depth = 0) {
    if (depth > 64) return false;
    ws(); if (p >= end) return false;
    if (lit("null")) { v.type = Value::Null; return true; }
    if (lit("true")) { v.type = Value::Bool; v.b = true; return true; }
    if (lit("false")) { v.type = Value::Bool; v.b = false; return true; }
    if (*p == '"') { v.type = Value::String; return string(v.s); }
    if (*p == '[') {
      p++; v.type = Value::Array; ws();
      if (p < end && *p == ']') { p++; return true; }
      for (;;) { Value e; if (!value(e, depth + 1)) return false; v.a.push_back(std::move(e)); ws(); if (p < end && *p == ',') { p++; continue; } if (p < end && *p == ']') { p++; return true; } return false; }
    }
    if (*p == '{') {
      p++; v.type = Value::Object; ws();
      if (p < end && *p == '}') { p++; return true; }
      for (;;) { ws(); std::string k; if (!string(k)) return false; ws(); if (p >= end || *p != ':') return false; p++; Value e; if (!value(e, depth + 1)) return false; v.o[k] = std::move(e); ws(); if (p < end && *p == ',') { p++; continue; } if (p < end && *p == '}') { p++; return true; } return false; }
    }
    char* q = nullptr; double d = strtod(p, &q);
    if (q == p) return false;
    v.type = Value::Number; v.n = d; p = q; return true;
  }
};

inline bool parse(const std::string& text, Value& out) { Reader r(text); out = Value(); if (!r.value(out)) return false; r.ws(); return r.p == r.end; }
// the first {...} object inside a text (a model answer may wrap it in prose or a code fence)
inline bool parseEmbedded(const std::string& text, Value& out) {
  size_t a = text.find('{'), b = text.rfind('}');
  if (a == std::string::npos || b == std::string::npos || b < a) return false;
  return parse(text.substr(a, b - a + 1), out);
}

} // namespace json
