# Eternal Terminal

Eternal Terminal is a remote shell that automatically reconnects without interrupting the session.

Website: <https://mistertea.github.io/EternalTerminal/>.

## Integration tests

Circle: [![CircleCI](https://circleci.com/gh/MisterTea/EternalTerminal/tree/master.svg?style=svg)](https://circleci.com/gh/MisterTea/EternalTerminal/tree/master)

Linux: ![Linux CI](https://github.com/MisterTea/EternalTerminal/workflows/Linux%20CI/badge.svg?branch=master)

## Installing

### macOS

The easiest way to install is using Homebrew:

	brew install MisterTea/et/et

Alternatively, a package is available in MacPorts:

	sudo port install et

### Ubuntu

For Ubuntu, use our PPA:

	sudo add-apt-repository ppa:jgmath2000/et
	sudo apt-get update
	sudo apt-get install et

Install and build from source:
```
sudo apt install build-essential libgflags-dev libprotobuf-dev protobuf-compiler libsodium-dev cmake git
git clone --recurse-submodules https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
cmake ../
make && sudo make install
sudo cp ../etc/et.cfg /etc/
```
Once built, the binary only requires `libgflags-dev` and `libprotobuf-dev`.

### Debian

For debian, use our deb repo. For buster:

	echo "deb https://github.com/MisterTea/debian-et/raw/master/debian-source/ buster main" | sudo tee -a /etc/apt/sources.list
	curl -sS https://github.com/MisterTea/debian-et/raw/master/et.gpg | sudo apt-key add -
	sudo apt update
	sudo apt install et


### CentOS 7

Up to the present day the only way to install is to [build from source](#centos-7).


### FreeBSD
On FreeBSD, use:

	pkg install eternalterminal

### Fedora (version 29 and later):
```
sudo dnf install et
```

### openSUSE

```
zypper ar -f obs://network
zypper ref
zypper in EternalTerminal
```

### Other Linux

Install dependencies:

* Fedora (tested on 25):

      sudo dnf install boost-devel libsodium-devel ncurses-devel protobuf-devel \
	protobuf-compiler cmake gflags-devel

* Gentoo:

      sudo emerge dev-libs/boost dev-libs/libsodium sys-libs/ncurses \
	dev-libs/protobuf dev-util/cmake dev-cpp/gflags

Download and install from source:

	git clone --recurse-submodules https://github.com/MisterTea/EternalTerminal.git
	cd EternalTerminal
	mkdir build
	cd build
	cmake ../
	make
	sudo make install

### Windows

Eternal Terminal works under WSL (Windows Subsystem for Linux).  Follow the ubuntu instructions.

## Verifying

Verify that the client is installed correctly by looking for the `et` executable: `which et`.

Verify that the server is installed correctly by checking the service status: `systemctl status et`.  On some operating systems, you may need to enable and start the service manually: `sudo systemctl enable --now et`.

You are ready to start using ET!

## Configuring

If you'd like to modify the server settings (e.g. to change the listening port), edit /etc/et.cfg.

## Using

ET uses ssh for handshaking and encryption, so you must be able to ssh into the machine from the client. Make sure that you can `ssh user@hostname`.

ET uses TCP, so you need an open port on your server. By default, it uses 2022.

Once you have an open port, the syntax is similar to ssh. Username is default to the current username starting the et process, use `-u` or `user@` to specify a different if necessary.
```
et hostname (etserver running on default port 2022, username is the same as current)
et user@hostname:8000 (etserver running on port 8000, different user)
```
You can specify a jumphost and the port et is running on jumphost using `-jumphost` and `-jport`. If no `-jport` is given, et will try to connect to default port 2022.
```
et hostname -jumphost jump_hostname (etserver running on port 2022 on both hostname and jumphost)
et hostname:8888 -jumphost jump_hostname -jport 9999
```
Additional arguments that et accept are port forwarding pairs with option `-t="18000:8000, 18001-18003:8001-8003"`, a command to run immediately after the connection is setup through `-c`.

Starting from the latest release, et supports parsing both user-specific and system-wide ssh config file.
The config file is required when your sshd on server/jumphost is listening on a port which is not 22.
Here is an example ssh config file showing how to setup when
- there is a jumphost in the middle
- sshd is listening on a port which is not 22
- connecting to a different username other than current one.

```
Host dev
  HostName 192.168.1.1
  User fred
  Port 5555
  ProxyJump user@jumphost.example.org:22
```

With the ssh config file set as above, you can simply call et with

```
et dev (etserver running on port 2022 on both hostname and jumphost)
et dev:8000 -jport 9000 (etserver running on port 9000 on jumphost)
```

## Building from source

### macOS

To build Eternal Terminal on Mac, the easiest way is to grab dependencies with Homebrew:

```
brew install --only-dependencies MisterTea/et/et
git clone --recurse-submodules https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
cmake ../
make
```

### Debian/Ubuntu

Grab the deps and then follow this process:

Debian/Ubuntu Dependencies:
```
sudo apt install libboost-dev libsodium-dev libncurses5-dev \
     libprotobuf-dev protobuf-compiler cmake libgflags-dev libutempter-dev cmake git
```

Source and setup:

```
git clone --recurse-submodules https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
cmake ../
make
sudo make install
```

### CentOS 7

Install dependencies:
```
sudo yum install epel-release
sudo yum install cmake3 boost-devel libsodium-devel ncurses-devel protobuf-devel \
     protobuf-compiler gflags-devel protobuf-lite-devel
```

Install scl dependencies
```
sudo yum install centos-release-scl
sudo yum install devtoolset-8
```

Download and install from source ([see #238 for details](https://github.com/MisterTea/EternalTerminal/issues/238)):
```
git clone --recurse-submodules https://github.com/MisterTea/EternalTerminal.git
cd EternalTerminal
mkdir build
cd build
scl enable devtoolset-8 'cmake3 ../'
scl enable devtoolset-8 'make && sudo make install'
sudo cp ../systemctl/et.service /etc/systemd/system/
sudo cp ../etc/et.cfg /etc/
```

Find the actual location of et:

	which etserver

Correct the service file (see [#180](https://github.com/MisterTea/EternalTerminal/issues/180) for details).

```
sudo sed -ie "s|ExecStart=.*[[:space:]]|ExecStart=$(which etserver) |" /etc/systemd/system/et.service
```

Alternativelly, open the file /etc/systemd/system/et.service in an editor and correct the `ExectStart=...` line to point to the correct path of the `etserver` binary.

	 ExecStart=/usr/local/bin/etserver --daemon --cfgfile=/etc/et.cfg

Reload systemd configs:

```
sudo systemctl daemon-reload
```

Start the et service:

```
sudo systemctl enable --now et.service
```

## Reporting issues

If you have any problems with installation or usage, please [file an issue on github](https://github.com/MisterTea/EternalTerminal/issues).

## Developers

- Jason Gauci: https://github.com/MisterTea
- Ailing Zhang: https://github.com/ailzhang
