# The toolchain every build script uses (sourced, not run). Defaults are this project's PC; CI sets ANDROID_SDK_ROOT
# (and DOTNET when .NET is installed elsewhere). Versions are pinned: a release must not change compiler by accident.
ANDROID_SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/opt/android-sdk}}
NDK_VERSION=29.0.14206865; BUILD_TOOLS_VERSION=35.0.0; PLATFORM_VERSION=android-36
NDK=$ANDROID_SDK/ndk/$NDK_VERSION   # never ANDROID_NDK_HOME: GitHub's runners set it to another version
NDK_BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
BUILD_TOOLS=$ANDROID_SDK/build-tools/$BUILD_TOOLS_VERSION
ANDROID_JAR=$ANDROID_SDK/platforms/$PLATFORM_VERSION/android.jar
