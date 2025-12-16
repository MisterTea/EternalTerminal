#ifndef __ET_PROCESS_HELPER__
#define __ET_PROCESS_HELPER__

#include "Headers.hpp"

/**
 * @brief Helpers for performing common process level operations.
 */
class ProcessHelper {
 public:
  /** @brief Turns the caller into a background daemon process. */
  static void daemonize();
};

#endif  // __ET_PROCESS_HELPER__
