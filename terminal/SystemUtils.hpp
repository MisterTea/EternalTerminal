#ifndef __ETERNAL_TCP_SYSTEM_UTILS__
#define __ETERNAL_TCP_SYSTEM_UTILS__

#include "Headers.hpp"

#include "PamHandler.hpp"

#include <pwd.h>

namespace et {
shared_ptr<PamHandler> rootToUser(passwd* pwd);
}

#endif  // __ETERNAL_TCP_SYSTEM_UTILS__
