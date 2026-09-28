#include "TerminalHandler.hpp"

namespace et {
TerminalHandler::~TerminalHandler() { stop(); }

#define MAX_BUFFER_LINES (1024)
#define MAX_BUFFER_CHARS (128 * MAX_BUFFER_LINES)

string TerminalHandler::bufferOutput(const string& newChars) {
  vector<string> tokens = split(newChars, '\n');
  for (auto& it : tokens) {
    bufferLength += it.length();
  }
  if (buffer.empty()) {
    buffer.insert(buffer.end(), tokens.begin(), tokens.end());
  } else {
    buffer.back().append(tokens.front());
    if (tokens.size() > 1) {
      buffer.insert(buffer.end(), tokens.begin() + 1, tokens.end());
    }
  }
  if (buffer.size() > MAX_BUFFER_LINES) {
    int amountToErase = buffer.size() - MAX_BUFFER_LINES;
    for (auto it = buffer.begin();
         it != buffer.end() && it != (buffer.begin() + amountToErase); it++) {
      bufferLength -= it->length();
    }
    buffer.erase(buffer.begin(), buffer.begin() + amountToErase);
  }
  while (bufferLength > MAX_BUFFER_CHARS) {
    bufferLength -= buffer.begin()->length();
    buffer.pop_front();
  }
  VLOG(1) << "BUFFER LINES: " << buffer.size() << " " << tokens.size();
  return newChars;
}
}  // namespace et
