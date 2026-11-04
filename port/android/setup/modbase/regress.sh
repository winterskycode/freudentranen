#!/bin/bash
# mod-base.bin made from the player's files, against the PC's and against damage. Run it after any change in this folder.
#   port/android/setup/modbase/regress.sh [--scratch DIR] [--files DIR] [--work DIR] [--rounds N]
# Reads switch_files (--files) and work/totk/extracted (--work; a worktree has neither: the main checkout's are used). Writes only
# to the scratch folder; game content stays there and is removed with it. Steps, each a line of PASS or FAIL:
#   - the base game's RomFS through the reader against the 1.0.0 image on disk
#   - the bytes of tools/make_mod_base.py's mod-base.bin, on 1 and on 16 threads
#   - damaged packs under ASan and UBSan, on all threads
set -u
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../../.." && pwd)
FILES=$ROOT/switch_files; WORK=$ROOT/work; SCRATCH=; ROUNDS=60
while [ $# -gt 0 ]; do
  case $1 in
    --scratch) SCRATCH=$2; shift 2 ;;
    --files) FILES=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --rounds) ROUNDS=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
[ -d "$WORK/totk/extracted" ] || WORK=${COMMON%/.git}/work
[ -d "$FILES" ] || FILES=${COMMON%/.git}/switch_files
K=$FILES/prod.keys; B="$FILES/The Legend of Zelda Tears of the Kingdom.nsp"; U=$FILES/totk-1.4.3.nsp; X=$WORK/totk/extracted
[ -n "$SCRATCH" ] || SCRATCH=$(mktemp -d)
mkdir -p "$SCRATCH"; SCRATCH=$(cd "$SCRATCH" && pwd)
J=$(nproc); FAILS=0
result() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; FAILS=$((FAILS + 1)); fi; }
export CC=clang
cmake -G Ninja -S "$HERE" -B "$SCRATCH/rel" > "$SCRATCH/rel.log" 2>&1 && cmake --build "$SCRATCH/rel" -j "$J" >> "$SCRATCH/rel.log" 2>&1 || { echo "the build failed: $SCRATCH/rel.log" >&2; exit 2; }
cmake -G Ninja -S "$HERE" -B "$SCRATCH/san" -DTOKM_SANITIZE=ON -DTOKM_TESTS=ON > "$SCRATCH/san.log" 2>&1 && cmake --build "$SCRATCH/san" -j "$J" >> "$SCRATCH/san.log" 2>&1 || { echo "the build failed: $SCRATCH/san.log" >&2; exit 2; }

"$SCRATCH/rel/modbase_cli" index "$K" "$B" "$U" "$SCRATCH/base-index.bin" > /dev/null
python3 - "$SCRATCH/base-index.bin" "$X/v0/romfs/index.json" <<'EOF'
import json, struct, sys
b = open(sys.argv[1], 'rb').read(); count, strings = struct.unpack_from('<QQ', b, 8)
got = {}
for i in range(count):
    off, size, name, n = struct.unpack_from('<QQII', b, 24 + 24*i)
    got[b[strings + name:strings + name + n - 1].decode()] = (off, size)
want = {f['path']: (f['offset'], f['size']) for f in json.load(open(sys.argv[2]))['files']}
print(len(got), 'files in the base index')
sys.exit(got != want)
EOF
result $? "the base game's index against the 1.0.0 image"
for t in 1 16; do
  "$SCRATCH/rel/modbase_cli" make "$K" "$B" "$U" "$SCRATCH/mod-base-$t.bin" --threads $t 2> /dev/null
  cmp "$SCRATCH/mod-base-$t.bin" "$X/v143/mod-base.bin"; result $? "mod-base.bin on $t threads equals the PC's"
done
pids=()
for i in $(seq 1 "$J"); do ( "$SCRATCH/san/modbase_damage" "$K" "$B" "$U" "$i" "$ROUNDS" ) > "$SCRATCH/damage-$i.log" 2>&1 & pids+=($!); done
bad=0
for p in "${pids[@]}"; do wait "$p" || bad=1; done
tail -qn 1 "$SCRATCH/damage-1.log"
result $bad "$J runs of damaged packs under ASan and UBSan (logs: $SCRATCH/damage-*.log)"
rm -f "$SCRATCH"/mod-base-*.bin
echo "failures: $FAILS"
[ "$FAILS" = 0 ]
