# Eternal Terminal

Eternal Terminal is a remote shell that automatically reconnects without interrupting the session.

Website: <https://mistertea.github.io/EternalTerminal/>.

## Packaging status

[![Packaging
status](https://repology.org/badge/vertical-allrepos/eternalterminal.svg?exclude_unsupported=1)](https://repology.org/project/eternalterminal/versions)

## Installing

### macOS

The easiest way to install is using Homebrew:

```bash
brew install MisterTea/et/et
```

If the install fails on including csignal, see https://github.com/MisterTea/EternalTerminal/issues/662#issuecomment-2408889829

Then if you want a daemon to launch `etserver` on every boot:

On m1 (Apple Silicon) Macs:

```bash
sudo sed 's:/usr/local/bin/etserver:/opt/homebrew/bin/etserver:g' ../init/launchd/homebrew.mxcl.et.plist | sudo tee /Library/LaunchDaemons/homebrew.mxcl.et.plist
sudo launchctl load -w /Library/LaunchDaemons/homebrew.mxcl.et.plist
```

On x86 Macs:

```bash
sudo cp ../init/launchd/homebrew.mxcl.et.plist /Library/LaunchDaemons/homebrew.mxcl.et.plist
sudo launchctl load -w /Library/LaunchDaemons/homebrew.mxcl.et.plist
```

Alternatively, a package is available in MacPorts:

```bash
sudo port install et
```

### Ubuntu

For Ubuntu, use our PPA:

```bash
sudo add-apt-repository ppa:jgmath2000/et
sudo apt-get update
sudo apt-get install et
```

Or see "Debian/Ubuntu" below to install and build from source (e.g., for ARM).

### Debian

For Debian, use our deb repo:

```bash
echo "deb [signed-by=/etc/apt/keyrings/et.gpg] https://mistertea.github.io/debian-et/debian-source/ $(grep VERSION_CODENAME /etc/os-release | cut -d= -f2) main" | sudo tee -a /etc/apt/sources.list.d/et.list
sudo mkdir -m 0755 -p /etc/apt/keyrings # only if you're using Debian 11 or older
curl -sSL https://github.com/MisterTea/debian-et/raw/master/et.gpg | sudo tee /etc/apt/keyrings/et.gpg >/dev/null
sudo apt update
sudo apt install et
```

### CentOS 7

Up to the present day the only way to install is to [build from source](#centos-7-1).

### CentOS 8

```bash
sudo dnf install epel-release
sudo dnf install et
```

### FreeBSD

On FreeBSD, use:

```bash
pkg install eternalterminal
```

### Fedora (version 29 and later):

```bash
sudo dnf install et
```

### openSUSE

```bash
zypper ar -f obs://network
zypper ref
zypper in EternalTerminal
```

### Other Linux

Install dependencies:

- Fedora (tested on 25):

  ```bash
  sudo dnf install boost-devel libsodium-devel protobuf-devel \
  	protobuf-compiler cmake gflags-devel libcurl-devel
  ```

- Gentoo:

  ```bash
  sudo emerge dev-libs/boost dev-libs/libsodium \
  	dev-libs/protobuf dev-util/cmake dev-cpp/gflags
  ```

Download and install from source:

```bash
git clone --recurse-submodules --depth 1 https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
cmake ../
make
sudo make install
```

### Windows

Eternal Terminal works under WSL (Windows Subsystem for Linux). Follow the ubuntu instructions.

### Docker Image

See [docker/README.md](docker/)

## Verifying

Verify that the client is installed correctly by looking for the `et` executable: `which et`.

Verify that the server is installed correctly by checking the service status: `systemctl status et`. On some operating systems, you may need to enable and start the service manually: `sudo systemctl enable --now et`.

You are ready to start using ET!

## Configuring

If you'd like to modify the server settings (e.g. to change the listening port), edit /etc/et.cfg.

## Using

ET uses ssh for handshaking and encryption, so you must be able to ssh into the machine from the client. Make sure that you can `ssh user@hostname`.

ET uses TCP, so you need an open port on your server. By default, it uses 2022.


Once you have an open port, the syntax is similar to ssh. Username is default to the current username starting the et process, use `-u` or `user@` to specify a different one if necessary.

```bash
et hostname (etserver running on default port 2022, username is the same as current)
et user@hostname:8000 (etserver running on port 8000, different user)
```

You can specify a jumphost and the port et is running on jumphost using `--jumphost` and `--jport`. If no `--jport` is given, et will try to connect to default port 2022.

```bash
et hostname -jumphost jump_hostname (etserver running on port 2022 on both hostname and jumphost)
et hostname:8888 --jumphost jump_hostname --jport 9999
```

Additional arguments that et accepts are port forwarding pairs with option `-t "18000:8000, 18001-18003:8001-8003"`, a command to run immediately after the connection is setup through `-c`.

Starting from the latest release, et supports parsing both user-specific and system-wide SSH config files.
The config file is required when your sshd on server/jumphost is listening on a port which is not 22.
Here is an example SSH config file showing how to setup when

- there is a jumphost in the middle
- sshd is listening on a port that is not 22
- connecting to a different username other than the current one.

```ssh-config
Host dev
  HostName 192.168.1.1
  User fred
  Port 5555
  ProxyJump user@jumphost.example.org:22
```

With the ssh config file set as above, you can simply call et with

```bash
et dev (etserver running on port 2022 on both hostname and jumphost)
et dev:8000 -jport 9000 (etserver running on port 9000 on jumphost)
```

## Building from Source

### macOS

To build Eternal Terminal on Mac, the easiest way is to grab dependencies with Homebrew:

```bash
brew install autoconf automake libtool
git clone --recurse-submodules --depth 1 https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
cmake ../
make -j$(nproc) && sudo make install
```

To run an `et` server for testing, run `./etserver`. To run an `et`
server daemon persistently across reboots:

```bash
sudo cp ../init/launchd/homebrew.mxcl.et.plist /Library/LaunchDaemons
sudo launchctl load -w /Library/LaunchDaemons/homebrew.mxcl.et.plist
```

### Debian/Ubuntu

Grab the deps and then follow this process.

Debian/Ubuntu Dependencies:

```bash
sudo apt install libsodium-dev autoconf libtool \
	libprotobuf-dev protobuf-compiler libutempter-dev libcurl4-openssl-dev \
    build-essential ninja-build cmake git zip pkg-config
```

Fetch source, build and install:

```bash
git clone --recurse-submodules --depth 1 https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
# For ARM (including OS/X with apple silicon):
if [[ $(uname -a | grep 'arm\|aarch64') ]]; then export VCPKG_FORCE_SYSTEM_BINARIES=1; fi
cmake -DCPACK_GENERATOR=DEB ../
make -j$(nproc) package
sudo dpkg --install *.deb
```

Once built, the binary only requires `libprotobuf-dev`.

Disable et server by `sudo systemctl disable --now et`

### CentOS 7

Install dependencies:

```bash
sudo yum install epel-release
sudo yum install cmake3 boost-devel libsodium-devel protobuf-devel \
     protobuf-compiler gflags-devel protobuf-lite-devel libcurl-devel \
     perl-IPC-Cmd perl-Data-Dumper libunwind-devel libutempter-devel
```

Install scl dependencies

```bash
sudo yum install centos-release-scl
sudo yum install devtoolset-11 devtoolset-11-libatomic-devel rh-git227
```

Download and install from source ([see #238 for details](https://github.com/MisterTea/EternalTerminal/issues/238)):

```bash
git clone --recurse-submodules --depth 1 https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
scl enable devtoolset-11 rh-git227 'cmake3 ../'
scl enable devtoolset-11 'make && sudo make install'
sudo cp ../systemctl/et.service /etc/systemd/system/
sudo cp ../etc/et.cfg /etc/
```

Find the actual location of et:

```bash
which etserver
```

Correct the service file (see [#180](https://github.com/MisterTea/EternalTerminal/issues/180) for details).

```bash
sudo sed -ie "s|ExecStart=[^[:space:]]*[[:space:]]|ExecStart=$(which etserver) |" /etc/systemd/system/et.service
```

Alternatively, open the file /etc/systemd/system/et.service in an editor and correct the `ExectStart=...` line to point to the correct path of the `etserver` binary.


```
ExecStart=/usr/local/bin/etserver --cfgfile=/etc/et.cfg
```

Reload systemd configs:

```bash
sudo systemctl daemon-reload
```

Start the et service:

```bash
sudo systemctl enable --now et.service
```

## Building using Docker

Builder Dockerfiles are located at [deployment/](deployment/). Supported OSes: CentOS 8, openSUSE and Ubuntu.

## Reporting issues

If you have any problems with installation or usage, please [file an issue on GitHub](https://github.com/MisterTea/EternalTerminal/issues).

## Developers

- Jason Gauci: https://github.com/MisterTea
- Ailing Zhang: https://github.com/ailzhang
- James Short: https://github.com/jshort
