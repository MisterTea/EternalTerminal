#include "tester.hpp"
#include "../sago/platform_folders.h"
#include <string>
#include <vector>

// This is all tests in one
int main() {
	// Test plain functions
	run_test(sago::getConfigHome());
	run_test(sago::getDataHome());
	run_test(sago::getCacheDir());
	// Test non-member functions
	run_test(sago::getDesktopFolder());
	run_test(sago::getDocumentsFolder());
	run_test(sago::getDownloadFolder());
	run_test(sago::getDownloadFolder1());
	run_test(sago::getPicturesFolder());
	run_test(sago::getPublicFolder());
	run_test(sago::getMusicFolder());
	run_test(sago::getVideoFolder());
	run_test(sago::getSaveGamesFolder1());
	run_test(sago::getSaveGamesFolder2());
	// Test class methods
	sago::PlatformFolders p;
	run_test(p.getDocumentsFolder());
	run_test(p.getDesktopFolder());
	run_test(p.getPicturesFolder());
	run_test(p.getMusicFolder());
	run_test(p.getVideoFolder());
	run_test(p.getDownloadFolder1());
	run_test(p.getSaveGamesFolder1());
	// Test vector function
	std::vector<std::string> extraData;
	sago::appendAdditionalDataDirectories(extraData);
	run_test(extraData);
	return 0;
}
