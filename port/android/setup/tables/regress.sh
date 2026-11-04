#!/bin/bash
# The tables against prepare_runtime.py, and the phone's whole path against every live file. Run it after any change to
# prepare_runtime.py or to this folder, the patcher or the container reader, and after an upstream sync.
#   port/android/setup/tables/regress.sh [--scratch DIR] [--files DIR] [--work DIR] [--tool FILE] [--synth N] [--mutants N]
#                                        [--fuzz N] [--libfuzzer S] [--bench N] [--no-full] [--no-fuzz]
# Reads the player's files from switch_files (--files), the live folders from work/ (--work) and the Python tool from the
# MacNativeGames checkout (--tool); a worktree has none of them: the main checkout's are used. Writes only to the scratch folder,
# which is a new temporary folder unless given. It holds images of the game: use a private disk, and delete it. Steps, each a
# line of PASS or FAIL:
#   - the output limit, bad calls, SHA-256's published answers, the UTF-8 check against Python's decoder
#   - tables.h names this prepare_runtime.py and patch_nso.py
#   - prepare_runtime.py run here on the live rewritten folder; tables_cli on the same folder; the live file: all the same
#   - the phone's path, under release and under ASan and UBSan: NSPs and keys through the reader, the rewriter and the tables,
#     compared with every file in rewritten and game-data (--no-full: without hashing the 17 GB image)
#   - the comparison itself refuses a changed byte, a missing file and an unknown file
#   - made-up modules and damaged real ones: same accept or refuse, same bytes as the Python tool (--synth N, --mutants N)
#   - the image hashes in the tables against sha256sum, the arm64 SHA-256 instructions in an emulator, and the arm64 build
#   - damaged made-up modules under ASan and UBSan (--fuzz N on most threads, N / 100 on the real modules on three; --no-fuzz
#     skips), libFuzzer for S seconds
# Exit status: 0 only when nothing failed.
set -u
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../../.." && pwd)
FILES=$ROOT/switch_files; WORK=$ROOT/work; TOOL=$ROOT/MacNativeGames/games/totk/scripts/prepare_runtime.py
SCRATCH=; SYNTH=600; MUTANTS=64; FUZZ=200000; LIBFUZZ=120; BENCH=21; FULL=1; DOFUZZ=1
while [ $# -gt 0 ]; do
  case $1 in
    --scratch) SCRATCH=$2; shift 2 ;;
    --files) FILES=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --tool) TOOL=$2; shift 2 ;;
    --synth) SYNTH=$2; shift 2 ;;
    --mutants) MUTANTS=$2; shift 2 ;;
    --fuzz) FUZZ=$2; shift 2 ;;
    --libfuzzer) LIBFUZZ=$2; shift 2 ;;
    --bench) BENCH=$2; shift 2 ;;
    --no-full) FULL=0; shift ;;
    --no-fuzz) DOFUZZ=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
[ -d "$WORK/android/v143/rewritten" ] || WORK=${COMMON%/.git}/work
[ -d "$FILES" ] || FILES=${COMMON%/.git}/switch_files
[ -f "$TOOL" ] || TOOL=${COMMON%/.git}/MacNativeGames/games/totk/scripts/prepare_runtime.py
[ -f "$TOOL" ] || { echo "no prepare_runtime.py: give --tool" >&2; exit 2; }
LIVE=$WORK/android/v143/rewritten; GAMEDATA=$WORK/android/v143/tree/build/game-data/v143; X=$WORK/totk/extracted/v143
KEYS=$FILES/prod.keys; BASE="$FILES/The Legend of Zelda Tears of the Kingdom.nsp"; UPDATE=$FILES/totk-1.4.3.nsp
[ -d "$LIVE" ] && [ -d "$GAMEDATA" ] || { echo "no live folders in $WORK: give --work" >&2; exit 2; }
[ -n "$SCRATCH" ] || SCRATCH=$(mktemp -d)
mkdir -p "$SCRATCH"; SCRATCH=$(cd "$SCRATCH" && pwd)
PY=$WORK/venv/bin/python; D2R=$(cd -P "$HERE/../../../rewriter/d2r" && pwd); J=$(nproc); FAILS=0
export PYTHONDONTWRITEBYTECODE=1
echo "scratch: $SCRATCH"
result() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; FAILS=$((FAILS + 1)); fi; }

