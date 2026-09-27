#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(realpath "${1:-$repo_dir/build-own-static}")"
output_dir="${2:-$repo_dir/build-appimage}"
linuxdeploy="${LINUXDEPLOY:-linuxdeploy}"
appimagetool="${APPIMAGETOOL:-appimagetool}"
qmake="${QMAKE:-qmake6}"
arch="$(uname -m)"

if [[ "$arch" != x86_64 ]]; then
    echo "This AppImage recipe currently supports x86_64 only." >&2
    exit 1
fi
if [[ ! -x "$build_dir/bin/okular" ]]; then
    echo "Build the desktop okular target with BUILD_SHARED_LIBS=OFF first." >&2
    exit 1
fi

mkdir -p "$output_dir"
output_dir="$(realpath "$output_dir")"
output="$output_dir/Okular-bundled-$arch.AppImage"
if [[ -e "$output" ]]; then
    echo "Output already exists: $output" >&2
    exit 1
fi

appdir="$(mktemp -d "$output_dir/Okular.AppDir.XXXXXX")"
plugin_dir="$("$qmake" -query QT_INSTALL_PLUGINS)"
export APPIMAGE_EXTRACT_AND_RUN=1

"$linuxdeploy" \
    --appdir="$appdir" \
    --executable="$build_dir/bin/okular" \
    --desktop-file="$repo_dir/shell/org.kde.okular.desktop" \
    --icon-file="$repo_dir/icons/128-apps-okular.png" \
    --icon-filename=okular

plugin_paths=(
    platforms/libqxcb.so
    platforms/libqoffscreen.so
    imageformats/libqsvg.so
    imageformats/libqjpeg.so
    imageformats/libqgif.so
    imageformats/libqico.so
    imageformats/libqtiff.so
    imageformats/libqwebp.so
    iconengines/libqsvgicon.so
)
deploy_args=()
for plugin_path in "${plugin_paths[@]}"; do
    install -Dm755 "$plugin_dir/$plugin_path" "$appdir/usr/plugins/$plugin_path"
    deploy_args+=("--deploy-deps-only=$appdir/usr/plugins/$plugin_path")
done
"$linuxdeploy" --appdir="$appdir" "${deploy_args[@]}"

if [[ ! -L "$appdir/AppRun" ]]; then
    echo "Expected linuxdeploy to create an AppRun symlink." >&2
    exit 1
fi
unlink "$appdir/AppRun"
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
set -eu
appdir=$(CDPATH= cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$appdir/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$appdir/usr/plugins/platforms"
export XDG_DATA_DIRS="$appdir/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$appdir/usr/bin/okular" "$@"
APPRUN
chmod +x "$appdir/AppRun"
install -Dm644 "$repo_dir/shell/org.kde.okular.appdata.xml" "$appdir/usr/share/metainfo/org.kde.okular.appdata.xml"

ARCH="$arch" "$appimagetool" --no-appstream "$appdir" "$output"
QT_QPA_PLATFORM=offscreen "$output" --version
echo "$output"
