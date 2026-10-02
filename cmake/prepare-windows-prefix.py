#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Create a Linux-readable view of a Windows dependency prefix."""

import argparse
import os
from pathlib import Path
import re
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--old-prefix", action="append", default=[])
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    destination = args.destination.resolve()
    if destination == source or destination.is_relative_to(source):
        parser.error("The destination must be outside the source prefix.")

    # CMake target files are usually relocatable. Some packages and .pc files
    # retain the original Windows installation path, which Linux cannot use.
    old_prefixes = set(args.old_prefix)
    for file in (source / "lib" / "pkgconfig").glob("*.pc"):
        match = re.search(r"^prefix=([A-Za-z]:[/\\].+)$", file.read_text(), re.MULTILINE)
        if match:
            old_prefixes.add(match.group(1).strip().replace("\\", "/"))
    ecm_config = source / "share" / "ECM" / "cmake" / "ECMConfig.cmake"
    if ecm_config.exists():
        match = re.search(r'set\(ECM_PREFIX "([A-Za-z]:/[^\"]+)"\)', ecm_config.read_text())
        if match:
            old_prefixes.add(match.group(1))

    destination.mkdir(parents=True, exist_ok=True)
    for name in ("bin", "include", "libexec", "mkspecs", "metatypes", "plugins", "qml", "translations", "licenses", "certs"):
        path = source / name
        link = destination / name
        if path.exists() and not link.exists() and not link.is_symlink():
            link.symlink_to(os.path.relpath(path, link.parent), target_is_directory=path.is_dir())

    for directory, copied in (("lib", {"cmake", "pkgconfig"}), ("share", {"ECM", "pkgconfig"})):
        root = destination / directory
        if root.is_symlink():
            parser.error(f"Metadata parent must not be a symlink: {root}")
        root.mkdir(exist_ok=True)
        for child in (source / directory).iterdir():
            path = root / child.name
            if child.name in copied:
                if path.is_symlink() or any(entry.is_symlink() for entry in path.rglob("*")):
                    parser.error(f"Metadata destination must not contain symlinks: {path}")
                shutil.copytree(child, path, dirs_exist_ok=True)
            elif not path.exists() and not path.is_symlink():
                path.symlink_to(os.path.relpath(child, path.parent), target_is_directory=child.is_dir())
        for subtree in copied:
            for file in (root / subtree).rglob("*"):
                if file.suffix not in (".cmake", ".pc"):
                    continue
                contents = file.read_text()
                for old in sorted(old_prefixes, key=len, reverse=True):
                    contents = contents.replace(old, str(destination))
                file.write_text(contents)
    print(destination)


if __name__ == "__main__":
    main()
