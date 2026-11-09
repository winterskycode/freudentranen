#!/bin/sh
# Builds MacNativeGames' shader translator (vendored Ryujinx 1.3.2 + TOTK patches + Program.cs/Service.cs) with NativeAOT
# for a given runtime id. linux-x64: pre-translation on the PC. linux-bionic-arm64: on-device translation.
# usage: port/xlate/publish.sh [rid]   -> port/xlate/<rid>/totk_xlate
# MNG_DIR: another MacNativeGames tree (an upstream sync worktree); XLATE_OUT: another output folder (its build folder
# beside it), so a second translator can be built next to the shipped one.
set -e
RID=${1:-linux-x64}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MNG=${MNG_DIR:-$ROOT/MacNativeGames}
SRC=$MNG/games/totk/tools/totk_xlate
OUT=${XLATE_OUT:-$ROOT/port/xlate/$RID}
# CI: the .NET SDK it installed
DOTNET=${DOTNET:-$ROOT/tools/dotnet/dotnet}
WORK=${XLATE_OUT:+$XLATE_OUT.work}; WORK=${WORK:-$ROOT/port/xlate/work-$RID}
# Android (linux-bionic): NativeAOT needs the runtime pack and is marked unsupported by default
case $RID in linux-bionic-*) EXTRA="-p:PublishAotUsingRuntimePack=true -p:DisableUnsupportedError=true";; *) EXTRA="";; esac
rm -rf "$WORK"; mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$MNG/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$SRC"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$SRC"/*.cs "$SRC"/totk_xlate.csproj "$WORK/xlate/"
DOTNET_CLI_TELEMETRY_OPTOUT=1 "$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r "$RID" -p:PublishAot=true \
  -p:InvariantGlobalization=true -p:RyuSrc="$WORK/ryujinx-src" $EXTRA -o "$OUT"
echo "built $OUT/totk_xlate"
