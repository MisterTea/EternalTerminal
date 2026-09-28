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
}  // namespace et

#endif  // __ET_PLATFORM_UTILS__
