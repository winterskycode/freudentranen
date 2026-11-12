# Building from source

Everything in the APK is built from source: the game host and Vulkan renderer, the shader translator, the bundled
Turnip driver and the libraries they use. Building needs no game files. They are only needed on the device, when the
game runs.

The build runs on Linux x86-64. Releases are built on Ubuntu 24.04; other recent Linux distributions work too.

## Prerequisites

1. **System packages.** The list is in `port/android/tools/ci-packages.txt`. On Ubuntu or Debian:

   ```sh
   sudo apt-get install -y --no-install-recommends $(cat port/android/tools/ci-packages.txt)
   ```

   Other distributions need the same tools: git, curl, Python 3 with venv, CMake, Ninja, make, perl, patch,
   pkg-config, bison, flex, glslangValidator, a JDK 17 or newer, zip, unzip and xz, plus ICU for .NET.
   `spirv-dis` (SPIRV-Tools) is needed only to regenerate the shader `.inc` files (`port/android/tools/gen_shaders.sh`).

2. **Android SDK parts**, in the versions pinned in `port/android/tools/env.sh`:

   ```sh
   sdkmanager "ndk;29.0.14206865" "build-tools;35.0.0" "platforms;android-36"
   ```

   The scripts look for the SDK in `$ANDROID_SDK_ROOT`, then `$ANDROID_HOME`, then `/opt/android-sdk`.

3. **.NET SDK 10.0.402.** `release.sh` installs it into `tools/dotnet` when it is missing. For the step-by-step build,
   install it the same way:

   ```sh
   mkdir -p tools && curl -sSfL https://dot.net/v1/dotnet-install.sh -o tools/dotnet-install.sh
   bash tools/dotnet-install.sh --version 10.0.402 --install-dir tools/dotnet --no-path
   ```

The first build downloads its sources (MacNativeGames, Mesa, the libraries, .NET and its packages) and needs about
3 GB of disk.

## One command

```sh
TOTK_RELEASE_DEBUG_KEY=1 port/android/tools/release.sh 20261029-1325
```

This builds every part from source, checks the result and writes `dist/totk-20261029-1325.apk` and its SHA-256. The
argument is a release tag in the form `YYYYMMDD-HHMM`; it becomes the app's version. `TOTK_RELEASE_DEBUG_KEY=1` signs
the APK with your Android debug key (made if you have none) instead of the project's release key. It took about ten minutes
on a 16-core PC.

An APK you build is signed with your key, so Android will not install it over the release builds, which are signed with
the project's key. Uninstall the release first (export your saves before, as uninstalling deletes the app's data).

## Step by step

Use this when working on one part. Each step can be repeated alone; later steps use the earlier ones' output.

| Step | Command | Produces |
| --- | --- | --- |
| MacNativeGames, the `android` branch of our fork at the pinned commit | `sh port/setup.sh` | `MacNativeGames/` |
| Opus, libvpx, zstd (and fetches Capstone and the shared-font stand-in) | `sh port/android/deps/build_deps.sh` | `port/android/deps/out/` |
| The code rewriter in C (not in the APK yet) | `sh port/android/setup/patcher/build_android.sh <dir>` | `<dir>/libtokpatcher.a` |
| The reader for the player's game files (not in the APK yet) | `sh port/android/setup/container/build_android.sh <dir>` | `<dir>/libtokcontainer.a` |
| libadrenotools and its hooks | `python3 port/android/tools/build_gpu_driver.py` | `work/android/gpu-driver/` |
| Turnip (below) | `sh port/android/tools/build_turnip.sh` | `work/android/turnip/libvulkan_freedreno.so` |
| The shader translator | `DOTNET=$PWD/tools/dotnet/dotnet sh port/xlate/publish_android.sh` | `port/xlate/linux-bionic-arm64/totk_xlate` |
| Configure the native build | see below | `work/android/v143/build-host/` |
| The APK | `sh port/android/tools/build_apk.sh` | `work/android/v143/apk/totk.apk` |

Configure the native build once:

```sh
. port/android/tools/env.sh
cmake -G Ninja -S port/android -B work/android/v143/build-host \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DTOTK_VERSION=v143 -DCMAKE_BUILD_TYPE=Release
```

After that, `build_apk.sh` rebuilds only what changed. `build_apk.sh --install` also installs the APK on the connected
device with `adb`. PC builds are named `dev` and get a version code from the current time, so each one installs over
the previous build signed with the same key.

The APK runs on Android 13 (API 33) or newer, on 64-bit ARM.

`release.sh` builds in a folder of its own, `work/android/v143/build-release`, with profile-guided optimisation and ThinLTO
(`-DTOTK_PGO=$PWD/port/android/pgo/totk.proftext -DTOTK_LTO=ON`). On our test device the translation thread used about 15%
fewer CPU cycles a frame. The profile comes from a run of the benchmark route; `port/android/tools/pgo.sh` makes a new one
on the connected device. A profile from older code still builds but helps less where the code changed, so make a new one
after large changes to the translation or the runtime.