export CC=clang
CAP=$ROOT/port/android/deps/src/capstone
[ -f "$CAP/cs.c" ] || { CAP=$SCRATCH/capstone; sh "$HERE/../patcher/fetch_capstone.sh" "$CAP" || exit 2; }
build() {   # <name> <cmake options...>
  local dir=$SCRATCH/$1; shift
  cmake -G Ninja -S "$HERE" -B "$dir" -DCAPSTONE_SRC="$CAP" "$@" > "$dir.log" 2>&1 && cmake --build "$dir" -j "$J" >> "$dir.log" 2>&1 ||
    { echo "the build failed: $dir.log" >&2; exit 2; }
}
build build
build build-san -DTOKT_SANITIZE=ON -DTOKT_FUZZER=ON
CLI=$SCRATCH/build/tables_cli

# ---- no game files ----
"$PY" -I "$HERE/tests/utf8_cases.py" > "$SCRATCH/utf8.txt"
"$SCRATCH/build/test_units" < "$SCRATCH/utf8.txt"; result $? "limits, bad calls, SHA-256 answers, UTF-8 against Python"
ASAN_OPTIONS=allocator_may_return_null=1:detect_leaks=1 "$SCRATCH/build-san/test_units" < "$SCRATCH/utf8.txt" > /dev/null; result $? "the same under ASan and UBSan"

# ---- which Python ----
want=$(grep -o 'TOKT_PY_SHA256 "[0-9a-f]*"' "$HERE/tables.h" | cut -d'"' -f2)
have=$(sha256sum "$TOOL" | cut -d' ' -f1)
[ "$want" = "$have" ]; result $? "tables.h names this prepare_runtime.py ($have)"
want=$(grep -o 'TOKP_PY_SHA256 "[0-9a-f]*"' "$HERE/../patcher/patcher.h" | cut -d'"' -f2)
have=$(sed -n 's/^REWRITER_SHA256 //p' "$GAMEDATA/rewriter.txt")
[ "$want" = "$have" ]; result $? "game-data/rewriter.txt names the patch_nso.py patcher.h mirrors ($have)"

# ---- the live folder through both tools ----
T=$SCRATCH/pytree
rm -rf "$T"; mkdir -p "$T/scripts" "$T/third_party" "$T/rewritten" "$T/analysis/v143-port"
cp "$TOOL" "$T/scripts/prepare_runtime.py"
ln -s "$D2R" "$T/third_party/d2r"; ln -s "$WORK/totk/extracted" "$T/extracted"; ln -s "$LIVE" "$T/rewritten/v143"
( cd "$T" && "$PY" -I -c "
import hashlib, json
d = {}
for m in ('main', 'sdk', 'subsdk0'):
    for k in (f'rewritten/v143/{m}.patched.bin', f'extracted/v143/exefs/{m}'):
        d[k] = hashlib.sha256(open(k, 'rb').read()).hexdigest()
json.dump(dict(status='pass', rom_version='v143', module_file_sha256=d), open('analysis/v143-port/phase12-validation.json', 'w'))" &&
  "$PY" scripts/prepare_runtime.py --version v143 > "$SCRATCH/python.log" 2>&1 ); result $? "prepare_runtime.py runs, unchanged, on the live folder"
PYOUT=$T/build/game-data/v143
"$CLI" files "$LIVE" "$X/exefs" "$SCRATCH/files-tables.bin"; result $? "tables_cli on the live folder"
cmp "$SCRATCH/files-tables.bin" "$PYOUT/runtime-tables.bin"; result $? "tables: C and Python the same"
cmp "$PYOUT/runtime-tables.bin" "$GAMEDATA/runtime-tables.bin"; result $? "tables: Python and the live file the same"
cmp "$PYOUT/rewriter.txt" "$GAMEDATA/rewriter.txt"; result $? "rewriter.txt: Python and the live file the same"

