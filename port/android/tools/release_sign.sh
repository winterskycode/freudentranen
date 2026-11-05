#!/bin/sh
# Signs a release that release.sh built: port/android/tools/release_sign.sh <tag>, with TOTK_KEYSTORE, TOTK_KEYSTORE_PASSWORD and
# TOTK_KEY_ALIAS. A separate step because the build runs third-party scripts, and none of them may see the key.
set -e
TAG=$1
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
. "$ROOT/port/android/tools/env.sh"
IN=$ROOT/work/android/v143/apk/aligned.apk
OUT=$ROOT/dist/totk-$TAG.apk
[ -n "$TAG" ] && [ -f "$OUT" ] && [ -f "$IN" ] || { echo "usage: release_sign.sh <tag>, after release.sh <tag>" >&2; exit 2; }
[ -n "$TOTK_KEYSTORE" ] || { echo "no release key: set TOTK_KEYSTORE and TOTK_KEYSTORE_PASSWORD" >&2; exit 2; }
"$BUILD_TOOLS/apksigner" sign --ks "$TOTK_KEYSTORE" --ks-key-alias "${TOTK_KEY_ALIAS:-totk}" --ks-pass env:TOTK_KEYSTORE_PASSWORD \
  --key-pass env:TOTK_KEYSTORE_PASSWORD --v4-signing-enabled false --out "$OUT" "$IN"
"$BUILD_TOOLS/apksigner" verify "$OUT"
if "$BUILD_TOOLS/apksigner" verify --print-certs "$OUT" | grep -q 'CN=Android Debug'; then echo "still signed with the debug key" >&2; exit 1; fi
(cd "$ROOT/dist" && sha256sum "totk-$TAG.apk" > "totk-$TAG.apk.sha256")
"$BUILD_TOOLS/apksigner" verify --print-certs "$OUT" | grep -E "certificate (DN|SHA-256)"
