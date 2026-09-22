#ifndef __ET_SOCKS_UTILS__
#define __ET_SOCKS_UTILS__

#include "Headers.hpp"
#include "TunnelUtils.hpp"

namespace et {

/** @brief Outcome of feeding bytes into a SOCKS handshake parser. */
enum class SocksParseStatus {
  NeedMore,
  Complete,
  Error,
};

/**
 * @brief Incremental SOCKS4/SOCKS5 CONNECT parser for dynamic (-D) forwards.
 *
 * OpenSSH-compatible: SOCKS5 no-auth and SOCKS4/4a CONNECT only.
 */
struct SocksHandshake {
  string input;
  string reply;
  SocketEndpoint destination;
  string error;
  bool socks5AuthDone = false;
  bool complete = false;
};

/** @brief Consumes available bytes; may append a reply for the client. */
SocksParseStatus feedSocksHandshake(SocksHandshake* state);

/**
 * @brief Parses `-D [bind_address:]port` into a listen SocketEndpoint.
 * @throws TunnelParseException on invalid syntax.
 */
SocketEndpoint parseDynamicForwardArg(const string& input);

/**
 * @brief Parses `-W host:port` (or a Unix path) into a destination endpoint.
 * @throws TunnelParseException on invalid syntax.
 */
SocketEndpoint parseStdioForwardArg(const string& input);

}  // namespace et

#endif  // __ET_SOCKS_UTILS__
