#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getMusicFolder());
	sago::PlatformFolders p;
	run_test(p.getMusicFolder());
	return 0;
}
