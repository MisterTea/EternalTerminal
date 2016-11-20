#include "Headers.hpp"

void equalOrFatal(ssize_t expected, ssize_t actual) {
  if (expected != actual) {
    std::stringstream ostr;
    ostr << "equalOrFatal " << expected << " != " << actual;
    throw std::runtime_error(ostr.str().c_str());
  }
}
