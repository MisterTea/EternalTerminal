#ifndef __ET_PLATFORM_UTILS__
#define __ET_PLATFORM_UTILS__

#include "Headers.hpp"

namespace et {
/**
 * @brief The current account's home directory from the OS account database
 * (the passwd entry on Unix, USERPROFILE on Windows), ignoring $HOME.
 *
 * Callers decide whether $HOME takes precedence.
 */
optional<string> GetAccountHomeDirectory();

/** @brief The current account's login name. */
optional<string> GetAccountUsername();

/**
 * @brief Writes all of @p buf to the process's stdout or stderr.
 *
 * @param stdFd STDOUT_FILENO or STDERR_FILENO. Windows writes through the
 *   matching standard handle because CRT descriptors are not sockets.
 */
void WriteToStdStream(int stdFd, const char* buf, size_t count);

#ifdef WIN32
/**
 * @brief Whether a console key record carries a character to forward.
 *
 * ReadConsoleInput reports a key-down record for every key, including bare
 * modifier presses and their auto-repeats while the key is held, even under
 * ENABLE_VIRTUAL_TERMINAL_INPUT; such records have UnicodeChar == 0. A zero
 * UnicodeChar stands for a real NUL only on the Ctrl chords that terminals
 * define as NUL: Ctrl+@, Ctrl+Space, Ctrl+2 and Ctrl+`, on whichever keys
 * produce those characters in the active keyboard layout.
 */
bool ConsoleKeyEventHasInput(const KEY_EVENT_RECORD& key);
#endif
}  // namespace et

#endif  // __ET_PLATFORM_UTILS__
