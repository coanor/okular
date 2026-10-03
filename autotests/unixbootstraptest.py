#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check SDK extraction, relocated resources and Linux host tool isolation."""

import io
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parent.parent
bootstrap = runpy.run_path(str(SOURCE / "cmake/bootstrap-unix.py"))


def archive(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w") as output:
        for name, value, kind in entries:
            entry = tarfile.TarInfo(name)
            if kind == "link":
                entry.type = tarfile.SYMTYPE
                entry.linkname = value
                output.addfile(entry)
            else:
                entry.size = len(value)
                output.addfile(entry, io.BytesIO(value))
    stream.seek(0)
    return stream


class UnixBootstrapTest(unittest.TestCase):
    def test_program_search_ignores_explicit_target_prefix(self):
        if not shutil.which("cmake"):
            self.skipTest("CMake is needed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("host", "target"):
                folder = root / name / "bin"
                folder.mkdir(parents=True)
                executable = folder / "okular-test-tool"
                executable.write_text(f"#!/bin/sh\necho {name}\n")
                executable.chmod(0o755)
            script = root / "probe.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.22)\nset(CMAKE_CROSSCOMPILING TRUE)\nset(CMAKE_SYSTEM_NAME Linux)\n'
                'set(CMAKE_SYSTEM_PROCESSOR aarch64)\nset(OKULAR_CROSS_SYSTEM Linux)\nset(OKULAR_CROSS_ARCH arm64)\n'
                f'set(CMAKE_PREFIX_PATH "{root}/target")\nset(CMAKE_FIND_ROOT_PATH "{root}/target")\n'
                f'set(CMAKE_PROGRAM_PATH "{root}/host/bin")\nset(QT_HOST_PATH "{root}/host")\n'
                f'set(KF6_HOST_TOOLING "{root}/host")\ninclude("{SOURCE}/cmake/check-unix-cross.cmake")\n'
                'find_program(PROBE okular-test-tool REQUIRED)\nexecute_process(COMMAND "${PROBE}" OUTPUT_VARIABLE RESULT)\n'
                'if(NOT RESULT STREQUAL "host\\n")\n message(FATAL_ERROR "Executed target program: ${PROBE}")\nendif()\n')
            subprocess.run(["cmake", "-P", str(script)], check=True)

    def test_absolute_sdk_links_remain_in_sysroot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = archive([("usr/lib/data", b"target", "file"), ("lib", "/usr/lib", "link"),
                               ("lib/another", b"inside", "file")])
            bootstrap["extract_tar"](payload, root)
            self.assertEqual(os.readlink(root / "lib"), "usr/lib")
            self.assertEqual((root / "lib/data").read_bytes(), b"target")
            self.assertEqual((root / "usr/lib/another").read_bytes(), b"inside")

    def test_rejects_tar_paths_and_links_that_escape(self):
        for entry in [("../escape", b"data", "file"), ("link", "../../escape", "link"),
                      ("link", "/../../escape", "link")]:
            with self.subTest(entry=entry), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(tarfile.FilterError):
                    bootstrap["extract_tar"](archive([entry]), Path(directory) / "sdk")
                self.assertFalse((Path(directory) / "escape").exists())

    def test_resource_relocation_is_restart_safe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            resource = root / "kdedbx45.dtd"
            resource.write_bytes(b'file:///usr/share/xml/docbook/docbookx.dtd\n')
            replacements = [("/usr/share/xml", root / "usr/share/xml")]
            bootstrap["relocate"](root, replacements)
            expected = resource.read_bytes()
            bootstrap["relocate"](root, replacements)
            self.assertEqual(resource.read_bytes(), expected)
            self.assertIn(str(root).encode(), expected)

    def test_native_wrapper_keeps_library_paths_local_and_recovers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            executable = root / "bin/python"
            shutil.copy2(Path(sys.executable).resolve(), executable)
            original_path = os.environ.get("LD_LIBRARY_PATH")
            bootstrap["wrap_tools"](root, ["bin"], [root / "lib"], root / "share")
            output = subprocess.check_output([executable, "-c", "import os; print(os.environ['LD_LIBRARY_PATH'])"], text=True)
            self.assertEqual(output.strip(), str(root / "lib"))
            self.assertEqual(os.environ.get("LD_LIBRARY_PATH"), original_path)
            executable.unlink()  # Simulate interruption between rename and wrapper installation.
            bootstrap["wrap_tools"](root, ["bin"], [root / "lib"], root / "share")
            subprocess.run([executable, "-c", "pass"], check=True)

    def test_lld_symlink_preserves_the_driver_name(self):
        linker = shutil.which("ld.lld-20") or shutil.which("ld.lld")
        if not linker:
            self.skipTest("Linux LLD is needed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tools = dict(cc="clang", cxx="clang++", ar="llvm-ar", ranlib="llvm-ranlib", linker=linker)
            chain = bootstrap["write_toolchain"]("linux", root, root / "sdk", tools, {})
            self.assertIn(f'--ld-path="{root}/ld.lld"', chain.read_text())
            result = subprocess.check_output([root / "ld.lld", "--version"], text=True)
            self.assertIn("LLD", result)
            self.assertNotIn("generic driver", result)


if __name__ == "__main__":
    unittest.main()
