#ifndef __ET_TERMINAL_CLIENT__
#define __ET_TERMINAL_CLIENT__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <optional>

#include "ClientConnection.hpp"
#include "Console.hpp"
#include "CryptoHandler.hpp"
#include "ETerminal.pb.h"
#include "ForwardSourceHandler.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "PortForwardHandler.hpp"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "SshSetupHandler.hpp"
#include "TcpSocketHandler.hpp"
#include "TitleParser.hpp"

namespace et {
/**
 * @brief Prepares and returns a stable SSH agent proxy socket path.
 */
string refreshAgentProxyPath(const string& id, const string& authSock);

/**
 * @brief Coordinates the lifecycle of a client connection, console, and
 * tunnels.
 */
class TerminalClient {
 public:
  static const string INVALID_SESSION_CONNECT_ERROR;

  /**
   * @brief Configures the client with the required sockets, console, and
   * tunnels.
   * @param sshAgentSessionBinds OpenSSH session-bind requests (from
   * SshSetupHandler) sent on each forwarded ssh-agent connection.
   */
  TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                 std::shared_ptr<SocketHandler> _pipeSocketHandler,
                 const SocketEndpoint& _socketEndpoint, const string& id,
                 const string& passkey, shared_ptr<Console> _console,
                 bool jumphost, const string& tunnels,
                 const string& reverseTunnels, bool forwardSshAgent,
                 const string& identityAgent, int _keepaliveDuration,
                 const vector<pair<string, string>>& envVars,
                 bool noPty = false, const string& command = "",
                 const vector<string>& dynamicForwards = {},
                 const string& stdioForward = "", int _maxConnectAttempts = 3,
                 bool _resumeSavedSession = false,
                 std::function<bool()> _sessionHeartbeat = {},
                 std::function<bool(const string&)> _sessionTitleUpdate = {},
                 optional<int> disconnectTimeoutMinutes = nullopt,
                 bool noShell = false, bool exitOnForwardFailure = false,
                 const vector<string>& sshAgentSessionBinds = {});
  /** @brief Tears down the client, closing sockets and stopping background
   * threads. */
  virtual ~TerminalClient();
  /** @brief Runs the interactive session for `command`, optionally staying
   * alive.
   * @return Remote command exit status when `command` is set and `noexit` is
   * false; otherwise 0.
   */
  int run(const string& command, const bool noexit);
  /** @brief True when `-W` is bridging stdio (no local shell UI). */
  bool isStdioForward() const { return stdioForwardActive; }
  /**
   * @brief After `run()` returns, keep keepalives and port forwards alive
   * until `keepGoing` is false (ControlPersist). Also services any passenger
   * session attached via `runPassengerSession`.
   */
  void serviceIdleUntil(const function<bool()>& keepGoing);
  /**
   * @brief Block until a mux passenger's stdio has been bridged through the
   * live ET connection by `serviceIdleUntil` / `run`, then return its status.
   *
   * Commanded PTY passengers run in an isolated subshell and report status via
   * an output marker (not by exiting the shared shell). Interactive attaches
   * return 0 on TERMINAL_CLOSE. Idle/error teardown returns 1. Local stdin EOF
   * does not complete the session.
   */
  uint32_t runPassengerSession(int inFd, int outFd, int errFd,
                               const string& command);
  /**
   * @brief Complete an in-flight passenger attach (e.g. mux control hangup).
   * Sticky only while not yet active: a hangup before `passenger.active` is
   * remembered and applied as soon as `runPassengerSession` attaches. Once
   * active, cancel completes the current session and suppresses further sticky
   * arms until `beginPassengerWatch`.
   */
  void cancelPassengerSession();
  /**
   * @brief Clear sticky-cancel suppression at the start of a mux passenger
   * watch so hangup-before-active still works. Call before any gate that
   * precedes `runPassengerSession`.
   */
  void beginPassengerWatch();
  /** @brief Port-forward handler owned by this client (for mux OPEN_FWD). */
  shared_ptr<PortForwardHandler> getPortForwardHandler() const {
    return portForwardHandler;
  }
  static void configureCloseOnHangup(bool enabled) { closeOnHangup = enabled; }
  static void requestHangupClose(int = 0) { hangupCloseRequested = true; }
  static bool waitForHangupClose(int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!hangupCloseCompleted &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return hangupCloseCompleted;
  }
  static void resetHangupClose() {
    closeOnHangup = false;
    hangupCloseRequested = false;
    hangupCloseCompleted = false;
  }
#ifdef WIN32
  static BOOL WINAPI consoleCtrlHandler(DWORD ctrlType);
#endif
  bool killSession(int timeoutSeconds);
  bool sessionEndedByServer() {
    return connection &&
           connection->lastStatus() == et::ConnectStatus::INVALID_KEY;
  }

