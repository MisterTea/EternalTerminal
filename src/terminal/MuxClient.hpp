#ifndef __ET_MUX_CLIENT__
#define __ET_MUX_CLIENT__

#include "MuxProtocol.hpp"

namespace et {

struct MuxOpenForwardRequest {
  uint32_t type = MUX_FWD_LOCAL;
  string listenHost;
  uint32_t listenPort = 0;
  string connectHost;
  uint32_t connectPort = 0;
};

/**
 * @brief Map a tunnel endpoint onto a PROTOCOL.mux host/port.
 * Name-only endpoints use the stream-local sentinel. A TCP port wins when
 * both name and port are set (the usual `localhost` + port parse).
 */
inline void fillMuxForwardAddress(const SocketEndpoint& endpoint, string* host,
                                  uint32_t* port) {
  if (endpoint.has_port()) {
    *port = static_cast<uint32_t>(endpoint.port());
    *host = endpoint.has_name() && !endpoint.name().empty() ? endpoint.name()
                                                            : "localhost";
    return;
  }
  if (endpoint.has_name()) {
    *host = endpoint.name();
    *port = static_cast<uint32_t>(-2);
    return;
  }
  *host = "localhost";
  *port = 0;
}

inline MuxOpenForwardRequest muxForwardFromTunnel(
    const PortForwardSourceRequest& request) {
  MuxOpenForwardRequest fwd;
  fwd.type = MUX_FWD_LOCAL;
  if (request.has_source()) {
    fillMuxForwardAddress(request.source(), &fwd.listenHost, &fwd.listenPort);
  } else {
    fwd.listenHost = "localhost";
  }
  if (request.has_destination()) {
    fillMuxForwardAddress(request.destination(), &fwd.connectHost,
                          &fwd.connectPort);
  } else {
    fwd.connectHost = "localhost";
  }
  return fwd;
}

/** @brief Stream-local (unix socket) sentinel used by PROTOCOL.mux. */
constexpr uint32_t MUX_STREAM_LOCAL_PORT = static_cast<uint32_t>(-2);

/**
 * @brief Build a PortForwardSourceRequest from mux OPEN_FWD fields.
 * Preserves remote connect hosts and stream-local socket paths; never
 * collapses to listenPort:connectPort alone.
 */
inline PortForwardSourceRequest portForwardRequestFromMux(
    const MuxOpenForwardRequest& fwd) {
  PortForwardSourceRequest pfsr;
  if (fwd.listenPort == MUX_STREAM_LOCAL_PORT) {
    pfsr.mutable_source()->set_name(fwd.listenHost);
  } else {
    pfsr.mutable_source()->set_name(fwd.listenHost.empty() ? "localhost"
                                                           : fwd.listenHost);
    pfsr.mutable_source()->set_port(static_cast<int32_t>(fwd.listenPort));
  }
  if (fwd.connectPort == MUX_STREAM_LOCAL_PORT) {
    pfsr.mutable_destination()->set_name(fwd.connectHost);
  } else {
    if (!fwd.connectHost.empty()) {
      pfsr.mutable_destination()->set_name(fwd.connectHost);
    }
    pfsr.mutable_destination()->set_port(static_cast<int32_t>(fwd.connectPort));
  }
  return pfsr;
}

/**
 * @brief OpenSSH mux client that attaches to a ControlMaster ControlPath.
 */
class MuxClient {
 public:
  explicit MuxClient(string controlPath);
  ~MuxClient();

  MuxClient(const MuxClient&) = delete;
  MuxClient& operator=(const MuxClient&) = delete;

  /** @brief Connect and exchange hellos. */
  bool connect(int timeoutMs = 5000);
  void disconnect();
  /**
   * @brief Shut down the control socket in place to wake blocked I/O without
   * destroying the connection object (safe from another thread).
   */
  void hangup();

  bool aliveCheck(uint32_t* masterPid = nullptr);
  bool terminateMaster();
  bool stopListening();
  bool openForward(const MuxOpenForwardRequest& fwd, string* error = nullptr);
  bool closeForward(const MuxOpenForwardRequest& fwd, string* error = nullptr);

  /**
   * @brief Request a shared command channel (MUX_C_NEW_SESSION).
   * Optionally passes stdin/stdout/stderr via SCM_RIGHTS when fds >= 0.
   * Blocks until MUX_S_EXIT_MESSAGE (or a hard error) and surfaces the
   * session exit status when exitStatus is non-null.
   */
  bool newSession(const string& command, bool wantTty, int stdinFd,
                  int stdoutFd, int stderrFd, uint32_t* sessionId = nullptr,
                  string* error = nullptr, uint32_t* exitStatus = nullptr);

  /** @brief Run -O check|exit|stop control commands. */
  int runCtlCommand(const string& command);

  bool connected() const { return conn && conn->valid(); }

 private:
  bool exchangeHello();
  bool expectReply(uint32_t expectedRequestId, uint32_t* typeOut,
                   MuxBuffer* body);

  string path;
  uint32_t nextRequestId = 0;
  unique_ptr<MuxConnection> conn;
};

/** @brief True when these options mean "attach to an existing master". */
bool shouldAttachToMuxMaster(const MuxOptions& opts);
/** @brief True when these options mean "become ControlMaster". */
bool shouldBecomeMuxMaster(const MuxOptions& opts);

}  // namespace et

#endif  // __ET_MUX_CLIENT__
