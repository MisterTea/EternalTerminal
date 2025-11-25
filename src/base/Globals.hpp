#ifndef __ET_GLOBALS__
#define __ET_GLOBALS__

namespace et {
/** @brief Logs a fatal error if a write loop exited unexpectedly. */
void fatalOnWriteError(ssize_t expected, ssize_t actual);
}

#endif  // __ET_GLOBALS__
