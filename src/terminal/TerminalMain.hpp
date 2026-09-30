#ifndef __ET_TERMINAL_MAIN__
#define __ET_TERMINAL_MAIN__

#include "Headers.hpp"

namespace et {
/**
 * @brief Runs etterminal and returns its process exit status.
 *
 * On success this daemonizes (Unix) before serving the terminal, so callers
 * that must stay in-process should only drive the failure paths.
 */
int TerminalMain(int argc, char** argv);
}  // namespace et

#endif  // __ET_TERMINAL_MAIN__
