# EternalTCP
Re-Connectable TCP connection


```
sudo apt install libgoogle-glog-dev libgflags-dev 
```


### Run the following Git commands
	git submodule update --init --recursive

### GTest
  To build google tests run the following 

  ```
  cd build/
  cmake ../ -DBUILD_TEST=1
  make
  ```

### Build Package
  To build a package 

  ```
  cd build/
  cmake ../ -DBUILD_PACKAGE=1
  make
  cpack
  ```

