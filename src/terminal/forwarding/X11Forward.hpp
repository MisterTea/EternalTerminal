#pragma once

#include <chrono>
#include <string_view>

#include "ET.pb.h"
#include "Headers.hpp"

namespace et {

inline constexpr std::string_view X11ForwardPrefix = "et-x11:";
inline constexpr size_t X11ForwardTokenSize = 32;
inline constexpr char X11AuthProtocol[] = "MIT-MAGIC-COOKIE-1";
inline constexpr size_t X11CookieSize = 16;
inline constexpr auto X11UntrustedTimeout = std::chrono::minutes(20);

struct X11Forward {
  SocketEndpoint endpoint;
  string destination;
  string localCookie;
  string proxyCookie;
  int screen = 0;
  std::chrono::steady_clock::time_point expires =
      std::chrono::steady_clock::time_point::max();
};

// The X11 setup header and authorization can arrive in separate ET packets.
class X11Setup {
 public:
  explicit X11Setup(const X11Forward& forward) : forward(forward) {}

  bool rewrite(const string& data, string* output) {
    if (complete) {
      *output = data;
      return true;
    }
    pending += data;
    if (pending.size() < 12) {
      return true;
    }
    const bool little = pending[0] == 'l';
    if (!little && pending[0] != 'B') {
      return false;
    }
    auto read16 = [&](size_t offset) {
      unsigned first = static_cast<unsigned char>(pending[offset]);
      unsigned second = static_cast<unsigned char>(pending[offset + 1]);
      return little ? first | (second << 8) : (first << 8) | second;
    };
    constexpr size_t protocolSize = sizeof(X11AuthProtocol) - 1;
    constexpr size_t cookieOffset = 12 + ((protocolSize + 3) & ~size_t(3));
    if (read16(2) != 11 || read16(6) != protocolSize ||
        read16(8) != X11CookieSize) {
      return false;
    }
    if (pending.size() < cookieOffset + X11CookieSize) {
      return true;
    }
    if (pending.compare(12, protocolSize, X11AuthProtocol) != 0 ||
        sodium_memcmp(pending.data() + cookieOffset, forward.proxyCookie.data(),
                      X11CookieSize) != 0 ||
        std::chrono::steady_clock::now() >= forward.expires) {
      return false;
    }
    pending.replace(cookieOffset, X11CookieSize, forward.localCookie);
    *output = std::move(pending);
    complete = true;
    return true;
  }

 private:
  const X11Forward& forward;
  string pending;
  bool complete = false;
};

}  // namespace et
