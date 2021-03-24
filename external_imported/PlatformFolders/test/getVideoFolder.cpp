#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getVideoFolder());
	sago::PlatformFolders p;
	run_test(p.getVideoFolder());
	return 0;
}
