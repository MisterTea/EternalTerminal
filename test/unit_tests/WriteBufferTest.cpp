#include "TestHeaders.hpp"
#include "WriteBuffer.hpp"

using namespace et;

TEST_CASE("WriteBuffer basic operations", "[WriteBuffer]") {
  WriteBuffer buffer;

  SECTION("Empty buffer state") {
    REQUIRE(buffer.canAcceptMore() == true);
    REQUIRE(buffer.hasPendingData() == false);
    REQUIRE(buffer.size() == 0);

    size_t count;
    const char* data = buffer.peekData(&count);
    REQUIRE(data == nullptr);
    REQUIRE(count == 0);
  }

  SECTION("Enqueue and peek") {
    buffer.enqueue("hello");
    REQUIRE(buffer.hasPendingData() == true);
    REQUIRE(buffer.size() == 5);

    size_t count;
    const char* data = buffer.peekData(&count);
    REQUIRE(data != nullptr);
    REQUIRE(count == 5);
    REQUIRE(string(data, count) == "hello");
  }

  SECTION("Consume partial") {
    buffer.enqueue("hello");
    buffer.consume(2);
    REQUIRE(buffer.size() == 3);

    size_t count;
    const char* data = buffer.peekData(&count);
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
    const char* data = buffer.peekData(&count);
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
    const char* data = buffer.peekData(&count);
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
  WriteBuffer buffer(WriteBufferMode::BACKPRESSURE);

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

  SECTION("default mode is backpressure") {
    WriteBuffer defaultBuffer;
    REQUIRE(defaultBuffer.getMode() == WriteBufferMode::BACKPRESSURE);
  }
}

TEST_CASE("WriteBuffer discard mode", "[WriteBuffer]") {
  WriteBuffer buffer(WriteBufferMode::DISCARD);

  SECTION("canAcceptMore always returns true") {
    string largeChunk(WriteBuffer::MAX_BUFFER_SIZE, 'x');
    buffer.enqueue(largeChunk);

    // In discard mode, canAcceptMore is always true
    REQUIRE(buffer.canAcceptMore() == true);
  }

  SECTION("old data is discarded when buffer overflows") {
    // Fill with chunk A (128KB)
    const size_t halfSize = WriteBuffer::MAX_BUFFER_SIZE / 2;
    string chunkA(halfSize, 'A');
    buffer.enqueue(chunkA);
    REQUIRE(buffer.size() == halfSize);

    // Fill with chunk B (128KB) -- total now equals MAX
    string chunkB(halfSize, 'B');
    buffer.enqueue(chunkB);
    REQUIRE(buffer.size() == WriteBuffer::MAX_BUFFER_SIZE);

    // Add chunk C (128KB) -- should discard chunk A
    string chunkC(halfSize, 'C');
    buffer.enqueue(chunkC);

    // Buffer should be within MAX_BUFFER_SIZE
    REQUIRE(buffer.size() <= WriteBuffer::MAX_BUFFER_SIZE);

    // The oldest data (chunk A) should be gone
    // The front of the buffer should now be chunk B
    size_t count;
    const char* data = buffer.peekData(&count);
    REQUIRE(data != nullptr);
    REQUIRE(data[0] == 'B');
  }

  SECTION("buffer stays bounded with many enqueues") {
    const size_t chunkSize = 4096;
    // Enqueue much more than MAX_BUFFER_SIZE
    for (size_t i = 0; i < WriteBuffer::MAX_BUFFER_SIZE * 4; i += chunkSize) {
      string chunk(chunkSize, 'A' + ((i / chunkSize) % 26));
      buffer.enqueue(chunk);
      // Buffer should never exceed MAX_BUFFER_SIZE + one chunk
      REQUIRE(buffer.size() <= WriteBuffer::MAX_BUFFER_SIZE + chunkSize);
    }
  }

  SECTION("newest data is preserved") {
    // Fill buffer well past capacity
    const size_t chunkSize = 1024;
    string lastChunk;
    for (size_t i = 0; i < WriteBuffer::MAX_BUFFER_SIZE * 2; i += chunkSize) {
      lastChunk = string(chunkSize, 'A' + ((i / chunkSize) % 26));
      buffer.enqueue(lastChunk);
    }

    // Drain the buffer and verify the last chunk is in the output
    string allData;
    while (buffer.hasPendingData()) {
      size_t count;
      const char* data = buffer.peekData(&count);
      allData.append(data, count);
      buffer.consume(count);
    }

    // The tail of the drained data should be our last chunk
    REQUIRE(allData.size() >= lastChunk.size());
    REQUIRE(allData.substr(allData.size() - lastChunk.size()) == lastChunk);
  }
}
