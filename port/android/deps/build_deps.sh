#!/bin/sh
# Opus v1.6.1 and libvpx v1.16.0 (the versions MacNativeGames' iOS build pins) as static arm64 Android libraries, and
# zstd v1.5.7 (BSD) for the startup screen's mod installer (libtotkmods.so). Capstone 5.0.9 (BSD) is only fetched here:
# the code rewriter (port/android/setup/patcher) compiles what it needs of it. So is the shared-font stand-in (tools/fetch_font.sh).
# usage: port/android/deps/build_deps.sh -> port/android/deps/out/{lib,include}
set -e
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/out; SRC=$HERE/src; B=$HERE/build
. "$HERE/../tools/env.sh"; API=35; J=$(nproc)
TC=$NDK_BIN
mkdir -p $SRC $B $OUT/lib $OUT/include
[ -d $SRC/opus/.git ] || git clone -q --depth 1 --branch v1.6.1 https://github.com/xiph/opus.git $SRC/opus
[ -d $SRC/zstd/.git ] || git clone -q --depth 1 --branch v1.5.7 https://github.com/facebook/zstd.git $SRC/zstd
[ -d $SRC/libvpx/.git ] || git clone -q --depth 1 --branch v1.16.0 https://chromium.googlesource.com/webm/libvpx.git $SRC/libvpx
sh "$HERE/../setup/patcher/fetch_capstone.sh" $SRC/capstone
sh "$HERE/../tools/fetch_font.sh"
(
cmake -G Ninja -S $SRC/opus -B $B/opus -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
  -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF > $B/opus-configure.log
cmake --build $B/opus -j $J > $B/opus-build.log
cp $B/opus/libopus.a $OUT/lib/ && cp -r $SRC/opus/include/. $OUT/include/
echo "opus ok"
) &
(
mkdir -p $B/libvpx && cd $B/libvpx
CC=$TC/aarch64-linux-android$API-clang CXX=$TC/aarch64-linux-android$API-clang++ AR=$TC/llvm-ar AS=$TC/aarch64-linux-android$API-clang \
  LD=$TC/aarch64-linux-android$API-clang STRIP=$TC/llvm-strip NM=$TC/llvm-nm \
  $SRC/libvpx/configure --target=arm64-android-gcc --disable-examples --disable-tools --disable-docs --disable-unit-tests \
  --disable-webm-io --enable-pic --disable-shared --enable-static --disable-vp8-encoder --disable-vp9-encoder > $B/vpx-configure.log
make -j $J libvpx.a > $B/vpx-build.log
cp libvpx.a $OUT/lib/ && mkdir -p $OUT/include/vpx && cp -r $SRC/libvpx/vpx/. $OUT/include/vpx/ && cp vpx_config.h $OUT/include/
echo "libvpx ok"
) &
(
cmake -G Ninja -S $SRC/zstd/build/cmake -B $B/zstd -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF \
  -DZSTD_LEGACY_SUPPORT=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON > $B/zstd-configure.log
cmake --build $B/zstd -j $J > $B/zstd-build.log
cp $B/zstd/lib/libzstd.a $OUT/lib/ && cp $SRC/zstd/lib/zstd.h $SRC/zstd/lib/zdict.h $SRC/zstd/lib/zstd_errors.h $OUT/include/
echo "zstd ok"
) &
wait
for l in libopus.a libvpx.a libzstd.a; do [ -f $OUT/lib/$l ] || { echo "$l was not built (logs in $B)" >&2; exit 1; }; done   # the three run in the background
ls -la $OUT/lib
