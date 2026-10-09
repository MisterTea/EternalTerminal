#include "ConsoleKeyEventWindows.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
constexpr uint32_t kRightCtrl = 0x0004;
constexpr uint32_t kLeftCtrl = 0x0008;
constexpr uint32_t kShift = 0x0010;
constexpr uint32_t kLeftAlt = 0x0002;
constexpr uint16_t kVkShift = 0x10;
constexpr uint16_t kVkControl = 0x11;
constexpr uint16_t kVkMenu = 0x12;
constexpr uint16_t kVkCapital = 0x14;
constexpr uint16_t kVkSpace = 0x20;
constexpr uint16_t kVkLWin = 0x5B;
constexpr uint16_t kVkLControl = 0xA2;
constexpr uint16_t kVkProcessKey = 0xE5;

ConsoleKeyEvent keyDown(uint16_t vk, uint16_t ch, uint32_t state = 0) {
  return ConsoleKeyEvent{true, vk, ch, state};
}

ConsoleKeyEvent keyUp(uint16_t vk, uint16_t ch, uint32_t state = 0) {
  return ConsoleKeyEvent{false, vk, ch, state};
}

std::string forwarded(const std::vector<ConsoleKeyEvent>& events) {
  std::string out;
  for (const auto& event : events) {
    if (consoleKeyEventHasInput(event)) {
      out += static_cast<char>(event.unicodeChar);
    }
  }
  return out;
}
}  // namespace

TEST_CASE("Console key events forward printable and control characters",
          "[ConsoleKeyEvent]") {
  CHECK(consoleKeyEventHasInput(keyDown('A', 'a')));
  CHECK(consoleKeyEventHasInput(keyDown('B', 0x02, kLeftCtrl)));
  CHECK(consoleKeyEventHasInput(keyDown(0, 0x1b)));
  CHECK(consoleKeyEventHasInput(keyDown(0, '[')));
}

TEST_CASE("Console key events ignore key releases", "[ConsoleKeyEvent]") {
  CHECK_FALSE(consoleKeyEventHasInput(keyUp('A', 'a')));
  CHECK_FALSE(consoleKeyEventHasInput(keyUp(kVkControl, 0, 0)));
}

TEST_CASE("Console key events drop bare modifier presses",
          "[ConsoleKeyEvent]") {
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkControl, 0, kLeftCtrl)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkLControl, 0, kLeftCtrl)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkControl, 0, kRightCtrl)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkShift, 0, kShift)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkMenu, 0, kLeftAlt)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkLWin, 0, 0)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkCapital, 0, 0)));
  CHECK_FALSE(
      consoleKeyEventHasInput(keyDown(kVkShift, 0, kShift | kLeftCtrl)));
}

TEST_CASE("Console key events forward the NUL of a Ctrl chord",
          "[ConsoleKeyEvent]") {
  CHECK(consoleKeyEventHasInput(keyDown(kVkSpace, 0, kLeftCtrl)));
  CHECK(consoleKeyEventHasInput(keyDown(kVkSpace, 0, kRightCtrl)));
  CHECK(consoleKeyEventHasInput(keyDown('2', 0, kLeftCtrl | kShift)));
}

TEST_CASE("Console key events drop characterless presses without Ctrl",
          "[ConsoleKeyEvent]") {
  // Dead keys and IME composition report no character until it is complete.
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(0xDE, 0, 0)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkProcessKey, 0, 0)));
  CHECK_FALSE(consoleKeyEventHasInput(keyDown(kVkProcessKey, 0, kLeftCtrl)));
}

TEST_CASE("Console key events keep a tmux prefix chord intact",
          "[ConsoleKeyEvent]") {
  // Holding Ctrl auto-repeats the modifier record around the chord itself.
  const std::vector<ConsoleKeyEvent> ctrlB_d = {
      keyDown(kVkControl, 0, kLeftCtrl),
      keyDown(kVkControl, 0, kLeftCtrl),
      keyDown('B', 0x02, kLeftCtrl),
      keyDown(kVkControl, 0, kLeftCtrl),
      keyUp(kVkControl, 0, 0),
      keyDown('D', 'd'),
      keyUp('D', 'd'),
  };
  CHECK(forwarded(ctrlB_d) ==
        "\x02"
        "d");

  const std::vector<ConsoleKeyEvent> ctrlSpace = {
      keyDown(kVkControl, 0, kLeftCtrl),
      keyDown(kVkSpace, 0, kLeftCtrl),
      keyUp(kVkSpace, 0, kLeftCtrl),
      keyUp(kVkControl, 0, 0),
  };
  CHECK(forwarded(ctrlSpace) == std::string("\0", 1));
}
