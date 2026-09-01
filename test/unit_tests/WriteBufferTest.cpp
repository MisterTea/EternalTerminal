#include "TestHeaders.hpp"
#include "WriteBuffer.hpp"

using namespace et;

TEST_CASE("WriteBuffer basic operations", "[WriteBuffer]") {
  WriteBuffer buffer;

  SECTION("empty buffer") {
    REQUIRE_FALSE(buffer.hasPendingData());
    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.canAcceptMore());
    REQUIRE_FALSE(buffer.shouldFlushOnInterrupt());
    size_t count = 123;
    REQUIRE(buffer.peekData(&count) == nullptr);
    REQUIRE(count == 0);
  }

  SECTION("enqueue and peek") {
    buffer.enqueue("hello");
    REQUIRE(buffer.hasPendingData());
    REQUIRE(buffer.size() == 5);
    size_t count = 0;
    const char* data = buffer.peekData(&count);
    REQUIRE(count == 5);
    REQUIRE(string(data, count) == "hello");
  }

  SECTION("empty enqueue is a no-op") {
    buffer.enqueue("");
    REQUIRE_FALSE(buffer.hasPendingData());
    REQUIRE(buffer.size() == 0);
  }

  SECTION("consume partial then remainder") {
    buffer.enqueue("abcdef");
    buffer.consume(2);
    REQUIRE(buffer.size() == 4);
    size_t count = 0;
    const char* data = buffer.peekData(&count);
    REQUIRE(string(data, count) == "cdef");
    buffer.consume(4);
    REQUIRE_FALSE(buffer.hasPendingData());
    REQUIRE(buffer.size() == 0);
  }

  SECTION("consume across chunks") {
    buffer.enqueue("aa");
    buffer.enqueue("bb");
    buffer.enqueue("cc");
    buffer.consume(3);
    REQUIRE(buffer.size() == 3);
    size_t count = 0;
    const char* data = buffer.peekData(&count);
    REQUIRE(string(data, count) == "b");
    buffer.consume(1);
    data = buffer.peekData(&count);
    REQUIRE(string(data, count) == "cc");
  }

  SECTION("consume zero is a no-op") {
    buffer.enqueue("hi");
    buffer.consume(0);
    REQUIRE(buffer.size() == 2);
  }
}

TEST_CASE("WriteBuffer interrupt detection", "[WriteBuffer]") {
  REQUIRE(WriteBuffer::containsInterruptByte(string(1, '\x03')));
  REQUIRE(WriteBuffer::containsInterruptByte(string(1, '\x1a')));
  REQUIRE(WriteBuffer::containsInterruptByte(string(1, '\x1c')));
  REQUIRE(WriteBuffer::containsInterruptByte(string("abc") + '\x03' + "def"));
  REQUIRE_FALSE(WriteBuffer::containsInterruptByte("hello"));
  REQUIRE_FALSE(WriteBuffer::containsInterruptByte(""));
}

TEST_CASE("WriteBuffer flush threshold", "[WriteBuffer]") {
  WriteBuffer buffer;

  SECTION("below threshold is not flushed") {
    buffer.enqueue(string(WriteBuffer::FLUSH_THRESHOLD - 1, 'x'));
    REQUIRE_FALSE(buffer.shouldFlushOnInterrupt());
    REQUIRE(buffer.flushIfLarge() == 0);
    REQUIRE(buffer.size() == WriteBuffer::FLUSH_THRESHOLD - 1);
  }

  SECTION("at threshold is flushed") {
    buffer.enqueue(string(WriteBuffer::FLUSH_THRESHOLD, 'x'));
    REQUIRE(buffer.shouldFlushOnInterrupt());
    REQUIRE(buffer.flushIfLarge() == WriteBuffer::FLUSH_THRESHOLD);
    REQUIRE(buffer.size() == 0);
    REQUIRE_FALSE(buffer.hasPendingData());
  }

  SECTION("clear drops everything regardless of size") {
    buffer.enqueue("abc");
    REQUIRE(buffer.clear() == 3);
    REQUIRE(buffer.size() == 0);
  }
}

TEST_CASE("WriteBuffer Ctrl+C drops a large backlog so the prompt is next",
          "[WriteBuffer]") {
  // Same shape as a slow-link manual test: ~200KB of unsent output, then
  // interrupt, then a prompt. Cheap: no sockets, no throttle, milliseconds.
  WriteBuffer buffer;
  const string flood(200 * 1024, 'A');
  buffer.enqueue(flood);
  REQUIRE(buffer.size() == flood.size());
  REQUIRE(buffer.shouldFlushOnInterrupt());

  REQUIRE(WriteBuffer::containsInterruptByte(string(1, '\x03')));
  REQUIRE(buffer.flushIfLarge() == flood.size());
  REQUIRE(buffer.size() == 0);

  buffer.enqueue("PROMPT");
  size_t count = 0;
  const char* data = buffer.peekData(&count);
  REQUIRE(string(data, count) == "PROMPT");
}

TEST_CASE("WriteBuffer hard cap", "[WriteBuffer]") {
  WriteBuffer buffer;
  REQUIRE(buffer.canAcceptMore());

  buffer.enqueue(string(WriteBuffer::MAX_BUFFER_SIZE, 'x'));
  REQUIRE(buffer.size() == WriteBuffer::MAX_BUFFER_SIZE);
  REQUIRE_FALSE(buffer.canAcceptMore());

  // Soft bound: one more chunk is still admitted by enqueue; callers must
  // stop reading when canAcceptMore() is false.
  buffer.enqueue("y");
  REQUIRE(buffer.size() == WriteBuffer::MAX_BUFFER_SIZE + 1);
  REQUIRE_FALSE(buffer.canAcceptMore());

  REQUIRE(buffer.flushIfLarge() == WriteBuffer::MAX_BUFFER_SIZE + 1);
  REQUIRE(buffer.canAcceptMore());
  REQUIRE(buffer.size() == 0);
}
