# os_release

Adapted from https://github.com/chef/os_release as data for unit-testing.

This repo contains the /etc/os-release file from Linux distros.

## About os-release

/etc/os-release is a standard Linux file used to identify the OS, os release, and similar distros. It's now a standard file in systemd distros, but it can be found in just about every Linux distro released over the last 3-4 years.

## Why collect them

The fields in /etc/os-release are standardized, but the values are not. The only way to know that Redhat Enterprise Linux is 'rhel' or that openSUSE 15 is 'opensuse-leap' is to install the distros and check the file. This repo is a large collection of os-release files so you don't need to install each and every distro.
