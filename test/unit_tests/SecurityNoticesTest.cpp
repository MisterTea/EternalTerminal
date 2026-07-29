/**
 * Regression tests for public security notices under security_notices/.
 *
 * ANT-2026-VAMER5RC reconnect passkey proof is intentionally not covered: that
 * requires a PROTOCOL_VERSION bump / wire-format change.
 */

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <queue>
#include <thread>

#include "BackedWriter.hpp"
#include "PipeSocketHandler.hpp"
#include "PortForwardHandler.hpp"
#include "ServerClientConnection.hpp"
#include "ServerConnection.hpp"
#include "TestHeaders.hpp"
#include "UserSocketOps.hpp"

using namespace et;

namespace {
// Minimal socket handler that works with socketpairs for handshake tests.
class SocketPairHandler : public SocketHandler {
 public:
  void queueConnectFd(int fd) { connectQueue.push(fd); }

  bool hasData(int fd) override { return waitOnSocketData(fd); }

  ssize_t read(int fd, void* buf, size_t count) override {
    return ::read(fd, buf, count);
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    return ::write(fd, buf, count);
  }

  int connect(const SocketEndpoint&) override {
    if (connectQueue.empty()) {
      return -1;
    }
    int fd = connectQueue.front();
    connectQueue.pop();
    return fd;
  }

  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int fd) override { return fd; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { ::close(fd); }
  vector<int> getActiveSockets() override { return {}; }

 private:
  std::queue<int> connectQueue;
};

class RecordingServerConnection : public ServerConnection {
 public:
  RecordingServerConnection(std::shared_ptr<SocketHandler> socketHandler,
                            const SocketEndpoint& endpoint)
      : ServerConnection(std::move(socketHandler), endpoint) {}

