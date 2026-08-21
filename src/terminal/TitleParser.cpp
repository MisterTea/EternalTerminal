#include "TitleParser.hpp"

#include <algorithm>

namespace et {
namespace {
constexpr unsigned char kEscape = 0x1b;
constexpr unsigned char kBell = 0x07;
constexpr size_t kMaxTitleBytes = 80;
constexpr size_t kMaxBufferedTitleBytes = 1024;

bool isUtf8Continuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

size_t utf8SequenceLength(unsigned char byte) {
  if ((byte & 0x80) == 0) {
    return 1;
  }
  if ((byte & 0xe0) == 0xc0) {
    return 2;
  }
  if ((byte & 0xf0) == 0xe0) {
    return 3;
  }
  if ((byte & 0xf8) == 0xf0) {
    return 4;
  }
  return 0;
}

bool decodeUtf8(const std::string& bytes, size_t offset, size_t* length,
                uint32_t* codePoint) {
  const unsigned char first = static_cast<unsigned char>(bytes[offset]);
  if (first <= 0x7f) {
    *length = 1;
    *codePoint = first;
    return true;
  }

  size_t expectedLength;
  uint32_t value;
  if (first >= 0xc2 && first <= 0xdf) {
    expectedLength = 2;
    value = first & 0x1f;
  } else if (first >= 0xe0 && first <= 0xef) {
    expectedLength = 3;
    value = first & 0x0f;
  } else if (first >= 0xf0 && first <= 0xf4) {
    expectedLength = 4;
    value = first & 0x07;
  } else {
    return false;
  }
  if (offset + expectedLength > bytes.size()) {
    return false;
  }

  const unsigned char second = static_cast<unsigned char>(bytes[offset + 1]);
  if (!isUtf8Continuation(second) || (first == 0xe0 && second < 0xa0) ||
      (first == 0xed && second >= 0xa0) || (first == 0xf0 && second < 0x90) ||
      (first == 0xf4 && second >= 0x90)) {
    return false;
  }
  for (size_t i = 1; i < expectedLength; ++i) {
    const unsigned char continuation =
        static_cast<unsigned char>(bytes[offset + i]);
    if (!isUtf8Continuation(continuation)) {
      return false;
    }
    value = (value << 6) | (continuation & 0x3f);
  }
  *length = expectedLength;
  *codePoint = value;
  return true;
}
}  // namespace

void TitleParser::beginOsc() {
  state = State::OSC_COMMAND;
  command.clear();
  title.clear();
  titleOsc = false;
}

std::string TitleParser::sanitizeTitle() const {
  std::string sanitized;
  sanitized.reserve(std::min(title.size(), kMaxTitleBytes));
  for (size_t offset = 0; offset < title.size();) {
    size_t length;
    uint32_t codePoint;
    if (!decodeUtf8(title, offset, &length, &codePoint)) {
      ++offset;
      continue;
    }
    if (codePoint >= 0x20 && codePoint != 0x7f &&
        !(codePoint >= 0x80 && codePoint <= 0x9f)) {
      sanitized.append(title, offset, length);
    }
    offset += length;
  }
  if (sanitized.size() <= kMaxTitleBytes) {
    return sanitized;
  }

  sanitized.resize(kMaxTitleBytes);
  size_t lastCharacter = sanitized.size() - 1;
  while (lastCharacter > 0 && isUtf8Continuation(static_cast<unsigned char>(
                                  sanitized[lastCharacter]))) {
    --lastCharacter;
  }
  const size_t sequenceLength =
      utf8SequenceLength(static_cast<unsigned char>(sanitized[lastCharacter]));
  if (sequenceLength == 0 ||
      lastCharacter + sequenceLength > sanitized.size()) {
    sanitized.resize(lastCharacter);
  }
  return sanitized;
}

std::optional<std::string> TitleParser::parse(const std::string& bytes) {
  std::optional<std::string> latestTitle;
  for (unsigned char byte : bytes) {
    switch (state) {
      case State::TEXT:
        if (byte == kEscape) {
          state = State::ESCAPE;
        }
        break;
      case State::ESCAPE:
        if (byte == ']') {
          beginOsc();
        } else if (byte != kEscape) {
          state = State::TEXT;
        }
        break;
      case State::OSC_COMMAND:
        if (byte == ';') {
          titleOsc = command == "0" || command == "2";
          state = State::OSC_TEXT;
        } else if (byte == kBell) {
          state = State::TEXT;
        } else if (byte == kEscape) {
          state = State::OSC_COMMAND_ESCAPE;
        } else if (command.size() < 8) {
          command.push_back(static_cast<char>(byte));
        }
        break;
      case State::OSC_COMMAND_ESCAPE:
        if (byte == ']') {
          beginOsc();
        } else {
          state = State::TEXT;
        }
        break;
      case State::OSC_TEXT:
        if (byte == kBell) {
          if (titleOsc) {
            latestTitle = sanitizeTitle();
          }
          state = State::TEXT;
        } else if (byte == kEscape) {
          state = State::OSC_TEXT_ESCAPE;
        } else if (titleOsc && title.size() < kMaxBufferedTitleBytes) {
          title.push_back(static_cast<char>(byte));
        }
        break;
      case State::OSC_TEXT_ESCAPE:
        if (byte == '\\') {
          if (titleOsc) {
            latestTitle = sanitizeTitle();
          }
          state = State::TEXT;
        } else if (byte == ']') {
          // An OSC inside an unterminated OSC is malformed, but restarting
          // here lets a later valid title recover without consuming forever.
          beginOsc();
        } else {
          if (titleOsc && title.size() < kMaxBufferedTitleBytes) {
            title.push_back(static_cast<char>(byte));
          }
          state = State::OSC_TEXT;
        }
        break;
    }
  }
  return latestTitle;
}

}  // namespace et