  // True when this client adopted a session that was already running rather
  // than creating one. The shell is mid-life, so connect-time setup has already
  // happened and re-running it would type into whatever is in the foreground.
  bool attachedToExisting() { return connection && connection->wasRecovered(); }

  /**
   * @brief Why `run()` returned, in a form fit to show a user.
   *
   * `run()` returning is what ends a control session, and every caller so far
   * has had to guess which of several very different things happened. Ask here
   * instead of inferring it.
   */
  string exitReason() {
    if (sessionEndedByServer()) {
      return "the remote session ended (server no longer has it)";
    }
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        return "shutdown was requested";
      }
    }
    return "the connection closed";
  }
  /**
   * @brief Flags the client loop to exit gracefully on the next iteration.
   */
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

  // True when the client currently holds a live connection to etserver.  ET
  // flips this to false during a drop and back to true once it reconnects, so
  // it distinguishes "link down" from "the daemon is gone" (the latter shows as
  // an unreachable control socket).
  bool isConnected() { return connection && !connection->isDisconnected(); }

 protected:
  /**
   * @brief Retargets the stable SSH agent proxy to the current auth sock.
   * No-op unless agent forwarding was established or a saved session resume
   * refreshed an existing proxy.
   */
  void refreshAgentProxy();

  /** @brief Console wrapper used for local terminal input/output. */
  shared_ptr<Console> console;
  /** @brief Client connection that talks to the ET server. */
  shared_ptr<ClientConnection> connection;
  /** @brief Handles local/remote port forwarding tunnels. */
  shared_ptr<PortForwardHandler> portForwardHandler;
  /** @brief Guarded flag that ends `run()` when set. */
  bool shuttingDown;
  /** @brief Synchronizes writes to `shuttingDown`. */
  recursive_mutex shutdownMutex;
  /** @brief Keepalive interval (seconds) sent to the server. */
  int keepaliveDuration;
  /** @brief True when this session uses the raw pipe command channel. */
  bool noPty;
  /** @brief Set when `-W` is active so stdout stays a pure byte pipe. */
  bool stdioForwardActive = false;
  /** @brief True when a stable agent proxy should be kept current. */
  bool agentProxyEnabled = false;
  /** @brief Client id used in the per-client agent proxy directory name. */
  string agentClientId;
  /**
   * @brief Explicit `--ssh-socket` path when set; otherwise SSH_AUTH_SOCK is
   * re-read on each refresh.
   */
  string agentIdentityAgent;

  struct PassengerAttach {
    int inFd = -1;
    int outFd = -1;
    int errFd = -1;
    string command;
    bool active = false;
    optional<uint32_t> exitStatus;
    /** Bumped on each attach so idle can reset per-session locals. */
    uint64_t generation = 0;
    /**
     * Idle-loop poll/read/write sections still using the passenger fds.
     * `runPassengerSession` waits for this to hit 0 before the caller closes
     * them, so close cannot race those calls.
     */
    int ioDepth = 0;
    /**
     * Set by `cancelPassengerSession` when not yet active so a hangup that
     * races ahead of attach still completes the session. Cleared when applied
     * or when the attach ends; not set while already active.
     */
    bool cancelRequested = false;
    /**
     * After an attach has been active (or finished), ignore further sticky
     * cancel arms so a late MuxMaster hangup poll cannot poison the next
     * passenger session.
     */
    bool suppressStickyCancel = false;
  };
  PassengerAttach passenger;
  mutex passengerMutex;
  condition_variable passengerCv;
  /** @brief True while `serviceIdleUntil` may accept mux passengers. */
  atomic<bool> idleServicing{false};

  static std::atomic<bool> closeOnHangup;
  static std::atomic<bool> hangupCloseRequested;
  static std::atomic<bool> hangupCloseCompleted;
  std::function<bool()> sessionHeartbeat;
  std::function<bool(const string&)> sessionTitleUpdate;
  TitleParser titleParser;
};

}  // namespace et
#endif  // __ET_TERMINAL_CLIENT__
