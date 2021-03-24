#ifndef SAGO_TEST_HPP
#define SAGO_TEST_HPP

#include <string>
#include <vector>

// std::string is expected input
void run_test(const std::string&);

// A special overload for the two funcs that take a vector
void run_test(const std::vector<std::string>&);

#endif
