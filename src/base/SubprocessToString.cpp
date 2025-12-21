#include "SubprocessToString.hpp"

namespace et {

// Backward compatibility wrapper
string SubprocessToStringInteractive(const string& command,
                                     const vector<string>& args) {
  SubprocessUtils utils;
  return utils.SubprocessToStringInteractive(command, args);
}

}  // namespace et
