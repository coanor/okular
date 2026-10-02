# Building Windows x64 from Linux

Run `make windows-cross` from a Linux x86_64 checkout. The entry point downloads
SHA256-pinned dependencies, builds matching Linux Qt/KConfig host tools, compiles
Okular with Linux Clang and LLD, and creates a deployable Windows x64 ZIP.
It does not read a Windows installation, Visual Studio directory, local Windows
SDK, or another checkout's `.deps/windows`. It never executes Windows programs.

## Linux prerequisites

On Ubuntu 24.04 with the updates repository enabled:

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends ca-certificates curl python3 make g++ \
  clang-20 lld-20 llvm-20 7zip gettext icoutils pkg-config zlib1g-dev libzstd-dev
make windows-cross
```

Python 3.12+ is required for safe tar extraction. The bootstrap downloads pinned
Linux CMake, Ninja and [xwin](https://github.com/Jake-Shadle/xwin) binaries. xwin
extracts SDK/runtime headers and import libraries from Microsoft's downloads;
it is not a compiler. Running this entry point accepts the SDK license through
xwin's `--accept-license` option.

The [toolchain](../cmake/toolchains/linux-clang-windows.cmake) uses the GNU-style
Linux `clang++` driver with `--target=x86_64-pc-windows-msvc`; LLD emits PE files.
No MinGW compiler or Windows compiler is used. Native Linux GCC builds the host
tools. Target libraries and Windows SDK data are necessary regardless of the
application's implementation language.

## Daily use

```bash
make windows-cross
# The same build directory is incremental; downloaded dependencies and host
# tools are reused after the first successful bootstrap.
```

The output is `build-windows-cross/okular-windows-x64.zip`; the unpacked runtime
is in `build-windows-cross/install`, with `bin/okular.exe`. Packaging includes
Qt/KF6/backend DLLs, MSVC runtime DLLs, plugins/QML modules, MIME data, translations, and
WebEngine's helper/resources/locales. Linux LLVM tools inspect PE architecture
and DLL imports; missing non-system DLLs fail packaging. No `windeployqt` runs.

Options are passed through `WINDOWS_CROSS_ARGS`:

```bash
make windows-cross WINDOWS_CROSS_ARGS='--jobs 4 --cache /srv/cache/okular-windows'
python3 cmake/build-windows.py --prepare-only
python3 cmake/build-windows.py --build-dir /srv/build/okular-windows
# A mirror changes download locations, never the required hashes:
python3 cmake/build-windows.py --mirror https://your-mirror.example/craft-cache/
```

The default cache is `${XDG_CACHE_HOME:-~/.cache}/okular-windows`. Keep it on a
Linux filesystem for efficient builds. The dependency lock's fingerprint selects
a separate SDK/library/host-tool cache. After updating the lock, use a new build
directory, or remove the old build directory before rebuilding. Failed downloads
are not marked complete; a failed bootstrap can be rerun.

## Dependency updates and scope

[windows-dependencies.lock.json](../cmake/windows-dependencies.lock.json) records
exact Craft binary package paths and SHA256 values, matching host-tool sources,
and an immutable Visual Studio package manifest. xwin verifies SDK packages
against the locked manifest. Normal builds do not query a moving Craft manifest
or Visual Studio channel URL. Update the lock deliberately and verify a complete
build/package before adopting a new dependency set.

The initial lock uses Qt 6.11.1, KDE Frameworks 6.30.0, Poppler 26.07.0,
MSVC CRT 14.44 and Windows SDK 10.0.26100. The libraries are prebuilt Windows
MSVC ABI packages from KDE's public Craft cache. The workflow cross-compiles
Okular, rather than rebuilding the entire dependency stack from source.
Some vendor C libraries used by those packages include GCC runtime DLLs such
as `libwinpthread-1.dll`. These prebuilt runtime files are downloaded and staged;
no MinGW compiler, headers, or build environment is installed or invoked.
The desktop UI, WebEngine AI renderer, and document backends remain enabled.
Documentation generation is disabled because matching native KDocTools are
not bootstrapped. This entry point currently targets Windows x64 only.

## Scheduled builds

The GitLab job `build_windows_from_linux` runs on an Ubuntu Linux runner when a
scheduled or web pipeline sets `OKULAR_WINDOWS_CROSS=1`. Configure a daily
schedule with that variable to publish the ZIP as a seven-day artifact. Its cache
is keyed by the dependency lock. Existing native Windows jobs remain available
for runtime verification. No GitHub workflow or Windows runner is added.

## Optional runtime verification

Executing the produced PE files requires Windows or a suitable compatibility
environment. Runtime testing is separate from the Linux bootstrap/build/package
and is not required for daily compilation. Check startup, PDF rendering,
annotations and the AI WebEngine view when validating a dependency update.

For cross-compiled tests, reconfigure the generated build directory with
`BUILD_TESTING=ON`. Set `OKULAR_TEST_DATA_DIR` to the runtime's absolute path to
`autotests` if it differs from the compiler's Linux source path. Run the test
binaries with the staged DLLs and plugins available. CTest on Linux must not
try to execute the Windows test programs without an explicit emulator.

## Linux-only bootstrap verification (2026-10-02)

The final lock was verified in a new Linux cache, using only Linux executables:
Clang/LLD/LLVM resource tools 20, GCC 13.3 for host tools, CMake 4.1.4, and Ninja
1.13.2. All 159 target packages were downloaded and SHA256-verified. xwin obtained
the SDK using the locked Microsoft package manifest.

The complete build produced `okular.exe`, `Okular6Core.dll`, `okularpart.dll`, and
14 document generator DLLs. Packaging inspected 449 Windows x64 PE binaries and
produced a roughly 301 MiB ZIP with no missing non-system DLL imports or required
WebEngine/QPA resources. Generated build commands have no references to a local
Windows installation or `.deps/windows`; AUTOGEN tools are Linux ELF files.

A second run reused the dependencies and host tools with no downloads or C/C++
recompilation; two translation-generation targets ran before installation and
packaging. Five Linux bootstrap regression checks passed. The independently
downloaded runtime package has not been executed on Windows. The GitLab job's
YAML was checked locally; a remote scheduled pipeline has not been run.

## Earlier manual runtime verification (2026-10-02)

Before the Linux-only bootstrap was added, Linux Clang 20 built the complete
Windows desktop application using a locally prepared Windows dependency prefix.
That build produced 14 document generators and passed `annotationsidecartest`
(8 cases), `documenttest` (18), `aiprovidertest` (4), and `aimarkdownviewtest` (4)
on Windows 11 through WSL interop. These are historical runtime results; they
do not replace runtime validation of the independently downloaded packages.
