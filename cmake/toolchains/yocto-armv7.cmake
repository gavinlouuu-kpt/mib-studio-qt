# Cross-compile the backend for the PZ7035 PS (Cortex-A9, armv7hf) with the YOFO Yocto SDK
# (pz7035-imx426 yocto/meta-yofo: bitbake yofo-image -c populate_sdk). Source the SDK's
# environment-setup-* script first: it exports CC/CXX with --sysroot and the CPU flags,
# OECORE_TARGET_SYSROOT and the pkg-config sysroot variables that FindPkgConfig (Aravis) uses.
if(NOT DEFINED ENV{OECORE_TARGET_SYSROOT})
    message(FATAL_ERROR "yocto-armv7: source the Yocto SDK environment-setup-* script first")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_SYSROOT "$ENV{OECORE_TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "$ENV{OECORE_TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# HDF5: the SDK's package config is unusable (meta-oe installs it with an absolute
# HDF5_INSTALL_CMAKE_DIR and imported targets pointing at /usr/lib), and FindHDF5 would otherwise
# interrogate the host's h5cc wrapper and pick up host headers. Skip both; with a wrapper that
# fails, FindHDF5 searches the sysroot for hdf5.h and libhdf5 itself.
set(HDF5_NO_FIND_PACKAGE_CONFIG_FILE TRUE CACHE BOOL "Skip the SDK's broken HDF5 package config")
set(HDF5_C_COMPILER_EXECUTABLE /bin/false CACHE FILEPATH "No target h5cc; search the sysroot")
