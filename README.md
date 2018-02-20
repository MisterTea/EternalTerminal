# Eternal Terminal

Eternal Terminal is a remote shell that automatically reconnects without interrupting the session.

Website: <https://mistertea.github.io/EternalTCP/>.

## Installing

### Mac OS X

The easiest way to install is using homebrew:

	brew install MisterTea/et/et

### Ubuntu

For Ubuntu, use our PPA:

	sudo add-apt-repository ppa:jgmath2000/et
	sudo apt-get update
	sudo apt-get install et

### Other Linux

Install dependencies:

    Debian:

	sudo apt-get install libboost-dev libsodium-dev libncurses5-dev \
	libprotobuf-dev protobuf-compiler cmake libgoogle-glog-dev \
	libgflags-dev unzip wget

    Fedora (tested on 25):

        sudo dnf install boost-devel libsodium-devel ncurses-devel protobuf-devel \
	protobuf-compiler cmake glog-devel gflags-devel wget unzip

    Gentoo:

        sudo emerge dev-libs/boost dev-libs/libsodium sys-libs/ncurses \
	dev-libs/protobuf dev-util/cmake dev-cpp/glog dev-cpp/gflags \
	net-misc/wget app-arch/unzip

Download and install from source:

	wget https://github.com/MisterTea/EternalTCP/archive/master.zip
	unzip master.zip
	cd EternalTCP-master
	mkdir build
	cd build
	cmake ../
	make
	sudo make install

### Windows

Eternal Terminal works under WSL (Windows Subsystem for Linux).  Follow the ubuntu instructions.

## Verifying

Verify that the client is installed correctly by looking for the `et` executable: `which et`.

Verify that the server is installed correctly by looking for the etserver executable: `which etserver`.

You are ready to start using ET!

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

### OS/X

To build eternal terminal on mac, the easiest way is to grab dependencies with homebrew:

```
brew install --only-dependencies MisterTea/et/et
git clone https://github.com/MisterTea/EternalTCP.git
cd EternalTCP
mkdir build
cd build
cmake ../
make
```

### Debian/Ubuntu

Grab the deps and then follow the same process as for OS/X:

```
sudo apt install libsodium-dev, libncurses5-dev, libprotobuf-dev, protobuf-compiler, cmake, libgoogle-glog-dev, libgflags-dev, libutempter-dev
git clone https://github.com/MisterTea/EternalTCP.git
cd EternalTCP
mkdir build
cd build
cmake ../
make
```

## Reporting issues

If you have any problems with installation or usage, please [file an issue on github](https://github.com/MisterTea/EternalTCP/issues).

## Developers

- Jason Gauci: https://github.com/MisterTea
- Ailing Zhang: https://github.com/ailzhang