  bool newClient(
      shared_ptr<ServerClientConnection> serverClientState) override {
    return true;
  }
};

class FdSocketHandler : public SocketHandler {
 public:
  bool hasData(int fd) override { return waitOnSocketData(fd); }
  ssize_t read(int fd, void* buf, size_t count) override {
    return ::read(fd, buf, count);
  }
  ssize_t write(int fd, const void* buf, size_t count) override {
    return ::write(fd, buf, count);
  }
  int connect(const SocketEndpoint&) override { return -1; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int) override { return -1; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { ::close(fd); }
  vector<int> getActiveSockets() override { return {}; }
};

string makeTempDir() {
  string pattern = GetTempDirectory() + "et_secnotice_XXXXXX";
  string dir = string(mkdtemp(&pattern[0]));
  REQUIRE_FALSE(dir.empty());
  return dir;
}

// CI containers (Debian/FreeBSD) run tests as root. Privilege-drop tests must
// target a non-root uid so DAC actually applies after setuid.
bool unprivilegedTestUser(uid_t* uid, gid_t* gid) {
  if (getuid() != 0) {
    *uid = getuid();
    *gid = getgid();
    return true;
  }
  const char* candidates[] = {"nobody", "nfsnobody", nullptr};
  for (int i = 0; candidates[i] != nullptr; ++i) {
    struct passwd* pw = getpwnam(candidates[i]);
    if (pw != nullptr && pw->pw_uid != 0) {
      *uid = pw->pw_uid;
      *gid = pw->pw_gid;
      return true;
    }
  }
  return false;
}
}  // namespace

// ---------------------------------------------------------------------------
// ANT-2026-5PETM5BV — pre-auth slowloris / oversized ConnectRequest
// ---------------------------------------------------------------------------

TEST_CASE(
    "ANT-2026-5PETM5BV handshake readProto rejects oversized length before "
    "allocating",
    "[SecurityNotice][ANT-2026-5PETM5BV]") {
  FdSocketHandler handler;
  int fds[2];
  REQUIRE(::pipe(fds) == 0);

  // Exactly the old 128 MiB cap must also be rejected for handshake reads.
  int64_t oversize = 128 * 1024 * 1024;
  REQUIRE(oversize > SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
  REQUIRE(handler.writeAllOrReturn(fds[1], &oversize, sizeof(oversize)) ==
          (int)sizeof(oversize));

  REQUIRE_THROWS_AS(
      handler.readProto<ConnectRequest>(
          fds[0], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH),
      std::runtime_error);

  handler.close(fds[0]);
  handler.close(fds[1]);
}

TEST_CASE(
    "ANT-2026-5PETM5BV readAll absolute timeout fires under per-byte trickle",
    "[SecurityNotice][ANT-2026-5PETM5BV]") {
  FdSocketHandler handler;
  int fds[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  std::atomic<bool> stop{false};
  std::thread trickle([&]() {
    // Keep resetting the idle timer with 1 byte ~every 400ms; without an
    // absolute deadline this would never time out.
    char b = 'x';
    while (!stop.load()) {
      if (::write(fds[1], &b, 1) < 0) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
  });

  char buf[64];
  auto start = std::chrono::steady_clock::now();
  // Idle allowance is long; absolute deadline is short.
  REQUIRE_THROWS_AS(handler.readAll(fds[0], buf, sizeof(buf), /*idle*/ 30,
                                    /*absolute*/ 2),
                    std::runtime_error);
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto elapsedSec =
      std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
  // Must not wait anywhere near the 30s idle timeout.
  REQUIRE(elapsedSec < 10);

  stop.store(true);
  trickle.join();
  handler.close(fds[0]);
  handler.close(fds[1]);
}

TEST_CASE(
    "ANT-2026-5PETM5BV ServerConnection rejects oversized ConnectRequest "
    "length",
    "[SecurityNotice][ANT-2026-5PETM5BV][ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  RecordingServerConnection server(handler, endpoint);

  int fds[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int64_t oversize = SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH + 1;
  REQUIRE(handler->writeAllOrReturn(fds[0], &oversize, sizeof(oversize)) ==
          (int)sizeof(oversize));

  // Must return (catch runtime_error), not abort the process.
  REQUIRE_NOTHROW(server.clientHandler(fds[1]));

  handler->close(fds[0]);
  server.shutdown();
}

// ---------------------------------------------------------------------------
// ANT-2026-VAMER5RC — pre-auth recover crash / force-disconnect
// (passkey-before-recover omitted: needs PROTOCOL_VERSION bump)
// ---------------------------------------------------------------------------

TEST_CASE(
    "ANT-2026-VAMER5RC BackedWriter recover throws instead of aborting on "
    "client-ahead sequence",
    "[SecurityNotice][ANT-2026-VAMER5RC][BackedIO]") {
  class InMemorySocketHandler : public SocketHandler {
   public:
    int createChannel() {
      int fd = nextFd++;
      buffers[fd] = {};
      return fd;
    }
    bool hasData(int fd) override { return !buffers[fd].empty(); }
    ssize_t read(int fd, void* buf, size_t count) override {
      auto& q = buffers[fd];
      if (q.empty()) {
        SetErrno(EPIPE);
        return 0;
      }
      size_t n = std::min(count, q.size());
      for (size_t i = 0; i < n; ++i) {
        static_cast<char*>(buf)[i] = q.front();
        q.pop_front();
      }
      return n;
    }
    ssize_t write(int fd, const void* buf, size_t count) override {
      auto* c = static_cast<const char*>(buf);
      for (size_t i = 0; i < count; ++i) {
        buffers[fd].push_back(c[i]);
      }
      return count;
    }
    int connect(const SocketEndpoint&) override { return -1; }
    set<int> listen(const SocketEndpoint&) override { return {}; }
    set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
    int accept(int) override { return -1; }
    void stopListening(const SocketEndpoint&) override {}
    void close(int) override {}
    vector<int> getActiveSockets() override { return {}; }

   private:
    std::atomic<int> nextFd{1};
    std::map<int, std::deque<char>> buffers;
  };

  auto handler = make_shared<InMemorySocketHandler>();
  auto crypto = make_shared<CryptoHandler>("12345678901234567890123456789012",
                                           0 /*verbosity*/);
  const int fd = handler->createChannel();
  BackedWriter writer(handler, crypto, fd);
  REQUIRE(writer.write(Packet(1, "one")) == BackedWriterWriteState::SUCCESS);
  writer.invalidateSocket();

  REQUIRE_THROWS_AS(writer.recover(writer.getSequenceNumber() + 1),
                    std::runtime_error);
}

TEST_CASE(
    "ANT-2026-VAMER5RC recoverClient leaves victim socket open on bad sequence",
    "[SecurityNotice][ANT-2026-VAMER5RC][ServerClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int live[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  ServerClientConnection connection(handler, "client-recover", live[0], key);
  REQUIRE(connection.getSocketFd() == live[0]);
  REQUIRE(::fcntl(live[0], F_GETFD) != -1);

  int attack[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, attack) == 0);

  std::thread attacker([&]() {
    handler->readProto<SequenceHeader>(
        attack[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    SequenceHeader bad;
    bad.set_sequencenumber(999999);
    handler->writeProto(attack[1], bad, true);
  });

  REQUIRE_FALSE(connection.recoverClient(attack[0]));
  // Victim session must remain on the original fd, which must still be open.
  REQUIRE(connection.getSocketFd() == live[0]);
  REQUIRE(::fcntl(live[0], F_GETFD) != -1);

  attacker.join();
  connection.shutdown();
  handler->close(live[0]);
  handler->close(live[1]);
  handler->close(attack[1]);
}

// ---------------------------------------------------------------------------
// ANT-2026-AVTT7HQH — reverse-tunnel source root unlink/chown
// ---------------------------------------------------------------------------

#ifndef WIN32
TEST_CASE(
    "ANT-2026-AVTT7HQH createSource as user does not destroy undeletable file",
    "[SecurityNotice][ANT-2026-AVTT7HQH][PortForwardHandler]") {
  uid_t sessionUid = 0;
  gid_t sessionGid = 0;
  if (!unprivilegedTestUser(&sessionUid, &sessionGid)) {
    SKIP("No unprivileged user available for privilege-drop test");
  }

  string dir = makeTempDir();
  string victim = dir + "/victim_file";
  {
    int fd = ::open(victim.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "keepme", 6) == 6);
    ::close(fd);
  }

  if (getuid() == 0) {
    // Root-owned directory: after setuid to nobody, unlink must fail. This is
    // the etserver-as-root threat model from the notice.
    REQUIRE(::chown(dir.c_str(), 0, 0) == 0);
    REQUIRE(::chmod(dir.c_str(), 0755) == 0);
    REQUIRE(::chown(victim.c_str(), 0, 0) == 0);
  } else {
    // Non-root: remove directory write so self cannot unlink.
    REQUIRE(::chmod(dir.c_str(), 0555) == 0);
  }

  auto networkHandler = make_shared<PipeSocketHandler>();
  auto pipeHandler = make_shared<PipeSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler, sessionUid,
                             sessionGid);

  PortForwardSourceRequest request;
  SocketEndpoint source;
  source.set_name(victim);
  *request.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9);
  *request.mutable_destination() = destination;

  PortForwardSourceResponse response =
      handler.createSource(request, nullptr, sessionUid, sessionGid);

  REQUIRE(response.has_error());

  if (getuid() != 0) {
    REQUIRE(::chmod(dir.c_str(), 0755) == 0);
  }
  struct stat st;
  REQUIRE(::stat(victim.c_str(), &st) == 0);
  REQUIRE(S_ISREG(st.st_mode));

  ::unlink(victim.c_str());
  ::rmdir(dir.c_str());
}

TEST_CASE(
    "ANT-2026-AVTT7HQH createSource as user creates socket on writable path",
    "[SecurityNotice][ANT-2026-AVTT7HQH][PortForwardHandler]") {
  uid_t sessionUid = 0;
  gid_t sessionGid = 0;
  if (!unprivilegedTestUser(&sessionUid, &sessionGid)) {
    SKIP("No unprivileged user available for privilege-drop test");
  }

  string dir = makeTempDir();
  string sockPath = dir + "/ok.sock";
  // Ensure the session user can create the socket in this directory.
  REQUIRE(::chmod(dir.c_str(), 0777) == 0);

  {
    auto networkHandler = make_shared<PipeSocketHandler>();
    auto pipeHandler = make_shared<PipeSocketHandler>();
    PortForwardHandler handler(networkHandler, pipeHandler, sessionUid,
                               sessionGid);

    PortForwardSourceRequest request;
    SocketEndpoint source;
    source.set_name(sockPath);
    *request.mutable_source() = source;
    SocketEndpoint destination;
    destination.set_port(9);
    *request.mutable_destination() = destination;

    PortForwardSourceResponse response =
        handler.createSource(request, nullptr, sessionUid, sessionGid);
    REQUIRE_FALSE(response.has_error());

    struct stat st;
    REQUIRE(::stat(sockPath.c_str(), &st) == 0);
    REQUIRE(S_ISSOCK(st.st_mode));
  }

  ::unlink(sockPath.c_str());
  ::rmdir(dir.c_str());
}

TEST_CASE(
    "ANT-2026-AVTT7HQH UserSocketOps listen fails on root-only path without "
    "deleting it",
    "[SecurityNotice][ANT-2026-AVTT7HQH][UserSocketOps]") {
  uid_t sessionUid = 0;
  gid_t sessionGid = 0;
  if (!unprivilegedTestUser(&sessionUid, &sessionGid)) {
    SKIP("No unprivileged user available for privilege-drop test");
  }
  if (sessionUid == 0) {
    SKIP("Test requires a non-root session user");
  }
  // /dev/null is a privileged node; listen/unlink as an unprivileged user must
  // fail and must not remove it.
  REQUIRE(::access("/dev/null", F_OK) == 0);
  int fd = UserSocketOps::listenUnixAsUser("/dev/null", sessionUid, sessionGid);
  REQUIRE(fd < 0);
  REQUIRE(::access("/dev/null", F_OK) == 0);
}

// ---------------------------------------------------------------------------
// ANT-2026-A3WQS3AG — forward destination root connect to arbitrary unix path
// ---------------------------------------------------------------------------

TEST_CASE(
    "ANT-2026-A3WQS3AG createDestination as session user can reach own socket",
    "[SecurityNotice][ANT-2026-A3WQS3AG][PortForwardHandler]") {
  uid_t sessionUid = 0;
  gid_t sessionGid = 0;
  if (!unprivilegedTestUser(&sessionUid, &sessionGid)) {
    SKIP("No unprivileged user available for privilege-drop test");
  }

  string dir = makeTempDir();
  REQUIRE(::chmod(dir.c_str(), 0777) == 0);
  string path = dir + "/dest.sock";

  int listenFd = UserSocketOps::listenUnixAsUser(path, sessionUid, sessionGid);
  REQUIRE(listenFd >= 0);

  auto networkHandler = make_shared<PipeSocketHandler>();
  auto pipeHandler = make_shared<PipeSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler, sessionUid,
                             sessionGid);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_name(path);
  *request.mutable_destination() = destination;
  request.set_fd(7);

  PortForwardDestinationResponse response = handler.createDestination(request);
  REQUIRE_FALSE(response.has_error());
  REQUIRE(response.has_socketid());

  int accepted = ::accept(listenFd, nullptr, nullptr);
  REQUIRE(accepted >= 0);
  ::close(accepted);
  ::close(listenFd);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}

TEST_CASE(
    "ANT-2026-A3WQS3AG createDestination as session user cannot open "
    "mode-000 socket",
    "[SecurityNotice][ANT-2026-A3WQS3AG][PortForwardHandler]") {
  uid_t sessionUid = 0;
  gid_t sessionGid = 0;
  if (!unprivilegedTestUser(&sessionUid, &sessionGid)) {
    SKIP("No unprivileged user available for privilege-drop test");
  }
  if (sessionUid == 0) {
    SKIP("Test requires a non-root session user");
  }

  string dir = makeTempDir();
  REQUIRE(::chmod(dir.c_str(), 0777) == 0);
  string path = dir + "/denied.sock";

  // Create the listener as root (or current user), then strip access. A root
  // connect would often still succeed; connecting after setuid to the session
  // user must fail. Keep the listen handler alive for the whole test.
  auto listenPipeHandler = make_shared<PipeSocketHandler>();
  int listenFd = -1;
  SocketEndpoint listenEp;
  listenEp.set_name(path);
  if (getuid() == 0) {
    set<int> fds = listenPipeHandler->listen(listenEp);
    REQUIRE_FALSE(fds.empty());
    listenFd = *fds.begin();
  } else {
    listenFd = UserSocketOps::listenUnixAsUser(path, sessionUid, sessionGid);
  }
  REQUIRE(listenFd >= 0);
  REQUIRE(::chmod(path.c_str(), 0) == 0);

  auto networkHandler = make_shared<PipeSocketHandler>();
  auto pipeHandler = make_shared<PipeSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler, sessionUid,
                             sessionGid);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_name(path);
  *request.mutable_destination() = destination;
  request.set_fd(8);

  PortForwardDestinationResponse response = handler.createDestination(request);
  REQUIRE(response.has_error());
  REQUIRE_FALSE(response.has_socketid());

  if (getuid() == 0) {
    listenPipeHandler->stopListening(listenEp);
  } else {
    ::close(listenFd);
  }
  ::chmod(path.c_str(), 0700);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}
#endif
