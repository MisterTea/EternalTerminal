#ifndef __ET_CONSOLE_UTF8_DECODER_WINDOWS__
#define __ET_CONSOLE_UTF8_DECODER_WINDOWS__

#include <cstddef>
#include <string>

namespace et {
/** @brief Decodes console output while retaining incomplete UTF-8 suffixes. */
class ConsoleUtf8Decoder {
 public:
  std::wstring decode(const char* buf, size_t count);
  void reset() { pending.clear(); }

 private:
  std::string pending;
};
}  // namespace et

#endif
