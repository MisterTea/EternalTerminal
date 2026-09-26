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
 * @brief Coordinates the lifecycle of a client connection, console, and
 * tunnels.
 */
class TerminalClient {
 public:
  static const string INVALID_SESSION_CONNECT_ERROR;

  /**
   * @brief Configures the client with the required sockets, console, and
   * tunnels.
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
                 bool noShell = false, bool exitOnForwardFailure = false);
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
  /**
   * @brief Flags the client loop to exit gracefully on the next iteration.
   */
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

 protected:
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
