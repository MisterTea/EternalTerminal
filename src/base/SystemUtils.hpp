#ifndef __ET_SYSTEM_UTILS__
#define __ET_SYSTEM_UTILS__

#include "Headers.hpp"

#include <pwd.h>

namespace et {
void rootToUser(passwd* pwd);
}

#endif  // __ET_SYSTEM_UTILS__
