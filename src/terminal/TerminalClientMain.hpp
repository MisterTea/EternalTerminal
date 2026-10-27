#ifndef __ET_TERMINAL_CLIENT_MAIN__
#define __ET_TERMINAL_CLIENT_MAIN__

#include "Headers.hpp"
#include "SubprocessUtils.hpp"

namespace et {
class TerminalClient;

/**
 * @brief Replaceable process-level dependencies of the et client.
 */
struct TerminalClientMainHooks {
  /** @brief Runs the bootstrap ssh command. Defaults to a real subprocess. */
  shared_ptr<SubprocessUtils> subprocessUtils;
  /**
   * @brief Called once the client has connected, before it runs the session.
   * Calling shutdown() on the client from here ends the session loop.
   */
  std::function<void(TerminalClient&)> onClientReady;
};

/**
 * @brief Runs the et client and returns its process exit status.
 */
int TerminalClientMain(
    int argc, char** argv,
    const TerminalClientMainHooks& hooks = TerminalClientMainHooks());
}  // namespace et

#endif  // __ET_TERMINAL_CLIENT_MAIN__
