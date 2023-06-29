---
layout: page
title: Download ET
---

# OS/X

The easiest way to install is using homebrew:

```
brew install MisterTea/et/et
```

# Ubuntu

For Ubuntu, use our PPA:

```
sudo apt-get install -y software-properties-common
sudo add-apt-repository ppa:jgmath2000/et
sudo apt-get update
sudo apt-get install et
```

# Debian

For Debian, use our deb repo.  For bullseye:

```
echo "deb https://mistertea.github.io/debian-et/debian-source/ bullseye main" | sudo tee /etc/apt/sources.list.d/et.list
curl -sS https://mistertea.github.io/debian-et/et.gpg | sudo apt-key add -
sudo apt update
sudo apt install et
```
See [the repo source](https://github.com/MisterTea/debian-et/tree/master/debian-source/dists) for a list of the other supported Debian versions.

# Other Linux/Unix

Check out the [README](https://github.com/MisterTea/EternalTerminal) for instructions.

# Windows

ET works under WSL (Windows Subsystem for Linux), but you must be running xenial or higher.  See this issue for details: https://github.com/microsoft/WSL/issues/482

As long as you are running Xenial, you can follow the ubuntu instructions for installation.


# Source Code

The source can be found on our [github page](https://github.com/MisterTea/EternalTerminal)
