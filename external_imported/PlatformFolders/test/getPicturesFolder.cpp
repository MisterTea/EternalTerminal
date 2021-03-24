#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getPicturesFolder());
	sago::PlatformFolders p;
	run_test(p.getPicturesFolder());
	return 0;
}
