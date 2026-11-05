#!/bin/sh
# The fonts of a game run from the player's own files, from Noto Sans CJK SC (SIL OFL 1.1) at a pinned commit:
# - Regular fills the firmware's six shared-font slots (host/player_files.c), cut down to what can reach those slots. The game's
#   CJK text comes from its own fonts in the RomFS; its complex fonts (Font/*.bfarc.zs) send it to the shared font only for the
#   ranges below: Latin, Greek, Cyrillic, punctuation, signs and kana. tools/check_shared_font.py checks this against the game's text.
# - Bold gives the letters on the PlayStation icons, drawn by tools/make_icon_asset.py (IconMod.java; nothing of the game).
# Both downloads and both results are checked, so a moved file or another fontTools stops the build.
# usage: fetch_font.sh -> port/android/deps/out/fonts/SharedFont.otf, port/android/deps/out/icons/playstation.bin (the originals
# stay in deps/src/noto-sans-cjk)
set -e
HERE=$(cd "$(dirname "$0")" && pwd); DEPS=$HERE/../deps; SRC=$DEPS/src/noto-sans-cjk; OUT=$DEPS/out/fonts/SharedFont.otf; ICONS=$DEPS/out/icons/playstation.bin
# notofonts/noto-cjk tag Sans2.004
COMMIT=523d033d6cb47f4a80c58a35753646f5c3608a78
FILE=NotoSansCJKsc-Regular.otf; FILE_SHA256=2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b
BOLD=NotoSansCJKsc-Bold.otf; BOLD_SHA256=b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a
FONTTOOLS=4.60.1; OUT_SHA256=571eecc7228b10992c8228569aaa7f97fc8dfb4d24f8b6a2c28e7f051fcc6b3c
ICONS_SHA256=7bb321841dac01ba399092f2e5c9da512f0017fe65affe3d2493861472b1404a
UNICODES=U+0020-007E,U+00A0-0FFF,U+2000-266F,U+3000-30FF,U+FF00-FFEF,U+FFFD
sum() { sha256sum "$1" | cut -d' ' -f1; }
fetch() {
  [ -f "$SRC/$1" ] || curl -sSfL -o "$SRC/$1" "https://raw.githubusercontent.com/notofonts/noto-cjk/$COMMIT/Sans/OTF/SimplifiedChinese/$1"
  [ "$(sum "$SRC/$1")" = "$2" ] || { echo "$SRC/$1 is not the pinned font" >&2; exit 1; }
}
[ -f "$OUT" ] && [ "$(sum "$OUT")" = "$OUT_SHA256" ] && [ -f "$ICONS" ] && [ "$(sum "$ICONS")" = "$ICONS_SHA256" ] && exit 0
mkdir -p "$SRC" "$(dirname "$OUT")" "$(dirname "$ICONS")"
fetch "$FILE" "$FILE_SHA256"
fetch "$BOLD" "$BOLD_SHA256"
V=$DEPS/build/font-venv
[ -x "$V/bin/pyftsubset" ] || { python3 -m venv "$V" && "$V/bin/pip" install -q "fonttools==$FONTTOOLS"; }
"$V/bin/pyftsubset" "$SRC/$FILE" --output-file="$OUT.tmp" --unicodes=$UNICODES --layout-features+=palt --notdef-outline --no-recalc-timestamp
[ "$(sum "$OUT.tmp")" = "$OUT_SHA256" ] || { echo "the cut-down font is not the pinned one: $(sum "$OUT.tmp")" >&2; exit 1; }
mv "$OUT.tmp" "$OUT"
"$V/bin/python" "$HERE/make_icon_asset.py" "$SRC/$BOLD" "$ICONS.tmp"
[ "$(sum "$ICONS.tmp")" = "$ICONS_SHA256" ] || { echo "the icon drawings are not the pinned ones: $(sum "$ICONS.tmp")" >&2; exit 1; }
mv "$ICONS.tmp" "$ICONS"
