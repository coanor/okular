#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Stage Windows runtime dependencies without running target executables."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


# DLLs supplied by supported Windows versions. API sets are resolved by Windows.
SYSTEM_DLLS = set("""
advapi32 avrt bcrypt bcryptprimitives cabinet cfgmgr32 comctl32 comdlg32
crypt32 cryptbase cryptnet cryptsp d2d1 d3d11 d3d12 d3d9 dcomp dbghelp
dhcpcsvc dnsapi dsound dwmapi dwrite dxgi dxva2 gdi32 gdiplus glu32
hid imm32 iphlpapi kernel32 kernelbase ksuser mf mfplat mfreadwrite
mfuuid mmdevapi mpr mscms msimg32 msvcrt mswsock ncrypt netapi32 normaliz
ntdll ole32 oleacc oleaut32 opengl32 powrprof propsys psapi rasapi32
rpcrt4 secur32 setupapi shcore shell32 shlwapi user32 userenv usp10
ucrtbase uxtheme version windowscodecs winhttp wininet winmm winspool
wintrust wldap32 ws2_32 wtsapi32 wtsapi32 winusb msdmo strmiids
urlmon win32u wlanapi dxcore fwpuclnt wscapi winsta ntmarta authz
evr fontsub uiautomationcore bthprops pdh security
""".split())


def system_dll(name):
    lower = name.lower()
    return lower.startswith(("api-ms-", "ext-ms-")) or Path(lower).stem in SYSTEM_DLLS


def copy_dependency(source, destination):
    # The installed application takes precedence over any cached SDK copy.
    if not Path(destination).exists():
        shutil.copy2(source, destination)
    return str(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--redist", type=Path, required=True)
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    source = args.dependencies.resolve()
    stage = args.install.resolve()
    readobj = shutil.which("llvm-readobj-20") or shutil.which("llvm-readobj")
    if not readobj:
        parser.error("Install Linux llvm-readobj.")
    # Install Okular first. A cached package must never overwrite its binaries.
    for relative in ("translations", "qml", "bin/resources",
                     "bin/qtwebengine_locales", "resources", "share/mime", "share/poppler",
                     "share/ghostscript", "share/fonts", "share/color", "share/locale", "share/gnupg", "certs", "licenses"):
        directory = source / relative
        if directory.exists():
            shutil.copytree(directory, stage / relative, dirs_exist_ok=True, copy_function=copy_dependency)
    for name in ("kf6", "knotifications6", "kstyle", "mime", "color-schemes", "icons",
                 "locale", "dbus-1", "iso-codes", "qlogging-categories6", "poppler", "ghostscript"):
        directory = source / "bin/data" / name
        if directory.exists():
            shutil.copytree(directory, stage / "bin/data" / name, dirs_exist_ok=True, copy_function=copy_dependency)
    for relative in ("lib/plugins", "plugins"):
        for directory in (source / relative).glob("*"):
            if directory.is_dir() and directory.name not in ("designer", "qmllint", "qmlls", "qmltooling"):
                shutil.copytree(directory, stage / "lib/plugins" / directory.name, dirs_exist_ok=True, copy_function=copy_dependency)
    for pattern in ("QtWebEngineProcess.exe", "kioworker.exe", "kioexec.exe", "kconf_update.exe",
                    "dbus-daemon.exe", "knewstuff-dialog.exe", "gswin64c.exe", "gpg*.exe", "dirmngr*.exe", "icudtl.dat",
                    "qtwebengine*.pak", "v8_context_snapshot.bin", "libcrypto*.dll", "libssl*.dll",
                    "libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll",
                    "vk_swiftshader*", "vulkan-1.dll", "opengl32sw.dll"):
        for file in (source / "bin").glob(pattern):
            copy_dependency(file, stage / "bin" / file.name)
    for pattern in ("*.ps", "Fontmap*", "*.icc"):
        for file in (source / "lib").glob(pattern):
            if file.is_file():
                copy_dependency(file, stage / "lib" / file.name)

    available = {file.name.casefold(): file for file in (source / "bin").glob("*.dll")}
    for file in args.redist.rglob("*.dll"):
        available[file.name.casefold()] = file
    queue = list(stage.rglob("*.dll")) + list(stage.rglob("*.exe"))
    visited = set()
    system = set()
    missing = set()

    def imports(file):
        output = subprocess.check_output([readobj, "--file-headers", "--coff-imports", str(file)], text=True)
        if "Machine: IMAGE_FILE_MACHINE_AMD64" not in output:
            raise RuntimeError(f"Expected Windows x64 PE: {file}")
        return re.findall(r"^  Name: (.+)$", output, re.MULTILINE)

    with ThreadPoolExecutor(max_workers=4) as pool:
        while queue:
            batch = [file for file in queue if file not in visited]
            visited.update(batch)
            queue = []
            for names in pool.map(imports, batch):
                for name in names:
                    if system_dll(name):
                        system.add(name)
                        continue
                    destination = stage / "bin" / name
                    if destination.exists():
                        continue
                    dependency = available.get(name.casefold())
                    if dependency is None:
                        missing.add(name)
                        continue
                    destination = stage / "bin" / dependency.name
                    if not destination.exists():
                        shutil.copy2(dependency, destination)
                        queue.append(destination)
    if missing:
        raise RuntimeError("Missing runtime DLLs: " + ", ".join(sorted(missing)))
    for relative in ("bin/okular.exe", "bin/Okular6Core.dll", "bin/QtWebEngineProcess.exe",
                     "bin/qtwebengine_resources.pak", "bin/icudtl.dat",
                     "lib/plugins/platforms/qwindows.dll", "translations/qtwebengine_locales/en-US.pak"):
        if not (stage / relative).is_file():
            raise RuntimeError(f"Missing runtime resource: {relative}")
    licenses = Path(__file__).resolve().parent.parent / "LICENSES"
    shutil.copytree(licenses, stage / "licenses/okular", dirs_exist_ok=True)
    (stage / "bin/qt.conf").write_text(
        "[Paths]\nPrefix=..\nBinaries=bin\nLibraries=bin\nLibraryExecutables=bin\n"
        "Data=bin\nPlugins=lib/plugins\nTranslations=translations\nQmlImports=qml\n")
    (stage / "runtime-system-imports.txt").write_text("\n".join(sorted(system)) + "\n")
    shutil.copy2(Path(__file__).with_name("windows-dependencies.lock.json"), stage / "windows-dependencies.lock.json")
    temporary = args.archive.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in sorted(stage.rglob("*")):
            if file.is_file():
                archive.write(file, file.relative_to(stage))
    temporary.replace(args.archive)
    print(f"Packaged {len(visited)} Windows x64 binaries: {args.archive}")


if __name__ == "__main__":
    main()
