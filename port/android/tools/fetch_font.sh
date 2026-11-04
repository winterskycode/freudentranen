#!/bin/sh
# The font that fills the firmware's six shared-font slots when the game runs from the player's own files (host/player_files.c):
# Noto Sans CJK SC Regular (SIL OFL 1.1) at a pinned commit, cut down to what can reach those slots. The game's CJK text comes
# from its own fonts in the RomFS; its complex fonts (Font/*.bfarc.zs) send it to the shared font only for the ranges below:
# Latin, Greek, Cyrillic, punctuation, signs and kana. tools/check_shared_font.py checks this against the game's text.
# Both the download and the result are checked, so a moved file or another fontTools stops the build.
# usage: fetch_font.sh -> port/android/deps/out/fonts/SharedFont.otf (the original stays in deps/src/noto-sans-cjk)
set -e
HERE=$(cd "$(dirname "$0")" && pwd); DEPS=$HERE/../deps; SRC=$DEPS/src/noto-sans-cjk; OUT=$DEPS/out/fonts/SharedFont.otf
COMMIT=523d033d6cb47f4a80c58a35753646f5c3608a78   # notofonts/noto-cjk tag Sans2.004
FILE=NotoSansCJKsc-Regular.otf; FILE_SHA256=2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b
FONTTOOLS=4.60.1; OUT_SHA256=571eecc7228b10992c8228569aaa7f97fc8dfb4d24f8b6a2c28e7f051fcc6b3c
UNICODES=U+0020-007E,U+00A0-0FFF,U+2000-266F,U+3000-30FF,U+FF00-FFEF,U+FFFD
sum() { sha256sum "$1" | cut -d' ' -f1; }
[ -f "$OUT" ] && [ "$(sum "$OUT")" = "$OUT_SHA256" ] && exit 0
mkdir -p "$SRC" "$(dirname "$OUT")"
[ -f "$SRC/$FILE" ] || curl -sSfL -o "$SRC/$FILE" "https://raw.githubusercontent.com/notofonts/noto-cjk/$COMMIT/Sans/OTF/SimplifiedChinese/$FILE"
[ "$(sum "$SRC/$FILE")" = "$FILE_SHA256" ] || { echo "$SRC/$FILE is not the pinned font" >&2; exit 1; }
V=$DEPS/build/font-venv
[ -x "$V/bin/pyftsubset" ] || { python3 -m venv "$V" && "$V/bin/pip" install -q "fonttools==$FONTTOOLS"; }
"$V/bin/pyftsubset" "$SRC/$FILE" --output-file="$OUT.tmp" --unicodes=$UNICODES --layout-features+=palt --notdef-outline --no-recalc-timestamp
[ "$(sum "$OUT.tmp")" = "$OUT_SHA256" ] || { echo "the cut-down font is not the pinned one: $(sum "$OUT.tmp")" >&2; exit 1; }
mv "$OUT.tmp" "$OUT"
