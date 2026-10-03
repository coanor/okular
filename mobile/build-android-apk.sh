#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
git_dir=$(git -C "$project_dir" rev-parse --path-format=absolute --git-common-dir)
source_repo=$(dirname "$git_dir")
craft_root=${CRAFT_ROOT:-"$(dirname "$source_repo")/craft-kde-android"}
sdk_root=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-"$HOME/.local/android-sdk"}}
build_tools=${ANDROID_BUILD_TOOLS_DIR:-"$sdk_root/build-tools/36.0.0"}
keystore=${ANDROID_KEYSTORE:-"$HOME/.android/debug.keystore"}
image=${ANDROID_CRAFT_IMAGE:-invent-registry.kde.org/sysadmin/ci-images/android-qt611}
output_dir="$project_dir/build-android"
output_apk="$output_dir/okular-mobile-arm64-selection-debug.apk"
unsigned_apk="$craft_root/tmp/okularkirigami-arm64-v8a.apk"
craft_work_key=$(printf '%s' "$project_dir" | git -C "$project_dir" hash-object --stdin)
craft_work_dir="$craft_root/build/okular-worktrees/${craft_work_key:0:16}/work"

if [[ ! -f "$craft_root/craft/craftenv.sh" ]]; then
    echo "Craft is not initialized at $craft_root (set CRAFT_ROOT to its location)." >&2
    exit 1
fi
for tool in "$build_tools/zipalign" "$build_tools/apksigner"; do
    if [[ ! -x "$tool" ]]; then
        echo "Missing Android build tool: $tool (set ANDROID_SDK_ROOT or ANDROID_BUILD_TOOLS_DIR)." >&2
        exit 1
    fi
done
if [[ ! -f "$keystore" ]]; then
    echo "Missing Android debug keystore: $keystore (set ANDROID_KEYSTORE to its location)." >&2
    exit 1
fi

mkdir -p "$craft_work_dir"
docker_mounts=(-v "$craft_root:/home/user/CraftRoot" -v "$craft_work_dir:/home/user/CraftRoot/build/kde/applications/okular/work" -v "$source_repo:/home/user/okular-repo" -v "$source_repo:$source_repo")
case "$project_dir" in
    "$source_repo") container_source=/home/user/okular-repo ;;
    "$source_repo"/*) container_source="/home/user/okular-repo${project_dir#"$source_repo"}" ;;
    *)
        container_source=/home/user/okular-worktree
        docker_mounts+=(-v "$project_dir:$container_source")
        ;;
esac

docker run --rm "${docker_mounts[@]}" \
    -e GRADLE_USER_HOME=/home/user/CraftRoot/gradle-home \
    -e "OKULAR_SOURCE_DIR=$container_source" \
    "$image" bash -euo pipefail -c '
        set +u
        source /home/user/CraftRoot/craft/craftenv.sh
        set -u
        # Fastlane requires a tracking remote for unpublished topic branches.
        if ! git -C "$OKULAR_SOURCE_DIR" rev-parse --verify "@{upstream}" >/dev/null 2>&1; then
            build_branch=$(git -C "$OKULAR_SOURCE_DIR" symbolic-ref --short HEAD)
            export GIT_CONFIG_COUNT=2
            export GIT_CONFIG_KEY_0="branch.$build_branch.remote" GIT_CONFIG_VALUE_0=origin
            export GIT_CONFIG_KEY_1="branch.$build_branch.merge" GIT_CONFIG_VALUE_1=refs/heads/master
        fi
        craft --options "okular.srcDir=$OKULAR_SOURCE_DIR" okular
        craft --options "okular.srcDir=$OKULAR_SOURCE_DIR" --configure okular

        build_dir=/home/user/CraftRoot/build/kde/applications/okular/work/build
        ninja -C "$build_dir" install
        craft --options "okular.srcDir=$OKULAR_SOURCE_DIR" --package okular

        plugin=libqml_org_kde_okular_okularplugin_arm64-v8a.so
        archive=/home/user/CraftRoot/build/kde/applications/okular/archive/lib/qml/org/kde/okular
        ninja -C "$build_dir" okularplugin
        cp "$build_dir/bin/org/kde/okular/$plugin" "$archive/$plugin"
        strip_tool=$(find /opt/android-sdk/ndk -path "*/linux-x86_64/bin/llvm-strip" -print -quit)
        if [[ -n "$strip_tool" ]]; then
            "$strip_tool" "$archive/$plugin"
        fi
        apk_dir="$build_dir/okularkirigami_build_apk"
        cp "$archive/$plugin" "$apk_dir/libs/arm64-v8a/$plugin"
        (cd "$apk_dir" && ./gradlew --offline assembleRelease)
        cp "$apk_dir/build/outputs/apk/release/okularkirigami_build_apk-release-unsigned.apk" \
            /home/user/CraftRoot/tmp/okularkirigami-arm64-v8a.apk
    '

if [[ ! -f "$unsigned_apk" ]]; then
    echo "Craft did not create $unsigned_apk." >&2
    exit 1
fi

mkdir -p "$output_dir"
temp_dir=$(mktemp -d "$output_dir/.android-apk.XXXXXXXX")
trap 'rm -rf -- "$temp_dir"' EXIT
"$build_tools/zipalign" -f -p 4 "$unsigned_apk" "$temp_dir/aligned.apk"
"$build_tools/apksigner" sign \
    --ks "$keystore" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android \
    --out "$temp_dir/signed.apk" "$temp_dir/aligned.apk"
"$build_tools/apksigner" verify "$temp_dir/signed.apk"
mv -- "$temp_dir/signed.apk" "$output_apk"
echo "Built $output_apk"
