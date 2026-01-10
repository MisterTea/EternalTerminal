#include "WriteBuffer.hpp"

#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("WriteBuffer basic operations", "[WriteBuffer]") {
  WriteBuffer buffer;

  SECTION("Empty buffer state") {
    REQUIRE(buffer.canAcceptMore() == true);
    REQUIRE(buffer.hasPendingData() == false);
    REQUIRE(buffer.size() == 0);

    size_t count;
    const char *data = buffer.peekData(&count);
    REQUIRE(data == nullptr);
    REQUIRE(count == 0);
  }

  SECTION("Enqueue and peek") {
    buffer.enqueue("hello");
    REQUIRE(buffer.hasPendingData() == true);
    REQUIRE(buffer.size() == 5);

    size_t count;
    const char *data = buffer.peekData(&count);
    REQUIRE(data != nullptr);
    REQUIRE(count == 5);
    REQUIRE(string(data, count) == "hello");
  }

  SECTION("Consume partial") {
    buffer.enqueue("hello");
    buffer.consume(2);
    REQUIRE(buffer.size() == 3);

    size_t count;
    const char *data = buffer.peekData(&count);
    REQUIRE(count == 3);
    REQUIRE(string(data, count) == "llo");
  }

  SECTION("Consume full chunk") {
    buffer.enqueue("hello");
    buffer.enqueue("world");
    REQUIRE(buffer.size() == 10);

    buffer.consume(5);
    REQUIRE(buffer.size() == 5);

    size_t count;
    const char *data = buffer.peekData(&count);
    REQUIRE(count == 5);
    REQUIRE(string(data, count) == "world");
  }

  SECTION("Consume across chunks") {
    buffer.enqueue("abc");
    buffer.enqueue("defgh");
    REQUIRE(buffer.size() == 8);

    buffer.consume(5);  // Consumes "abc" + "de"
    REQUIRE(buffer.size() == 3);

    size_t count;
    const char *data = buffer.peekData(&count);
    REQUIRE(count == 3);
    REQUIRE(string(data, count) == "fgh");
  }

  SECTION("Clear buffer") {
    buffer.enqueue("hello");
    buffer.enqueue("world");
    buffer.clear();

    REQUIRE(buffer.hasPendingData() == false);
    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.canAcceptMore() == true);
  }

  SECTION("Empty string enqueue is ignored") {
    buffer.enqueue("");
    REQUIRE(buffer.hasPendingData() == false);
    REQUIRE(buffer.size() == 0);
  }
}

TEST_CASE("WriteBuffer backpressure", "[WriteBuffer]") {
  WriteBuffer buffer;

  SECTION("canAcceptMore returns false when buffer is full") {
    // Fill the buffer to capacity
    string largeChunk(WriteBuffer::MAX_BUFFER_SIZE, 'x');
    buffer.enqueue(largeChunk);

    REQUIRE(buffer.canAcceptMore() == false);
    REQUIRE(buffer.size() == WriteBuffer::MAX_BUFFER_SIZE);

    // Consume some data
    buffer.consume(1024);
    REQUIRE(buffer.canAcceptMore() == true);
  }
}
