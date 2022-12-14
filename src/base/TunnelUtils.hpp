#ifndef __ET_TUNNEL_UTILS__
#define __ET_TUNNEL_UTILS__

#include "ETerminal.pb.h"

namespace et {

vector<PortForwardSourceRequest> parseRangesToRequests(const string& input);

class TunnelParseException : public std::exception {
 public:
  TunnelParseException(const string& msg) : message(msg) {}
  const char* what() const noexcept override { return message.c_str(); }

 private:
  std::string message = " ";
};

}  // namespace et
#endif  // __ET_TUNNEL_UTILS__
