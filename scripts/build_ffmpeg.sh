#!/usr/bin/env bash
#
# Builds a minimal LGPL FFmpeg (shared libs) for Android using the NDK.
# Output: app/src/main/cpp/ffmpeg/include and app/src/main/cpp/ffmpeg/lib/<abi>/*.so
#
# Works on Linux/macOS and on Windows under Git Bash (uses the NDK's bundled make).
#
# Usage: scripts/build_ffmpeg.sh [abi ...]      (default: arm64-v8a x86_64)
#   Env: ANDROID_NDK_HOME   path to NDK (default: newest in $ANDROID_SDK/ndk)
#        API                min SDK level (default: 24)
#        JOBS               parallel make jobs (default: nproc)

set -euo pipefail

FFMPEG_VERSION="8.1.3"
API="${API:-24}"
ABIS=("${@:-arm64-v8a x86_64}")
# shellcheck disable=SC2206
ABIS=(${ABIS[*]})

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Git Bash: the NDK's make.exe is a native Windows program and can't read /c/... paths.
if command -v cygpath >/dev/null 2>&1; then
    ROOT="$(cygpath -m "$ROOT")"
fi
WORK="$ROOT/.ffmpeg-build"
OUT="$ROOT/app/src/main/cpp/ffmpeg"

# --- Locate the NDK -----------------------------------------------------------
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    if [[ -z "$SDK" ]]; then
        case "$(uname -s)" in
            MINGW*|MSYS*) SDK="$(cygpath -u "$LOCALAPPDATA")/Android/Sdk" ;;
            Darwin)       SDK="$HOME/Library/Android/sdk" ;;
            *)            SDK="$HOME/Android/Sdk" ;;
        esac
    fi
    ANDROID_NDK_HOME="$SDK/ndk/$(ls "$SDK/ndk" | sort -V | tail -1)"
fi
# Git Bash: env vars may hold Windows paths with backslashes; normalise to C:/... form.
if command -v cygpath >/dev/null 2>&1; then
    ANDROID_NDK_HOME="$(cygpath -m "$ANDROID_NDK_HOME")"
fi
echo "Using NDK: $ANDROID_NDK_HOME"

case "$(uname -s)" in
    MINGW*|MSYS*) HOST_TAG="windows-x86_64"; EXE=".exe" ;;
    Darwin)       HOST_TAG="darwin-x86_64";  EXE="" ;;
    *)            HOST_TAG="linux-x86_64";   EXE="" ;;
esac

TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG"
SYSROOT="$TOOLCHAIN/sysroot"

# Use the NDK's make on Windows (Git Bash doesn't ship one).
MAKE="make"
if ! command -v make >/dev/null 2>&1; then
    MAKE="$ANDROID_NDK_HOME/prebuilt/$HOST_TAG/bin/make$EXE"
fi
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# configure needs a POSIX temp dir (Windows %TEMP% breaks it under Git Bash).
export TMPDIR="$WORK/tmp"
mkdir -p "$TMPDIR"

# --- Fetch source --------------------------------------------------------------
mkdir -p "$WORK"
SRC="$WORK/ffmpeg-$FFMPEG_VERSION"
if [[ ! -d "$SRC" ]]; then
    TARBALL="$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz"
    [[ -f "$TARBALL" ]] || curl -fL -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    tar -xJf "$TARBALL" -C "$WORK"
fi

LIBS=(avutil avcodec avformat swscale)

