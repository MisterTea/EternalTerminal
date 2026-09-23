#ifndef __PIPE_USER_TERMINAL_HPP__
#define __PIPE_USER_TERMINAL_HPP__

#ifdef WIN32
#include "PipeUserTerminalWindows.hpp"
#else
#include "PipeUserTerminalUnix.hpp"
#endif

#endif
