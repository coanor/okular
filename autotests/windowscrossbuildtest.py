#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Linux-only regression checks for the Windows dependency bootstrap."""

import hashlib
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch


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
