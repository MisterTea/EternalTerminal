#include "tester.hpp"
#include "../sago/platform_folders.h"

int main() {
	run_test(sago::getSaveGamesFolder1());
	sago::PlatformFolders p;
	run_test(p.getSaveGamesFolder1());
	return 0;
}
