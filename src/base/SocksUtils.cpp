#include "SocksUtils.hpp"

namespace et {
namespace {

constexpr uint8_t SOCKS4_VERSION = 0x04;
constexpr uint8_t SOCKS4_CONNECT = 0x01;
constexpr uint8_t SOCKS4_GRANTED = 0x5a;

constexpr uint8_t SOCKS5_VERSION = 0x05;
constexpr uint8_t SOCKS5_NOAUTH = 0x00;
constexpr uint8_t SOCKS5_NO_ACCEPTABLE = 0xff;
constexpr uint8_t SOCKS5_CONNECT = 0x01;
constexpr uint8_t SOCKS5_IPV4 = 0x01;
constexpr uint8_t SOCKS5_DOMAIN = 0x03;
constexpr uint8_t SOCKS5_IPV6 = 0x04;
constexpr uint8_t SOCKS5_SUCCESS = 0x00;
constexpr uint8_t SOCKS4_REJECTED = 0x5b;
constexpr uint8_t SOCKS5_CONN_REFUSED = 0x05;
constexpr size_t kMaxSocksHandshake = 4096;
constexpr size_t kMaxSocksIdent = 255;

bool isSocketPath(const string& s) { return !s.empty() && s[0] == '/'; }

int parseTcpPort(const string& token, const string& context) {
  if (token.empty()) {
    throw TunnelParseException("Invalid port in " + context);
  }
  size_t consumed = 0;
  long value = 0;
  try {
    value = std::stol(token, &consumed, 10);
  } catch (const std::logic_error&) {
    throw TunnelParseException("Invalid port in " + context);
  }
  if (consumed != token.size() || value < 1 || value > 65535) {
    throw TunnelParseException("Invalid port in " + context);
  }
  return static_cast<int>(value);
}

SocksParseStatus parseSocks4(SocksHandshake* state) {
  const string& in = state->input;
  // VN(1) CD(1) DSTPORT(2) DSTIP(4) USERID(variable) NUL [DOMAIN NUL for 4a]
  if (in.size() < 8) {
    return SocksParseStatus::NeedMore;
  }
  if (static_cast<uint8_t>(in[1]) != SOCKS4_CONNECT) {
    state->error = "SOCKS4 only supports CONNECT";
    return SocksParseStatus::Error;
  }
  uint16_t port =
      (static_cast<uint8_t>(in[2]) << 8) | static_cast<uint8_t>(in[3]);
  const uint8_t* ip = reinterpret_cast<const uint8_t*>(in.data() + 4);
  bool socks4a = (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] != 0);

  size_t useridEnd = 8;
  while (useridEnd < in.size() && in[useridEnd] != '\0') {
    if (useridEnd - 8 >= kMaxSocksIdent) {
      state->error = "SOCKS4 userid too long";
      return SocksParseStatus::Error;
    }
    ++useridEnd;
  }
  if (useridEnd >= in.size()) {
    if (in.size() > kMaxSocksIdent + 8) {
      state->error = "SOCKS4 userid too long";
      return SocksParseStatus::Error;
    }
    return SocksParseStatus::NeedMore;
  }
  ++useridEnd;  // skip NUL

  string host;
  size_t consumed = useridEnd;
  if (socks4a) {
    size_t domainEnd = useridEnd;
    while (domainEnd < in.size() && in[domainEnd] != '\0') {
      if (domainEnd - useridEnd >= kMaxSocksIdent) {
        state->error = "SOCKS4a domain too long";
        return SocksParseStatus::Error;
      }
      ++domainEnd;
    }
    if (domainEnd >= in.size()) {
      if (in.size() - useridEnd > kMaxSocksIdent) {
        state->error = "SOCKS4a domain too long";
        return SocksParseStatus::Error;
      }
      return SocksParseStatus::NeedMore;
    }
    host = in.substr(useridEnd, domainEnd - useridEnd);
    consumed = domainEnd + 1;
  } else {
    char buf[INET_ADDRSTRLEN];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    host = buf;
  }

  if (isSocketPath(host)) {
    state->destination.set_name(host);
  } else {
    state->destination.set_name(host);
    state->destination.set_port(port);
  }

