#ifndef __HTM_CLIENT_H__
#define __HTM_CLIENT_H__

#include "Headers.hpp"
#include "IpcPairClient.hpp"

namespace et {
/**
 * @brief IPC client that sends local stdin keystrokes to `htmd` and prints its
 * output.
 *
 * Unix uses `select()` on STDIN and the pipe fd (HtmClientUnix.cpp). Windows
 * reads the stdin handle on a thread and polls the IPC socket because stdin is
 * not a selectable socket (HtmClientWindows.cpp).
 */
class HtmClient : public IpcPairClient {
 public:
  /** @brief Initialises the HTM client, binding to the provided pipe. */
  HtmClient(shared_ptr<SocketHandler> _socketHandler,
            const SocketEndpoint& endpoint);
  /** @brief Event loop that forwards data between stdin and the HTM daemon. */
  void run();
};

/** @brief Discard pending stdin so leftover tmux -CC lines do not reach the
 * wrapping shell after ``htm`` exits (``/bin/sh -c 'htm; exec $SHELL'``).
 * Unix only. */
void drainHtmStdin();

/** @brief Records this process's pid in the htm client pid file at `path`,
 * which sits at a predictable name in the shared temp directory, so it is
 * created with CreatePrivateFile. Unix only; exposed for tests. */
bool writeHtmClientPidFile(const string& path);
}  // namespace et

#endif  // __HTM_CLIENT_H__
