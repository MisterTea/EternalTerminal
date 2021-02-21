#include "tester.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// This should be passed either be char* or std::string for this to work
static void test_internal(const std::string& data) {
	try {
		// Check that it actually got anything
		if (data.empty()) {
			throw std::logic_error("Got empty data");
		}
	}
	catch (const std::exception& e)   {
		// Take any standard exception & output its message
		std::cerr << e.what() << std::endl;
		std::exit(EXIT_FAILURE);
	}
	catch (...)   {
		// If any non-std exception is thrown, also fail
		std::cerr << "Unknown exception!" << std::endl;
		std::exit(EXIT_FAILURE);
	}
}

// std::string is expected input
void run_test(const std::string& input) {
	test_internal(input);
}

// A special overload for the two funcs that take a vector
void run_test(const std::vector<std::string>& vec) {
	// Check each value for validity
	for (const std::string& elem : vec) {
		test_internal(elem);
	}
}
