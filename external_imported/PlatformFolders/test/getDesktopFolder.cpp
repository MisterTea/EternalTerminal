#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getDesktopFolder());
	sago::PlatformFolders p;
	run_test(p.getDesktopFolder());
	return 0;
}
