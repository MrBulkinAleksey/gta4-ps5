#!/usr/bin/env bash
# Gets Liberty Recompiled for Android at the commit this port is built on,
# applies the PS5 port, and fetches the dependencies the PS5 build uses.
#
#   ./setup.sh [folder]   (default: ./LibertyRecompAndroid)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(realpath -m "${1:-$here/LibertyRecompAndroid}")
base=bc5e88607636a96f7c4ab6244b6f96e215fc384d

if [ ! -d "$src/.git" ]; then
  git init -q "$src"
  git -C "$src" remote add origin https://github.com/vaduur/LibertyRecompAndroid.git
fi
git -C "$src" fetch --depth 1 origin "$base"
git -C "$src" checkout -q -B ps5 FETCH_HEAD
git -C "$src" apply "$here/patches/liberty-ps5.patch"
rm -rf "$src/ps5"
cp -r "$here/ps5" "$src/ps5"

cd "$src"
git -c submodule.recurse=false submodule update --init --depth 1 --jobs 4 \
  $(grep "path = glue/rexglue-sdk-main/thirdparty" .gitmodules | awk '{print $3}') tools/XenosRecomp
git -C tools/XenosRecomp submodule update --init --depth 1 thirdparty/smol-v thirdparty/zstd thirdparty/xxHash
git -C glue/rexglue-sdk-main/thirdparty/FFmpeg apply ../../patches/ffmpeg-ps5.patch
git -C glue/rexglue-sdk-main/thirdparty/libmspack apply --ignore-space-change \
  "$src/cmake/dependency-patches/libmspack-xmem-block-padding.patch"
echo "Ready: $src (build steps: ps5/BUILD.md)"
