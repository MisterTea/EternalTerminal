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

For debian, use our deb repo.  For stretch:

```
echo "deb https://mistertea.github.io/debian-et/debian-source/ buster main" | sudo tee /etc/apt/sources.list.d/et.list
curl -sS https://mistertea.github.io/debian-et/et.gpg | sudo apt-key add -
sudo apt update
sudo apt install et
```

# Other Linux/Unix

Check out the README for instructions: https://github.com/MisterTea/EternalTerminal

# Windows

ET works under WSL (Windows Subsystem for Linux), but you must be running xenial or higher.  See this issue for details: https://github.com/microsoft/WSL/issues/482

As long as you are running Xenial, you can follow the ubuntu instructions for installation.


# Source Code

The source can be found on our [github page](https://github.com/MisterTea/EternalTerminal)
