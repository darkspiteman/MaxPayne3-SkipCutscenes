cmake_minimum_required(VERSION 3.20)
project(MP3SkipCutscenes VERSION 0.1.0 LANGUAGES CXX)

# Max Payne 3 is a 32-bit game: everything here must be built for x86.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "MP3SkipCutscenes must be built as 32-bit (x86). "
                        "Use cmake/toolchain-llvm-mingw-i686.cmake or 'cmake -A Win32' with Visual Studio.")
endif()
if(NOT WIN32)
    message(FATAL_ERROR "Windows target only.")
endif()

option(MP3SC_BUILD_LOADER "Build the optional open-source ASI loader (dinput8.dll)" ON)
option(MP3SC_BUILD_TESTS "Build the hook tests and the simulated-game test" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Static CRT: the plugin must not depend on a VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /GR- /EHs-c- /D_HAS_EXCEPTIONS=0)
else()
    add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti
                        # Our hooks are entered from MSVC-built game code that only guarantees a 4-byte aligned stack.
                        -mstackrealign)
    # No dependency on libgcc/libstdc++/libc++/libunwind/winpthread DLLs.
    add_link_options(-static)
    # Release binaries carry no symbols or debug sections.
    add_link_options($<$<CONFIG:Release>:-s>)
endif()

set(MOD_SOURCES
    src/config.cpp
    src/hooks.cpp
    src/log.cpp
    src/mem.cpp
    src/skip.cpp)

# --- the mod ---------------------------------------------------------------------------------------------------------
add_library(MP3SkipCutscenes SHARED ${MOD_SOURCES} src/dllmain.cpp)
set_target_properties(MP3SkipCutscenes PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "MP3SkipCutscenes")
target_link_libraries(MP3SkipCutscenes PRIVATE user32 kernel32)
if(NOT MSVC)
    # A plugin exports nothing (MinGW linkers export every symbol of a DLL by default).
    target_link_options(MP3SkipCutscenes PRIVATE -Wl,--exclude-all-symbols)
endif()

# --- optional ASI loader ---------------------------------------------------------------------------------------------
if(MP3SC_BUILD_LOADER)
    if(MSVC)
        add_library(mp3_asi_loader SHARED loader/dinput8.cpp loader/dinput8.msvc.def)
    else()
        add_library(mp3_asi_loader SHARED loader/dinput8.cpp loader/dinput8.mingw.def)
    endif()
    set_target_properties(mp3_asi_loader PROPERTIES PREFIX "" SUFFIX ".dll" OUTPUT_NAME "dinput8"
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/loader"
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/loader"
                          ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/loader")
    target_link_libraries(mp3_asi_loader PRIVATE user32 kernel32)
endif()

# --- tests -----------------------------------------------------------------------------------------------------------
if(MP3SC_BUILD_TESTS)
    enable_testing()

    add_executable(test_hooks tests/test_hooks.cpp src/mem.cpp src/hooks.cpp)
    target_include_directories(test_hooks PRIVATE src)
    add_test(NAME hooks COMMAND test_hooks)

    add_executable(test_sim tests/test_sim.cpp ${MOD_SOURCES})
    target_include_directories(test_sim PRIVATE src)
    target_link_libraries(test_sim PRIVATE user32 kernel32)
    add_test(NAME simulated_game COMMAND test_sim)

    add_executable(test_load tests/test_load.cpp)
    add_test(NAME load_in_foreign_process COMMAND test_load $<TARGET_FILE:MP3SkipCutscenes>)

    if(MP3SC_BUILD_LOADER)
        add_executable(test_loader tests/test_loader.cpp)
        target_link_libraries(test_loader PRIVATE ole32)
        add_test(NAME asi_loader COMMAND test_loader $<TARGET_FILE:mp3_asi_loader> $<TARGET_FILE:MP3SkipCutscenes>)
    endif()
endif()
