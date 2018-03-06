#include "FakeSocketHandler.hpp"

namespace et {
FakeSocketHandler::FakeSocketHandler() : remoteHandler(NULL), nextFd(1) {}

FakeSocketHandler::FakeSocketHandler(
    std::shared_ptr<FakeSocketHandler> remoteHandler_)
    : remoteHandler(remoteHandler_),  //
      nextFd(1) {}

#define FAKE_READ_TIMEOUT (5)

bool FakeSocketHandler::hasData(int fd) {
  if (inBuffers[fd].length() > 0) {
    return true;
  }
  return false;
}

ssize_t FakeSocketHandler::read(int fd, void* buf, size_t count) {
  LOG(INFO) << "Reading " << count << " bytes";
  time_t timeout = time(NULL) + FAKE_READ_TIMEOUT;
  while (true) {
    if (time(NULL) > timeout) {
      LOG(ERROR) << "Timeout during read " << fd;
      errno = ECONNRESET;
      return -1;
    }
    bool keepWaiting = false;
    // VLOG(1) << int64_t(this) << ": Reading on fd " << fd;
    {
      std::lock_guard<std::mutex> guard(handlerMutex);
      if (closedFds.find(fd) != closedFds.end()) {
        // Socket was closed by force, this is a goner
        LOG(INFO) << "Trying to read from closed socket";
        errno = ECONNRESET;
        return -1;
      }
      if (inBuffers[fd].length() < count) {
        keepWaiting = true;
      }
    }
    if (keepWaiting) {
      sleep(1);
      continue;
    }
    {
      std::lock_guard<std::mutex> guard(handlerMutex);
      memcpy(buf, &inBuffers[fd][0], count);
      inBuffers[fd] =
          inBuffers[fd].substr(count);  // Very slow, only for testing
      return count;
    }
  }
}

ssize_t FakeSocketHandler::write(int fd, const void* buf, size_t count) {
  if (remoteHandler.get() == NULL) {
    throw std::runtime_error("Invalid remote handler");
  }
  if (fd < 0) {
    LOG(FATAL) << "Tried to write with an invalid socket descriptor: " << fd;
  }
  remoteHandler->push(fd, (const char*)buf, count);
  return count;
}

int FakeSocketHandler::connect(const std::string&, int) {
  int fd;
  {
    std::lock_guard<std::mutex> guard(handlerMutex);
    fd = nextFd++;
    VLOG(1) << "CLIENT: Connecting to server with fd " << fd;
    inBuffers[fd] = "";
  }
  remoteHandler->addConnection(fd);
  while (true) {
    if (!remoteHandler->hasPendingConnection()) {
      VLOG(1) << "CLIENT: Connect finished with server and fd " << fd;
      return fd;
    }
  }
}

void FakeSocketHandler::listen(int) {}

int FakeSocketHandler::accept(int) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  if (futureConnections.empty()) {
    return -1;
  }
  int retval = futureConnections.back();
  VLOG(1) << "SERVER: Accepting client with fd " << retval;
  inBuffers[retval] = "";
  futureConnections.pop_back();
  return retval;
}

void FakeSocketHandler::stopListening(int) {}

void FakeSocketHandler::close(int fd) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  closedFds.insert(fd);
  if (inBuffers.find(fd) == inBuffers.end()) {
    VLOG(1) << int64_t(this) << ": Got request to erase client " << fd
            << " but it was already gone ";
    return;
  }
  VLOG(1) << int64_t(this) << ": Erasing client " << fd;
  inBuffers.erase(inBuffers.find(fd));
}

void FakeSocketHandler::push(int fd, const char* buf, size_t count) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  VLOG(1) << "Accepting buffer from " << fd << " of size " << count;
  if (inBuffers.find(fd) == inBuffers.end()) {
    VLOG(1) << "Tried to accept buffer from invalid fd: " << fd;
    return;
  }
  inBuffers[fd].append(buf, count);
}

void FakeSocketHandler::addConnection(int fd) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  VLOG(1) << "SERVER: Adding pending connection from " << fd;
  futureConnections.push_back(fd);
}

bool FakeSocketHandler::hasPendingConnection() {
  std::lock_guard<std::mutex> guard(handlerMutex);
  return !futureConnections.empty();
}
}  // namespace et