# ---- the phone's path ----
if [ -f "$KEYS" ] && [ -f "$BASE" ] && [ -f "$UPDATE" ]; then
  args=(--rewritten "$LIVE" --game-data "$GAMEDATA")
  "$CLI" chain "$KEYS" "$BASE" "$UPDATE" "${args[@]}" --bench "$BENCH" --out "$SCRATCH" > "$SCRATCH/chain.log" 2>&1; rc=$?
  grep -v '^same ' "$SCRATCH/chain.log"; echo "$(grep -c '^same ' "$SCRATCH/chain.log") files the same"
  result $rc "NSPs and keys to every live file"
  cmp "$SCRATCH/runtime-tables.bin" "$GAMEDATA/runtime-tables.bin" && cmp "$SCRATCH/rewriter.txt" "$GAMEDATA/rewriter.txt"; result $? "what chain --out writes"
  if [ "$FULL" = 1 ]; then
    "$CLI" chain "$KEYS" "$BASE" "$UPDATE" --game-data "$GAMEDATA" --full > "$SCRATCH/chain-full.log" 2>&1; rc=$?
    grep -E '^(ms: whole|FAIL|not compared)' "$SCRATCH/chain-full.log"
    result $rc "ROMFS_IMAGE_SHA256 from the whole image through the reader"
  fi
  ASAN_OPTIONS=detect_leaks=1 "$SCRATCH/build-san/tables_cli" chain "$KEYS" "$BASE" "$UPDATE" "${args[@]}" > "$SCRATCH/chain-san.log" 2>&1
  result $? "the same under ASan and UBSan, no leaks"

  # The comparison must be able to fail.
  N=$SCRATCH/negative; rm -rf "$N"; mkdir -p "$N"
  cp "$GAMEDATA"/* "$N/"
  "$PY" -I -c "
import sys
f = open(sys.argv[1], 'r+b'); f.seek(5000000); b = f.read(1); f.seek(5000000); f.write(bytes([b[0] ^ 1]))" "$N/runtime-tables.bin"
  "$CLI" chain "$KEYS" "$BASE" "$UPDATE" --game-data "$N" 2>&1 | grep -q 'FAIL  runtime-tables.bin  *differs at offset 5000000'; result $? "a changed byte is found, at its offset"
  cp "$GAMEDATA/runtime-tables.bin" "$N/"; rm "$N/rewriter.txt"
  "$CLI" chain "$KEYS" "$BASE" "$UPDATE" --game-data "$N" 2>&1 | grep -q 'FAIL  rewriter.txt  *made, but not in the folder'; result $? "a missing file is found"
  cp "$GAMEDATA/rewriter.txt" "$N/"; : > "$N/extra.bin"
  ! "$CLI" chain "$KEYS" "$BASE" "$UPDATE" --game-data "$N" > /dev/null 2>&1; result $? "an unknown file is refused"
  rm -rf "$N"
else
  echo "SKIP the phone's path: no keys or NSPs in $FILES"
fi

# ---- the Python tool case by case ----
one_case() {
  local d=$1 c=out rc_py rc_c
  ( cd "$d" && "$PY" scripts/prepare_runtime.py --version v143 ) > "$d/python.log" 2>&1; rc_py=$?
  "$CLI" files "$d/rewritten/v143" "$d/extracted/v143/exefs" "$d/c-tables.bin" > "$d/c.log" 2>&1; rc_c=$?
  if [ $rc_py = 0 ] && [ $rc_c = 0 ]; then
    if cmp -s "$d/build/game-data/v143/runtime-tables.bin" "$d/c-tables.bin"; then c='ok'; else c='DIFFERENT BYTES'; fi
  elif [ $rc_py != 0 ] && [ $rc_c != 0 ]; then c='refused'
  elif [ $rc_py = 0 ]; then c='ONLY C REFUSED'
  else c='ONLY PYTHON REFUSED'; fi
  echo "${d##*/} [$(cat "$d/fault.txt")]: $c"
}
export -f one_case; export PY CLI
run_cases() {   # <folder> <title>
  find "$1" -mindepth 1 -maxdepth 1 -type d -name 'case_*' | sort | xargs -P "$J" -I{} bash -c 'one_case {}' > "$1.results" 2>&1
  local ok refused bad
  ok=$(grep -c ': ok$' "$1.results"); refused=$(grep -c ': refused$' "$1.results"); bad=$(grep -vc -e ': ok$' -e ': refused$' "$1.results")
  [ "$bad" = 0 ] || grep -v -e ': ok$' -e ': refused$' "$1.results" | head -20
  [ "$ok" -gt 0 ] && [ "$bad" = 0 ]; result $? "$2: $ok same bytes, $refused refused by both, $bad disagreements"
}
if [ "$SYNTH" -gt 0 ]; then
  rm -rf "$SCRATCH/synth"; mkdir -p "$SCRATCH/synth"
  "$PY" -I "$HERE/tests/gen_synth.py" "$TOOL" "$D2R" made-up "$SCRATCH/synth" "$SYNTH" 1 && run_cases "$SCRATCH/synth" "made-up modules, half with a fault"
