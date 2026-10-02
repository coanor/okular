#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Download pinned target dependencies and build Windows x64 entirely on Linux."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile


SOURCE = Path(__file__).resolve().parent.parent
LOCK = SOURCE / "cmake/windows-dependencies.lock.json"


def run(*args):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def download(spec, cache, urls=None):
    destination = cache / spec["sha256"]
    if destination.exists() and digest(destination) == spec["sha256"]:
        return destination
    temporary = destination.with_suffix(".part")
    for url in urls or [spec["url"]]:
        try:
            run("curl", "--fail", "--location", "--silent", "--show-error",
                "--retry", "3", "--connect-timeout", "30", "--output", temporary, url)
            if digest(temporary) != spec["sha256"]:
                raise ValueError(f"SHA256 mismatch: {url}")
            temporary.replace(destination)
            return destination
        except (subprocess.CalledProcessError, ValueError) as error:
            print(error, file=sys.stderr)
    raise RuntimeError(f"Cannot download verified package: {spec}")


def safe_names(names):
    for name in names:
        path = Path(name.replace("\\", "/"))
        if path.is_absolute() or ".." in path.parts or ":" in name:
            raise ValueError(f"Unsafe archive entry: {name}")


def extract(archive, destination, kind, sevenzip):
    destination.mkdir(parents=True, exist_ok=True)
    if kind == "tar":
        with tarfile.open(archive) as file:
            safe_names(file.getnames())
            for entry in file.getmembers():
                if entry.issym() or entry.islnk():
                    safe_names([entry.linkname])
                    resolved = (destination / entry.name).parent / entry.linkname
                    if not resolved.resolve().is_relative_to(destination.resolve()):
                        raise ValueError(f"Archive link escapes destination: {entry.name}")
            file.extractall(destination, filter="data")
    elif kind == "zip":
        with zipfile.ZipFile(archive) as file:
            safe_names(file.namelist())
            file.extractall(destination)
    else:
        listing = subprocess.check_output([sevenzip, "l", "-slt", "-ba", str(archive)], text=True)
        safe_names(line.removeprefix("Path = ") for line in listing.splitlines() if line.startswith("Path = "))
        # Craft packages are regular files/directories, not Unix symlinks.
        if "Symbolic Link = " in listing or "Hard Link = " in listing:
            raise ValueError(f"Unexpected link in Craft package: {archive}")
        subprocess.run([sevenzip, "x", "-y", "-aoa", "-bso0", "-bsp0", f"-o{destination}", str(archive)], check=True)


def native_tool(*names):
    for name in names:
        path = shutil.which(name)
        if path:
            with open(path, "rb") as file:
                if file.read(4) != b"\x7fELF":
                    raise RuntimeError(f"Expected a Linux ELF host tool: {path}")
            return path
    raise RuntimeError(f"Install Linux tool: {' / '.join(names)}")


def normalize_headers(include):
    # Windows packing merges case-only directories (KParts/kparts, etc.).
    # Public headers use both spellings; provide lower/uppercase aliases.
    for directory in list(include.rglob("*")):
        if directory.is_dir() and not directory.is_symlink():
            for spelling in (directory.name.lower(), directory.name.upper()):
                alias = directory.with_name(spelling)
                if alias != directory and not alias.exists():
                    alias.symlink_to(directory.name, target_is_directory=True)


