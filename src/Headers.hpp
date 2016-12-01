#ifndef __ETERNAL_TCP_HEADERS__
#define __ETERNAL_TCP_HEADERS__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>    /* POSIX Threads */
#include <errno.h>
#include <time.h>

#include <string>
#include <algorithm>
#include <iostream>
#include <vector>
#include <array>
#include <memory>
#include <exception>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>

#include <boost/circular_buffer.hpp>

#include <glog/logging.h>

using namespace std;

void equalOrFatal(ssize_t expected, ssize_t actual);

#endif
