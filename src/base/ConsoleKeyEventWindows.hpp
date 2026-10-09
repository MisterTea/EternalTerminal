#ifndef __ET_CONSOLE_KEY_EVENT_WINDOWS__
#define __ET_CONSOLE_KEY_EVENT_WINDOWS__

#include <cstdint>

namespace et {
/**
 * @brief The fields of a Windows KEY_EVENT_RECORD that decide whether a key
 * press carries input, held as plain integers so the policy compiles and is
 * tested on every platform.
 */
struct ConsoleKeyEvent {
  bool keyDown;
  uint16_t virtualKeyCode;
  uint16_t unicodeChar;
  uint32_t controlKeyState;
};

/**
 * @brief Whether a console key record carries a character to forward.
 *
 * ReadConsoleInput reports a key-down record for every key, including bare
 * modifier presses and their auto-repeats while the key is held, even under
 * ENABLE_VIRTUAL_TERMINAL_INPUT. Those records have UnicodeChar == 0 and must
 * be dropped, or every Ctrl press leaks a NUL into the session. A zero
 * UnicodeChar is a real NUL only for a Ctrl chord on a non-modifier key, which
 * is how Ctrl+Space and Ctrl+@ reach programs such as emacs.
 */
inline bool consoleKeyEventHasInput(const ConsoleKeyEvent& event) {
  if (!event.keyDown) {
    return false;
  }
  if (event.unicodeChar != 0) {
    return true;
  }
  constexpr uint32_t kRightCtrlPressed = 0x0004;
  constexpr uint32_t kLeftCtrlPressed = 0x0008;
  if ((event.controlKeyState & (kLeftCtrlPressed | kRightCtrlPressed)) == 0) {
    return false;
  }
  switch (event.virtualKeyCode) {
    case 0x10:  // VK_SHIFT
    case 0x11:  // VK_CONTROL
    case 0x12:  // VK_MENU
    case 0x14:  // VK_CAPITAL
    case 0x5B:  // VK_LWIN
    case 0x5C:  // VK_RWIN
    case 0x90:  // VK_NUMLOCK
    case 0x91:  // VK_SCROLL
    case 0xA0:  // VK_LSHIFT
    case 0xA1:  // VK_RSHIFT
    case 0xA2:  // VK_LCONTROL
    case 0xA3:  // VK_RCONTROL
    case 0xA4:  // VK_LMENU
    case 0xA5:  // VK_RMENU
    case 0xE5:  // VK_PROCESSKEY (IME composition)
      return false;
    default:
      return true;
  }
}
}  // namespace et

#endif
