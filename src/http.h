// autojam: a minimal HTTP/1.1 client over a POSIX socket, enough for the two local servers (llama-server and
// tts-server on 127.0.0.1): POST a JSON body, then read the answer as a stream of bytes (Content-Length or
// chunked). The language model's answer is read line by line (SSE), the voice's as raw PCM chunks.
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

namespace http {

struct Url { std::string host; int port = 80; std::string path; };
inline Url parseUrl(const std::string& u) {
  Url r; std::string s = u;
  if (s.rfind("http://", 0) == 0) s = s.substr(7);
  size_t slash = s.find('/'); std::string hp = slash == std::string::npos ? s : s.substr(0, slash);
  r.path = slash == std::string::npos ? "/" : s.substr(slash);
  size_t colon = hp.find(':');
  r.host = colon == std::string::npos ? hp : hp.substr(0, colon);
  r.port = colon == std::string::npos ? 80 : atoi(hp.c_str() + colon + 1);
  if (r.path.size() > 1 && r.path.back() == '/') r.path.pop_back();
  return r;
}

class Response {
public:
  int status = 0;
  std::string error;
  ~Response() { close(); }
  void close() { if (fd >= 0) { ::close(fd); fd = -1; } }
  bool ok() const { return fd >= 0 && error.empty(); }
  // next piece of the body (empty at the end or on error); blocks up to timeoutMs for data
  std::string read(int timeoutMs = 30000) {
    if (fd < 0 || done) return "";
    if (chunked) {
      if (chunkLeft == 0) {
        std::string line; if (!readLine(line, timeoutMs)) { done = true; return ""; }
        if (line.empty() && !readLine(line, timeoutMs)) { done = true; return ""; }   // the CRLF after the last chunk's data
        chunkLeft = strtol(line.c_str(), nullptr, 16);
        if (chunkLeft == 0) { done = true; return ""; }
      }
      std::string out = take(chunkLeft, timeoutMs);
      chunkLeft -= (long)out.size();
      if (out.empty()) done = true;
      return out;
    }
    if (length >= 0) {
      if (length == 0) { done = true; return ""; }
      std::string out = take(length, timeoutMs); length -= (long)out.size();
      if (out.empty()) done = true;
      return out;
    }
    std::string out = take(65536, timeoutMs);     // until the server closes
    if (out.empty()) done = true;
    return out;
  }
  // a whole body
  std::string all(int timeoutMs = 30000) { std::string s; for (;;) { std::string p = read(timeoutMs); if (p.empty()) break; s += p; } return s; }
  // one text line (without the newline); false at the end
  bool line(std::string& out, int timeoutMs = 30000) {
    for (;;) {
      size_t nl = lineBuf.find('\n');
      if (nl != std::string::npos) { out = lineBuf.substr(0, nl); lineBuf.erase(0, nl + 1); if (!out.empty() && out.back() == '\r') out.pop_back(); return true; }
      std::string p = read(timeoutMs);
      if (p.empty()) { if (lineBuf.empty()) return false; out = lineBuf; lineBuf.clear(); return true; }
      lineBuf += p;
    }
  }

private:
  friend class Client;
  int fd = -1; bool chunked = false, done = false; long length = -1, chunkLeft = 0;
  std::string buf, lineBuf;
  bool fill(int timeoutMs) {
    fd_set r; FD_ZERO(&r); FD_SET(fd, &r); timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int s = select(fd + 1, &r, nullptr, nullptr, &tv);
    if (s <= 0) { if (s == 0) error = "timeout"; return false; }
    char tmp[16384]; ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n); return true;
  }
  std::string take(long maxBytes, int timeoutMs) {
    if (buf.empty() && !fill(timeoutMs)) return "";
    size_t n = std::min((size_t)maxBytes, buf.size());
    std::string out = buf.substr(0, n); buf.erase(0, n); return out;
  }
  bool readLine(std::string& out, int timeoutMs) {
    for (;;) {
      size_t nl = buf.find("\r\n");
      if (nl != std::string::npos) { out = buf.substr(0, nl); buf.erase(0, nl + 2); return true; }
      if (!fill(timeoutMs)) return false;
    }
  }
};

class Client {
public:
  // Sends the request; the response headers are read, the body is left for Response::read.
  static bool request(const std::string& base, const std::string& method, const std::string& path, const std::string& body,
                      Response& res, int connectTimeoutMs = 2000, const std::string& contentType = "application/json") {
    Url u = parseUrl(base);
    res.close(); res.status = 0; res.error.clear(); res.done = false; res.chunked = false; res.length = -1; res.chunkLeft = 0; res.buf.clear(); res.lineBuf.clear();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { res.error = "socket"; return false; }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)u.port);
    if (inet_pton(AF_INET, u.host.c_str(), &a.sin_addr) != 1) {
      hostent* h = gethostbyname(u.host.c_str());
      if (!h) { ::close(fd); res.error = "resolve " + u.host; return false; }
      memcpy(&a.sin_addr, h->h_addr, sizeof a.sin_addr);
    }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (::connect(fd, (sockaddr*)&a, sizeof a) < 0 && errno != EINPROGRESS) { ::close(fd); res.error = "connect"; return false; }
    fd_set w; FD_ZERO(&w); FD_SET(fd, &w); timeval tv{connectTimeoutMs / 1000, (connectTimeoutMs % 1000) * 1000};
    if (select(fd + 1, nullptr, &w, nullptr, &tv) <= 0) { ::close(fd); res.error = "connect timeout"; return false; }
    int err = 0; socklen_t el = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { ::close(fd); res.error = std::string("connect: ") + strerror(err); return false; }
    fcntl(fd, F_SETFL, fl);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    std::string p = u.path == "/" ? path : u.path + path;
    std::string req = method + " " + p + " HTTP/1.1\r\nHost: " + u.host + "\r\nConnection: close\r\nAccept: */*\r\n";
    if (!body.empty() || method == "POST") req += "Content-Type: " + contentType + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n"; req += body;
    size_t off = 0;
    while (off < req.size()) { ssize_t n = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL); if (n <= 0) { ::close(fd); res.error = "send"; return false; } off += (size_t)n; }
    res.fd = fd;
    std::string line;
    if (!res.readLine(line, 30000)) { res.error = res.error.empty() ? "no status line" : res.error; res.close(); return false; }
    if (line.size() < 12 || line.compare(0, 5, "HTTP/") != 0) { res.error = "bad status line"; res.close(); return false; }
    res.status = atoi(line.c_str() + 9);
    for (;;) {
      if (!res.readLine(line, 30000)) { res.error = "headers"; res.close(); return false; }
      if (line.empty()) break;
      std::string low = line; for (auto& c : low) c = (char)tolower((unsigned char)c);
      if (low.rfind("content-length:", 0) == 0) res.length = atol(line.c_str() + 15);
      else if (low.rfind("transfer-encoding:", 0) == 0 && low.find("chunked") != std::string::npos) res.chunked = true;
    }
    return true;
  }
  static bool get(const std::string& base, const std::string& path, std::string& out, int timeoutMs = 3000) {
    Response r; if (!request(base, "GET", path, "", r, timeoutMs)) return false;
    out = r.all(timeoutMs); return r.status >= 200 && r.status < 300;
  }
  static bool post(const std::string& base, const std::string& path, const std::string& body, std::string& out, int timeoutMs = 30000) {
    Response r; if (!request(base, "POST", path, body, r)) return false;
    out = r.all(timeoutMs); return r.status >= 200 && r.status < 300;
  }
};

} // namespace http