  // CONNECT success is written only after the destination accepts.
  state->version = SOCKS4_VERSION;
  state->earlyData = in.substr(consumed);
  state->input.clear();
  state->complete = true;
  return SocksParseStatus::Complete;
}

SocksParseStatus parseSocks5Auth(SocksHandshake* state) {
  const string& in = state->input;
  if (in.size() < 2) {
    return SocksParseStatus::NeedMore;
  }
  uint8_t nmethods = static_cast<uint8_t>(in[1]);
  if (in.size() < static_cast<size_t>(2 + nmethods)) {
    return SocksParseStatus::NeedMore;
  }
  bool foundNoAuth = false;
  for (uint8_t i = 0; i < nmethods; ++i) {
    if (static_cast<uint8_t>(in[2 + i]) == SOCKS5_NOAUTH) {
      foundNoAuth = true;
      break;
    }
  }
  state->reply.clear();
  state->reply.push_back(static_cast<char>(SOCKS5_VERSION));
  if (!foundNoAuth) {
    state->reply.push_back(static_cast<char>(SOCKS5_NO_ACCEPTABLE));
    state->error = "SOCKS5 client did not offer no-auth";
    return SocksParseStatus::Error;
  }
  state->reply.push_back(static_cast<char>(SOCKS5_NOAUTH));
  state->input.erase(0, 2 + nmethods);
  state->socks5AuthDone = true;
  return SocksParseStatus::NeedMore;
}

SocksParseStatus parseSocks5Request(SocksHandshake* state) {
  const string& in = state->input;
  // VER CMD RSV ATYP ...
  if (in.size() < 4) {
    return SocksParseStatus::NeedMore;
  }
  if (static_cast<uint8_t>(in[0]) != SOCKS5_VERSION) {
    state->error = "Invalid SOCKS5 request version";
    return SocksParseStatus::Error;
  }
  if (static_cast<uint8_t>(in[1]) != SOCKS5_CONNECT) {
    state->error = "SOCKS5 only supports CONNECT";
    return SocksParseStatus::Error;
  }
  uint8_t atyp = static_cast<uint8_t>(in[3]);
  string host;
  size_t portOffset = 0;
  if (atyp == SOCKS5_IPV4) {
    if (in.size() < 10) {
      return SocksParseStatus::NeedMore;
    }
    char buf[INET_ADDRSTRLEN];
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(in.data() + 4);
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    host = buf;
    portOffset = 8;
  } else if (atyp == SOCKS5_DOMAIN) {
    if (in.size() < 5) {
      return SocksParseStatus::NeedMore;
    }
    uint8_t len = static_cast<uint8_t>(in[4]);
    if (in.size() < static_cast<size_t>(5 + len + 2)) {
      return SocksParseStatus::NeedMore;
    }
    host = in.substr(5, len);
    portOffset = 5 + len;
  } else if (atyp == SOCKS5_IPV6) {
    if (in.size() < 22) {
      return SocksParseStatus::NeedMore;
    }
    char buf[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, in.data() + 4, buf, sizeof(buf)) == nullptr) {
      state->error = "Invalid SOCKS5 IPv6 address";
      return SocksParseStatus::Error;
    }
    host = buf;
    portOffset = 20;
  } else {
    state->error = "Unsupported SOCKS5 address type";
    return SocksParseStatus::Error;
  }

  uint16_t port = (static_cast<uint8_t>(in[portOffset]) << 8) |
                  static_cast<uint8_t>(in[portOffset + 1]);

  if (isSocketPath(host)) {
    state->destination.set_name(host);
  } else {
    state->destination.set_name(host);
    state->destination.set_port(port);
  }

  size_t consumed = portOffset + 2;
  state->version = SOCKS5_VERSION;
  state->earlyData = in.substr(consumed);
  state->input.clear();
  state->complete = true;
  return SocksParseStatus::Complete;
}

}  // namespace

