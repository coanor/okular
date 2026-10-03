#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Prepare checksum-pinned Unix target SDKs and Linux host tools."""

from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shlex
import shutil
import subprocess
import sys
import tarfile


SOURCE = Path(__file__).resolve().parent.parent
# Share the Windows workflow's verified downloader and native Qt/KConfig build.
WINDOWS = runpy.run_path(str(SOURCE / "cmake/build-windows.py"))
verified_download = WINDOWS["download"]
native_tool = WINDOWS["native_tool"]


def download(spec, cache, urls=None):
    # Different platform builds share archives, but not extraction/build trees.
    with (cache / (spec["sha256"] + ".lock")).open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return verified_download(spec, cache, urls)


def extract_tar(stream, destination):
    """Keep archive links inside the SDK, including Debian's absolute links."""
    destination.mkdir(parents=True, exist_ok=True)
    def entry_filter(entry, directory):
        if entry.issym() and entry.linkname.startswith("/"):
            entry.linkname = os.path.relpath(destination / entry.linkname.lstrip("/"),
                                            destination / entry.name / "..")
        if entry.islnk():
            entry.linkname = entry.linkname.lstrip("/")
        return tarfile.data_filter(entry, directory)
    with tarfile.open(fileobj=stream, mode="r|*") as archive:
        archive.extractall(destination, filter=entry_filter)


def unpack(archive, destination, kind, sevenzip):
    if kind == "deb":
        command = ["dpkg-deb", "--fsys-tarfile", str(archive)]
    elif kind == "tar.7z":
        command = [sevenzip, "x", "-so", str(archive)]
    else:
        with archive.open("rb") as stream:
            extract_tar(stream, destination)
        return
    with subprocess.Popen(command, stdout=subprocess.PIPE) as process:
        extract_tar(process.stdout, destination)
        if process.wait():
            raise RuntimeError(f"Cannot unpack {archive}")


def packages(specs, destination, downloads, sevenzip, base=""):
    stamp = destination / ".complete"
    if stamp.exists() and stamp.read_text() == str(destination):
        return
    shutil.rmtree(destination, ignore_errors=True)
    def fetch(spec):
        urls = [spec["url"]] if "url" in spec else [base + spec["file"]]
        return download(spec, downloads, urls)
    with ThreadPoolExecutor(max_workers=4) as pool:
        for spec, archive in zip(specs, pool.map(fetch, specs)):
            kind = "deb" if spec.get("url", "").endswith(".deb") else (
                "tar.7z" if spec["file"].endswith(".tar.7z") else "tar")
            print("Unpack:", spec["name"], spec["version"], flush=True)
            unpack(archive, destination, kind, sevenzip)
    stamp.write_text(str(destination))


def relocate(tree, replacements):
    # Include XML resources: DocBook catalogs/XSL files retain install prefixes.
    extensions = {".cmake", ".pc", ".xml", ".xsl", ".dtd", ".ent", ".mod", ".elements"}
    for path in tree.rglob("*"):
        if not path.is_file() or path.is_symlink() or path.suffix not in extensions:
            continue
        original = contents = path.read_bytes()
        for old, new in replacements:
            if old.encode() in str(new).encode():
                contents = contents.replace(str(new).encode(), old.encode())
            contents = contents.replace(old.encode(), str(new).encode())
        if contents != original:
            path.write_bytes(contents)


