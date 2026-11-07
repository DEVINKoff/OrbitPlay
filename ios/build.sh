#!/bin/bash
# Builds Orbit Play for iPad (arm64, iPadOS 16+) on macOS with Xcode installed.
#   ios/build.sh            dependencies (cached in ios/deps) + app -> build-ios/OrbitPlay.ipa (unsigned)
#   ios/build.sh deps       only the dependencies
# The .ipa is not signed: sign/install it with your own sideloading tool.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/ios/deps"          # sources + build trees
P="$DEPS/prefix"               # installed static libraries
MIN_IOS=16.0
JOBS=$(sysctl -n hw.ncpu)
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$DEPS" "$P"
cd "$DEPS"

export PKG_CONFIG_LIBDIR="$P/lib/pkgconfig"
unset PKG_CONFIG_PATH

CMAKE_IOS=(-G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS
	-DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P"
	-DCMAKE_FIND_ROOT_PATH="$P" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO)

step() { echo; echo "==== $*"; }
done_marker() { [ -f "$P/.built-$1" ]; }
mark() { touch "$P/.built-$1"; }
cmk() { # name src [cmake args...]
	local name=$1 src=$2; shift 2
	done_marker "$name" && { echo "($name cached)"; return; }
	step "$name"
	cmake -S "$src" -B "$src/build-ios" "${CMAKE_IOS[@]}" "$@"
	cmake --build "$src/build-ios" -j "$JOBS"
	cmake --install "$src/build-ios"
	mark "$name"
}
fetch() { [ -d "$2" ] || git clone -q --depth 1 -b "$3" "$1" "$2"; }

# ---- SDL3 (controllers incl. DualSense, audio, window, touch)
fetch https://github.com/libsdl-org/SDL.git SDL release-3.2.24
cmk sdl3 SDL -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF

# ---- Opus, json-c, miniupnpc, libevent (chiaki-ng)
fetch https://github.com/xiph/opus.git opus v1.5.2
cmk opus opus -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
fetch https://github.com/json-c/json-c.git json-c json-c-0.18-20240915
cmk json-c json-c -DBUILD_TESTING=OFF -DDISABLE_EXTRA_LIBS=ON -DBUILD_APPS=OFF
fetch https://github.com/miniupnp/miniupnp.git miniupnp miniupnpc_2_2_8
cmk miniupnpc miniupnp/miniupnpc -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF
fetch https://github.com/libevent/libevent.git libevent release-2.1.12-stable
cmk libevent libevent -DEVENT__LIBRARY_TYPE=STATIC -DEVENT__DISABLE_OPENSSL=ON -DEVENT__DISABLE_TESTS=ON \
	-DEVENT__DISABLE_SAMPLES=ON -DEVENT__DISABLE_BENCHMARK=ON -DEVENT__DISABLE_REGRESS=ON -DEVENT__DISABLE_MBEDTLS=ON

# ---- OpenSSL (chiaki-ng crypto, curl)
fetch https://github.com/openssl/openssl.git openssl openssl-3.3.2
if ! done_marker openssl; then
	step openssl
	( cd openssl
	  export CROSS_TOP="$(xcrun --sdk iphoneos --show-sdk-platform-path)/Developer" CROSS_SDK="$(basename "$SDK")"
	  export CC="$(xcrun --sdk iphoneos -f clang)"
	  ./Configure ios64-cross no-shared no-tests no-docs no-apps -mios-version-min=$MIN_IOS --prefix="$P" --libdir=lib
	  make -j "$JOBS" build_libs
	  make install_dev )
	mark openssl
fi

# ---- FFmpeg: H.264/HEVC through VideoToolbox (hardware), AAC + MP4 for recording
fetch https://github.com/FFmpeg/FFmpeg.git ffmpeg n7.1
if ! done_marker ffmpeg; then
	step ffmpeg
	( cd ffmpeg
	  ./configure --prefix="$P" --enable-cross-compile --target-os=darwin --arch=arm64 \
		--cc="$(xcrun --sdk iphoneos -f clang)" --sysroot="$SDK" \
		--extra-cflags="-arch arm64 -mios-version-min=$MIN_IOS" --extra-ldflags="-arch arm64 -mios-version-min=$MIN_IOS" \
		--enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-debug \
		--disable-everything --disable-avdevice --disable-avfilter --disable-postproc \
		--enable-videotoolbox --enable-hwaccel=h264_videotoolbox --enable-hwaccel=hevc_videotoolbox \
		--enable-decoder=h264 --enable-decoder=hevc --enable-parser=h264 --enable-parser=hevc \
		--enable-encoder=aac --enable-muxer=mp4 --enable-muxer=mov --enable-protocol=file \
		--disable-audiotoolbox --disable-securetransport --disable-iconv
	  make -j "$JOBS"
	  make install )
	mark ffmpeg
fi

[ "${1:-}" = "deps" ] && { echo "Dependencies ready in $P"; exit 0; }

# ---- the app
step "Orbit Play"
B="$ROOT/build-ios"
cmake -S "$ROOT" -B "$B" "${CMAKE_IOS[@]}" -DCMAKE_INSTALL_PREFIX="$B/install" \
	-DCHIAKI_LIB_ENABLE_MBEDTLS=OFF -DCHIAKI_ENABLE_RUDP=OFF -DOPENSSL_ROOT_DIR="$P" -DOPENSSL_USE_STATIC_LIBS=ON \
	-DSDL3_DIR="$P/lib/cmake/SDL3"
cmake --build "$B" -j "$JOBS" --target OrbitPlay

# ---- package: Payload/OrbitPlay.app -> OrbitPlay.ipa (unsigned)
APP="$(find "$B" -maxdepth 2 -name 'OrbitPlay.app' -type d | head -1)"
[ -n "$APP" ] || { echo "OrbitPlay.app not found"; exit 1; }
rm -rf "$B/Payload" "$B/OrbitPlay.ipa"
mkdir -p "$B/Payload"
cp -R "$APP" "$B/Payload/"
( cd "$B" && zip -qry OrbitPlay.ipa Payload )
echo
echo "Built $B/OrbitPlay.ipa ($(du -h "$B/OrbitPlay.ipa" | cut -f1))"
