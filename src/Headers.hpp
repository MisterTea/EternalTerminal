#ifndef __ETERNAL_TCP_HEADERS__
#define __ETERNAL_TCP_HEADERS__

#include <errno.h>
#include <pthread.h> /* POSIX Threads */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <google/protobuf/message.h>
#include "ET.pb.h"

using namespace std;

static const int PROTOCOL_VERSION = 2;

#define FATAL_FAIL(X) \
  if ((X == -1)) LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno);

#endif