def wrap_tools(root, directories, libraries, data, loader=None):
    """Run native ELF tools with their own libraries, without polluting Clang."""
    for directory in directories:
        for path in (root / directory).rglob("*"):
            if path.is_symlink() or not path.is_file():
                continue
            recovered = path.name.endswith(".elf")
            if recovered:
                executable = path
                path = path.with_name(path.name.removesuffix(".elf"))
                if path.exists():
                    continue
            with (executable if recovered else path).open("rb") as stream:
                header = stream.read(20)
            if header[:4] != b"\x7fELF":
                continue
            if int.from_bytes(header[18:20], "little") != 62:
                raise RuntimeError(f"Expected Linux x86_64 host tool: {path}")
            if not recovered:
                executable = path.with_name(path.name + ".elf")
                path.rename(executable)
            environment = f"XDG_DATA_DIRS={shlex.quote(str(data) + ':/usr/share')} "
            library_path = ":".join(map(str, libraries))
            if loader:
                command = [loader, "--library-path", library_path, executable]
            else:
                environment += f"LD_LIBRARY_PATH={shlex.quote(library_path)} "
                command = [executable]
            temporary = path.with_name(path.name + ".wrapper")
            temporary.write_text("#!/bin/sh\n" + environment + "exec "
                                 + shlex.join(list(map(str, command))) + ' "$@"\n')
            temporary.chmod(0o755)
            temporary.replace(path)


def tooling(directory, package, filename, executables):
    folder = directory / package
    folder.mkdir(parents=True, exist_ok=True)
    contents = "# Linux host executables; target libraries come from the SDK.\n"
    for name, executable in executables.items():
        if not executable.is_file():
            raise RuntimeError(f"Missing host tool: {executable}")
        contents += (f"if(NOT TARGET KF6::{name})\n"
                     f"  add_executable(KF6::{name} IMPORTED)\n"
                     f'  set_property(TARGET KF6::{name} PROPERTY IMPORTED_LOCATION "{executable}")\n'
                     "endif()\n")
    (folder / filename).write_text(contents)


def linux(lock, root, downloads, sevenzip):
    target, host = root / "sysroot", root / "host-root"
    packages(lock["target_packages"], target, downloads, sevenzip)
    packages(lock["host_packages"], host, downloads, sevenzip)
    stamp = root / ".linux-prepared"
    if not stamp.exists() or stamp.read_text() != str(root):
        relocate(host / "usr/share", [("/usr/share/xml", host / "usr/share/xml")])
        relocate(target / "usr/share/kf6/kdoctools", [("/usr/share/xml", host / "usr/share/xml")])
        wrap_tools(host, ["usr/bin", "usr/lib/qt6/bin", "usr/lib/qt6/libexec",
                          "usr/lib/libexec", "usr/lib/x86_64-linux-gnu/libexec"],
                   [host / "usr/lib/x86_64-linux-gnu", host / "usr/lib"], host / "usr/share",
                   host / "usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2")
        # Debian's qtpaths launcher embeds /usr even outside a system install.
        launcher = host / "usr/bin/x86_64-linux-gnu-qtpaths6"
        launcher.write_text(launcher.read_text().replace("exec /usr/", f"exec {host}/usr/"))
        stamp.write_text(str(root))
    host_cmake = root / "host-tooling"
    tooling(host_cmake, "KF6Config", "KF6ConfigCompilerTargets.cmake",
            {"kconfig_compiler": host / "usr/lib/libexec/kf6/kconfig_compiler_kf6"})
    tooling(host_cmake, "KF6DocTools", "KF6DocToolsToolsTargets.cmake",
            {name: host / "usr/bin" / name for name in ("meinproc6", "checkXML6")})
    os.environ["PATH"] = f"{host}/usr/bin:{os.environ['PATH']}"
    definitions = [f"-DCMAKE_PREFIX_PATH={target}/usr", f"-DQT_HOST_PATH={host}/usr",
                   f"-DQT_HOST_PATH_CMAKE_DIR={host}/usr/lib/x86_64-linux-gnu/cmake"]
    return target, host_cmake, definitions


