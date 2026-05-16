#ifndef __ET_HOST_PARSING__
#define __ET_HOST_PARSING__

#include "Headers.hpp"

namespace et {

// Parsed components of a [user@]host[:port] string
struct ParsedHostString {
  string user;
  string host;
  string portSuffix;  // includes colon, e.g. ":22"
};

// Parse a host string in [user@]host[:port] format
// Handles IPv6 addresses in bracket notation: [::1], [::1]:22, user@[::1]:22
inline ParsedHostString parseHostString(const string& hostString) {
  ParsedHostString result;
  string remaining = hostString;

  // Extract user@ prefix if present
  size_t atIndex = remaining.find("@");
  if (atIndex != string::npos) {
    result.user = remaining.substr(0, atIndex);
    remaining = remaining.substr(atIndex + 1);
  }

  // Handle IPv6 addresses in bracket notation: [ipv6]:port
  if (!remaining.empty() && remaining[0] == '[') {
    size_t closeBracket = remaining.find(']');
    if (closeBracket != string::npos) {
      // Extract IPv6 address including brackets
      result.host = remaining.substr(0, closeBracket + 1);
      // Check for :port after the closing bracket
      if (closeBracket + 1 < remaining.length() &&
          remaining[closeBracket + 1] == ':') {
        result.portSuffix = remaining.substr(closeBracket + 1);
      }
    } else {
      // Malformed: opening bracket without closing, treat as-is
      result.host = remaining;
    }
  } else {
    // Non-IPv6: extract :port suffix if present
    size_t colonIndex = remaining.find(":");
    if (colonIndex != string::npos) {
      result.portSuffix = remaining.substr(colonIndex);  // ":port"
      remaining = remaining.substr(0, colonIndex);
    }
    result.host = remaining;
  }

  return result;
}

}  // namespace et

#endif  // __ET_HOST_PARSING__
