#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Linux-only regression checks for the Windows dependency bootstrap."""

import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile


def load(name):
    path = Path(__file__).resolve().parent.parent / "cmake" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


build = load("build-windows")
package = load("package-windows")


class WindowsCrossBuildTest(unittest.TestCase):
    def test_cached_download_must_match_hash(self):
        payload = b"verified dependency"
        spec = {"sha256": hashlib.sha256(payload).hexdigest(), "url": "https://example.invalid/package"}
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            (cache / spec["sha256"]).write_bytes(b"corrupted cached download")
            def fetch(*args):
                Path(args[args.index("--output") + 1]).write_bytes(payload)
            with patch.object(build, "run", side_effect=fetch) as run:
                self.assertEqual(build.download(spec, cache).read_bytes(), payload)
                self.assertEqual(run.call_count, 1)

    def test_bad_mirror_hash_is_rejected(self):
        spec = {"sha256": hashlib.sha256(b"expected").hexdigest(), "url": "https://example.invalid/package"}
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            def fetch(*args):
                Path(args[args.index("--output") + 1]).write_bytes(b"wrong package")
            with patch.object(build, "run", side_effect=fetch), self.assertRaises(RuntimeError):
                build.download(spec, cache)
            self.assertFalse((cache / spec["sha256"]).exists())

    def test_tar_link_cannot_escape_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "unsafe.tar"
            with tarfile.open(archive, "w") as file:
                link = tarfile.TarInfo("escape")
                link.type = tarfile.SYMTYPE
                link.linkname = "../outside"
                file.addfile(link)
            with self.assertRaises(ValueError):
                build.extract(archive, root / "destination", "tar", "")
            self.assertFalse((root / "outside").exists())

    def test_7z_unix_symlink_is_rejected_before_extraction(self):
        sevenzip = shutil.which("7zz") or shutil.which("7z")
        if not sevenzip:
            self.skipTest("Linux 7zip is not installed")
        sevenzip = str(Path(sevenzip).resolve())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "regular").write_text("payload")
            (root / "alias").symlink_to("regular")
            archive = root / "links.7z"
            subprocess.run([sevenzip, "a", "-snl", str(archive), "alias", "regular"],
                           cwd=root, check=True, stdout=subprocess.DEVNULL)
            destination = root / "destination"
            with self.assertRaises(ValueError):
                build.extract(archive, destination, "7z", sevenzip)
            self.assertEqual(list(destination.iterdir()), [])

    def test_build_rejects_changed_dependency_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "build"
            output.mkdir()
            (output / "dependency-lock.sha256").write_text("locked-dependencies\n")
            old = root / "cache-a"
            (output / "CMakeCache.txt").write_text(
                f"OKULAR_WINDOWS_SDK:PATH={old}/sdk\n"
                f'CMAKE_CXX_FLAGS:STRING=-isystem "{old}/sdk/crt/include"\n')
            build.validate_build(output, old, "locked-dependencies", {})
            with self.assertRaisesRegex(RuntimeError, "Build context changed"):
                build.validate_build(output, root / "cache-b", "locked-dependencies", {})

    def test_build_rejects_changed_compiler(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "dependency-lock.sha256").write_text("locked-dependencies\n")
            (output / "CMakeCache.txt").write_text("CMAKE_CXX_COMPILER:FILEPATH=/old/clang++\n")
            with self.assertRaisesRegex(RuntimeError, "CMAKE_CXX_COMPILER"):
                build.validate_build(output, output / "cache", "locked-dependencies",
                                     {"CMAKE_CXX_COMPILER": "/new/clang++"})

    def test_build_rejects_unmanaged_cache_and_changed_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "CMakeCache.txt").touch()
            with self.assertRaisesRegex(RuntimeError, "no dependency lock"):
                build.validate_build(output, output / "cache", "new-lock", {})
            (output / "dependency-lock.sha256").write_text("old-lock\n")
            with self.assertRaisesRegex(RuntimeError, "Dependency lock changed"):
                build.validate_build(output, output / "cache", "new-lock", {})

    def test_packaging_preserves_built_plugins(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, stage, redist = (root / name for name in ("dependencies", "install", "redist"))
            redist.mkdir()
            plugin = "lib/plugins/okular_generators/okularGenerator_poppler.dll"
            for directory, payload in ((source, b"cached plugin"), (stage, b"newly built plugin")):
                (directory / plugin).parent.mkdir(parents=True)
                (directory / plugin).write_bytes(payload)
            # The dependency-only files must still be copied and inspected.
            for relative in ("bin/QtWebEngineProcess.exe", "bin/qtwebengine_resources.pak", "bin/icudtl.dat",
                             "lib/plugins/platforms/qwindows.dll", "translations/qtwebengine_locales/en-US.pak"):
                (source / relative).parent.mkdir(parents=True, exist_ok=True)
                (source / relative).write_bytes(b"runtime dependency")
            (stage / "bin").mkdir()
            for name in ("okular.exe", "Okular6Core.dll"):
                (stage / "bin" / name).write_bytes(b"built application")
            archive = root / "okular.zip"
            arguments = ["package-windows.py", "--dependencies", str(source), "--redist", str(redist),
                         "--install", str(stage), "--archive", str(archive)]
            # Tiny fixture files stand in for PE binaries; test the staging/archive flow.
            with patch.object(sys, "argv", arguments), patch.object(package.shutil, "which", return_value="llvm-readobj"), \
                    patch.object(package.subprocess, "check_output", return_value="Machine: IMAGE_FILE_MACHINE_AMD64\n") as inspect:
                package.main()
            self.assertEqual(inspect.call_count, 5)
            with zipfile.ZipFile(archive) as file:
                self.assertEqual(file.read(plugin), b"newly built plugin")
                self.assertEqual(file.read("lib/plugins/platforms/qwindows.dll"), b"runtime dependency")

    def test_merged_windows_header_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            include = Path(temporary)
            (include / "KParts").mkdir()
            (include / "KParts/mainwindow.h").touch()
            (include / "kio").mkdir()
            (include / "kio/Global").touch()
            build.normalize_headers(include)
            build.normalize_headers(include)
            self.assertTrue((include / "kparts/mainwindow.h").is_file())
            self.assertTrue((include / "KIO/Global").is_file())

    def test_runtime_requires_redistributable_dlls(self):
        self.assertTrue(package.system_dll("WINSPOOL.DRV"))
        self.assertTrue(package.system_dll("api-ms-win-crt-runtime-l1-1-0.dll"))
        self.assertFalse(package.system_dll("msvcp140.dll"))
        self.assertFalse(package.system_dll("mfc140.dll"))
        self.assertFalse(package.system_dll("libwinpthread-1.dll"))


if __name__ == "__main__":
    unittest.main()
