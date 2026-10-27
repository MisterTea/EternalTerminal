#ifndef __ET_TERMINAL_SERVER_MAIN__
#define __ET_TERMINAL_SERVER_MAIN__

#include "Headers.hpp"

namespace et {
class TerminalServer;

/**
 * @brief Runs etserver and returns its process exit status.
 * @param onServerReady Called after the server is listening and before its
 * accept loop starts. Calling `shutdown()` from here stops the server.
 */
int TerminalServerMain(
    int argc, char** argv,
    const std::function<void(TerminalServer&)>& onServerReady = nullptr);
}  // namespace et

#endif  // __ET_TERMINAL_SERVER_MAIN__
