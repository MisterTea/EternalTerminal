#include "FakeSocketHandler.hpp"

FakeSocketHandler::FakeSocketHandler() :
  remoteHandler(NULL) {
}

FakeSocketHandler::FakeSocketHandler(
  std::shared_ptr<FakeSocketHandler> remoteHandler_) :
  remoteHandler(remoteHandler_) {
}

ssize_t FakeSocketHandler::read(int, void* buf, size_t count) {
  while (true) {
    bool keepWaiting = false;
    {
      boost::lock_guard<boost::mutex> guard(inBufferMutex);
      if (inBuffer.length() < count) {
        keepWaiting = true;
      }
    }
    if (keepWaiting) {
      sleep(1);
      continue;
    }
    {
      boost::lock_guard<boost::mutex> guard(inBufferMutex);
      memcpy(buf,&inBuffer[0],count);
      inBuffer = inBuffer.substr(count); // Very slow, only for testing
      return count;
    }
  }
}

ssize_t FakeSocketHandler::write(int, const void* buf, size_t count) {
  if (remoteHandler.get() == NULL) {
    throw std::runtime_error("Invalid remote handler");
  }
  remoteHandler->push((const char*)buf,count);
  return count;
}

void FakeSocketHandler::push(const char* buf, size_t count) {
  boost::lock_guard<boost::mutex> guard(inBufferMutex);
  inBuffer.append(buf,count);
}
