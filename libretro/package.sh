#!/usr/bin/env bash
# Packs the three Libretro cores of a build for one platform, with their
# RetroArch information files: cores/ and info/ at the root of the archive.
#   libretro/package.sh BUILD_DIR PLATFORM NAME
# PLATFORM is linux-*, windows-*, macos-* or android-*; the archive is
# NAME.tar.gz on Linux, NAME.zip elsewhere, in the current directory.
set -euo pipefail
build=$1 platform=$2 name=$3
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/cores" "$stage/info"
cp "$build"/libretro/info/*_libretro.info "$stage/info/"
case "$platform" in
    linux-*) cp "$build"/libretro/*_libretro.so "$stage/cores/" ;;
    # Visual Studio is a multi-config generator.
    windows-*) cp "$build"/libretro/Release/*_libretro.dll "$stage/cores/" ;;
    macos-*) cp "$build"/libretro/*_libretro.dylib "$stage/cores/" ;;
    # RetroArch for Android names its cores *_libretro_android.so.
    android-*)
        for core in "$build"/libretro/*_libretro.so; do
            cp "$core" "$stage/cores/$(basename "$core" .so)_android.so"
        done ;;
    *) echo "Unknown platform: $platform" >&2; exit 1 ;;
esac
count=$(find "$stage/cores" -type f | wc -l)
if [ "$count" -ne 3 ]; then
    echo "Expected 3 cores for $platform, found $count" >&2; exit 1
fi
case "$platform" in
    linux-*) tar -C "$stage" -czf "$name.tar.gz" cores info ;;
    *)
        archive="$PWD/$name.zip"
        if command -v zip >/dev/null; then (cd "$stage" && zip -qr "$archive" cores info)
        else (cd "$stage" && 7z a -tzip -bd "$archive" cores info >/dev/null)
        fi ;;
esac
