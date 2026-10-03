#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check cross-build isolation and compile small ELF/Mach-O executables on Linux."""

import importlib.util
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


REPOSITORY = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("build_unix", REPOSITORY / "cmake/build-unix.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def tool(*names):
    return next((path for name in names if (path := shutil.which(name))), None)


def quiet_run(*arguments):
    try:
        subprocess.check_output(list(map(str, arguments)), stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as error:
        print(error.output.decode(), file=sys.stderr)
        raise


class UnixCrossBuildTest(unittest.TestCase):
    def test_rejects_host_binary_for_arm64(self):
        if platform.machine() != "x86_64":
            self.skipTest("This fixture uses an x86_64 Linux host executable")
        with self.assertRaisesRegex(RuntimeError, "Expected linux arm64"):
            build.verify_binary(Path(sys.executable).resolve(), "linux", "arm64")
        with self.assertRaisesRegex(RuntimeError, "Expected macos x86_64"):
            build.verify_binary(Path(sys.executable).resolve(), "macos", "x86_64")

    def test_rejects_reserved_cmake_override_before_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            toolchain = root / "toolchain.cmake"
            toolchain.touch()
            output = root / "output"
            arguments = ["build-unix.py", "linux", "--toolchain", str(toolchain), "--build-dir", str(output),
                         "--", "-DCMAKE_TOOLCHAIN_FILE=/another/toolchain"]
            with patch.object(sys, "argv", arguments), self.assertRaises(SystemExit), patch.object(build, "run") as run:
                build.main()
            run.assert_not_called()
            self.assertFalse(output.exists())

    def test_rejects_changed_toolchain_before_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            toolchain = root / "toolchain.cmake"
            toolchain.write_text("# original toolchain\n")
            output = root / "output"
            arguments = ["build-unix.py", "linux", "--toolchain", str(toolchain), "--build-dir", str(output)]
            with patch.object(sys, "argv", arguments), patch.object(build, "run", side_effect=RuntimeError("stop at configure")), \
                    self.assertRaisesRegex(RuntimeError, "stop at configure"):
                build.main()
            toolchain.write_text("# another compiler/SDK\n")
            with patch.object(sys, "argv", arguments), patch.object(build, "run") as run, self.assertRaises(SystemExit):
                build.main()
            run.assert_not_called()

    def test_rejects_changed_sdk_environment_before_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            toolchain = root / "toolchain.cmake"
            toolchain.write_text('set(CMAKE_SYSROOT "$ENV{OKULAR_TEST_SYSROOT}")\n')
            arguments = ["build-unix.py", "linux", "--toolchain", str(toolchain), "--build-dir", str(root / "output")]
            with patch.object(sys, "argv", arguments), patch.dict(os.environ, {"OKULAR_TEST_SYSROOT": "/old-sdk"}), \
                    patch.object(build, "run", side_effect=RuntimeError("stop at configure")), self.assertRaises(RuntimeError):
                build.main()
            with patch.object(sys, "argv", arguments), patch.dict(os.environ, {"OKULAR_TEST_SYSROOT": "/new-sdk"}), \
                    patch.object(build, "run") as run, self.assertRaises(SystemExit):
                build.main()
            run.assert_not_called()

    def fixture(self, root):
        source = root / "source"
        (source / "cmake").mkdir(parents=True)
        shutil.copy2(REPOSITORY / "cmake/check-unix-cross.cmake", source / "cmake")
        (source / "main.cpp").write_text("int main() { return 0; }\n")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(okular LANGUAGES CXX)\n'
            'add_executable(okular main.cpp)\n'
            'set_target_properties(okular PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")\n'
            'target_link_options(okular PRIVATE -nostdlib)\n'
            'if(APPLE)\n target_link_options(okular PRIVATE "-Wl,-e,_main")\n'
            'else()\n target_link_options(okular PRIVATE "-Wl,-e,main")\nendif()\n'
            'install(TARGETS okular RUNTIME DESTINATION bin)\n')
        return source

    def test_rejects_native_toolchain(self):
        if not tool("cmake") or not tool("ninja") or not tool("g++"):
            self.skipTest("CMake, Ninja and native g++ are needed")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.fixture(root)
            toolchain = root / "native.cmake"
            toolchain.write_text(f'set(CMAKE_CXX_COMPILER "{tool("g++")}")\n')
            arguments = ["build-unix.py", "linux", "--arch", "x86_64", "--toolchain", str(toolchain),
                         "--build-dir", str(root / "output")]
            with patch.object(build, "SOURCE", source), patch.object(sys, "argv", arguments), patch.object(build, "run", side_effect=quiet_run), \
                    self.assertRaises(subprocess.CalledProcessError) as error:
                build.main()
            self.assertIn(b"toolchain must cross-compile", error.exception.output)

    def test_rejects_compiler_that_produces_the_wrong_cpu(self):
        if platform.machine() != "x86_64" or not all((tool("cmake"), tool("ninja"), tool("g++"))):
            self.skipTest("This fixture needs CMake, Ninja and x86_64 Linux g++")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.fixture(root)
            toolchain = root / "wrong-compiler.cmake"
            toolchain.write_text('set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR aarch64)\n'
                                 f'set(CMAKE_CXX_COMPILER "{tool("g++")}")\nset(CMAKE_FIND_ROOT_PATH "{root}")\n')
            arguments = ["build-unix.py", "linux", "--toolchain", str(toolchain), "--build-dir", str(root / "output"),
                         "--", f"-DQT_HOST_PATH={root}", f"-DKF6_HOST_TOOLING={root}"]
            with patch.object(build, "SOURCE", source), patch.object(sys, "argv", arguments), \
                    patch.object(build, "run", side_effect=quiet_run), self.assertRaisesRegex(RuntimeError, "Expected linux arm64"):
                build.main()

    def test_real_cross_compile_and_incremental_build(self):
        cxx = tool("clang++-20", "clang++")
        ar = tool("llvm-ar-20", "llvm-ar")
        ranlib = tool("llvm-ranlib-20", "llvm-ranlib")
        if not all((cxx, ar, ranlib, tool("cmake"), tool("ninja"))):
            self.skipTest("CMake, Ninja, Linux Clang and LLVM archive tools are needed")
        for target, arch in (("linux", "arm64"), ("macos", "arm64"), ("macos", "x86_64")):
            with self.subTest(target=target, arch=arch), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = self.fixture(root)
                linker = tool("ld.lld-20", "ld.lld") if target == "linux" else tool("ld64.lld-20", "ld64.lld")
                install_name = tool("llvm-install-name-tool-20", "llvm-install-name-tool")
                if not linker or (target == "macos" and not install_name):
                    self.skipTest("The target LLD linker/install-name tool is needed")
                if target == "macos":
                    # Use the LLVM installation's conventional linker name.
                    linker = str(Path(linker).resolve().with_name("ld64.lld"))
                processor = "aarch64" if target == "linux" else arch
                triple = "aarch64-linux-gnu" if target == "linux" else f"{arch}-apple-macos11.0"
                # These freestanding compiler probes need no target SDK/runtime;
                # only the fixture skips linking CMake's compiler check.
                contents = (f'set(CMAKE_SYSTEM_NAME {"Linux" if target == "linux" else "Darwin"})\n'
                            f'set(CMAKE_SYSTEM_PROCESSOR {processor})\n'
                            f'set(CMAKE_CXX_COMPILER "{cxx}")\nset(CMAKE_CXX_COMPILER_TARGET {triple})\n'
                            f'set(CMAKE_AR "{ar}")\nset(CMAKE_RANLIB "{ranlib}")\n'
                            f'set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld={linker}")\n'
                            f'set(CMAKE_FIND_ROOT_PATH "{root}")\n'
                            'set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n')
                if target == "macos":
                    contents += (f'set(CMAKE_OSX_ARCHITECTURES {arch} CACHE STRING "")\n'
                                 'set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")\n'
                                 'set(CMAKE_CXX_FLAGS_INIT "-mlinker-version=520")\n'
                                 f'set(CMAKE_INSTALL_NAME_TOOL "{install_name}")\n')
                toolchain = root / "cross.cmake"
                toolchain.write_text(contents)
                output = root / "output"
                arguments = ["build-unix.py", target, "--arch", arch, "--toolchain", str(toolchain),
                             "--build-dir", str(output), "--", f"-DQT_HOST_PATH={root}", f"-DKF6_HOST_TOOLING={root}"]
                with patch.object(build, "SOURCE", source), patch.object(sys, "argv", arguments), patch.object(build, "run", side_effect=quiet_run):
                    build.main()
                    executable = output / "bin/okular"
                    modified = executable.stat().st_mtime_ns
                    (output / "install/stale-plugin").touch()
                    build.main()
                    self.assertEqual(executable.stat().st_mtime_ns, modified)
                    self.assertFalse((output / "install/stale-plugin").exists())
                    build.verify_binary(output / "install/bin/okular", target, arch)


if __name__ == "__main__":
    unittest.main()
