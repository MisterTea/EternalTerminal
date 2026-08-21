#ifndef __ET_TITLE_PARSER__
#define __ET_TITLE_PARSER__

#include <optional>
#include <string>

namespace et {

/** @brief Incrementally extracts sanitized OSC 0/2 terminal titles. */
class TitleParser {
 public:
  /** @brief Consumes bytes and returns the latest completed title, if any. */
  std::optional<std::string> parse(const std::string& bytes);

 private:
  enum class State {
    TEXT,
    ESCAPE,
    OSC_COMMAND,
    OSC_COMMAND_ESCAPE,
    OSC_TEXT,
    OSC_TEXT_ESCAPE,
  };

  void beginOsc();
  std::string sanitizeTitle() const;

  State state = State::TEXT;
  std::string command;
  std::string title;
  bool titleOsc = false;
};

}  // namespace et

#endif  // __ET_TITLE_PARSER__
