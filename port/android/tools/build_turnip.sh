#!/bin/sh
# Build Turnip (Mesa's Vulkan driver for Adreno) for the Odin 3's Adreno 830 from whitebelyash's turnip/gen8 branch (the
# base of StevenMXZ's "Turnip Gen8" packages) with our fix, into work/android/turnip/libvulkan_freedreno.so, which
# build_apk.sh bundles. The previous driver is kept in work/android/turnip/previous/.
#   port/android/tools/build_turnip.sh [--debug]   (--debug: debugoptimized, unstripped, ~70 MB)
# Fix (patches/turnip-a8xx-pvtmem-stack-pad.patch): on a8xx the last SP's hardware stack spills past the end of the
# private-memory BO and overwrites whatever is allocated next (LSFG's constant buffers: its first stage then computes
# from zeros and frame generation shows a scaled double image).
# Needs: git, python3 with venv, ninja, glslangValidator, bison, flex; the NDK below.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); W=$ROOT/work/turnip-build
. "$ROOT/port/android/tools/env.sh"; NDK=$NDK_BIN
REPO=https://github.com/whitebelyash/mesa-unified.git; BRANCH=turnip/gen8; COMMIT=9c7e022677dfa3abb356c2b6732cbd2e25783d01
TYPE=release; STRIP=true; [ "$1" = "--debug" ] && TYPE=debugoptimized && STRIP=false
mkdir -p "$W"
[ -x "$W/venv/bin/meson" ] || { python3 -m venv "$W/venv"; "$W/venv/bin/pip" -q install meson mako pyyaml packaging; }
if [ ! -d "$W/mesa/.git" ]; then git clone -q --filter=blob:none -b $BRANCH $REPO "$W/mesa"; fi
cd "$W/mesa"
git fetch -q origin $BRANCH || true
git checkout -q -f $COMMIT && git clean -qfd
# Android NDK r29 build fixes (as StevenMXZ's build_turnip.sh applies them)
sed -i 's/typedef const native_handle_t\* buffer_handle_t;/typedef void* buffer_handle_t;/g' include/android_stub/cutils/native_handle.h
sed -i 's/, hnd->handle/, (void *)hnd->handle/g' src/util/u_gralloc/u_gralloc_fallback.c
sed -i -E 's/([a-z_]+)->handle->/((const native_handle_t *)\1->handle)->/g' src/vulkan/runtime/vk_android.c
sed -i "/-Werror=gnu-empty-initializer/d" meson.build
git apply "$ROOT/patches/turnip-a8xx-pvtmem-stack-pad.patch"
cat > "$W/android-aarch64.txt" <<EOF
[binaries]
ar = '$NDK/llvm-ar'
c = ['$NDK/aarch64-linux-android35-clang']
cpp = ['$NDK/aarch64-linux-android35-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = '$NDK/ld.lld'
cpp_ld = '$NDK/ld.lld'
strip = '$NDK/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$NDK/pkg-config', '/usr/bin/pkg-config']

[built-in options]
c_args = ['-D__ANDROID__', '-Wno-error', '-Wno-gnu-empty-initializer', '-Wno-deprecated-declarations', '-Wno-incompatible-pointer-types-discards-qualifiers', '-Wno-incompatible-pointer-types']
cpp_args = ['-D__ANDROID__', '-Wno-error', '-Wno-gnu-empty-initializer', '-Wno-deprecated-declarations', '-Wno-incompatible-pointer-types-discards-qualifiers', '-Wno-incompatible-pointer-types']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
B="$W/build-$TYPE"
export PATH="$W/venv/bin:$PATH"
[ -f "$B/build.ninja" ] || meson setup "$B" --cross-file "$W/android-aarch64.txt" -Dbuildtype=$TYPE -Dstrip=$STRIP \
    -Dplatforms=android -Dvideo-codecs= -Dplatform-sdk-version=35 -Dandroid-stub=true -Dgallium-drivers= \
    -Dvulkan-drivers=freedreno -Dvulkan-beta=true -Dfreedreno-kmds=kgsl -Degl=disabled -Dandroid-libbacktrace=disabled
ninja -C "$B" -j"$(nproc)" src/freedreno/vulkan/libvulkan_freedreno.so
OUT=$ROOT/work/android/turnip; mkdir -p "$OUT/previous"
[ -f "$OUT/libvulkan_freedreno.so" ] && cp "$OUT/libvulkan_freedreno.so" "$OUT/previous/"
cp "$B/src/freedreno/vulkan/libvulkan_freedreno.so" "$OUT/libvulkan_freedreno.so"
echo "built $OUT/libvulkan_freedreno.so ($(du -h "$OUT/libvulkan_freedreno.so" | cut -f1), Mesa $(cat VERSION) $(git rev-parse --short HEAD) + pvtmem fix)"
