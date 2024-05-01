# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [4.2.0] 2022-02-06

### Added
 - Support for "state" dir aka Local Machine

### Changed
 - PlatformFolders specific CMake variables are now prefixed with "PLATFORMFOLDERS_" (Thanks @OlivierLDff)

## [4.1.0] 2021-08-08

### Added
 - CHANGELOG.md
 - Support for CMAKE_DEBUG_POSTFIX. Makes it possible to add a postfix to debug builds

### Changed
 - README.md is now included in the Doxygen documentation
 - Should now be thread safe
 - Minor internal changes

## [4.0.0] 2018-06-24

### Added
 - Support for "Public" folder
 - Support for "Saved Games 2". This will be "Saved Games" in Vista and newer
 - Unit tests


### Changed
 - The correct "Download" folder is now returned on Windows
 - Now requires a C++11 compatible compiler (was C++98)
 - Minimum Windows version raised from Windows XP to Windows 7.
 - The Mac version no longer depends on CoreServices
 - The XDG implementation are slightly more resilient to unrelated environments in user-dirs.dirs
 - Improved CMake system

### Removed
 - C++98 compatibility
 - Windows XP compatibility
 - No longer needs CoreServices on Mac OS X

## [3.2.0] 2018-05-28

### Added
 - Stand-alone functions. Method calls are no longer needed or recommended.

### Changed
 - Fixed a bug in the xdg cache function that caused sago::getCacheDir() to return the wrong folder if XDG_CONFIG_HOME and/or XDG_CACHE_HOME were set

## [3.1] 2018-04-21

### Added
 - Now uses CMake for testing (Thanks @sum01)
 - It is now possible to compile as a (static) library. (Thanks @sum01)
 - Appveyor CI integration (Thanks @sum01)

### Changed
 - Optimised use of iterator.
 - No longer keeps empty data structure on Mac and Windows platforms.

## [3.0] 2016-10-08

### Changed
 - On Windows the library now returns UTF-8 encoded paths as default.

## [2.2] 2016-09-06

### Fixed
 - Fixed a buffer overflow introduced in 2.1 (originally created to ensure C++03 compatibility)
 - The example file no longer abuses namespaces.


## [2.1] 2016-02-22

### Changed
 - Updated documentation URL
 - No longer uses a variable length array internally


## [2.0] 2015-10-26

### Added
- Mac OS X support using Core Framework

## [1.0] 2015-09-21
Designed to work with Linux and Windows XP+
