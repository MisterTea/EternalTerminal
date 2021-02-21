#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getDownloadFolder1());
	sago::PlatformFolders p;
	run_test(p.getDownloadFolder1());
	return 0;
}
