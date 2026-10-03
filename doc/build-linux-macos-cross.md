# Linux and macOS cross-builds

`make linux-cross` and `make macos-cross` configure, compile and install Okular
using a supplied CMake toolchain on a Linux host, including WSL2. The default
target architecture is ARM64; `--arch x86_64` selects x86_64. Target libraries,
SDK/sysroot and matching Linux Qt/KF6 host tools must already be prepared.
These entry points do not bootstrap those dependencies or produce standalone
deployment packages. The existing [Windows bootstrap](build-windows-from-linux.md)
downloads its pinned dependency set.

## Commands

Replace the `/path/to/...` placeholders with your prepared SDK/tool directories:

```bash
make linux-cross LINUX_CROSS_ARGS='--toolchain /path/to/linux-arm64.cmake -- -DCMAKE_PREFIX_PATH=/path/to/linux-sysroot/usr -DQT_HOST_PATH=/path/to/linux-host/qt -DKF6_HOST_TOOLING=/path/to/linux-host/kf6/lib/cmake'
make macos-cross MACOS_CROSS_ARGS='--toolchain /path/to/macos-arm64.cmake -- -DCMAKE_PREFIX_PATH=/path/to/macos-prefix -DQT_HOST_PATH=/path/to/linux-host/qt -DKF6_HOST_TOOLING=/path/to/linux-host/kf6/lib/cmake'
```

Pass `--arch x86_64` with a corresponding x86_64 toolchain. Both commands accept
`--jobs 4` and `--build-dir /path/to/output` before `--`. CMake `-D` definitions go
after `--`. Use `make linux-cross LINUX_CROSS_ARGS=--help` or
`make macos-cross MACOS_CROSS_ARGS=--help` to inspect the options.

Default build directories are `build-linux-cross-arm64` and
`build-macos-cross-arm64` under the repository; x86_64 uses the corresponding
`-x86_64` directory. The installation tree is `<build-dir>/install`. The runtime
install prefix is `/usr/local`, with `CMAKE_STAGING_PREFIX` directing installation
into that local tree. macOS application bundles are staged under `install/bin`.
The installed executable is checked for ELF/Mach-O format and the requested CPU.

The build is incremental, and concurrent calls sharing a build directory are
serialized. Changing the target, architecture, toolchain contents, CMake
definitions, or environment variables referenced directly by the toolchain
requires a new build directory. Keep dependency versions/paths fixed; changes in
included toolchain files, SDK contents or indirectly referenced environment
variables also require a new directory. The entry point rejects unmanaged CMake
caches and recreates its installation tree to remove stale plugins.

## Toolchain requirements

Use a Linux-hosted target compiler, CMake >= 3.22, Ninja and Python >= 3.9. The
toolchain must set `CMAKE_SYSTEM_NAME` to `Linux` or `Darwin`, plus
`CMAKE_SYSTEM_PROCESSOR` to `aarch64`/`arm64` or `x86_64`. For macOS, also set
`CMAKE_OSX_ARCHITECTURES` to the requested single architecture. The helper checks
these settings and requires `CMAKE_CROSSCOMPILING` to be true.

Set `CMAKE_SYSROOT` for Linux, or `CMAKE_OSX_SYSROOT` for macOS, and populate
`CMAKE_FIND_ROOT_PATH` with the target dependency prefix where necessary.
The helper confines library/header/package discovery to target roots and program
discovery to the host. Configure `PKG_CONFIG_LIBDIR` in the toolchain to include
only target `.pc` directories, with an appropriate `PKG_CONFIG_SYSROOT_DIR` for
your metadata layout. The helper clears `PKG_CONFIG_PATH`; if no target
`PKG_CONFIG_LIBDIR` is set, pkg-config's default host directories are disabled.

Linux ARM64 commonly uses `aarch64-linux-gnu-gcc/g++` with a complete target
sysroot. Clang toolchains must set `CMAKE_C_COMPILER_TARGET` and
`CMAKE_CXX_COMPILER_TARGET`, plus target C++ headers/runtime and linker paths.
See the official [CMake cross-compiling examples](https://cmake.org/cmake/help/v3.22/manual/cmake-toolchains.7.html).

macOS needs an SDK, libc++ headers/libraries, macOS Qt/KF6/backend libraries and
a Linux-hosted Darwin linker. A Linux Clang/LLD toolchain can set the compiler
target to `arm64-apple-macos11.0` (or `x86_64-apple-macos11.0`) and use
`ld64.lld`, LLVM archive tools and `llvm-install-name-tool`. Match the deployment
target to the SDK and prebuilt dependencies. If passing an absolute LLD path,
Clang 20 may need `-mlinker-version=520` in its initial C/C++ flags to emit the
required `-platform_version` linker option. This was verified with the Linux
LLVM 20 tools; see [Mach-O LLD](https://lld.llvm.org/MachO/index.html) and
[Clang's Darwin linker options](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.2/clang/lib/Driver/ToolChains/Darwin.cpp).

Supply `QT_HOST_PATH` and `KF6_HOST_TOOLING` as CMake definitions or in the
toolchain. They point to Linux tools, including `moc`, `rcc`, `uic` and
`kconfig_compiler_kf6`; prepare matching native KDocTools if building documentation.
Qt host tools must match target Qt as described in [Cross-compiling Qt](https://doc.qt.io/qt-6/cross-compiling-qt.html).
The desktop UI, AI/WebEngine and existing document backend options are preserved.

## Verification scope

`python3 autotests/unixcrossbuildtest.py` tests the helper's safeguards and builds
small freestanding C++ executables for Linux ARM64 and macOS ARM64/x86_64 using
Linux Clang/LLD. It checks their binary format/CPU, incremental builds and removal
of stale installed files. The fixtures need no SDK and do not execute target
programs. These tests also run in the opt-in GitLab Linux cross-build job.

The complete Linux ARM64/macOS Okular builds and target runtime tests have not
been verified in the current WSL environment: their SDK and full Qt/KF6/backend
dependency stacks are not available. Startup, rendering, annotations, plugins and
WebEngine deployment still need verification with the actual target dependencies.
