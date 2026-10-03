# SPDX-License-Identifier: BSD-2-Clause
# Windows x64/MSVC ABI with Linux Clang and LLVM tools.
# OKULAR_WINDOWS_SDK contains crt/{include,lib/x86_64} and
# sdk/{include/{ucrt,um,shared},lib/{ucrt,um}/x86_64}.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(OKULAR_WINDOWS_SDK "" CACHE PATH "Windows SDK and C/C++ runtime for the target")
set(OKULAR_WINDOWS_PREFIX "" CACHE PATH "Windows Qt, KF6 and document backend dependencies")
foreach(_directory OKULAR_WINDOWS_SDK OKULAR_WINDOWS_PREFIX)
    if(NOT IS_DIRECTORY "${${_directory}}")
        message(FATAL_ERROR "Set ${_directory} to an existing directory.")
    endif()
endforeach()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    OKULAR_WINDOWS_SDK OKULAR_WINDOWS_PREFIX)

find_program(CMAKE_C_COMPILER NAMES clang-20 clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++-20 clang++ REQUIRED)
find_program(CMAKE_LINKER NAMES lld-link-20 lld-link REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar-20 llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib-20 llvm-ranlib REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES llvm-rc-20 llvm-rc REQUIRED)

set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Match the release DLL runtime used by the Windows dependency prefix.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# The MSVC offsetof implementation is not constexpr under Clang. Use the
# SDK's supported builtin variant, also used by clang-cl.
set(_system_includes "-D_CRT_USE_BUILTIN_OFFSETOF")
foreach(_include crt/include sdk/include/ucrt sdk/include/um sdk/include/shared)
    string(APPEND _system_includes " -isystem \"${OKULAR_WINDOWS_SDK}/${_include}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_system_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_system_includes}")

# Clang's MSVC driver searches for the unversioned lld-link name. Resolve the
# LLVM installation directory so distro tools such as lld-link-20 also work.
get_filename_component(_llvm_linker "${CMAKE_LINKER}" REALPATH)
get_filename_component(_llvm_linker_directory "${_llvm_linker}" DIRECTORY)
set(_system_libraries "-B\"${_llvm_linker_directory}\"")
foreach(_lib crt/lib/x86_64 sdk/lib/ucrt/x86_64 sdk/lib/um/x86_64)
    string(APPEND _system_libraries " -Xlinker \"/libpath:${OKULAR_WINDOWS_SDK}/${_lib}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_system_libraries}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_system_libraries}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_system_libraries}")

set(CMAKE_RC_FLAGS_INIT "")
foreach(_include um shared ucrt)
    string(APPEND CMAKE_RC_FLAGS_INIT " -I \"${OKULAR_WINDOWS_SDK}/sdk/include/${_include}\"")
endforeach()

set(CMAKE_FIND_ROOT_PATH "${OKULAR_WINDOWS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)

# pkg-config runs on Linux but must read only target package metadata.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${OKULAR_WINDOWS_PREFIX}/lib/pkgconfig:${OKULAR_WINDOWS_PREFIX}/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