def prepare(args, lock, root, downloads):
    sevenzip = shutil.which("7zz") or shutil.which("7z")
    if not sevenzip:
        raise RuntimeError("Install Linux 7zip (7zz or 7z).")
    sevenzip = str(Path(sevenzip).resolve())
    tools = root / "tools"
    if not (tools / ".complete").exists():
        for name, spec in lock["tools"].items():
            extract(download(spec, downloads), tools, "zip" if name == "ninja" else "tar", sevenzip)
        (tools / "ninja").chmod(0o755)
        (tools / ".complete").touch()
    os.environ["PATH"] = f"{tools / lock['tools']['cmake']['directory'] / 'bin'}:{tools}:{os.environ['PATH']}"

    sdk = root / "sdk"
    if not (sdk / ".complete").exists():
        spec = lock["sdk_manifest"]
        manifest = download(spec, downloads)
        xwin_cache = root / "xwin-cache"
        (xwin_cache / "dl").mkdir(parents=True, exist_ok=True)
        shutil.copy2(manifest, xwin_cache / "dl" / f"pkg_manifest_{spec['sha256']}.vsman")
        channel = root / "channel.json"
        # The immutable package manifest pins xwin's default SDK/CRT selection.
        channel.write_text(json.dumps({"channelItems": [{"id": "okular-pinned-manifest",
            "version": "17", "type": "Manifest", "payloads": [{"fileName": "VisualStudio.vsman",
            "sha256": spec["sha256"], "size": manifest.stat().st_size, "url": spec["url"]}]}]}))
        run(tools / lock["tools"]["xwin"]["directory"] / "xwin", "--accept-license",
            "--manifest", channel, "--arch", "x86_64", "--cache-dir", xwin_cache,
            "splat", "--output", sdk)
        (sdk / ".complete").touch()

    raw = root / "packages"
    if not (raw / ".complete").exists():
        urls = [args.mirror or lock["craft_base"], lock["craft_mirror"]]
        def fetch(package):
            if "url" in package:
                return download(package, downloads, [package["url"], package["mirror"]])
            return download(package, downloads, [base.rstrip("/") + "/" + package["file"] for base in dict.fromkeys(urls)])
        # Parallel downloads; serialize extraction because packages share directories.
        with ThreadPoolExecutor(max_workers=4) as pool:
            for package, archive in zip(lock["packages"], pool.map(fetch, lock["packages"])):
                print("Extract:", package["name"], package["version"], flush=True)
                extract(archive, raw, "7z", sevenzip)
        (raw / ".complete").touch()
    normalize_headers(raw / "include")
    target = root / "target"
    target_stamp = target / ".complete"
    if not target_stamp.exists() or target_stamp.read_text() != str(target):
        run(sys.executable, SOURCE / "cmake/prepare-windows-prefix.py", raw, target,
            "--old-prefix", lock["craft_prefix"])
        target_stamp.write_text(str(target))

    redist = root / "redist"
    if not (redist / ".complete").exists():
        extract(download(lock["crt_redist"], downloads), redist, "zip", sevenzip)
        (redist / ".complete").touch()
    return target, sdk


