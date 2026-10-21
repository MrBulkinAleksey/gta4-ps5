#!/usr/bin/env bash
# Fetches the third-party code the PS5 build uses (git submodules) and applies
# the PS5's patches to FFmpeg and libmspack. Build steps: ps5/BUILD.md.
#
#   ./setup.sh
set -euo pipefail
cd "$(dirname "$0")"
git -c submodule.recurse=false submodule update --init --depth 1 --jobs 4 \
  $(grep "path = glue/rexglue-sdk-main/thirdparty" .gitmodules | awk '{print $3}') tools/XenosRecomp
git -C tools/XenosRecomp submodule update --init --depth 1 thirdparty/smol-v thirdparty/zstd thirdparty/xxHash
git -C glue/rexglue-sdk-main/thirdparty/FFmpeg apply ../../patches/ffmpeg-ps5.patch
git -C glue/rexglue-sdk-main/thirdparty/libmspack apply --ignore-space-change \
  "$PWD/cmake/dependency-patches/libmspack-xmem-block-padding.patch"
echo "Ready (build steps: ps5/BUILD.md)"
