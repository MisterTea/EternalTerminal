#ifndef __ET_TIME_PARSER__
#define __ET_TIME_PARSER__

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

namespace et {

// Parse a time string as a sequence of the form time[qualifier] where time is a
// positive integer value and qualifier is one of the following:
//   <none> : seconds
//   s | S  : seconds
//   m | M  : minutes
//   h | H  : hours
//   d | D  : days
//   w | W  : weeks
inline long parseTimeString(std::string_view in, long invalid) {
  if (in.empty()) {
    return invalid;
  }

  int64_t time{};

  while (!in.empty()) {
    int64_t t;
    auto [ptr, ec] = std::from_chars(in.begin(), in.begin() + in.size(), t);

    if (ec != std::errc{}) {
      return invalid;
    }

    in = in.substr(ptr - in.begin());

    int64_t mult = 1;
    if (!in.empty()) {
      switch (in[0]) {
        case 's':
        case 'S':
          break;
        case 'm':
        case 'M':
          mult = 60;
          break;
        case 'h':
        case 'H':
          mult = 3600;
          break;
        case 'd':
        case 'D':
          mult = 86400;
          break;
        case 'w':
        case 'W':
          mult = 604800;
          break;
        default:
          return invalid;
      }
      in = in.substr(1);
    }
    if (t > (std::numeric_limits<int64_t>::max() - time) / mult) {
      return invalid;
    }
    time += t * mult;
  }

  if (time > std::numeric_limits<long>::max()) {
    return invalid;
  }

  return static_cast<long>(time);
}

}  // namespace et

#endif  // __ET_TIME_PARSER__
