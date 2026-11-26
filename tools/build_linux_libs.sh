#!/usr/bin/env bash
# Builds the runtime's video and audio libraries for Linux x86_64 from their pinned upstream sources:
#   OpenH264 2.4.1 (BSD 2-clause)  -> runtime/oh264/lin/libopenh264.a   (game movies, NVDEC H.264)
#   Opus 1.5.2     (BSD 3-clause)  -> runtime/opus/lin/libopus.a        (hwopus voices)
# Same versions as the Windows libraries in runtime/*/win. Needs: git, make, a C/C++ compiler, cmake, nasm (optional).
#   tools/build_linux_libs.sh            (run from the repository root or anywhere)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/oj-linux-libs"
mkdir -p "$WORK" "$ROOT/runtime/oh264/lin" "$ROOT/runtime/opus/lin"

clone() {  # repo tag commit dir: shallow clone of a tag, then check the commit it points at
  local repo=$1 tag=$2 commit=$3 dir=$4
  [ -d "$dir/.git" ] || git clone -q --depth 1 --branch "$tag" "$repo" "$dir"
  local got; got=$(git -C "$dir" rev-parse HEAD)
  if [ "$got" != "$commit" ]; then echo "$repo $tag is $got, expected $commit" >&2; exit 1; fi
}

if [ ! -f "$ROOT/runtime/oh264/lin/libopenh264.a" ]; then
  clone https://github.com/cisco/openh264.git v2.4.1 "${OH264_COMMIT:-c59550a2147c255cc8e09451f6deb96de2526b6d}" "$WORK/openh264"
  asm=Yes; command -v nasm >/dev/null || asm=No
  make -C "$WORK/openh264" -j"$(nproc)" OS=linux ARCH=x86_64 USE_ASM=$asm CFLAGS_OPT=-O2 libopenh264.a >/dev/null
  cp "$WORK/openh264/libopenh264.a" "$ROOT/runtime/oh264/lin/"
  echo "OpenH264 2.4.1 -> runtime/oh264/lin/libopenh264.a (asm: $asm)"
fi

if [ ! -f "$ROOT/runtime/opus/lin/libopus.a" ]; then
  clone https://github.com/xiph/opus.git v1.5.2 "${OPUS_COMMIT:-ddbe48383984d56acd9e1ab6a090c54ca6b735a6}" "$WORK/opus"
  cmake -S "$WORK/opus" -B "$WORK/opus/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
  cmake --build "$WORK/opus/build" -j"$(nproc)" >/dev/null
  cp "$WORK/opus/build/libopus.a" "$ROOT/runtime/opus/lin/"
  echo "Opus 1.5.2 -> runtime/opus/lin/libopus.a"
fi
