#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getConfigHome());
	return 0;
}