# --- Build per ABI -------------------------------------------------------------
build_abi() {
    local abi="$1" arch cpu triple extra=()
    case "$abi" in
        arm64-v8a)   arch=aarch64; cpu=armv8-a; triple=aarch64-linux-android ;;
        armeabi-v7a) arch=arm;     cpu=armv7-a; triple=armv7a-linux-androideabi
                     extra+=(--enable-neon) ;;
        x86_64)      arch=x86_64;  cpu=x86-64;  triple=x86_64-linux-android
                     extra+=(--disable-x86asm) ;;   # x86 asm needs nasm; not in the NDK
        x86)         arch=x86;     cpu=i686;    triple=i686-linux-android
                     extra+=(--disable-x86asm) ;;
        *) echo "Unknown ABI: $abi" >&2; exit 1 ;;
    esac

    local build="$WORK/build-$abi"
    local prefix="$WORK/install-$abi"
    rm -rf "$build" "$prefix"
    mkdir -p "$build"
    cd "$build"

    # --host-cc: configure insists on a working host C compiler, but this minimal build never
    # runs host programs, so the target compiler (which has libc headers) is good enough.
    echo "=== Configuring FFmpeg $FFMPEG_VERSION for $abi ==="
    "$SRC/configure" \
        --prefix="$prefix" \
        --target-os=android \
        --arch="$arch" \
        --cpu="$cpu" \
        --enable-cross-compile \
        --sysroot="$SYSROOT" \
        --cc="$TOOLCHAIN/bin/${triple}${API}-clang" \
        --cxx="$TOOLCHAIN/bin/${triple}${API}-clang++" \
        --ar="$TOOLCHAIN/bin/llvm-ar$EXE" \
        --nm="$TOOLCHAIN/bin/llvm-nm$EXE" \
        --ranlib="$TOOLCHAIN/bin/llvm-ranlib$EXE" \
        --strip="$TOOLCHAIN/bin/llvm-strip$EXE" \
        --host-cc="$TOOLCHAIN/bin/${triple}${API}-clang" \
        --enable-shared --disable-static \
        --enable-pic \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-swresample \
        --disable-network --disable-everything \
        --enable-jni --enable-mediacodec \
        --enable-protocol=file \
        --enable-demuxer=mov \
        --enable-muxer=mp4 \
        --enable-decoder=h264,hevc \
        --enable-encoder=h264_mediacodec \
        --enable-parser=h264,hevc,aac \
        --enable-bsf=extract_extradata,h264_metadata,h264_mp4toannexb,hevc_mp4toannexb \
        "${extra[@]}"

    # Git Bash: configure records its own /c/... source path; rewrite it for the native make.exe.
    if command -v cygpath >/dev/null 2>&1; then
        local posix_src
        posix_src="$(cygpath -u "$SRC")"
        sed -i "s|$posix_src|$SRC|g" Makefile ffbuild/config.mak
    fi

    echo "=== Building FFmpeg for $abi ==="
    "$MAKE" -j"$JOBS"
    # Headers are copied by copy_headers: on Windows, make's header install command exceeds
    # the command-line length limit and silently installs nothing.
    "$MAKE" install-libs

    mkdir -p "$OUT/lib/$abi"
    for lib in "${LIBS[@]}"; do
        cp "$prefix/lib/lib$lib.so" "$OUT/lib/$abi/"
    done
    # Public headers are the same for all little-endian Android ABIs; keep one copy.
    copy_headers "$OUT/include"
}

# Copies each library's public headers (HEADERS/ARCH_HEADERS from the source Makefile,
# BUILT_HEADERS from the current build dir) - the same set `make install-headers` installs.
copy_headers() {
    local dest="$1" lib var h
    rm -rf "$dest"
    for lib in "${LIBS[@]}"; do
        mkdir -p "$dest/lib$lib"
        for var in HEADERS ARCH_HEADERS BUILT_HEADERS; do
            # Join backslash-continued lines, then take the value of "$var = ...".
            for h in $(sed -e ':a' -e '/\\$/N; s/\\\n//; ta' "$SRC/lib$lib/Makefile" \
                       | sed -n "s/^$var[[:space:]]*=//p"); do
                if [[ "$var" == BUILT_HEADERS ]]; then
                    cp "lib$lib/$h" "$dest/lib$lib/"
                else
                    cp "$SRC/lib$lib/$h" "$dest/lib$lib/"
                fi
            done
        done
    done
}

for abi in "${ABIS[@]}"; do
    build_abi "$abi"
done

echo "Done. Libraries in $OUT/lib, headers in $OUT/include"
