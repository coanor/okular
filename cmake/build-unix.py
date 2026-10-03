#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Cross-build Linux or macOS on Linux, preparing pinned ARM64 SDKs by default."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shutil
import subprocess
import sys


SOURCE = Path(__file__).resolve().parent.parent


def run(*arguments):
    print("+", " ".join(map(str, arguments)), flush=True)
    subprocess.run(list(map(str, arguments)), check=True)


def verify_binary(path, target, arch):
    with path.open("rb") as stream:
        header = stream.read(20)
    if target == "linux" and len(header) == 20 and header[:5] == b"\x7fELF\x02" and header[5] in (1, 2):
        machine = int.from_bytes(header[18:20], "little" if header[5] == 1 else "big")
        kind = int.from_bytes(header[16:18], "little" if header[5] == 1 else "big")
        valid = machine == {"arm64": 183, "x86_64": 62}[arch] and kind in (2, 3)
    elif target == "macos" and len(header) == 20 and header[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
        machine = int.from_bytes(header[4:8], "little" if header[0] == 0xcf else "big")
        kind = int.from_bytes(header[12:16], "little" if header[0] == 0xcf else "big")
        valid = machine == {"arm64": 0x0100000c, "x86_64": 0x01000007}[arch] and kind == 2
    else:
        valid = False
    if not valid:
        raise RuntimeError(f"Expected {target} {arch} executable: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("linux", "macos"))
    parser.add_argument("--toolchain", type=Path, help="Use a supplied SDK instead of the pinned automatic ARM64 SDK")
    parser.add_argument("--cache", type=Path, default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "okular-unix")
    parser.add_argument("--prepare-only", action="store_true", help="Prepare the automatic SDK without building Okular")
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default="arm64")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.epilog = "Pass extra CMake -D definitions after -- (target prefix and matching Linux host tools)."
    args, definitions = parser.parse_known_args()
    if platform.system() != "Linux":
        parser.error("These entry points require a Linux host, including WSL2.")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if definitions[:1] == ["--"]:
        definitions = definitions[1:]
    reserved = {"CMAKE_TOOLCHAIN_FILE", "CMAKE_STAGING_PREFIX", "CMAKE_INSTALL_PREFIX",
                "CMAKE_PROJECT_okular_INCLUDE", "OKULAR_CROSS_SYSTEM", "OKULAR_CROSS_ARCH",
                "CMAKE_INSTALL_BINDIR", "KDE_INSTALL_BINDIR", "KDE_INSTALL_BUNDLEDIR"}
    for definition in definitions:
        key = definition[2:].split("=", 1)[0].split(":", 1)[0]
        if not definition.startswith("-D") or "=" not in definition or key in reserved:
            parser.error(f"Expected a non-reserved CMake -D definition: {definition}")
    build = (args.build_dir or SOURCE / f"build-{args.target}-cross-{args.arch}").resolve()
    if build == SOURCE or SOURCE.is_relative_to(build):
        parser.error("The build directory must not be the source root or one of its parents.")
    if not args.prepare_only and not (build / "cross-build-context.json").exists() and (build / "CMakeCache.txt").exists():
        parser.error("Use a new --build-dir; this CMake cache belongs to another build workflow.")
    dependency_hash = None
    if args.toolchain:
        toolchain = args.toolchain.resolve()
        if not toolchain.is_file():
            parser.error(f"Missing CMake toolchain: {toolchain}")
        if args.prepare_only:
            parser.error("--prepare-only applies to automatic SDK preparation; omit --toolchain.")
    else:
        dependency_hash = hashlib.sha256((SOURCE / f"cmake/{args.target}-dependencies.lock.json").read_bytes()).hexdigest()
        stamp = build / "cross-build-context.json"
        if not args.prepare_only and stamp.exists() and json.loads(stamp.read_text()).get("dependency_sha256") != dependency_hash:
            parser.error("Dependency lock changed; use a new --build-dir or remove the old directory.")
        bootstrap = runpy.run_path(str(SOURCE / "cmake/bootstrap-unix.py"))
        toolchain, prepared_definitions = bootstrap["prepare"](args.target, args.arch, args.cache, args.jobs)
        definitions = prepared_definitions + definitions
        if args.prepare_only:
            print("Prepared SDK toolchain:", toolchain)
            return
    install = build / "install"
    contents = toolchain.read_text()
    environment = {name: os.environ.get(name) for name in re.findall(r"\$ENV\{([^}]+)\}", contents)}
    context = {"source": str(SOURCE), "target": args.target, "arch": args.arch,
               "toolchain": str(toolchain), "sha256": hashlib.sha256(toolchain.read_bytes()).hexdigest(),
               "definitions": definitions,
               "environment_sha256": hashlib.sha256(json.dumps(environment, sort_keys=True).encode()).hexdigest()}
    if dependency_hash:
        context["dependency_sha256"] = dependency_hash
    build.mkdir(parents=True, exist_ok=True)
    with (build / ".cross-build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stamp = build / "cross-build-context.json"
        if stamp.exists():
            if json.loads(stamp.read_text()) != context:
                parser.error("Build context changed; use a new --build-dir or remove the old directory.")
        elif (build / "CMakeCache.txt").exists():
            parser.error("Use a new --build-dir; this CMake cache belongs to another build workflow.")
        stamp.write_text(json.dumps(context, sort_keys=True) + "\n")
        # SDK/compiler/host-tool changes need a new tree: CMake initializes
        # flags and imported package locations during the first configuration.
        run("cmake", "-S", SOURCE, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DOKULAR_UI=desktop", *definitions,
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DCMAKE_STAGING_PREFIX={install}",
            "-DCMAKE_INSTALL_PREFIX=/usr/local", "-DKDE_INSTALL_USE_QT_SYS_PATHS=OFF",
            "-DCMAKE_INSTALL_BINDIR=bin", "-DKDE_INSTALL_BINDIR=bin", "-DKDE_INSTALL_BUNDLEDIR=bin",
            f"-DCMAKE_PROJECT_okular_INCLUDE={SOURCE}/cmake/check-unix-cross.cmake",
            f"-DOKULAR_CROSS_SYSTEM={'Linux' if args.target == 'linux' else 'Darwin'}",
            f"-DOKULAR_CROSS_ARCH={args.arch}")
        run("cmake", "--build", build, "--parallel", args.jobs)
        if install.exists():
            shutil.rmtree(install)
        run("cmake", "--install", build)
        executable = install / "bin/okular"
        if args.target == "macos" and not executable.exists():
            executable = install / "bin/okular.app/Contents/MacOS/okular"
        verify_binary(executable, args.target, args.arch)
        print("Installed cross build:", install)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
