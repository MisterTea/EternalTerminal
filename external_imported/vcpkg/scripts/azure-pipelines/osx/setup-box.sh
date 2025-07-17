#!/bin/sh
hdiutil attach clt.dmg -mountpoint /Volumes/setup-installer
sudo installer -pkg "/Volumes/setup-installer/Command Line Tools.pkg" -target /
hdiutil detach /Volumes/setup-installer
rm clt.dmg
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
if [ `uname -m` = 'arm64' ]; then
    (echo; echo 'eval "$(/opt/homebrew/bin/brew shellenv)"') >> /Users/vcpkg/.zprofile
    eval "$(/opt/homebrew/bin/brew shellenv)"
else
    (echo; echo 'eval "$(/usr/local/bin/brew shellenv)"') >> /Users/vcpkg/.zprofile
    eval "$(/usr/local/bin/brew shellenv)"
fi
brew install autoconf-archive autoconf automake bison cmake gettext gfortran gperf gtk-doc libtool meson mono nasm ninja pkg-config powershell texinfo yasm
mkdir ~/Data
if [ `uname -m` = 'arm64' ]; then
curl -s -o ~/Downloads/azure-agent.tar.gz https://vstsagentpackage.azureedge.net/agent/3.232.1/vsts-agent-osx-arm64-3.232.1.tar.gz
else
curl -s -o ~/Downloads/azure-agent.tar.gz https://vstsagentpackage.azureedge.net/agent/3.232.1/vsts-agent-osx-x64-3.232.1.tar.gz
fi
mkdir ~/myagent
tar xf ~/Downloads/azure-agent.tar.gz -C ~/myagent
rm ~/Downloads/azure-agent.tar.gz
rm setup-box.sh
