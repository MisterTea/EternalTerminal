## Xbox Scarlett Build

### Requirements

* Visual Studio - tested with `2022`
* CMake - tested with `3.27.1 or newer`
* Xbox GDK - tested with `2023.03.03` and `2024.06.03`
* Optional: git installed and on your PATH

### Steps

* `cd` into the SDK's root folder
* Run `git submodule update --init --recursive`
* Run "Xbox Manager GDK"
* Open Visual Studio Command Prompt for "Xbox Scarlett Gaming".
* Change to the source directory in the prompt
* Configure your build using
  ```
  cmake 
    -B build 
    -G "Visual Studio 17 2022" 
    -A "Gaming.Xbox.Scarlett.x64" 
    -DCMAKE_TOOLCHAIN_FILE="./toolchains/xbox/gxdk_xs_toolchain.cmake"
  ```
* Optionally you can specify the GDK version
  ```
    -DGDK_VERSION="241000"
  ```
* After this you can either build the library directly in the CLI with 
  `cmake --build build --config RelWithDebInfo` (or any other build config)
  or in Visual Studio by opening the solution in the `build` directory.
* `cmake --install build --prefix install --config RelWithDebInfo` installs all required development and release files 
  (`dll`, `pdb`, `lib`, `h`) to include into any Xbox X/S game project into the directory `install`.
