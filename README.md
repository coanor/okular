# Okular – Universal Document Viewer

Okular can view and annotate documents of various formats, including PDF, Postscript, Comic Book, and various image formats.
It supports native PDF annotations.

### Downloads

For download and installation instructions, see https://okular.kde.org/download.php

### User manual

https://docs.kde.org/?application=okular&branch=stable5

### Bugs

https://bugs.kde.org/buglist.cgi?product=okular

Please report bugs on Bugzilla (https://bugs.kde.org/enter_bug.cgi?product=okular), and not on our GitLab instance (https://invent.kde.org).

### Mailing list

https://mail.kde.org/mailman/listinfo/okular-devel

### Source code

https://invent.kde.org/graphics/okular.git

The Okular repository contains the source code for:
 * the `okular` desktop application (the “shell”),
 * the `okularpart` KParts plugin,
 * the `okularkirigami` mobile application,
 * several `okularGenerator_xyz` plugins, which provide backends for different document types.

### Apidox

https://api.kde.org/okular/html/index.html

## Contributing

Okular uses the merge request workflow.
Merge requests are required to run pre-commit CI jobs; please don’t push to the master branch directly.
See https://community.kde.org/Infrastructure/GitLab for an introduction.

### Build instructions

Okular can be built like many other applications developed by KDE.
See https://community.kde.org/Get_Involved/development for an introduction.

If your build environment is set up correctly, you can also build Okular using CMake:

```bash
git clone https://invent.kde.org/graphics/okular.git
cd okular
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/path/to/your/install/dir ..
make
make install
```

Okular also builds tests in the build tree. To run them, you have to run `make install` first.

If you install Okular in a different path than your system install directory it is possible that you need to run

```bash
source prefix.sh
```

so that the correct Okular instance and libraries are picked up.
Afterwards one can run `okular` inside the shell instance.
The source command is also required to run the tests manually.

As stated above, Okular has various build targets.
Two of them are executables.
You can choose which executable to build by passing a flag to CMake:

```bash
cmake -DCMAKE_INSTALL_PREFIX=/path/to/your/install/dir -DOKULAR_UI=desktop ..
```
Available options are `desktop`, `mobile`, and `both`.

### Built-in dictionary

Desktop and mobile builds include the bundled MDX parser and the
[ECDICT 1.0.28 English-Chinese dictionary](https://github.com/skywind3000/ECDICT/releases/tag/1.0.28)
by default. CMake downloads the pinned archive (about 93 MiB) and verifies its
SHA-256 checksum. The dictionary and its MIT license are embedded in the package;
word lookup works offline after installation. Enable automatic word lookup in
settings to show definitions when selecting a word.

The built-in dictionary is selected for new configurations. Existing custom MDX
paths and explicit Eudic selections are preserved. On mobile, use **Use built-in**
to restore ECDICT or **Use Eudic** to switch to Eudic. On desktop, restore the
General settings defaults to select ECDICT again.

For an offline build, pass `-DOKULAR_DICTIONARY_FILE=/path/to/ecdict.mdx`.
Set `-DOKULAR_BUNDLE_DICTIONARY=OFF` to omit the dictionary, or
`-DOKULAR_USE_BUNDLED_MDICT=OFF` to use a system mdict-cpp installation.
See [thirdparty/ecdict/README.okular](thirdparty/ecdict/README.okular) for attribution.

### Android APK

With Docker, an initialized Android Craft root next to the main checkout (`craft-kde-android`), Android SDK build tools 36.0.0, and an Android debug keystore, build and sign the arm64 APK with:

```bash
make android-apk
```

The target loads `~/.local/share/okular-android/environment.sh` when it exists
and uses `sg docker` if the current shell lacks access to the Docker socket.
Set `ANDROID_ENV_FILE=/path/to/environment.sh` to use another environment file.

The APK is written to `build-android/okular-mobile-arm64-selection-debug.apk`. Set `CRAFT_ROOT`, `ANDROID_SDK_ROOT`, `ANDROID_BUILD_TOOLS_DIR`, or `ANDROID_KEYSTORE` if they are in other locations. See [KDE's Android Craft setup guide](https://develop.kde.org/docs/packaging/android/building_applications/) for the initial Craft setup.

The build forwards HTTP proxy environment variables to the container. Set
`ANDROID_DOCKER_NETWORK=host` when the proxy listens on the WSL host's loopback
address. Gradle uses Java proxy settings from
`$CRAFT_ROOT/gradle-home/gradle.properties`.

### clang-format

The Okular project uses clang-format to enforce source code formatting.
See [README.clang_format](https://invent.kde.org/graphics/okular/-/blob/master/README.clang-format) for more information.
