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

	sudo apt-get install libboost-dev libsodium-dev libncurses5-dev libprotobuf-dev protobuf-compiler cmake libgoogle-glog-dev libgflags-dev unzip wget

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

Note that starting from the latest release, et starts from etclient instead of `/launcher/et` script. And you can specify a jumphost and the port et is running on using `--jumphost` and `--jport`.
```
etclient --host hostname --port 2022
etclient --host hostname --port 2022 --jumphost jump_hostname --jport 9999
```
You can pass additional arguments to etclient such as port forwarding pairs with option `--portforward="18000:8000, 18001-18003:8001-8003"`, or a command to run immediately after the connection is setup through `--command`. Username is default to the current username starting the et process, use `--user` to specify a different if necessary.


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

## Reporting issues

If you have any problems with installation or usage, please [file an issue on github](https://github.com/MisterTea/EternalTCP/issues).