def macos(lock, root, downloads, sevenzip, jobs):
    target = root / "target"
    packages(lock["packages"], target, downloads, sevenzip, lock["craft_base"])
    sdk = root / "sdk"
    if not (sdk / ".complete").exists():
        shutil.rmtree(sdk, ignore_errors=True)
        unpack(download(lock["sdk"], downloads), sdk, "tar", sevenzip)
        (sdk / ".complete").touch()
    relocate(target, [(prefix, target) for prefix in lock["craft_prefixes"]])
    WINDOWS["normalize_headers"](target / "include")
    host = WINDOWS["build_host"](lock, root, downloads, target, jobs, downloader=download)
    docs = root / "doc-tools"
    packages(lock["host_packages"], docs, downloads, sevenzip)
    stamp = root / ".macos-host-prepared"
    if not stamp.exists() or stamp.read_text() != str(root):
        relocate(docs, [(prefix, docs) for prefix in sorted({p["prefix"] for p in lock["host_packages"]})])
        wrap_tools(docs, ["bin"], [host / "lib", docs / "lib"], docs / "share")
        stamp.write_text(str(root))
    xml = target / "share/xml/docbook"
    xml.parent.mkdir(parents=True, exist_ok=True)
    if not xml.exists():
        xml.symlink_to(os.path.relpath(docs / "share/xml/docbook", xml.parent))
    host_cmake = root / "host-tooling"
    tooling(host_cmake, "KF6Config", "KF6ConfigCompilerTargets.cmake",
            {"kconfig_compiler": host / "lib/libexec/kf6/kconfig_compiler_kf6"})
    tooling(host_cmake, "KF6DocTools", "KF6DocToolsToolsTargets.cmake",
            {name: docs / "bin" / name for name in ("meinproc6", "checkXML6")})
    os.environ["PATH"] = f"{docs}/bin:{os.environ['PATH']}"
    return target, host_cmake, [f"-DCMAKE_PREFIX_PATH={target}", f"-DQT_HOST_PATH={host}"]


def host_tools(target):
    """Check all host prerequisites before downloading or preparing an SDK."""
    names = {
        "cc": ("clang-20", "clang"), "cxx": ("clang++-20", "clang++"),
        "linker": (("ld.lld-20", "ld.lld") if target == "linux" else ("ld64.lld-20", "ld64.lld")),
        "ar": ("llvm-ar-20", "llvm-ar"), "ranlib": ("llvm-ranlib-20", "llvm-ranlib"),
        "sevenzip": ("7zz", "7z"),
        "make": ("make",), "pkg-config": ("pkg-config",)}
    if target == "macos":
        names.update(nm=("llvm-nm-20", "llvm-nm"),
                     install_name_tool=("llvm-install-name-tool-20", "llvm-install-name-tool"),
                     gcc=("gcc",), gxx=("g++",), msgfmt=("msgfmt",))
    else:
        names.update(cmake=("cmake",), ninja=("ninja",), dpkg=("dpkg-deb",))
    tools, problems = {}, []
    elf_tools = {"cc", "cxx", "linker", "ar", "ranlib", "nm", "install_name_tool", "gcc", "gxx"}
    for name, candidates in names.items():
        try:
            if name in elf_tools:
                tools[name] = native_tool(*candidates)
            else:
                path = next((path for candidate in candidates if (path := shutil.which(candidate))), None)
                if not path:
                    raise RuntimeError(f"Install Linux tool: {' / '.join(candidates)}")
                tools[name] = path
        except (RuntimeError, OSError) as error:
            problems.append(str(error))
    version = ""
    for name, pattern in (("cc", r"clang version (\d+)"), ("cxx", r"clang version (\d+)"),
                          ("linker", r"LLD (\d+)")):
        if name not in tools:
            continue
        try:
            output = subprocess.check_output([tools[name], "--version"], text=True, stderr=subprocess.STDOUT)
            match = re.search(pattern, output)
            if not match or int(match[1]) < 20:
                problems.append(f"{' / '.join(names[name])}: version 20 or newer required")
            if name == "cc":
                version = output.splitlines()[0] if output else ""
        except (OSError, subprocess.CalledProcessError) as error:
            problems.append(f"Cannot run {tools[name]}: {error}")
    if problems:
        raise RuntimeError("Missing or incompatible Linux host tools:\n  " + "\n  ".join(problems)
                           + "\nOn Ubuntu 24.04, install the host prerequisites:\n"
                           "  sudo apt-get update\n"
                           "  sudo apt-get install --no-install-recommends ca-certificates curl python3 make cmake "
                           "ninja-build g++ clang-20 lld-20 llvm-20 7zip gettext pkg-config zlib1g-dev libzstd-dev\n"
                           "See doc/build-linux-macos-cross.md for setup instructions.")
    # 7z locates its shared objects relative to the real executable.
    sevenzip = str(Path(tools.pop("sevenzip")).resolve())
    return tools, sevenzip, version


