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

#ifndef WIN32
/**
 * @brief Reads the effective uid and gid of the process at the other end of
 * the connected Unix-domain socket @p fd, as recorded by the kernel.
 *
 * @return false if the kernel cannot report them.
 */
bool GetPeerCredentials(int fd, uid_t* uid, gid_t* gid);

/**
 * @brief Creates @p path for writing with mode 0600, never following a
 * symlink or opening another user's file. A stale regular file owned by this
 * user is replaced once.
 *
 * @return the descriptor, or -1 with errno set.
 */
int CreatePrivateFile(const string& path);
#endif
}  // namespace et

#endif  // __ET_PLATFORM_UTILS__
