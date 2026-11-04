#!/bin/bash
# The rules of host/prompts_romfs.c against the PC's overlays and against damage, on a PC. Run it after any change to that file.
#   port/android/host/tests/regress.sh [--scratch DIR] [--files DIR] [--work DIR] [--rounds N]
# Reads switch_files (--files) and work/totk/extracted/v143 (--work; a worktree has neither: the main checkout's are used). The
# file is built alone here, with tests/stub/runtime.h; in the app it is part of libtotk (CMakeLists.txt).
set -u
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd); A=$(cd "$HERE/../.." && pwd); ROOT=$(cd "$A/../.." && pwd)
FILES=$ROOT/switch_files; WORK=$ROOT/work; SCRATCH=; ROUNDS=2000
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
K=$FILES/prod.keys; B="$FILES/The Legend of Zelda Tears of the Kingdom.nsp"; U=$FILES/totk-1.4.3.nsp; X=$WORK/totk/extracted/v143
[ -n "$SCRATCH" ] || SCRATCH=$(mktemp -d)
mkdir -p "$SCRATCH"; SCRATCH=$(cd "$SCRATCH" && pwd)
J=$(nproc); FAILS=0
cmake -G Ninja -S "$A/setup/container" -B "$SCRATCH/c-rel" -DCMAKE_C_COMPILER=clang > /dev/null && cmake --build "$SCRATCH/c-rel" --target tokcontainer -j "$J" > /dev/null || exit 2
cmake -G Ninja -S "$A/setup/container" -B "$SCRATCH/c-san" -DCMAKE_C_COMPILER=clang -DTOKC_SANITIZE=ON > /dev/null && cmake --build "$SCRATCH/c-san" --target tokcontainer -j "$J" > /dev/null || exit 2
F=(-std=gnu11 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter "-I$HERE/stub" "-I$A/host" "-I$A/setup/container")
clang "${F[@]}" -O2 "$HERE/romfs_rules_check.c" "$SCRATCH/c-rel/libtokcontainer.a" -lzstd -lm -o "$SCRATCH/rules_check" || exit 2
clang "${F[@]}" -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer "$HERE/romfs_rules_check.c" "$SCRATCH/c-san/libtokcontainer.a" -lzstd -lm -o "$SCRATCH/rules_check_san" || exit 2

"$SCRATCH/rules_check" compare "$K" "$B" "$U" "$X/romfs-patches" "$X/romfs-patches-out" 2> /dev/null || FAILS=$((FAILS + 1))
pids=()
for i in $(seq 1 "$J"); do
  ( ASAN_OPTIONS=detect_leaks=1 "$SCRATCH/rules_check_san" malformed "$K" "$B" "$U" "$i" "$ROUNDS" 2>&1 | grep -v '^\[totk-' > "$SCRATCH/damage-$i.log"; exit "${PIPESTATUS[0]}" ) & pids+=($!)
done
bad=0
for p in "${pids[@]}"; do wait "$p" || bad=1; done
tail -qn 2 "$SCRATCH/damage-1.log"
if [ $bad = 0 ]; then echo "PASS $J runs of damaged files under ASan and UBSan"; else echo "FAIL damaged files: $SCRATCH/damage-*.log"; FAILS=$((FAILS + 1)); fi
echo "failures: $FAILS"
[ "$FAILS" = 0 ]
