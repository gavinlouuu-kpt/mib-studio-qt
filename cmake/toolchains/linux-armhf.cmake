# Cross-compile the backend for armv7hf (the PZ7035 PS: Cortex-A9, NEON) with the distro
# toolchain, for the `linux-armhf-smoke` preset: gcc-arm-linux-gnueabihf plus the armhf
# multiarch development packages (env/apt-packages.txt, section `armhf-cross`;
# scripts/ci/enable-armhf-apt.sh adds the architecture first).
#
# This is a compile smoke: it proves the PL-specific code and `MIB_PL_SCIENCE` still build for
# 32-bit ARM. It is not the target: the shipped binary is built against the Yocto SDK
# (`yocto-armv7.cmake`, the `linux-armv7-yocto` preset), whose library versions differ.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)

# The Cortex-A9 of the Zynq-7000 (what the Yocto SDK's cortexa9t2hf-neon tune builds for).
set(_mib_armhf_cpu "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${_mib_armhf_cpu}")
set(CMAKE_CXX_FLAGS_INIT "${_mib_armhf_cpu}")

# armhf libraries live in the multiarch directories under /usr; programs come from the host.
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf /)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# HDF5: FindHDF5 must not interrogate the host's h5cc wrapper (it would pick up host headers);
# with a wrapper that fails it searches the armhf multiarch paths (hdf5/serial) itself.
set(HDF5_C_COMPILER_EXECUTABLE /bin/false CACHE FILEPATH "No target h5cc; search the armhf paths")
