#include "FlakyFakeSocketHandler.hpp"

FlakyFakeSocketHandler::FlakyFakeSocketHandler(int _chance) :
  FakeSocketHandler(),
  chance(_chance) {
}

FlakyFakeSocketHandler::FlakyFakeSocketHandler(
  std::shared_ptr<FakeSocketHandler> remoteHandler_,
  int _chance) :
  FakeSocketHandler(remoteHandler_),
  chance(_chance) {
}

ssize_t FlakyFakeSocketHandler::read(int i, void* buf, size_t count) {
  if (rand()%chance==1) {
    cout << "read failed\n";
    errno = ECONNRESET;
    return -1;
  }
  return FakeSocketHandler::read(i,buf,count);
}

ssize_t FlakyFakeSocketHandler::write(int i, const void* buf, size_t count) {
  if (rand()%chance==1) {
    cout << "write failed\n";
    errno = EPIPE;
    return -1;
  }
  return FakeSocketHandler::write(i,buf,count);
}
