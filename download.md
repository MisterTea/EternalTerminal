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
sudo add-apt-repository ppa:jgmath2000/et
sudo apt-get update
sudo apt-get install et
```

# Other Linux

Download and install from source (you may need some dependencies):

```
wget https://github.com/MisterTea/EternalTCP/archive/master.zip
unzip master.zip
cd master
mkdir build
cd build
cmake ../
make
sudo make install
```


# Windows

Currently only unix sockets are supported.  If you would like winsock
support, file an issue.

# Source Code

The source can be found on our [github page](https://github.com/MisterTea/EternalTCP)
