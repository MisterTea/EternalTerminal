#ifndef __ET_CONTROL_LISTENER_HPP__
#define __ET_CONTROL_LISTENER_HPP__

#include <atomic>
#include <functional>

#include "ControlConsole.hpp"
#include "Headers.hpp"

namespace et {

/*
 * The daemon-side endpoint of the control plane.  It owns a local unix-domain
 * socket (0600) and a thread that accepts `etctl` connections, translating each
 * framed request into an action on the ControlConsole (inject input, read
 * output, resize) or a lifecycle action (kill).  It is the sibling of ET's
 * router bridge, but speaks the (unencrypted, local) control framing.
 *
 * Connections are short-lived: one request, one response, close.  Streaming
 * reads (etctl read --follow / observe) are done by the client polling CTL_READ,
 * so the daemon keeps no per-reader state.
 */
class ControlListener {
 public:
  ControlListener(shared_ptr<ControlConsole> console, const string& socketPath,
                  std::function<void()> onKill,
                  std::function<bool()> isConnected = nullptr,
                  const string& host = "");
  ~ControlListener();

  // Bind + listen + start the accept thread.  Throws on bind failure.
  void start();

  // Stop accepting, join the thread, and unlink the socket.
  void shutdown();

 private:
  void acceptLoop();
  void handleConnection(int connFd);
  bool peerAuthorized(int connFd) const;

  shared_ptr<ControlConsole> console;
  string socketPath;
  std::function<void()> onKill;
  std::function<bool()> isConnected;
  string host;

  int listenFd;
  std::atomic<bool> running;
  std::thread acceptThread;
};

}  // namespace et

#endif  // __ET_CONTROL_LISTENER_HPP__
