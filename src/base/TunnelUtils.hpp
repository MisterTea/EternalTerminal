#ifndef __ET_TUNNEL_UTILS__
#define __ET_TUNNEL_UTILS__

#include "Headers.hpp"

namespace et {

/**
 * @brief Parses a comma-separated list of tunnel arguments into proto messages.
 * @throws TunnelParseException when the syntax is invalid.
 */
vector<PortForwardSourceRequest> parseRangesToRequests(const string& input);

vector<string> parseSshTunnelArg(const string& input);

/**
 * @brief Thrown when an invalid tunnel source/destination string is
 * encountered.
 */
class TunnelParseException : public std::exception {
 public:
  explicit TunnelParseException(const string& msg) : message(msg) {}
  const char* what() const noexcept override { return message.c_str(); }

 private:
  std::string message = " ";
};

}  // namespace et
#endif  // __ET_TUNNEL_UTILS__
