#include "FakeSocketHandler.hpp"

FakeSocketHandler::FakeSocketHandler() :
  remoteHandler(NULL),
  nextFd(1) {
}

FakeSocketHandler::FakeSocketHandler(
  std::shared_ptr<FakeSocketHandler> remoteHandler_) :
  remoteHandler(remoteHandler_),
  nextFd(1) {
}

#define FAKE_READ_TIMEOUT (5)

ssize_t FakeSocketHandler::read(int fd, void* buf, size_t count) {
  time_t timeout = time(NULL) + FAKE_READ_TIMEOUT;
  while (true) {
    if (time(NULL) > timeout) {
      errno = ECONNRESET;
      return -1;
    }
    bool keepWaiting = false;
    //cout << int64_t(this) << ": Reading on fd " << fd << endl;
    {
      std::lock_guard<std::mutex> guard(handlerMutex);
      if (closedFds.find(fd) != closedFds.end()) {
        // Socket was closed by force, this is a goner
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
      memcpy(buf,&inBuffers[fd][0],count);
      inBuffers[fd] = inBuffers[fd].substr(count); // Very slow, only for testing
      return count;
    }
  }
}

ssize_t FakeSocketHandler::write(int fd, const void* buf, size_t count) {
  if (remoteHandler.get() == NULL) {
    throw std::runtime_error("Invalid remote handler");
  }
  //std::lock_guard<std::mutex> guard(handlerMutex);
  remoteHandler->push(fd, (const char*)buf,count);
  return count;
}

int FakeSocketHandler::connect(const std::string &, int) {
  int fd;
  {
    std::lock_guard<std::mutex> guard(handlerMutex);
    fd = nextFd++;
    cout << "CLIENT: Connecting to server with fd " << fd << endl;
    inBuffers[fd] = "";
  }
  remoteHandler->addConnection(fd);
  while (true) {
    if (!remoteHandler->hasPendingConnection()) {
      cout << "CLIENT: Connect finished with server and fd " << fd << endl;
      return fd;
    }
  }
}

int FakeSocketHandler::listen(int) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  if(futureConnections.empty()) {
    return -1;
  }
  int retval = futureConnections.back();
  cout << "SERVER: Accepting client with fd " << retval << endl;
  inBuffers[retval] = "";
  futureConnections.pop_back();
  return retval;
}

void FakeSocketHandler::close(int fd) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  closedFds.insert(fd);
  if (inBuffers.find(fd) == inBuffers.end()) {
    cout << int64_t(this) << ": Got request to erase client " << fd << " but it was already gone " << endl;
    return;
  }
  cout << int64_t(this) << ": Erasing client " << fd << endl;
  inBuffers.erase(inBuffers.find(fd));
}

void FakeSocketHandler::push(int fd, const char* buf, size_t count) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  cout << "Accepting buffer from " << fd << " of size " << count << endl;
  if (inBuffers.find(fd) == inBuffers.end()) {
    cout << "Tried to accept buffer from invalid fd" << endl;
    return;
  }
  inBuffers[fd].append(buf,count);
}

void FakeSocketHandler::addConnection(int fd) {
  std::lock_guard<std::mutex> guard(handlerMutex);
  cout << "SERVER: Adding pending connection from " << fd << endl;
  futureConnections.push_back(fd);
}

bool FakeSocketHandler::hasPendingConnection() {
  std::lock_guard<std::mutex> guard(handlerMutex);
  return !futureConnections.empty();
}
