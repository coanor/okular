# Okular cross-build 调研

调研日期：2026-10-02。源码基线：`a835d335b`。范围：从 Linux host 构建 Linux ARM64、Windows、macOS，重点区分当前实现的依赖限制与 GUI runtime 问题。本文记录初步文档、源码审查与 configure 探测；下面的环境结果是准备完整 SDK 前的记录。

后续新增了 Linux Clang → Windows MSVC ABI 的实际构建路线，最初复用 `.deps/windows`，目前已改为从 Linux 下载并校验固定版本的 target dependencies，再构建匹配的 Linux host tools。它保留 Qt WebEngine，不使用 MinGW compiler。日常入口与验证结果见 [Building Windows from Linux](build-windows-from-linux.md)。

## 结论

Qt Widgets 本身支持多个 OS 和 CPU architecture。阻力主要来自 target toolchain、完整的 target 依赖栈、host code-generation tools，以及本 fork 的 desktop AI renderer 对 Qt WebEngine 的强依赖。Qt 的总体平台支持表不能替代具体 module 的支持表；Qt 明确指出 WebEngine 还受 Chromium 的限制。[Qt supported platforms](https://doc.qt.io/qt-6.10/supported-platforms.html)

必须区分两种工作：

- **跨 architecture、同一 OS**：例如 Linux x86_64 → Linux ARM64，主要需要 ARM64 compiler、sysroot、target Qt/KF6/文档后端，以及 Linux host tools。
- **跨 OS**：例如 Linux → Windows/macOS，即使 CPU 同为 x86_64，也需要目标 OS 的 ABI、系统 SDK、Qt/KF6 和全部链接依赖。改 `CMAKE_SYSTEM_NAME` 或 compiler 不能生成缺失的 SDK 与库。

CMake 的官方示例将 compiler、sysroot 和查找根目录放在 toolchain file 中，并要求构建时执行的程序从 host 查找、headers/libraries/packages 从 target 查找。[CMake toolchains](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling-for-linux)

## 官方能力与实际路线

| Target | 官方能力 | 从 Linux host 构建当前 desktop 实现的主要约束 |
| --- | --- | --- |
| Linux ARM64 | Qt 列出 Linux ARM64 配置，但桌面 Arm 支持使用 Raspberry Pi 5 / Ubuntu 24.04 作为参考硬件。[平台说明](https://doc.qt.io/qt-6.10/supported-platforms.html) | 可行性最高。需要 target sysroot 中的完整依赖；Qt WebEngine 的 cross-build 检查明确允许 Linux `arm64`。[WebEngine configure 源码](https://raw.githubusercontent.com/qt/qtwebengine/6.10/configure.cmake) |
| Windows x86_64 | Qt 支持 MSVC 和 MinGW 配置。[平台说明](https://doc.qt.io/qt-6.10/supported-platforms.html) | 当前 WebEngine 依赖使常见 MinGW cross-build 路线受阻：Qt 明确说明 WebEngine 不能使用 MinGW 构建。Windows + MSVC + Craft 是官方 KDE 文档已有的路线。[WebEngine platform notes](https://doc.qt.io/qt-6.10/qtwebengine-platform-notes.html)、[KDE Craft](https://develop.kde.org/docs/getting-started/building/craft/) |
| Windows ARM64 | Qt 支持 Windows ARM64 / MSVC；Qt 6.10 release notes 明确说明 Windows ARM 预编译包包含 WebEngine。[平台说明](https://doc.qt.io/qt-6.10/supported-platforms.html)、[Qt 6.10 changes](https://doc.qt.io/qt-6.12/whatsnew610.html) | 不能笼统说 ARM64 不支持 WebEngine。需选实际提供该 module 的 Qt 版本/包，并准备 ARM64 版本的 KF6 和全部后端；Qt 最低版本要求本身不保证所有版本、包具备这一组合。 |
| macOS x86_64 / ARM64 | Qt 官方使用 Xcode toolchain + macOS SDK；支持用 `CMAKE_OSX_ARCHITECTURES` cross-compile architecture 或生成 universal binary。[Qt for macOS](https://doc.qt.io/qt-6.10/macos.html) | macOS host 上的 x86_64 ↔ ARM64 属于官方路线。Linux → macOS 需要额外 SDK/toolchain 集成，本文未找到标准的 Qt/KDE 文档路线；WebEngine 的配置还要求 macOS compiler 为 `AppleClang`，普通 Linux Clang 不满足这项检查。[WebEngine configure 源码](https://raw.githubusercontent.com/qt/qtwebengine/6.10/configure.cmake) |

这里的“Linux → Windows/macOS 受阻”描述当前常规工具链路线，不是理论上的绝对不可能。Chromium 自己有 Linux/macOS → Windows 的专门 cross-build 说明，要求准备 Windows SDK/toolchain；它不等价于 Qt WebEngine、KF6、Okular 已有相同构建支持。[Chromium Windows cross-build](https://chromium.googlesource.com/chromium/src/+/a43b34dfc661b96f44d9e02528d62c0d711b9dc6/docs/win_cross.md)

## Qt 与 KDE 的 host tools

构建 target Qt 时，需要在 host 可运行的 `moc`、`rcc`、`qmlcachegen`、`qsb` 等。Qt 要求 host 与 target 使用相同版本，通过 `QT_HOST_PATH` 指向 host Qt；target 使用 Quick 时，host 工具集也须包含相应 module 的 tools。host tools 必须能在 host 的执行环境中运行，不能仅因 target SDK 含有同名工具就直接使用。[Cross-compiling Qt](https://doc.qt.io/qt-6.10/cross-compiling-qt.html)

KDE 也有同类分离机制。KConfig 的 package config 在 cross-compiling 且设置 `KF6_HOST_TOOLING` 时，导入 host 的 `KF6ConfigCompilerTargets.cmake`；KDocTools 同样导入 host 的工具 targets。因此只准备 target libraries 仍不够，还要提供 host `kconfig_compiler_kf6`，构建文档时还需 host DocTools。[KConfig package config](https://raw.githubusercontent.com/KDE/kconfig/master/KF6ConfigConfig.cmake.in)、[KConfig compiler target](https://raw.githubusercontent.com/KDE/kconfig/master/src/kconfig_compiler/CMakeLists.txt)、[KDocTools package config](https://raw.githubusercontent.com/KDE/kdoctools/master/KF6DocToolsConfig.cmake.in)

Craft 能构建和打包 Windows、Android、macOS、Linux 的 KDE 软件，并能使用依赖缓存。这说明它可管理依赖栈，不意味着单一 Linux host 自动支持任意 target OS/architecture。KDE 的 Windows 教程直接使用 Windows 上的 Craft 环境。[KDE Craft](https://develop.kde.org/docs/getting-started/building/craft/)

## WebEngine 为什么比普通 GUI 更限制构建

Qt WebEngine 只支持 Windows、Linux、macOS；源代码配置将 Qt WebEngine 与可以支持 Android/iOS 的 Qt PDF 明确分开。WebEngine 也不支持 static build。Windows 构建要求 Visual Studio 和 Windows SDK，并且不支持 MinGW。[WebEngine overview](https://doc.qt.io/qt-6.10/qtwebengine-overview.html#platform-notes)、[WebEngine platform notes](https://doc.qt.io/qt-6.10/qtwebengine-platform-notes.html)、[configure 源码](https://raw.githubusercontent.com/qt/qtwebengine/6.10/configure.cmake)

如果要从源码构建 target WebEngine，还会增加 Chromium 的 C++20 compiler、Python、Node.js、GN/Ninja、gperf/bison/flex，以及 Linux 上的 NSS、Fontconfig、D-Bus、graphics headers 等要求。直接使用适配 target 的预编译 WebEngine 可减少这部分工作，但不会减少 Okular 对 target libraries 和 host tools 的要求。[WebEngine platform notes](https://doc.qt.io/qt-6.10/qtwebengine-platform-notes.html)

本仓库最低 Qt 为 6.6；本文引用 Qt 6.10 的版本固定文档作为已明确的平台能力基线，不能把这些能力自动套到 6.6。查询时，通用 `/qt-6/qtwebengine-platform-notes.html` 跳到 development snapshot，因此这里避免用该页面推断发行版要求。

## GUI 的问题主要出现在运行与打包阶段

Qt 的 QPA plugins 已提供 Windows (`qwindows`)、macOS (`qcocoa`)、Linux X11 (`qxcb`) / Wayland (`qwayland`) 的窗口系统接口，GUI 不要求 host 与 target 使用相同窗口系统。编译和链接不会要求显示目标系统的窗口；启动与 GUI 测试才需要对应 runtime 环境。[Qt Platform Abstraction](https://doc.qt.io/qt-6.10/qpa.html)

完成链接也不等于可发布：要打包 target Qt/KF6、document generator plugins、image plugins、QPA plugin 和相应 resources。Qt 会检查 plugin 的 Qt 版本与构建配置，可通过 `QT_DEBUG_PLUGINS` 排查加载问题。[Deploying plugins](https://doc.qt.io/qt-6.10/deployment-plugins.html)

AI renderer 还必须携带 `QtWebEngineProcess`、`.pak`、ICU/V8 resources、locales；macOS 要为 helper process 正确签名。Windows/macOS 的 `windeployqt` / `macdeployqt` 自动处理 Qt 部分，但外部 KDE/文档后端依赖仍需相应打包逻辑。[Deploying WebEngine](https://doc.qt.io/qt-6.10/qtwebengine-deploying.html)、[Windows deployment](https://doc.qt.io/qt-6.10/windows-deployment.html)

## Android 作为现有 cross-build 参考

KDE 已有 Craft + container 的 Android cross-build 文档；ECM 的 Android toolchain 使用 `CMAKE_TOOLCHAIN_FILE`，并要求 Android NDK/SDK，同时支持 target 依赖查找根目录。这可用作检查 host/target 分离方式的现有参考。[KDE Android build](https://develop.kde.org/docs/packaging/android/building_applications/)、[ECM Android toolchain](https://api.kde.org/ecm/toolchain/Android.html)

Android 上可采用 Okular 已有 mobile UI 路线；Qt 支持 Android 不意味着 desktop WebEngine renderer 也可移植到 Android。[WebEngine overview](https://doc.qt.io/qt-6.10/qtwebengine-overview.html#platform-notes)

## 当前仓库的具体限制

- desktop 在顶层 [`CMakeLists.txt`](../CMakeLists.txt) 第 105 行强制查找 `Network WebEngineWidgets`，第 598 行把 WebEngine 链接进 `okularpart`。AI sources 同样始终加入 desktop target；目前没有单独关闭 AI/WebEngine 的 CMake option。仅在界面中隐藏 AI panel 不会解除这个 build dependency。[AI renderer](../part/aimarkdownview.h)
- 需要 Qt 6、KF6、ZLIB，以及所启用的 document backends 的 target 版本。PDF backend 要求 Poppler >= 24.08 的 Qt6 binding。多个看似 optional 的 package 默认会被标为 REQUIRED，最后由 `feature_summary(... FATAL_ON_MISSING_REQUIRED_PACKAGES)` 检查；精简依赖需使用现有 `FORCE_NOT_REQUIRED_DEPENDENCIES`，不能省略 PDF 所需 Poppler 后仍宣称完整 PDF 功能。[依赖与 feature summary](../CMakeLists.txt)、[generator 条件](../generators/CMakeLists.txt)
- 初步调研时仓库没有通用 desktop cross-build toolchain 或 presets；后续新增了 [Linux Clang → Windows x64 toolchain](../cmake/toolchains/linux-clang-windows.cmake) 和 [`Makefile`](../Makefile) 的 `windows-cross` 入口。Android [脚本](../mobile/build-android-apk.sh) 将 `arm64-v8a`、plugin 文件名和 Linux x86_64 NDK host 工具路径写死，尚不是多 OS/architecture 的通用入口。
- 已有 [GitLab CI 配置](../.gitlab-ci.yml) 引用 Linux、Windows、Android、macOS x86_64/ARM64 等模板。这说明项目已有多平台构建配置，不能据此认定所有 target 都从 Linux cross-build，或本 fork 当前的每个 job 都通过。
- 本 fork 的 annotation sidecar 使用 `QSQLITE`；打包时还需 target SQLite driver plugin。desktop AI renderer 则另需 WebEngine helper/resources。源码能链接成功与完整应用能运行应分别验证。[sidecar 实现](../core/annotationsidecar.cpp)、[Qt WebEngine deployment](https://doc.qt.io/qt-6.10/qtwebengine-deploying.html)

## 本地 configure 探测

环境为 WSL2 Linux x86_64，CMake 3.28.3、native GCC 13.3。探测使用独立 `/tmp` build directories，未修改既有 build tree。每次都设置 `-DOKULAR_UI=desktop -DBUILD_TESTING=OFF`。

| 探测 | 实际结果 | 能证明的范围 |
| --- | --- | --- |
| native Linux configure | 第 18 行找不到 ECM >= 5.240 的 package config | 当前 Linux 环境尚未具备基本 KDE build dependencies |
| Linux ARM64 configure | `CMAKE_SYSTEM_NAME=Linux`、`CMAKE_SYSTEM_PROCESSOR=aarch64`；找不到 `aarch64-linux-gnu-gcc/g++` | 缺 cross compiler，尚未进入 Qt/KF6 查找或源码编译 |
| Windows x64 MinGW configure | `CMAKE_SYSTEM_NAME=Windows`、`CMAKE_SYSTEM_PROCESSOR=AMD64`；找不到 `x86_64-w64-mingw32-gcc/g++` | 缺 cross compiler；WebEngine 的 MinGW 限制来自官方文档与源码审查，未在本机实际触发 |
| macOS | 未执行 configure | 本机没有准备 macOS SDK 或 target dependency stack |

Linux 标准安装路径及当前 `pkg-config` 查找中也未找到 Qt6 / Poppler Qt6 development packages；`QT_HOST_PATH`、`CMAKE_PREFIX_PATH` 未设置。完整编译、GUI smoke tests 和 autotests 均未运行，没有生成 target binaries。

另发现现有 `D:/CraftRoot-Okular`：Qt Core/WebEngineWidgets 6.11.1 DLL 为 Windows x86_64 PE，`qt.toolchain.cmake` 使用 `win32-msvc`；主 checkout 的 build cache 使用 Visual Studio 17 2022。它是已有 Windows native SDK/build 配置，不能直接作为 Linux ARM64 sysroot，也不能和 MinGW C++ 依赖混用。此处仅检查配置和文件格式，未重建该目录。

## 建议实施顺序

1. 先准备 Linux ARM64 compiler、固定版本 sysroot 和 target Qt/KF6/Poppler；同时提供匹配的 Linux host Qt tools 与 `KF6_HOST_TOOLING`。将外部依赖准备与 Okular configure/build 分开，形成可复现入口，并确保 CMake 与 `pkg-config` 查找不会混入 host libraries。
2. 若希望保留完整 AI 功能，Windows 优先沿用 Windows + MSVC/Craft 构建，macOS 使用 macOS + Xcode/Craft 构建。在 macOS 上继续支持 Intel/ARM64；这属于多平台构建方案，不能称作全部由 Linux cross-build。
3. 若 Linux → Windows MinGW 是硬要求，先让 desktop AI/WebEngine 真正可选或替换 renderer，再准备 MinGW ABI 的整套 Qt/KF6/backends。解除 WebEngine 限制只是必要的一步，不保证整个依赖栈自动构建成功。
4. 每个 target 先验证编译、安装和 plugin packaging，再在目标环境验证启动、PDF 打开/渲染、annotation sidecar 与 AI renderer，并运行适用的 autotests。
