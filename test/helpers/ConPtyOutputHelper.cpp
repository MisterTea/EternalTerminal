#include <windows.h>

#include <array>
#include <cstddef>

int main() {
  // Inherited std handles may not be the pseudoconsole; CONOUT$ always is.
  HANDLE output =
      CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (output == nullptr || output == INVALID_HANDLE_VALUE) {
    return 1;
  }

  std::array<char, 16 * 1024> chunk;
  chunk.fill('A');
  constexpr size_t totalBytes = 1024 * 1024;
  size_t totalWritten = 0;
  while (totalWritten < totalBytes) {
    size_t chunkOffset = 0;
    while (chunkOffset < chunk.size()) {
      DWORD written = 0;
      if (!WriteFile(output, chunk.data() + chunkOffset,
                     static_cast<DWORD>(chunk.size() - chunkOffset), &written,
                     nullptr) ||
          written == 0) {
        CloseHandle(output);
        return 2;
      }
      chunkOffset += written;
      totalWritten += written;
    }
  }
  CloseHandle(output);
  return 0;
}
