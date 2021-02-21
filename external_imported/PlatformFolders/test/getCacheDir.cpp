#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getCacheDir());
	return 0;
}
