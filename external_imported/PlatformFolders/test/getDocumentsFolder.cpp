#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getDocumentsFolder());
	sago::PlatformFolders p;
	run_test(p.getDocumentsFolder());
	return 0;
}
