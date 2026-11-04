#!/bin/sh
# Builds MacNativeGames' shader translator (vendored Ryujinx 1.3.2 + TOTK patches + Program.cs/Service.cs) with NativeAOT
# for a given runtime id. linux-x64: pre-translation on the PC. linux-bionic-arm64: on-device translation (later).
# usage: port/xlate/publish.sh [rid]   -> port/xlate/<rid>/totk_xlate
set -e
RID=${1:-linux-x64}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/MacNativeGames/games/totk/tools/totk_xlate
DOTNET=${DOTNET:-$ROOT/tools/dotnet/dotnet}   # CI: the .NET SDK it installed
WORK=$ROOT/port/xlate/work-$RID
# Android (linux-bionic): NativeAOT needs the runtime pack and is marked unsupported by default
case $RID in linux-bionic-*) EXTRA="-p:PublishAotUsingRuntimePack=true -p:DisableUnsupportedError=true";; *) EXTRA="";; esac
rm -rf "$WORK"; mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$ROOT/MacNativeGames/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$SRC"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$SRC"/*.cs "$SRC"/totk_xlate.csproj "$WORK/xlate/"
DOTNET_CLI_TELEMETRY_OPTOUT=1 "$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r "$RID" -p:PublishAot=true \
  -p:InvariantGlobalization=true -p:RyuSrc="$WORK/ryujinx-src" $EXTRA -o "$ROOT/port/xlate/$RID"
echo "built $ROOT/port/xlate/$RID/totk_xlate"
