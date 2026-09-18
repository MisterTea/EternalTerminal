#ifndef __PSUEDO_USER_TERMINAL_HPP__
#define __PSUEDO_USER_TERMINAL_HPP__

#ifdef WIN32
#include "PseudoUserTerminalWindows.hpp"
#else
#include "PseudoUserTerminalUnix.hpp"
#endif

#endif