string socksConnectReply(int version, bool success) {
  if (version == SOCKS4_VERSION) {
    string reply(8, '\0');
    reply[1] = static_cast<char>(success ? SOCKS4_GRANTED : SOCKS4_REJECTED);
    return reply;
  }
  string reply;
  reply.push_back(static_cast<char>(SOCKS5_VERSION));
  reply.push_back(
      static_cast<char>(success ? SOCKS5_SUCCESS : SOCKS5_CONN_REFUSED));
  reply.push_back(0);
  reply.push_back(static_cast<char>(SOCKS5_IPV4));
  reply.append(6, '\0');
  return reply;
}

SocksParseStatus feedSocksHandshake(SocksHandshake* state) {
  if (state->complete) {
    return SocksParseStatus::Complete;
  }
  if (state->input.size() > kMaxSocksHandshake) {
    state->error = "SOCKS handshake too large";
    return SocksParseStatus::Error;
  }
  if (state->input.empty()) {
    return SocksParseStatus::NeedMore;
  }

  uint8_t version = static_cast<uint8_t>(state->input[0]);
  if (version == SOCKS4_VERSION) {
    return parseSocks4(state);
  }
  if (version == SOCKS5_VERSION) {
    if (!state->socks5AuthDone) {
      auto status = parseSocks5Auth(state);
      string authReply = state->reply;
      state->reply.clear();
      if (status == SocksParseStatus::Error) {
        state->reply = authReply;
        return status;
      }
      if (state->input.empty()) {
        state->reply = authReply;
        return SocksParseStatus::NeedMore;
      }
      // Client pipelined the CONNECT request after auth methods.
      status = parseSocks5Request(state);
      state->reply = authReply + state->reply;
      return status;
    }
    return parseSocks5Request(state);
  }
  state->error = "Unsupported SOCKS version";
  return SocksParseStatus::Error;
}

SocketEndpoint parseDynamicForwardArg(const string& input) {
  if (input.empty()) {
    throw TunnelParseException("Dynamic forward (-D) requires [bind:]port");
  }

  SocketEndpoint endpoint;
  if (input.front() == '[') {
    auto close = input.find(']');
    if (close == string::npos || close + 1 >= input.size() ||
        input[close + 1] != ':') {
      throw TunnelParseException(
          "Dynamic forward IPv6 bind must look like [::1]:1080");
    }
    endpoint.set_name(input.substr(1, close - 1));
    endpoint.set_port(parseTcpPort(input.substr(close + 2), input));
    return endpoint;
  }

  auto colon = input.rfind(':');
  if (colon == string::npos) {
    endpoint.set_name("127.0.0.1");
    endpoint.set_port(parseTcpPort(input, input));
    return endpoint;
  }

  string host = input.substr(0, colon);
  string portStr = input.substr(colon + 1);
  if (host.empty()) {
    host = "0.0.0.0";
  }
  endpoint.set_name(host);
  endpoint.set_port(parseTcpPort(portStr, input));
  return endpoint;
}

SocketEndpoint parseStdioForwardArg(const string& input) {
  if (input.empty()) {
    throw TunnelParseException("Stdio forward (-W) requires host:port");
  }
  if (isSocketPath(input)) {
    SocketEndpoint endpoint;
    endpoint.set_name(input);
    return endpoint;
  }

  if (input.front() == '[') {
    auto close = input.find(']');
    if (close == string::npos || close + 1 >= input.size() ||
        input[close + 1] != ':') {
      throw TunnelParseException(
          "Stdio forward IPv6 destination must look like [::1]:8080");
    }
    SocketEndpoint endpoint;
    endpoint.set_name(input.substr(1, close - 1));
    endpoint.set_port(parseTcpPort(input.substr(close + 2), input));
    return endpoint;
  }

  auto colon = input.rfind(':');
  if (colon == string::npos) {
    throw TunnelParseException(
        "Stdio forward (-W) requires host:port or a Unix path");
  }
  SocketEndpoint endpoint;
  endpoint.set_name(input.substr(0, colon));
  endpoint.set_port(parseTcpPort(input.substr(colon + 1), input));
  if (endpoint.name().empty()) {
    throw TunnelParseException("Stdio forward host must not be empty");
  }
  return endpoint;
}

}  // namespace et
