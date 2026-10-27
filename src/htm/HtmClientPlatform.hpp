#ifndef __HTM_CLIENT_PLATFORM_H__
#define __HTM_CLIENT_PLATFORM_H__

#include "Headers.hpp"

namespace et {
/**
 * @brief OS-specific pieces of the htm client entry point: htmd lifecycle,
 * termination handling and final exit.
 *
 * Implemented in HtmClientPlatformUnix.cpp and HtmClientPlatformWindows.cpp.
 */
namespace htm_client_platform {
/**
 * @brief Installs handlers for external termination requests.
 *
 * @param restoreTerminal Called before exiting where that is safe.
 */
void installTerminationHandlers(void (*restoreTerminal)());

/**
 * @brief Starts htmd if it is not running and waits for its pipe.
 *
 * @param killExisting Stop the current user's htmd first.
 */
void ensureDaemon(bool killExisting);

/**
 * @brief Ends the control-mode session and returns main()'s exit code.
 *
 * Unix exits the process here instead of returning, so a stuck terminal can
 * not block teardown.
 */
int finishClient(int code, void (*restoreTerminal)());
}  // namespace htm_client_platform
}  // namespace et

#endif  // __HTM_CLIENT_PLATFORM_H__
