#ifndef __ET_UTEMPTER__
#define __ET_UTEMPTER__

#ifdef UTEMPTER_DLOPEN
#include <dlfcn.h>
#else
#include <utempter.h>
#endif

namespace et {
#ifdef UTEMPTER_DLOPEN
/**
 * @brief libutempter entry points, resolved on first use. Both stay null when
 * the library is not installed, and records are then skipped.
 */
struct UtempterApi {
  int (*addRecord)(int, const char*) = nullptr;
  int (*removeRecord)(int) = nullptr;
};

inline const UtempterApi& utempterApi() {
  static const UtempterApi api = [] {
    UtempterApi loaded;
    if (void* handle = dlopen("libutempter.so.0", RTLD_NOW | RTLD_LOCAL)) {
      loaded.addRecord = reinterpret_cast<int (*)(int, const char*)>(
          dlsym(handle, "utempter_add_record"));
      loaded.removeRecord = reinterpret_cast<int (*)(int)>(
          dlsym(handle, "utempter_remove_record"));
    }
    return loaded;
  }();
  return api;
}

inline void utempterAddRecord(int masterFd, const char* hostname) {
  if (auto add = utempterApi().addRecord) {
    add(masterFd, hostname);
  }
}

inline void utempterRemoveRecord(int masterFd) {
  if (auto remove = utempterApi().removeRecord) {
    remove(masterFd);
  }
}
#else
inline void utempterAddRecord(int masterFd, const char* hostname) {
  utempter_add_record(masterFd, hostname);
}

inline void utempterRemoveRecord(int masterFd) {
  utempter_remove_record(masterFd);
}
#endif
}  // namespace et

#endif  // __ET_UTEMPTER__