def prepare(target, arch, cache, jobs):
    if sys.version_info < (3, 12) or platform.system() != "Linux" or platform.machine() != "x86_64":
        raise RuntimeError("Automatic SDK preparation requires Linux x86_64 and Python 3.12+.")
    lock_path = SOURCE / f"cmake/{target}-dependencies.lock.json"
    lock = json.loads(lock_path.read_text())
    if arch != lock["target_arch"]:
        raise RuntimeError(f"The pinned {target} SDK currently supports {lock['target_arch']}; supply --toolchain for {arch}.")
    if target == "macos" and any(character.isspace() for character in str(cache.resolve())):
        raise RuntimeError("LibSpectre's Autoconf build requires a --cache path without whitespace.")
    tools, sevenzip, version = host_tools(target)
    # Keep SDK identity tied to target compilation tools, as before preflight.
    compilers = {name: tools[name] for name in ("cc", "cxx", "linker", "ar", "ranlib")}
    identity = lock_path.read_bytes() + json.dumps(compilers, sort_keys=True).encode() + version.encode()
    root = cache.resolve() / hashlib.sha256(identity).hexdigest()[:16] / f"{target}-{arch}"
    root.mkdir(parents=True, exist_ok=True)
    downloads = cache.resolve() / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    with (root / ".prepare.lock").open("w") as mutex:
        fcntl.flock(mutex, fcntl.LOCK_EX)
        if target == "macos":
            directory = root / "tools"
            if not (directory / ".complete").exists():
                for name, spec in lock["tools"].items():
                    WINDOWS["extract"](download(spec, downloads), directory, "zip" if name == "ninja" else "tar", sevenzip)
                (directory / ".complete").touch()
            (directory / "ninja").chmod(0o755)
            os.environ["PATH"] = f"{directory / lock['tools']['cmake']['directory'] / 'bin'}:{directory}:{os.environ['PATH']}"
            prefix, host_cmake, definitions = macos(lock, root, downloads, sevenzip, jobs)
        else:
            prefix, host_cmake, definitions = linux(lock, root, downloads, sevenzip)
        definitions += [f"-DKF6_HOST_TOOLING={host_cmake}",
                        f"-DKDOCTOOLS_TARGETSFILE={host_cmake}/KF6DocTools/KF6DocToolsToolsTargets.cmake"]
        chain = write_toolchain(target, root, prefix, tools, lock)
        if target == "macos":
            build_spectre(root, prefix, tools, lock, downloads, sevenzip, jobs)
    return chain, definitions


