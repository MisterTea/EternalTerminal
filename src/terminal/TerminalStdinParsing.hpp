#ifndef __ET_TERMINAL_STDIN_PARSING__
#define __ET_TERMINAL_STDIN_PARSING__

#include "Headers.hpp"

namespace et {

// The line the client pipes into etterminal: "<id>/<passkey>_<TERM>"
struct TerminalStdinLine {
  string idpasskey;
  string term;
};

// Splits the stdin line at the first underscore. The id and passkey are
// alphanumeric, so everything after that underscore is the client's TERM,
// which may itself contain underscores (for example "xterm_256color").
// Returns false when either half is missing.
inline bool parseTerminalStdinLine(const string& line,
                                   TerminalStdinLine* result) {
  size_t underscore = line.find('_');
  if (underscore == string::npos || underscore == 0 ||
      underscore + 1 == line.length()) {
    return false;
  }
  result->idpasskey = line.substr(0, underscore);
  result->term = line.substr(underscore + 1);
  return true;
}

}  // namespace et

#endif  // __ET_TERMINAL_STDIN_PARSING__
