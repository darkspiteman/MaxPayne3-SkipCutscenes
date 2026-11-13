# CMake toolchain file for a portable llvm-mingw (https://github.com/mstorsjo/llvm-mingw), 32-bit x86 target.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw-i686.cmake -DLLVM_MINGW_ROOT=<unpacked llvm-mingw folder>
#
# LLVM_MINGW_ROOT may also come from the environment.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if(NOT LLVM_MINGW_ROOT)
    message(FATAL_ERROR "Set LLVM_MINGW_ROOT to the unpacked llvm-mingw folder (the one that contains bin/).")
endif()
file(TO_CMAKE_PATH "${LLVM_MINGW_ROOT}" LLVM_MINGW_ROOT)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)

set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/i686-w64-mingw32-clang.exe")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/i686-w64-mingw32-clang++.exe")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/i686-w64-mingw32-windres.exe")
set(CMAKE_AR "${LLVM_MINGW_ROOT}/bin/llvm-ar.exe")
set(CMAKE_RANLIB "${LLVM_MINGW_ROOT}/bin/llvm-ranlib.exe")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/i686-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
