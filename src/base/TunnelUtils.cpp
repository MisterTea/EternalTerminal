#include "TunnelUtils.hpp"

namespace et {

bool isSocketPath(const string& s) { return !s.empty() && s[0] == '/'; }

void processEtStyleTunnelArg(vector<PortForwardSourceRequest>& pfsrs,
                             const vector<string> sourceDestination,
                             const string& input) {
  if (sourceDestination.size() < 2) {
    throw TunnelParseException(
        "Tunnel argument must have source and destination between a ':'");
  }
  try {
    bool srcIsSocket = isSocketPath(sourceDestination[0]);
    bool srcIsNumeric =
        sourceDestination[0].find_first_not_of("0123456789-") == string::npos;
    bool dstIsSocket = isSocketPath(sourceDestination[1]);

    if (srcIsSocket || (dstIsSocket && srcIsNumeric)) {
      PortForwardSourceRequest pfsr;
      if (srcIsSocket) {
        pfsr.mutable_source()->set_name(sourceDestination[0]);
      } else {
        pfsr.mutable_source()->set_name("localhost");
        pfsr.mutable_source()->set_port(stoi(sourceDestination[0]));
      }
      if (dstIsSocket) {
        pfsr.mutable_destination()->set_name(sourceDestination[1]);
      } else {
        pfsr.mutable_destination()->set_port(stoi(sourceDestination[1]));
      }
      pfsrs.push_back(pfsr);
    } else if (sourceDestination[0].find_first_not_of("0123456789-") !=
                   string::npos &&
               sourceDestination[1].find_first_not_of("0123456789-") !=
                   string::npos) {
      // forwarding named pipes with environment variables (don't set source)
      PortForwardSourceRequest pfsr;
      pfsr.set_environmentvariable(sourceDestination[0]);
      pfsr.mutable_destination()->set_name(sourceDestination[1]);
      pfsrs.push_back(pfsr);
    } else if (sourceDestination[0].find('-') != string::npos &&
               sourceDestination[1].find('-') != string::npos) {
      // ranges
      vector<string> sourcePortRange = split(sourceDestination[0], '-');
      int sourcePortStart = stoi(sourcePortRange[0]);
      int sourcePortEnd = stoi(sourcePortRange[1]);

      vector<string> destinationPortRange = split(sourceDestination[1], '-');
      int destinationPortStart = stoi(destinationPortRange[0]);
      int destinationPortEnd = stoi(destinationPortRange[1]);

      if (sourcePortEnd - sourcePortStart !=
          destinationPortEnd - destinationPortStart) {
        throw TunnelParseException(
            "source/destination port range must have same length");
      } else {
        int portRangeLength = sourcePortEnd - sourcePortStart + 1;
        for (int i = 0; i < portRangeLength; ++i) {
          PortForwardSourceRequest pfsr;
          pfsr.mutable_source()->set_name("localhost");
          pfsr.mutable_source()->set_port(sourcePortStart + i);
          pfsr.mutable_destination()->set_port(destinationPortStart + i);
          pfsrs.push_back(pfsr);
        }
      }
    } else if (sourceDestination[0].find('-') != string::npos ||
               sourceDestination[1].find('-') != string::npos) {
      throw TunnelParseException(
          "Invalid port range syntax: if source is a range, "
          "destination must be a range (and vice versa)");
    } else {
      // normal port:port
      PortForwardSourceRequest pfsr;
      pfsr.mutable_source()->set_name("localhost");
      pfsr.mutable_source()->set_port(stoi(sourceDestination[0]));
      pfsr.mutable_destination()->set_port(stoi(sourceDestination[1]));
      pfsrs.push_back(pfsr);
    }
  } catch (const TunnelParseException& e) {
    throw e;
  } catch (const std::logic_error& lr) {
    throw TunnelParseException("Invalid tunnel argument '" + input +
                               "': " + lr.what());
  }
}

namespace {

bool isPortToken(const string& value) {
  return !value.empty() &&
         value.find_first_not_of("0123456789") == string::npos;
}

// Bracket-aware ':' split. Square brackets are not part of a field, so an
// IPv6 address stays one field.
vector<string> splitTunnelFields(const string& input) {
  const char colon = ':';
  const char l_bracket = '[';
  const char r_bracket = ']';

  bool inBrackets = false;
  string currentPart;
  vector<string> parts;
  for (char c : input) {
    if (c == l_bracket) {
      inBrackets = true;
    } else if (c == r_bracket) {
      inBrackets = false;
    } else if (c == colon && !inBrackets) {
      parts.push_back(currentPart);
      currentPart.clear();
    } else {
      currentPart += c;
    }
  }
  parts.push_back(currentPart);
  return parts;
}

void appendSshStyleTunnel(vector<PortForwardSourceRequest>& pfsrs,
                          const vector<string>& parts, const string& input) {
  if (parts.size() == 3) {
    // OpenSSH -L/-R port:host:hostport. Bind address defaults to localhost.
    if (!isPortToken(parts[0]) || !isPortToken(parts[2])) {
      throw TunnelParseException(
          "OpenSSH 3-field forward must be port:host:hostport, got '" + input +
          "'");
    }
    PortForwardSourceRequest pfsr;
    pfsr.mutable_source()->set_name("localhost");
    pfsr.mutable_source()->set_port(stoi(parts[0]));
    pfsr.mutable_destination()->set_name(parts[1]);
    pfsr.mutable_destination()->set_port(stoi(parts[2]));
    pfsrs.push_back(pfsr);
    return;
  }
  if (parts.size() != 4) {
    throw TunnelParseException(
        "Ipv6 addresses must be inside of square brackets, ie "
        "[::1]:8080:[::]:9090");
  }
  PortForwardSourceRequest pfsr;
  pfsr.mutable_source()->set_name(parts[0]);
  pfsr.mutable_source()->set_port(stoi(parts[1]));
  pfsr.mutable_destination()->set_name(parts[2]);
  pfsr.mutable_destination()->set_port(stoi(parts[3]));
  pfsrs.push_back(pfsr);
}

void appendOneTunnel(vector<PortForwardSourceRequest>& pfsrs,
                     const string& element) {
  vector<string> parts = splitTunnelFields(element);
  if (parts.size() <= 2) {
    processEtStyleTunnelArg(pfsrs, parts, element);
    return;
  }
  appendSshStyleTunnel(pfsrs, parts, element);
}

}  // namespace

// This is necessary rather than using simply split with ":" due to the fact
// that ipv6 addresses must be within square brackets for the ssh-style
// tunneling args
vector<string> parseSshTunnelArg(const string& input) {
  vector<string> sshArgParts = splitTunnelFields(input);
  if (sshArgParts.size() < 4) {
    throw TunnelParseException(
        "The 4 part ssh-style tunneling arg (bind_address:port:host:hostport) "
        "must be supplied.");
  }
  if (sshArgParts.size() > 4) {
    throw TunnelParseException(
        "Ipv6 addresses must be inside of square brackets, ie "
        "[::1]:8080:[::]:9090");
  }
  return sshArgParts;
}

vector<PortForwardSourceRequest> parseRangesToRequests(const string& input) {
  vector<PortForwardSourceRequest> pfsrs;
  auto splitByComma = split(input, ',');
  for (auto& element : splitByComma) {
    // -L [bind_address:]port:host:hostport
    // -L port:host:hostport (bind address defaults to localhost)
    // Socket forms other than ET's two-field syntax are not supported.
    appendOneTunnel(pfsrs, element);
  }
  return pfsrs;
}

}  // namespace et
