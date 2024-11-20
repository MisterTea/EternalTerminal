---
layout: page
title: Download ET
---

# macOS

The easiest way to install, is using [Homebrew](https://brew.sh):

```bash
brew install MisterTea/et/et
```

# Ubuntu

For Ubuntu, use our PPA:

```bash
sudo apt-get install -y software-properties-common
sudo add-apt-repository ppa:jgmath2000/et
sudo apt-get update
sudo apt-get install et
```

# Debian

For Debian, use our deb repo:

```bash
echo "deb [signed-by=/etc/apt/keyrings/et.gpg] https://mistertea.github.io/debian-et/debian-source/ $(grep VERSION_CODENAME /etc/os-release | cut -d= -f2) main" | sudo tee -a /etc/apt/sources.list.d/et.list
curl -sSL https://github.com/MisterTea/debian-et/raw/master/et.gpg | sudo tee /etc/apt/keyrings/et.gpg >/dev/null
sudo apt update
sudo apt install et
```

If you're using **Debian 11 or older**, before adding the repo, you must create the `keyrings` folder first:

```bash
sudo mkdir -m 0755 -p /etc/apt/keyrings
```

See [the repo source](https://github.com/MisterTea/debian-et/tree/master/debian-source/dists) for a list of all supported Debian versions.

# Other Linux/Unix

Check out the [README](https://github.com/MisterTea/EternalTerminal#installing) for instructions.

# Windows

ET works under [WSL (Windows Subsystem for Linux)](https://learn.microsoft.com/en-us/windows/wsl/).

As long as you are running Ubuntu, you can follow the [Ubuntu instructions](https://eternalterminal.dev/download/#ubuntu) for installation, but you must be **running `Ubuntu-16.04` or newer**. See [this issue](https://github.com/microsoft/WSL/issues/482) for details.

# Source Code

The source can be found on our [GitHub page](https://github.com/MisterTea/EternalTerminal).