## Updating MacNativeGames

The `android` branch of our fork is upstream MacNativeGames with the Android changes merged in, and `port/setup.sh` pins
one commit of it. To bring in a newer upstream:

1. In a worktree of `MacNativeGames` on the `android` branch, merge upstream's `main`. Merge rather than rebase: the
   commits that older releases pin stay on the branch.
2. Build against the worktree in a build folder of its own, so the current build stays as it is:

   ```sh
   cmake -G Ninja -S port/android -B work/android/v143/build-sync -DMNG_DIR=$PWD/work/next-mng ...   # the options above
   TOTK_BUILD_DIR=$PWD/work/android/v143/build-sync sh port/android/tools/build_apk.sh
   MNG_DIR=$PWD/work/next-mng sh port/xlate/publish_android.sh
   ```

3. If the shader translator changed, build it from both trees on the PC and compare what they make of every shader
   (this step needs the game files):

   ```sh
   XLATE_OUT=$PWD/work/xlate-cmp/old sh port/xlate/publish.sh
   MNG_DIR=$PWD/work/next-mng XLATE_OUT=$PWD/work/xlate-cmp/new sh port/xlate/publish.sh
   python3 port/android/tools/aot_shaders.py --profile <request-profile.json> --translator work/xlate-cmp/old/totk_xlate
   ```

   Run `aot_shaders.py` once with each translator, then compare the `.spv` files and check the changed ones with
   `spirv-val`. A translation that changes for every shader also makes every player's shader cache start over.
4. Test on a device: start the game, play with sound, and run `port/android/tools/bench.py`. Then push the branch to
   the fork and put its new commit in `port/setup.sh`.

What upstream changes most often break on Android: calls into Mac-only code (answered by weak stubs in
`port/android/compat/src/mng_hooks.c`), macOS register names in signal handlers (`runtime/uc_compat.h` has portable
ones), and defaults chosen for the Mac. The translation thread (`TOTK_ASYNC_REPLAY`), for one, stays on for Android.

## Turnip

The bundled Vulkan driver for Adreno GPUs is Turnip, from Mesa. `port/android/tools/build_turnip.sh` builds it:

- **Base:** the `turnip/gen8` branch of [whitebelyash/mesa-unified](https://github.com/whitebelyash/mesa-unified) at
  commit `9c7e022677` (Mesa 26.3.0-devel), the base of StevenMXZ's "Turnip Gen8" driver packages.
- **Our fix:** `patches/turnip-a8xx-pvtmem-stack-pad.patch`. On Adreno 8xx GPUs the last shader core's hardware stack
  starts at the end of the driver's private-memory buffer, and on the Adreno 830 it spills past it, over the next
  allocation. The patch adds one core's worth of padding to the buffer on 8xx GPUs only.
- **Build fixes:** four small source edits so the branch compiles with NDK r29 (as StevenMXZ's build script makes
  them). They do not change what the driver does.
- **Options:** a release build, stripped: the Vulkan driver only, for Qualcomm's kernel driver (KGSL), for Android.

```sh
sh port/android/tools/build_turnip.sh           # work/android/turnip/libvulkan_freedreno.so (about 18 MB)
sh port/android/tools/build_turnip.sh --debug   # with debug information and less optimisation (about 70 MB)
```

The sources go to `work/turnip-build/mesa`. The previous driver is kept in `work/android/turnip/previous/`.
`build_apk.sh` bundles whatever is in `work/android/turnip/`; without it, the APK only offers the system driver.

### As a driver package

The app's GPU driver page (Settings > System > GPU driver), and emulators that take driver zips, install a zip holding
the library and a `meta.json`:

```sh
cd work/android/turnip
cat > meta.json <<'EOF'
{"schemaVersion": 1, "name": "Turnip (custom build)", "description": "Mesa Turnip with the a8xx pvtmem fix",
 "author": "you", "packageVersion": "1", "vendor": "Mesa", "driverVersion": "26.3.0-devel", "minApi": 28,
 "libraryName": "libvulkan_freedreno.so"}
EOF
zip turnip-custom.zip meta.json libvulkan_freedreno.so
```

A driver installed that way gets its own shader cache, so it may run shaders at another width than the bundled one.
Turnip builds from Mesa 27 run them 64 wide on Adreno 8xx GPUs, the bundled build 128 wide.

## Running it

The APK holds no game files. The game needs your own copy of Tears of the Kingdom: the base game 1.0.0, the 1.4.3
update and your console's keys. The app's Game files page reads them on the device, where they are (README). For
development, `port/build_all.py` and `port/android/tools/push_data.sh` prepare the same data on a PC.

Shaders do not need preparing. The app translates each one on the device the first time the game uses it. Prepare
shaders on the startup screen translates them all ahead of time on the device, and `port/android/tools/aot_shaders.py`
with `push_shaders.sh` does the same from a PC. Either only removes the stutter of first visits.