fi
if [ "$MUTANTS" -gt 0 ]; then
  rm -rf "$SCRATCH/mutants"; mkdir -p "$SCRATCH/mutants"
  "$PY" -I "$HERE/tests/gen_synth.py" "$TOOL" "$D2R" mutate "$LIVE" "$X/exefs" "$SCRATCH/mutants" "$MUTANTS" 1 && run_cases "$SCRATCH/mutants" "the real modules with a fault"
  rm -rf "$SCRATCH"/mutants/case_*
fi

# ---- SHA-256 and arm64 ----
bad=0
for m in main subsdk0 sdk; do
  grep -a -q "$(sha256sum "$LIVE/$m.patched.bin" | cut -d' ' -f1)" "$SCRATCH/files-tables.bin" || bad=1
done
result $bad "image hashes in the tables are the files' own (sha256sum)"
. "$HERE/../../tools/env.sh"
if [ -x "$NDK_BIN/clang" ]; then
  sh "$HERE/build_android.sh" "$SCRATCH/build-android" "$CAP" > "$SCRATCH/build-android.log" 2>&1; result $? "arm64 build with the pinned NDK"
  "$NDK_BIN/clang" --target=aarch64-linux-android33 -O2 -ffreestanding -fno-stack-protector -c "$HERE/sha256_hw.c" -o "$SCRATCH/sha256_hw.arm64.o" &&
    "$NDK_BIN/ld.lld" -e 0 -o "$SCRATCH/sha256_hw.arm64.elf" "$SCRATCH/sha256_hw.arm64.o" && "$PY" -I "$HERE/tests/sha_arm64_check.py" "$SCRATCH/sha256_hw.arm64.elf"
  result $? "arm64 SHA-256 against hashlib, emulated"
else
  echo "SKIP arm64: no NDK"
fi

# ---- damage ----
if [ "$DOFUZZ" = 1 ]; then
  FZ=$SCRATCH/build-san/fuzz_malformed; pids=(); bad=0
  export ASAN_OPTIONS=allocator_may_return_null=1:detect_leaks=1
  # even cases have no fault
  for i in $(seq 1 $((J - 3))); do
    d=$SCRATCH/synth/$(printf 'case_%05d' $((2 * i)))
    ( "$FZ" "$d/rewritten/v143" "$d/extracted/v143/exefs" "$FUZZ" "$i" ) > "$SCRATCH/fuzz-made-up-$i.log" 2>&1 & pids+=($!)
  done
  for i in 1 2 3; do
    ( "$FZ" "$LIVE" "$X/exefs" $((FUZZ / 100 + 1)) "$((100 + i))" ) > "$SCRATCH/fuzz-real-$i.log" 2>&1 & pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || bad=1; done
  tail -qn 1 "$SCRATCH/fuzz-made-up-1.log" "$SCRATCH/fuzz-real-1.log"
  result $bad "damaged modules under ASan and UBSan (logs: $SCRATCH/fuzz-*.log)"
  if [ "$LIBFUZZ" -gt 0 ]; then
    mkdir -p "$SCRATCH/corpus" "$SCRATCH/libfuzzer"
    ( cd "$SCRATCH/libfuzzer" && TOKT_FUZZ_CASE=$SCRATCH/synth/case_00000 "$SCRATCH/build-san/fuzz_libfuzzer" -max_total_time="$LIBFUZZ" -jobs="$J" -workers="$J" -max_len=384 -rss_limit_mb=4096 \
      -artifact_prefix="$SCRATCH/libfuzzer/crash-" "$SCRATCH/corpus" > "$SCRATCH/libfuzzer.log" 2>&1 )
    bad=0
    grep -lE 'ERROR: |SUMMARY: |deadly signal' "$SCRATCH"/libfuzzer/fuzz-*.log && bad=1
    ls "$SCRATCH"/libfuzzer/crash-* > /dev/null 2>&1 && bad=1
    grep -hE '^Done [0-9]+ runs' "$SCRATCH"/libfuzzer/fuzz-*.log | awk '{n += $2} END {print n " runs"}'
    result $bad "libFuzzer, $J workers for $LIBFUZZ s (logs: $SCRATCH/libfuzzer)"
  fi
fi

echo "failures: $FAILS"
[ "$FAILS" = 0 ]