def build_host(lock, root, downloads, target, jobs):
    host = root / "host"
    host_stamp = host / ".complete"
    if host_stamp.exists() and host_stamp.read_text() == str(host):
        return host
    if host_stamp.exists():
        # Native tool RPATHs and CMake build trees can contain absolute paths.
        # Rebuild host tools if a CI cache was restored at a different location.
        shutil.rmtree(host)
        shutil.rmtree(root / "host-build", ignore_errors=True)
    sources = root / "sources"
    cc = native_tool("gcc")
    cxx = native_tool("g++")
    for name, spec in lock["host_sources"].items():
        source_stamp = sources / spec["directory"] / ".complete"
        if not source_stamp.exists():
            extract(download(spec, downloads), sources, "tar", "")
            source_stamp.touch()
        build = root / "host-build" / name
        common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={host}",
                  f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
                  "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_TESTING=OFF"]
        if name == "qtbase":
            flags = [f"-DFEATURE_{feature}=OFF" for feature in
                     ("gui", "widgets", "dbus", "network", "printsupport", "sql", "testlib", "concurrent", "icu")]
            flags += ["-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF"]
        else:
            flags = [f"-DCMAKE_PREFIX_PATH={host}", f"-DECM_DIR={target}/share/ECM/cmake",
                     "-DKDE_INSTALL_LIBDIR=lib", "-DKCONFIG_USE_GUI=OFF", "-DKCONFIG_USE_QML=OFF",
                     "-DUSE_DBUS=OFF", "-DKF_SKIP_PO_PROCESSING=ON"]
        run("cmake", "-S", sources / spec["directory"], "-B", build, *common, *flags)
        run("cmake", "--build", build, "--parallel", jobs)
        run("cmake", "--install", build)
    for name in ("moc", "rcc", "uic", "kconfig_compiler_kf6"):
        tool = next(host.rglob(name), None)
        if tool is None:
            raise RuntimeError(f"Missing native host tool: {name}")
        with tool.open("rb") as file:
            if file.read(4) != b"\x7fELF":
                raise RuntimeError(f"Invalid host tool: {tool}")
    host_stamp.write_text(str(host))
    return host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "okular-windows")
    parser.add_argument("--build-dir", type=Path, default=SOURCE / "build-windows-cross")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--mirror", help="Craft cache base URL; every package still requires the locked SHA256")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Python 3.12+ is required for safe tar extraction.")
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("This entry point currently supports Linux x86_64 → Windows x64.")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    # Reject Windows compilers even if WSL imported them into PATH.
    clang = native_tool("clang-20", "clang")
    clangxx = native_tool("clang++-20", "clang++")
    for names in (("lld-link-20", "lld-link"), ("llvm-rc-20", "llvm-rc"),
                  ("llvm-ar-20", "llvm-ar"), ("llvm-ranlib-20", "llvm-ranlib"),
                  ("llvm-readobj-20", "llvm-readobj")):
        native_tool(*names)
    version = subprocess.check_output([clang, "-dumpversion"], text=True).strip()
    if int(version.split(".")[0]) < 19:
        parser.error("The pinned MSVC STL requires Clang >= 19; Clang 20 is recommended.")
    lock = json.loads(LOCK.read_text())
    build = args.build_dir.resolve()
    stamp = build / "dependency-lock.sha256"
    if not args.prepare_only:
        if stamp.exists() and stamp.read_text().strip() != digest(LOCK):
            parser.error("Dependency lock changed; use a new --build-dir or remove the old build directory.")
        if (build / "CMakeCache.txt").exists() and not stamp.exists():
            parser.error("Use a new --build-dir for this bootstrap; the existing CMake cache has no dependency lock.")
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    cache_lock = (cache / ".lock").open("w")
    fcntl.flock(cache_lock, fcntl.LOCK_EX)
    downloads = cache / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    # Changing a dependency lock never mixes packages or stale host-tool exports.
    root = cache / digest(LOCK)[:16]
    root.mkdir(parents=True, exist_ok=True)
    target, sdk = prepare(args, lock, root, downloads)
    host = build_host(lock, root, downloads, target, args.jobs)
    if args.prepare_only:
        print("Prepared Linux cache:", root)
        return
    build.mkdir(parents=True, exist_ok=True)
    stamp.write_text(digest(LOCK) + "\n")
    install = build / "install"
    run("cmake", "-S", SOURCE, "-B", build, "-G", "Ninja",
        "--toolchain", SOURCE / "cmake/toolchains/linux-clang-windows.cmake",
        f"-DCMAKE_C_COMPILER={clang}", f"-DCMAKE_CXX_COMPILER={clangxx}",
        f"-DOKULAR_WINDOWS_SDK={sdk}", f"-DOKULAR_WINDOWS_PREFIX={target}",
        f"-DCMAKE_PREFIX_PATH={target}", f"-DQT_HOST_PATH={host}",
        f"-DKF6_HOST_TOOLING={host}/lib/cmake", "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={install}", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DKDE_INSTALL_USE_QT_SYS_PATHS=OFF", "-DOKULAR_UI=desktop", "-DBUILD_TESTING=OFF",
        "-DFORCE_NOT_REQUIRED_DEPENDENCIES=KF6DocTools", "-DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON")
    run("cmake", "--build", build, "--parallel", args.jobs)
    # Recreate the deployable tree to remove stale plugins from earlier builds.
    if install.exists():
        shutil.rmtree(install)
    run("cmake", "--install", build)
    run(sys.executable, SOURCE / "cmake/package-windows.py", "--dependencies", target,
        "--redist", root / "redist", "--install", install, "--archive", build / "okular-windows-x64.zip")
    print("Windows artifact:", build / "okular-windows-x64.zip")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
