#include "TunnelUtils.hpp"

namespace et {
vector<PortForwardSourceRequest> parseRangesToRequests(const string& input) {
  vector<PortForwardSourceRequest> pfsrs;
  auto j = split(input, ',');
  for (auto& pair : j) {
    vector<string> sourceDestination = split(pair, ':');
    if (sourceDestination.size() < 2) {
      throw TunnelParseException(
          "Tunnel argument must have source and destination between a ':'");
    }
    try {
      if (sourceDestination[0].find_first_not_of("0123456789-") !=
              string::npos &&
          sourceDestination[1].find_first_not_of("0123456789-") !=
              string::npos) {
        PortForwardSourceRequest pfsr;
        pfsr.set_environmentvariable(sourceDestination[0]);
        pfsr.mutable_destination()->set_name(sourceDestination[1]);
        pfsrs.push_back(pfsr);
      } else if (sourceDestination[0].find('-') != string::npos &&
                 sourceDestination[1].find('-') != string::npos) {
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
        PortForwardSourceRequest pfsr;
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
  return pfsrs;
}

}  // namespace et
