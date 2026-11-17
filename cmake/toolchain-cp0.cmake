# Cross toolchain for the M5Stack CardputerZero (Raspberry Pi CM0, aarch64, Debian trixie).
#
# Compiler: Arm GNU Toolchain 14.2 unpacked in .cache/toolchain (no root needed).
# Sysroot:  the official CardputerZero BSP unpacked in .cache/sdk_bsp-src.
# The Arm toolchain is not Debian-multiarch aware, so the multiarch include and
# library directories of the sysroot are added explicitly.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

get_filename_component(_ZB_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_ZB_TC "${_ZB_ROOT}/.cache/toolchain/bin/aarch64-none-linux-gnu-")
set(_ZB_SYSROOT "${_ZB_ROOT}/.cache/sdk_bsp-src")
set(_ZB_MA "aarch64-linux-gnu")

if(NOT EXISTS "${_ZB_TC}gcc" OR NOT EXISTS "${_ZB_SYSROOT}/usr/include")
    message(FATAL_ERROR "Cross toolchain or BSP sysroot missing; run scripts/setup-toolchain.sh")
endif()

set(CMAKE_C_COMPILER "${_ZB_TC}gcc")
set(CMAKE_CXX_COMPILER "${_ZB_TC}g++")
set(CMAKE_SYSROOT "${_ZB_SYSROOT}")

file(GLOB _ZB_GCC_RT LIST_DIRECTORIES true "${_ZB_SYSROOT}/usr/lib/gcc/${_ZB_MA}/*")
list(GET _ZB_GCC_RT 0 _ZB_GCC_RT)

set(_ZB_INC "-idirafter ${_ZB_SYSROOT}/usr/include/${_ZB_MA}")
set(CMAKE_C_FLAGS_INIT "${_ZB_INC} -mcpu=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "${_ZB_INC} -mcpu=cortex-a53")

set(_ZB_LINK "-B${_ZB_SYSROOT}/usr/lib/${_ZB_MA}/ -B${_ZB_GCC_RT}/ -L${_ZB_SYSROOT}/usr/lib/${_ZB_MA} -L${_ZB_SYSROOT}/lib/${_ZB_MA} -Wl,-rpath-link,${_ZB_SYSROOT}/usr/lib/${_ZB_MA} -Wl,-rpath-link,${_ZB_SYSROOT}/lib/${_ZB_MA}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ZB_LINK}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ZB_LINK}")

set(CMAKE_FIND_ROOT_PATH "${_ZB_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
