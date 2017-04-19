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
	cd master
	mkdir build
	cd build
	cmake ../
	make
	sudo make install

### Windows

Currently only unix sockets are supported. If you would like winsock support, file an issue.

## Verifying

Verify that the client is installed correctly by looking for the `et` executable: `which et`.

Verify that the server is installed correctly by looking for the etserver executable: `which etserver`.

You are ready to start using ET!

## Using

ET uses ssh for handshaking and encryption, so you must be able to ssh into the machine from the client. Make sure that you can `ssh user@hostname`.

ET uses TCP, so you need an open port on your server. By default, it uses 2022.

Once you have an open port, the syntax is similar to `ssh`: `et user@hostname[:port]`. You can pass additional arguments to `ssh` using the `-s` parameter. For instance, if you have `sshd` listening on port 5000: `et -s="-p 5000 user@host" user@host`.

## Reporting issues

If you have any problems with installation or usage, please [file an issue on github](https://github.com/MisterTea/EternalTCP/issues).
