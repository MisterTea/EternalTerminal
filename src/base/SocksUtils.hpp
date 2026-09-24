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
  /** @brief 4 or 5 once CONNECT parsing finishes. */
  int version = 0;
  /**
   * @brief Application bytes that arrived after the CONNECT request, either
   * pipelined in the same buffer or read while still awaiting
   * takeCompletedSocks.
   */
  string earlyData;
};

/**
 * @brief CONNECT reply. Success is deferred until the remote destination
 * accepts; failure is sent when that request errors.
 */
string socksConnectReply(int version, bool success);

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