def write_toolchain(target, root, prefix, tools, lock):
    contents = f"set(CMAKE_SYSTEM_NAME {'Linux' if target == 'linux' else 'Darwin'})\nset(CMAKE_SYSTEM_PROCESSOR {'aarch64' if target == 'linux' else 'arm64'})\n"
    for name, key in [("cc", "CMAKE_C_COMPILER"), ("cxx", "CMAKE_CXX_COMPILER"),
                      ("ar", "CMAKE_AR"), ("ranlib", "CMAKE_RANLIB")]:
        contents += f'set({key} "{tools[name]}")\n'
    triple = "aarch64-linux-gnu" if target == "linux" else "arm64-apple-macos13.3"
    for language in ("C", "CXX"):
        contents += f"set(CMAKE_{language}_COMPILER_TARGET {triple})\n"
        flags = f'--gcc-toolchain="{prefix}/usr"' if target == "linux" else "-mlinker-version=520"
        contents += f'set(CMAKE_{language}_FLAGS_INIT [=[{flags}]=])\n'
    # LLD chooses its object format from argv[0]; distro symlinks often resolve
    # to the generic `lld` binary, which cannot be invoked directly.
    driver = root / ("ld.lld" if target == "linux" else "ld64.lld")
    if not driver.exists():
        driver.symlink_to(Path(tools["linker"]).resolve())
    linker = f'--ld-path="{driver}"'
    if target == "linux":
        contents += f'set(CMAKE_SYSROOT "{prefix}")\n'
        linker += f' -Wl,-rpath-link,"{prefix}/usr/lib/aarch64-linux-gnu"'
        host = root / "host-root/usr/lib/x86_64-linux-gnu/cmake"
        contents += f'set(Qt6CoreTools_DIR "{host}/Qt6CoreTools" CACHE PATH "")\n'
        pkgconfig = f"{prefix}/usr/lib/aarch64-linux-gnu/pkgconfig:{prefix}/usr/share/pkgconfig"
        contents += f'set(ENV{{PKG_CONFIG_SYSROOT_DIR}} "{prefix}")\n'
        roots = prefix
    else:
        sdk = root / "sdk" / lock["sdk"]["directory"]
        contents += f'set(CMAKE_OSX_SYSROOT "{sdk}" CACHE PATH "")\nset(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")\nset(CMAKE_OSX_DEPLOYMENT_TARGET 13.3 CACHE STRING "")\n'
        contents += f'set(CMAKE_INSTALL_NAME_TOOL "{tools["install_name_tool"]}")\n'
        pkgconfig = f"{prefix}/lib/pkgconfig:{prefix}/share/pkgconfig"
        contents += 'set(ENV{PKG_CONFIG_SYSROOT_DIR} "")\n'
        roots = f"{prefix};{sdk}"
    for kind in ("EXE", "SHARED", "MODULE"):
        contents += f'set(CMAKE_{kind}_LINKER_FLAGS_INIT [=[{linker}]=])\n'
    contents += f'set(CMAKE_FIND_ROOT_PATH "{roots}")\nset(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n'
    for kind in ("LIBRARY", "INCLUDE", "PACKAGE"):
        contents += f"set(CMAKE_FIND_ROOT_PATH_MODE_{kind} ONLY)\n"
    contents += f'set(ENV{{PKG_CONFIG_LIBDIR}} "{pkgconfig}")\nset(ENV{{PKG_CONFIG_PATH}} "")\n'
    path = root / "toolchain.cmake"
    if not path.exists() or path.read_text() != contents:
        path.write_text(contents)
    return path


def build_spectre(root, prefix, tools, lock, downloads, sevenzip, jobs):
    stamp = root / ".spectre-built"
    if stamp.exists() and stamp.read_text() == str(root):
        return
    sources = root / "sources"
    unpack(download(lock["spectre"], downloads), sources, "tar", sevenzip)
    source = sources / lock["spectre"]["directory"]
    build = root / "build-spectre"
    build.mkdir(exist_ok=True)
    sdk = root / "sdk" / lock["sdk"]["directory"]
    environment = dict(os.environ, CC=shlex.join([tools["cc"], "--target=arm64-apple-macos13.3", "-isysroot", str(sdk),
                       "-mlinker-version=520", f"--ld-path={root / 'ld64.lld'}"]),
                       LD=tools["linker"], NM=tools["nm"],
                       AR=tools["ar"], RANLIB=tools["ranlib"], lt_cv_prog_gnu_ld="no",
                       CFLAGS=f"-O2 -I{prefix}/include", CPPFLAGS=f"-I{source}",
                       LDFLAGS=f"-L{prefix}/lib", PKG_CONFIG_PATH="", PKG_CONFIG_LIBDIR=f"{prefix}/lib/pkgconfig")
    subprocess.run([source / "configure", "--host=aarch64-apple-darwin", "--build=x86_64-pc-linux-gnu",
                    f"--prefix={prefix}", "--disable-static"], cwd=build, env=environment, check=True)
    subprocess.run(["make", f"-j{jobs}"], cwd=build, env=environment, check=True)
    subprocess.run(["make", "install"], cwd=build, env=environment, check=True)
    stamp.write_text(str(root))
