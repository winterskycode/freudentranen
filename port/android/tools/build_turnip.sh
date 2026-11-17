#!/bin/sh
# Build Turnip (Mesa's Vulkan driver for Adreno) for the Odin 3's Adreno 830 with our patches, into
# work/android/turnip/libvulkan_freedreno.so, which build_apk.sh bundles. The previous driver is kept in
# work/android/turnip/previous/.
#   [TURNIP_BASE=gen8|upstream] port/android/tools/build_turnip.sh [--debug]   (--debug: debugoptimized, unstripped, ~70 MB)
# Bases:
# - gen8 (default): whitebelyash's turnip/gen8 branch (Mesa 26.3, the base of StevenMXZ's "Turnip Gen8" packages), which
#   runs a8xx 128 wide and detects UBWC gralloc buffers by itself: the pvtmem and empty-shader patches only.
# - upstream: Mesa main with all four patches. On the Odin it was followed by system crashes in other processes and
#   reboots under long full-load runs, which the gen8 base did not show in the same runs; kept to bisect that.
# Each base has its own source tree (work/turnip-build/mesa, mesa-gen8) and build directory.
# Patches:
# - turnip-a8xx-pvtmem-stack-pad.patch: on a8xx the last SP's hardware stack spills past the end of the private-memory
#   BO and overwrites whatever is allocated next (LSFG's constant buffers: its first stage then computes from zeros and
#   frame generation shows a scaled double image).
# - turnip-a8xx-double-threadsize.patch: 128-wide shaders on a8xx, as the Turnip Gen8 packages run them. Upstream runs
#   a8xx 64 wide only; the shader caches (files/shader-cache-turnip, host-subgroup-size.txt) are built for 128.
# - turnip-gralloc-ubwc-detect.patch: reads the UBWC flag of every gralloc buffer, not only of buffers carrying the
#   'gmsm' magic. The Odin's swapchain buffers lack the magic, so upstream takes them as linear while the compositor
#   reads them compressed: the whole screen is noise in vertical stripes.
# - turnip-empty-shader-dynamic-sizes.patch: the placeholder tessellation and geometry shaders report each descriptor
#   set's dynamic descriptor size as unknown (-1), as the placeholder fragment shader does. Left at 0, they overwrite the
#   vertex shader's sizes when a fast-linked pipeline merges its stages, so a fragment shader from a library with
#   independent sets finds its dynamic uniform buffers at the vertex set's start (a blown-out picture).
# Needs: git, python3 with venv, ninja, glslangValidator, bison, flex; the NDK below.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); W=$ROOT/work/turnip-build
. "$ROOT/port/android/tools/env.sh"; NDK=$NDK_BIN
UPSTREAM=https://gitlab.freedesktop.org/mesa/mesa.git; GEN8=https://github.com/whitebelyash/mesa-unified.git
BASE=${TURNIP_BASE:-gen8}
case $BASE in
    upstream) COMMIT=312b4ef0a1cf952e13c30fc610603dad58f2f0c1; SRC=$W/mesa;;
    gen8) COMMIT=9c7e022677dfa3abb356c2b6732cbd2e25783d01; SRC=$W/mesa-gen8;;
    *) echo "TURNIP_BASE must be gen8 or upstream"; exit 1;;
esac
TYPE=release; STRIP=true; [ "$1" = "--debug" ] && TYPE=debugoptimized && STRIP=false
mkdir -p "$W"
[ -x "$W/venv/bin/meson" ] || { python3 -m venv "$W/venv"; "$W/venv/bin/pip" -q install meson mako pyyaml packaging; }
if [ ! -d "$W/mesa/.git" ]; then git clone -q --filter=blob:none -b main $UPSTREAM "$W/mesa"; fi
git -C "$W/mesa" remote get-url upstream >/dev/null 2>&1 || git -C "$W/mesa" remote add upstream $UPSTREAM
git -C "$W/mesa" remote get-url gen8 >/dev/null 2>&1 || git -C "$W/mesa" remote add gen8 $GEN8
git -C "$W/mesa" cat-file -e $COMMIT 2>/dev/null || case $BASE in
    upstream) git -C "$W/mesa" fetch -q --filter=blob:none upstream main;;
    gen8) git -C "$W/mesa" fetch -q --filter=blob:none gen8 turnip/gen8;;
esac
[ -e "$SRC/.git" ] || git -C "$W/mesa" worktree add -q --detach "$SRC" $COMMIT
cd "$SRC"
git checkout -q -f $COMMIT && git clean -qfd
[ $BASE = upstream ] && git apply "$ROOT/patches/turnip-gralloc-ubwc-detect.patch"
# Android NDK r29 build fixes (as StevenMXZ's build_turnip.sh applies them)
sed -i 's/typedef const native_handle_t\* buffer_handle_t;/typedef void* buffer_handle_t;/g' include/android_stub/cutils/native_handle.h
sed -i 's/, hnd->handle/, (void *)hnd->handle/g' src/util/u_gralloc/u_gralloc_fallback.c
sed -i -E 's/([a-z_]+)->handle->/((const native_handle_t *)\1->handle)->/g' src/vulkan/runtime/vk_android.c
sed -i "/-Werror=gnu-empty-initializer/d" meson.build
git apply "$ROOT/patches/turnip-a8xx-pvtmem-stack-pad.patch"
[ $BASE = upstream ] && git apply "$ROOT/patches/turnip-a8xx-double-threadsize.patch"
git apply "$ROOT/patches/turnip-empty-shader-dynamic-sizes.patch"
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
B="$W/build-$BASE-$TYPE"
export PATH="$W/venv/bin:$PATH"
[ -f "$B/build.ninja" ] || meson setup "$B" --cross-file "$W/android-aarch64.txt" -Dbuildtype=$TYPE -Dstrip=$STRIP \
    -Dplatforms=android -Dvideo-codecs= -Dplatform-sdk-version=35 -Dandroid-stub=true -Dgallium-drivers= \
    -Dvulkan-drivers=freedreno -Dvulkan-beta=true -Dfreedreno-kmds=kgsl -Degl=disabled -Dandroid-libbacktrace=disabled
ninja -C "$B" -j"$(nproc)" src/freedreno/vulkan/libvulkan_freedreno.so
OUT=$ROOT/work/android/turnip; mkdir -p "$OUT/previous"
[ -f "$OUT/libvulkan_freedreno.so" ] && cp "$OUT/libvulkan_freedreno.so" "$OUT/previous/"
cp "$B/src/freedreno/vulkan/libvulkan_freedreno.so" "$OUT/libvulkan_freedreno.so"
echo "built $OUT/libvulkan_freedreno.so ($(du -h "$OUT/libvulkan_freedreno.so" | cut -f1), $BASE base, Mesa $(cat VERSION) $(git rev-parse --short HEAD) + patches)"
