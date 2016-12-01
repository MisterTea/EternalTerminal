#ifndef __ETERNAL_TCP_C_API__
#define __ETERNAL_TCP_C_API__

#include <unistd.h>

struct CSocketHandler {
  ssize_t (*read)(int fd, void* buf, size_t count);
  ssize_t (*write)(int fd, const void* buf, size_t count);

  ssize_t (*readAll)(int fd, void* buf, size_t count);
  ssize_t (*writeAll)(int fd, const void* buf, size_t count);

  ssize_t (*readAllTimeout)(int fd, void* buf, size_t count);
  ssize_t (*writeAllTimeout)(int fd, const void* buf, size_t count);

  int (*connect)(const void* context);
  int (*listen)(int port);
  void (*close)(int fd);
};

#endif // __ETERNAL_TCP_C_API__
