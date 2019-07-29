#ifndef __DAEMON_CREATOR_H__
#define __DAEMON_CREATOR_H__

#include "Headers.hpp"

namespace et {
class DaemonCreator {
 public:
  static int create(bool terminateParent, string childPidFile);
  static const int PARENT = 1;
  static const int CHILD = 2;
};
}  // namespace et

#endif  // __DAEMON_CREATOR_H__
