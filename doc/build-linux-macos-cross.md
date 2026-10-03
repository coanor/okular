# Linux and macOS cross-builds

On a Linux x86_64 host, including WSL2, these commands prepare checksum-pinned
target dependencies and build the complete desktop Okular for ARM64:

```bash
make linux-cross
make macos-cross
```

Both preserve AI/WebEngine, all 14 document generators, speech, multimedia,
wallet integration and documentation. No ARM64 machine, macOS installation,
Xcode installation or target compiler executable is used during the build.
Linux Clang/LLD compile and link the target application. Native GCC builds the
matching Linux Qt/KConfig tools needed by the macOS dependency stack.

## Host prerequisites

On Ubuntu 24.04:

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends ca-certificates curl python3 make cmake ninja-build g++ clang-20 lld-20 llvm-20 7zip gettext pkg-config zlib1g-dev libzstd-dev
```

Automatic preparation requires Python >= 3.12 and Linux Clang/LLVM >= 20.
The Linux path also uses `dpkg-deb`, supplied by Ubuntu's `dpkg` package.
The macOS path downloads pinned Linux CMake 4.1.4 and Ninja 1.13.2.

The default outputs are `build-linux-cross-arm64/install/bin/okular` and
`build-macos-cross-arm64/install/bin/okular.app/Contents/MacOS/okular`.
Installed binaries are checked for ELF/Mach-O format and the requested CPU.
The runtime installation prefix is `/usr/local`; `CMAKE_STAGING_PREFIX` directs
installation into the build directory. These installation trees contain Okular,
plugins and resources; target Qt/KF6 libraries and WebEngine deployment resources
remain in the prepared SDK. They are not standalone deployment packages.

## Dependencies and caching

[linux-dependencies.lock.json](../cmake/linux-dependencies.lock.json) pins Debian
13 (trixie) ARM64 target packages with Qt 6.8.2, KF6 6.13 and Poppler 25.03, plus
matching x86_64 Linux host tools. Packages are extracted into private directories;
they are not installed on the host. Clang uses the target GCC C++ headers and
runtime libraries without invoking target GCC. Native tools run with their own
Linux dynamic loader/libraries so Debian's newer glibc does not replace the host's.

[macos-dependencies.lock.json](../cmake/macos-dependencies.lock.json) pins KDE
Craft ARM64 packages with Qt 6.11.1, KF6 6.30 and Poppler 26.07, Linux documentation
tools, native Qt/KConfig sources, LibSpectre 0.2.12 sources and the MacOSX26.1 SDK
archive from [macosx-sdks](https://github.com/joseluisq/macosx-sdks/releases/tag/26.1).
The application targets macOS 13.3 or later, matching the prebuilt KF6 libraries.
The SDK is downloaded on Linux; no local Apple SDK directory is required.
LibSpectre is compiled with Linux Clang instead of dropping the PostScript backend.

Every downloaded archive requires its committed SHA256. The shared download cache
defaults to `$XDG_CACHE_HOME/okular-unix` or `~/.cache/okular-unix`; prepared SDKs
are keyed by dependency lock, compiler paths and Clang version. The initial
download is roughly 370 MiB for the Linux target plus 166 MiB for its host tools;
macOS additionally needs its Craft archives, SDK and host sources. Extracted SDKs
occupy several GiB. Subsequent builds reuse the verified downloads and host tools.
An unavailable pinned URL requires a verified mirror or a reviewed lock update.

The build is incremental, and calls sharing an SDK or build directory are
serialized. Installation trees are recreated to remove stale plugins. A changed
dependency lock, SDK/compiler location, generated toolchain or CMake definitions
requires a new build directory. Both workflows use Linux executables for code
generation and translation; target `bin` directories are excluded from program
discovery, including when explicitly present in `CMAKE_PREFIX_PATH`.

Use a Linux filesystem for faster WSL builds:

```bash
make linux-cross LINUX_CROSS_ARGS='--cache /home/user/.cache/okular-unix --build-dir /home/user/build/okular-linux-arm64 --jobs 8'
make macos-cross MACOS_CROSS_ARGS='--cache /home/user/.cache/okular-unix --build-dir /home/user/build/okular-macos-arm64 --jobs 8'
make macos-cross MACOS_CROSS_ARGS=--prepare-only
```

`--prepare-only` downloads/prepares the SDK and host tools without compiling
Okular. Automatic macOS preparation currently requires a cache path without spaces
because LibSpectre's Autoconf flags are split by its shell build scripts.

## Other SDKs and architectures

`--toolchain` retains the supplied-SDK workflow. For x86_64 or another dependency
stack, prepare target libraries and matching Linux host tools, then run:

```bash
make linux-cross LINUX_CROSS_ARGS='--arch x86_64 --toolchain /path/to/linux-x86_64.cmake -- -DCMAKE_PREFIX_PATH=/path/to/sysroot/usr -DQT_HOST_PATH=/path/to/linux-qt -DKF6_HOST_TOOLING=/path/to/linux-kf6/lib/cmake'
make macos-cross MACOS_CROSS_ARGS='--arch x86_64 --toolchain /path/to/macos-x86_64.cmake -- -DCMAKE_PREFIX_PATH=/path/to/target -DQT_HOST_PATH=/path/to/linux-qt -DKF6_HOST_TOOLING=/path/to/linux-kf6/lib/cmake'
```

Automatic package provisioning currently covers ARM64 only. Full x86_64 Okular
cross-builds have not been verified. Both commands accept extra CMake `-D` options
after `--`; target, toolchain and staging settings are reserved.

Custom toolchains must set `CMAKE_SYSTEM_NAME=Linux`/`Darwin`, the matching
`CMAKE_SYSTEM_PROCESSOR`, and target SDK/search roots. macOS also requires a single
matching `CMAKE_OSX_ARCHITECTURES`. Libraries/headers/packages are confined to target
roots; programs come from Linux. Set `PKG_CONFIG_LIBDIR` and
`PKG_CONFIG_SYSROOT_DIR` for target metadata. Changes in included toolchains, SDK
contents or indirectly referenced environment variables require a fresh build tree.
See [CMake toolchains](https://cmake.org/cmake/help/v3.22/manual/cmake-toolchains.7.html),
[Mach-O LLD](https://lld.llvm.org/MachO/index.html) and
[Qt host tools](https://doc.qt.io/qt-6/cross-compiling-qt.html).

## Daily CI and verification

The existing GitLab configuration has an opt-in `build_unix_from_linux` job.
Set `OKULAR_UNIX_CROSS=1` in a scheduled or manually started pipeline. It prepares
both ARM64 SDKs on Ubuntu 24.04, runs helper tests, builds both applications and
archives their installation trees. The schedule itself must be configured in the
GitLab project; no remote pipeline or schedule was started during local validation.

Validated on 2026-10-03 in WSL2 with Linux Clang/LLD 20.1.2 and GCC 13.3:

- Both automatic Makefile commands completed configure, build and install from
  newly assembled SDKs. macOS's Qt/KConfig host tools were built from pinned sources.
- Both installation trees contain the desktop shell, KParts plugin and all 14
  document generators, with AI/WebEngine enabled. Repeated builds required no
  downloads, SDK extraction, host-tool rebuilding or C++ recompilation.
- The Linux ARM64 executable ran `--version` under QEMU with target libraries and
  Qt's offscreen platform. This checks basic startup; rendering/annotations and
  WebEngine subprocess execution were not tested under emulation.
- `unixcrossbuildtest.py`, `unixbootstraptest.py` and `windowscrossbuildtest.py`
  cover CPU/format rejection, context changes, native program discovery, safe
  archive links, restarted preparation, incremental builds and stale-file cleanup.

The macOS executable has not been run on macOS. Standalone deployment, platform
integration and target GUI behavior still require runtime validation.
