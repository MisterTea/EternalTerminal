#include <sddl.h>
#include <windows.h>

#include "SshAgentSessionBind.hpp"

namespace et {
namespace {
// Win32 OpenSSH uses named pipes for IdentityAgent, rather than AF_UNIX.
struct Handle {
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE value;
};

uint32_t messageLength(const char* p) {
  return (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) |
         (uint32_t(uint8_t(p[2])) << 8) | uint32_t(uint8_t(p[3]));
}

string frameMessage(const string& body) {
  uint32_t size = static_cast<uint32_t>(body.size());
  return string{char(size >> 24), char(size >> 16), char(size >> 8),
                char(size)} +
         body;
}
}  // namespace

struct SshAgentSessionBindRecorder::WindowsImpl {
  explicit WindowsImpl(SshAgentSessionBindRecorder* owner) : owner(owner) {
    if (!stop.value) throw std::runtime_error("agent proxy event setup failed");
    // Only the current user may open the proxy, even when the account belongs
    // to Administrators. Reject remote pipe clients as well.
    HANDLE tokenValue = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue)) {
      throw std::runtime_error("agent proxy token query failed");
    }
    Handle token(tokenValue);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    vector<char> user(size);
    if (!GetTokenInformation(token.value, TokenUser, user.data(), size,
                             &size)) {
      throw std::runtime_error("agent proxy user query failed");
    }
    LPSTR sid = nullptr;
    if (!ConvertSidToStringSidA(
            reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
      throw std::runtime_error("agent proxy SID conversion failed");
    }
    string sddl = "D:P(A;;GA;;;" + string(sid) + ")";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            sddl.c_str(), SDDL_REVISION_1, &security, nullptr)) {
      throw std::runtime_error("agent proxy security setup failed");
    }
  }

  ~WindowsImpl() {
    SetEvent(stop.value);
    if (worker.joinable()) worker.join();
    LocalFree(security);
  }

  HANDLE listener(bool first) {
    SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), security,
                                      FALSE};
    return CreateNamedPipeA(owner->socketPath_.c_str(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES,
                            SSH_AGENT_MAX_MESSAGE_LENGTH + 4,
                            SSH_AGENT_MAX_MESSAGE_LENGTH + 4, 0, &attributes);
  }

  // All waits include stop. Cancel and drain the operation before its buffer
  // or OVERLAPPED is destroyed, including when shutdown interrupts an agent
  // confirmation prompt or a client that stopped reading.
  bool finish(HANDLE pipe, OVERLAPPED* op, DWORD* transferred,
              bool* warnedSlow = nullptr) {
    HANDLE events[] = {stop.value, op->hEvent};
    while (true) {
      DWORD result = WaitForMultipleObjects(
          2, events, FALSE, warnedSlow && !*warnedSlow ? 5000 : INFINITE);
      if (result == WAIT_TIMEOUT) {
        *warnedSlow = true;
        LOG(WARNING) << "ssh agent " << owner->upstreamAgentPath_
                     << " has not answered for 5s; still waiting "
                        "(it may be asking to confirm a key)";
        continue;
      }
      if (result == WAIT_OBJECT_0 + 1) {
        return GetOverlappedResult(pipe, op, transferred, FALSE) != FALSE;
      }
      CancelIoEx(pipe, op);
      GetOverlappedResult(pipe, op, transferred, TRUE);
      return false;
    }
  }

  bool transfer(HANDLE pipe, char* data, size_t size, bool writing,
                bool* warnedSlow = nullptr) {
    size_t pos = 0;
    while (pos < size && WaitForSingleObject(stop.value, 0) == WAIT_TIMEOUT) {
      Handle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
      if (!event.value) return false;
      OVERLAPPED op = {};
      op.hEvent = event.value;
      DWORD count = 0;
      DWORD remaining = static_cast<DWORD>(size - pos);
      BOOL ok = writing ? WriteFile(pipe, data + pos, remaining, nullptr, &op)
                        : ReadFile(pipe, data + pos, remaining, nullptr, &op);
      if (!ok && (GetLastError() != ERROR_IO_PENDING ||
                  !finish(pipe, &op, &count, warnedSlow))) {
        return false;
      }
      // Even immediate completion on an overlapped handle reports its byte
      // count through GetOverlappedResult, rather than ReadFile/WriteFile.
      if (ok && !GetOverlappedResult(pipe, &op, &count, FALSE)) return false;
      if (!count) return false;
      pos += count;
    }
    return pos == size;
  }

  bool readFrame(HANDLE pipe, string* body, bool upstream = false) {
    bool warnedSlow = false;
    char length[4];
    bool* warning = upstream ? &warnedSlow : nullptr;
    if (!transfer(pipe, length, sizeof(length), false, warning)) return false;
    uint32_t size = messageLength(length);
    if (size == 0 || size > SSH_AGENT_MAX_MESSAGE_LENGTH) return false;
    body->assign(size, '\0');
    return transfer(pipe, body->data(), size, false, warning);
  }

  bool writeFrame(HANDLE pipe, const string& body) {
    string framed = frameMessage(body);
    return transfer(pipe, framed.data(), framed.size(), true);
  }

  HANDLE connectUpstream() {
    if (owner->upstreamAgentPath_.empty()) return INVALID_HANDLE_VALUE;
    while (WaitForSingleObject(stop.value, 0) == WAIT_TIMEOUT) {
      HANDLE pipe = CreateFileA(owner->upstreamAgentPath_.c_str(),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
        return pipe;
      }
      // A busy agent is still usable. Bound the wait so shutdown also
      // interrupts a connection queued behind another confirmation prompt.
      if (!WaitNamedPipeA(owner->upstreamAgentPath_.c_str(), 50) &&
          GetLastError() != ERROR_SEM_TIMEOUT)
        return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
  }

  void serve(HANDLE clientValue) {
    Handle client(clientValue);
    Handle upstream(connectUpstream());
    if (upstream.value == INVALID_HANDLE_VALUE &&
        !owner->upstreamAgentPath_.empty()) {
      LOG(WARNING) << "Could not connect to ssh agent "
                   << owner->upstreamAgentPath_ << ": Windows error "
                   << GetLastError();
    }
    string request;
    while (readFrame(client.value, &request)) {
      auto bind = forwardedSessionBind(request);
      if (bind) {
        lock_guard<std::mutex> guard(owner->mutex_);
        owner->lastForwardedBind_ = *bind;
        VLOG(1) << "Agent proxy saw session binding for host key "
                << sessionBindHostKeyFingerprint(*bind).value_or("?");
      }
      string reply(1, char(5));
      if (upstream.value != INVALID_HANDLE_VALUE &&
          (!writeFrame(upstream.value, request) ||
           !readFrame(upstream.value, &reply, true)))
        break;
      if (!writeFrame(client.value, reply)) break;
    }
    DisconnectNamedPipe(client.value);
  }

  void run(HANDLE initial) {
    HANDLE next = initial;
    struct ClientWorker {
      std::thread thread;
      std::shared_ptr<std::atomic<bool>> done;
    };
    vector<ClientWorker> clients;
    while (next != INVALID_HANDLE_VALUE) {
      Handle pipe(next);
      if (WaitForSingleObject(stop.value, 0) != WAIT_TIMEOUT) break;
      Handle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
      if (!event.value) break;
      OVERLAPPED op = {};
      op.hEvent = event.value;
      DWORD count = 0;
      BOOL connected = ConnectNamedPipe(pipe.value, &op);
      DWORD error = connected ? ERROR_SUCCESS : GetLastError();
      if (!connected && error != ERROR_PIPE_CONNECTED &&
          (error != ERROR_IO_PENDING || !finish(pipe.value, &op, &count)))
        break;
      // Keep an instance listening before handing this one to its worker.
      next = listener(false);
      clients.erase(std::remove_if(clients.begin(), clients.end(),
                                   [](ClientWorker& client) {
                                     if (!*client.done) return false;
                                     client.thread.join();
                                     return true;
                                   }),
                    clients.end());
      auto done = make_shared<std::atomic<bool>>(false);
      HANDLE client = pipe.value;
      clients.push_back({std::thread([this, client, done] {
                           serve(client);
                           *done = true;
                         }),
                         done});
      pipe.value = INVALID_HANDLE_VALUE;
    }
    SetEvent(stop.value);
    for (auto& client : clients) client.thread.join();
  }

  SshAgentSessionBindRecorder* owner;
  Handle stop{CreateEventA(nullptr, TRUE, FALSE, nullptr)};
  PSECURITY_DESCRIPTOR security = nullptr;
  std::thread worker;
};

SshAgentSessionBindRecorder::SshAgentSessionBindRecorder(
    const string& upstreamAgentPath)
    : upstreamAgentPath_(upstreamAgentPath) {
  socketPath_ = "\\\\.\\pipe\\et-ssh-bind-" + genRandomAlphaNum(24);
  windows_ = make_unique<WindowsImpl>(this);
  HANDLE listener = windows_->listener(true);
  if (listener == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("agent proxy named pipe setup failed");
  }
  try {
    windows_->worker = std::thread(&WindowsImpl::run, windows_.get(), listener);
  } catch (...) {
    CloseHandle(listener);
    throw;
  }
  LOG(INFO) << "Agent proxy " << socketPath_ << " relays to ssh agent "
            << upstreamAgentPath_;
}

SshAgentSessionBindRecorder::~SshAgentSessionBindRecorder() {
  // Join before destroying the mutex and recorded binding used by workers.
  windows_.reset();
}

optional<string> SshAgentSessionBindRecorder::lastForwardedBind() {
  lock_guard<std::mutex> guard(mutex_);
  return lastForwardedBind_;
}
}  // namespace et
