#ifndef __ETERNAL_TCP_FLAKY_FAKE_SOCKET_HANDLER__
#define __ETERNAL_TCP_FLAKY_FAKE_SOCKET_HANDLER__

#include "FakeSocketHandler.hpp"

class FlakyFakeSocketHandler : public FakeSocketHandler {
public:
  explicit FlakyFakeSocketHandler();

  explicit FlakyFakeSocketHandler(std::shared_ptr<FakeSocketHandler> remoteHandler);

  virtual ssize_t read(int fd, void* buf, size_t count);
  virtual ssize_t write(int fd, const void* buf, size_t count);

protected:
};

#endif // __ETERNAL_TCP_FLAKY_FAKE_SOCKET_HANDLER__
