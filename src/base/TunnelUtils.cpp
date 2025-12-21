#include "TunnelUtils.hpp"

namespace et {
void processEtStyleTunnelArg(vector<PortForwardSourceRequest>& pfsrs,
                             const vector<string> sourceDestination,
                             const string& input) {
  if (sourceDestination.size() < 2) {
    throw TunnelParseException(
        "Tunnel argument must have source and destination between a ':'");
  }
  try {
    if (sourceDestination[0].find_first_not_of("0123456789-") != string::npos &&
        sourceDestination[1].find_first_not_of("0123456789-") != string::npos) {
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

// This is necessary rather than using simply split with ":" due to the fact
// that ipv6 addresses must be within square brackets for the ssh-style
// tunneling args
vector<string> parseSshTunnelArg(const string& input) {
  const char colon = ':';
  const char l_bracket = '[';
  const char r_bracket = ']';

  bool inBrackets = false;
  string currentPart;
  vector<string> sshArgParts;

  for (char c : input) {
    if (c == l_bracket) {
      inBrackets = true;
    } else if (c == r_bracket) {
      inBrackets = false;
    } else if (c == colon && !inBrackets) {
      sshArgParts.push_back(currentPart);
      currentPart.clear();
    } else {
      currentPart += c;
    }
  }
  // pushback last part
  sshArgParts.push_back(currentPart);
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
  if (splitByComma.size() > 1) {
    for (auto& element : splitByComma) {
      vector<string> sourceDestination = split(element, ':');
      processEtStyleTunnelArg(pfsrs, sourceDestination, input);
    }
  } else {
    // no commas
    auto tunnelArg = splitByComma[0];
    vector<string> sourceDestination = split(tunnelArg, ':');
    if (sourceDestination.size() <= 2) {
      // et style tunnel arg
      processEtStyleTunnelArg(pfsrs, sourceDestination, input);
    } else {
      // ssh style tunnel arg
      // -L [bind_address:]port:host:hostport (supported with bind_address)
      // -L [bind_address:]port:remote_socket (not supported yet)
      // -L local_socket:host:hostport (not supported yet)
      // -L local_socket:remote_socket (not supported yet)
      auto sshStyleArgParts = parseSshTunnelArg(tunnelArg);
      PortForwardSourceRequest pfsr;
      pfsr.mutable_source()->set_name(sshStyleArgParts[0]);
      pfsr.mutable_source()->set_port(stoi(sshStyleArgParts[1]));
      pfsr.mutable_destination()->set_name(sshStyleArgParts[2]);
      pfsr.mutable_destination()->set_port(stoi(sshStyleArgParts[3]));
      pfsrs.push_back(pfsr);
    }
  }
  return pfsrs;
}

}  // namespace et
