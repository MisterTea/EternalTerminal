#pragma once

#include <set>

#ifndef WIN32
#include <sys/types.h>
#endif

#ifdef __linux__
#include <atomic>
#include <chrono>
#include <list>
#include <optional>

#include "ETerminal.pb.h"
#include "ThreadPool.h"
#endif

namespace et {
class Connection;
class PipeSocketHandler;
class PortForwardDestinationRequest;
class PortForwardHandler;
class TerminalUserInfo;

#ifndef WIN32
class TerminalSocketOpener {
 public:
  ssize_t readCommand(int routerFd, char* command);
#ifdef __linux__
  ~TerminalSocketOpener() { stopping = true; }

 private:
  std::atomic<bool> stopping{false};
  std::atomic<size_t> pending{0};
  std::optional<ThreadPool> worker;
#endif
};
#endif

// etterminal opens the destination with the SSH login's credentials. A separate
// reply socket for each open lets etserver keep draining terminal output.
class TerminalSocketRequests {
 public:
#ifdef __linux__
  TerminalSocketRequests(const TerminalUserInfo& user, PipeSocketHandler& pipe,
                         PortForwardHandler& forwards, Connection& client);
  TerminalSocketRequests(const TerminalSocketRequests&) = delete;
  TerminalSocketRequests& operator=(const TerminalSocketRequests&) = delete;
  ~TerminalSocketRequests();
  bool tryHandle(PortForwardDestinationRequest& request);
  void addFds(std::set<int>& read, std::set<int>& write,
              std::set<int>& refresh) const;
  void update(std::set<int>& readable, const std::set<int>& writable);

 private:
  void respond(const PortForwardDestinationRequest& request, int fd, int error);
  int routerFd;
  PipeSocketHandler& pipe;
  PortForwardHandler& forwards;
  Connection& client;
  using Clock = std::chrono::steady_clock;
  struct Request {
    PortForwardDestinationRequest destination;
    Clock::time_point deadline;
    int replyFd = -1;
    Clock::time_point retryAt{};
  };
  std::list<Request> pending;
#else
  TerminalSocketRequests(const TerminalUserInfo&, PipeSocketHandler&,
                         PortForwardHandler&, Connection&) {}
  bool tryHandle(PortForwardDestinationRequest&) const { return false; }
  void addFds(std::set<int>&, std::set<int>&, std::set<int>&) const {}
  void update(std::set<int>&, const std::set<int>&) const {}
#endif
};
}  // namespace et
