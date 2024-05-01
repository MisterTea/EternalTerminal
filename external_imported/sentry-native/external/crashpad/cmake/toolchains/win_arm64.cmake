# Toolchain file that should provide required and non-conflicting build-
# parameters to allow normal and cross-compilation to ARM64 targets on 
# Windows using any generator.
SET(CMAKE_GENERATOR_PLATFORM "ARM64")
SET(CMAKE_SYSTEM_PROCESSOR "ARM64")
SET(CMAKE_SYSTEM_NAME "Windows")