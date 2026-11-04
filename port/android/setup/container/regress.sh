#!/bin/bash
# The reader against the game files on this PC and against damage. Run it after any change in this folder.
#   port/android/setup/container/regress.sh [--scratch DIR] [--files DIR] [--work DIR] [--random N] [--fuzz N] [--libfuzzer S]
#                                           [--no-full] [--no-fuzz]
# Reads the player's files from switch_files (--files) and the merged image from work/totk/extracted/v143 (--work; a worktree
# has neither: the main checkout's are used). Writes only build output and logs, to the scratch folder, which is a new temporary
# folder unless given. Decrypted bytes stay in memory. Steps, each a line of PASS or FAIL:
#   - crypto vectors and made-up files: release, ASan and UBSan, ThreadSanitizer
#   - the arm64 AES code in an emulator against OpenSSL, and the arm64 build
#   - modules, index, every RomFS file, the whole image and random reads against what is on disk (--no-full: the quick ones)
#   - the same through the portable AES and under ASan and UBSan, fewer reads
#   - key derivation on the real prod.keys; refusals with the real files swapped
#   - damaged made-up files under ASan and UBSan (--fuzz N each on all threads, --no-fuzz skips), libFuzzer for S seconds
# Exit status: 0 only when nothing failed.
set -u
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../../.." && pwd)
FILES=$ROOT/switch_files; WORK=$ROOT/work; SCRATCH=; RANDOM_READS=2000000; FUZZ=2000; LIBFUZZ=120; FULL=1; DOFUZZ=1
while [ $# -gt 0 ]; do
  case $1 in
    --scratch) SCRATCH=$2; shift 2 ;;
    --files) FILES=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --random) RANDOM_READS=$2; shift 2 ;;
    --fuzz) FUZZ=$2; shift 2 ;;
    --libfuzzer) LIBFUZZ=$2; shift 2 ;;
    --no-full) FULL=0; shift ;;
    --no-fuzz) DOFUZZ=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
[ -d "$WORK/totk/extracted" ] || WORK=${COMMON%/.git}/work
[ -d "$FILES" ] || FILES=${COMMON%/.git}/switch_files
KEYS=$FILES/prod.keys; BASE="$FILES/The Legend of Zelda Tears of the Kingdom.nsp"; UPDATE=$FILES/totk-1.4.3.nsp
X=$WORK/totk/extracted/v143; IMAGE=$X/romfs/romfs.bin; INDEX=$X/romfs/runtime-index.bin
[ -n "$SCRATCH" ] || SCRATCH=$(mktemp -d)
mkdir -p "$SCRATCH"; SCRATCH=$(cd "$SCRATCH" && pwd)
J=$(nproc); FAILS=0
echo "scratch: $SCRATCH"
result() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; FAILS=$((FAILS + 1)); fi; }

export CC=clang
build() {   # <name> <cmake options...>
  local dir=$SCRATCH/$1; shift
  cmake -G Ninja -S "$HERE" -B "$dir" "$@" > "$dir.log" 2>&1 && cmake --build "$dir" -j "$J" >> "$dir.log" 2>&1 ||
    { echo "the build failed: $dir.log" >&2; exit 2; }
}
build build
build build-san -DTOKC_SANITIZE=ON -DTOKC_FUZZER=ON
build build-tsan -DTOKC_TSAN=ON
build build-portable -DTOKC_NO_HW=ON
CLI=$SCRATCH/build/container_cli

# ---- no game files ----
export ASAN_OPTIONS=allocator_may_return_null=1:detect_leaks=1
for b in build build-san build-tsan build-portable; do
  "$SCRATCH/$b/test_synth" > "$SCRATCH/$b-synth.log" 2>&1; rc=$?
  tail -n 1 "$SCRATCH/$b-synth.log"; result $rc "made-up files, $b"
done

# ---- arm64 ----
. "$HERE/../../tools/env.sh"
PY=$WORK/venv/bin/python
if [ -x "$NDK_BIN/clang" ]; then
  sh "$HERE/build_android.sh" "$SCRATCH/build-android" > "$SCRATCH/build-android.log" 2>&1; result $? "arm64 build with the pinned NDK"
  "$NDK_BIN/clang" --target=aarch64-linux-android33 -O2 -ffreestanding -fno-stack-protector -c "$HERE/aes_hw.c" -I "$HERE" -o "$SCRATCH/aes_hw.arm64.o" &&
    "$NDK_BIN/ld.lld" -e 0 -o "$SCRATCH/aes_hw.arm64.elf" "$SCRATCH/aes_hw.arm64.o" && PYTHONDONTWRITEBYTECODE=1 "$PY" -I "$HERE/tests/aes_arm64_check.py" "$SCRATCH/aes_hw.arm64.elf"
  result $? "arm64 AES against OpenSSL, emulated"
else
  echo "SKIP arm64: no NDK"
fi

# ---- the real files ----
if [ -f "$KEYS" ] && [ -f "$BASE" ] && [ -f "$UPDATE" ]; then
  "$SCRATCH/build/key_check" "$KEYS"; result $? "key derivation"
  "$CLI" info "$KEYS" "$BASE" "$UPDATE"; result $? "the files identify themselves"
  out=$("$CLI" check "$KEYS" "$UPDATE" "$BASE" 2>&1); rc=$?
  echo "$out"; [ $rc != 0 ] && [[ $out == *"is the update"* ]]; result $? "base and update swapped are refused"
  if [ -f "$IMAGE" ]; then
    args=(--modules "$X/exefs" --index "$INDEX")
    [ "$FULL" = 1 ] && args+=(--files "$INDEX" "$IMAGE" --image "$IMAGE")
    "$CLI" check "$KEYS" "$BASE" "$UPDATE" "${args[@]}" --seed "$RANDOM" --random "$RANDOM_READS" "$IMAGE"
    result $? "reader against the extracted files and the merged image"
    "$SCRATCH/build-portable/container_cli" check "$KEYS" "$BASE" "$UPDATE" --modules "$X/exefs" --index "$INDEX" --random 20000 "$IMAGE"
    result $? "the same with the portable AES, fewer reads"
    ASAN_OPTIONS=detect_leaks=0 "$SCRATCH/build-san/container_cli" check "$KEYS" "$BASE" "$UPDATE" --modules "$X/exefs" --index "$INDEX" --random 20000 "$IMAGE"
    result $? "the same under ASan and UBSan"
  else
    echo "SKIP the merged image: none at $IMAGE"
  fi
else
  echo "SKIP the real files: none in $FILES"
fi

# ---- damage ----
if [ "$DOFUZZ" = 1 ]; then
  FZ=$SCRATCH/build-san/fuzz_malformed; pids=()
  for i in $(seq 1 "$J"); do
    ( "$FZ" "$FUZZ" "$i" ) > "$SCRATCH/fuzz-$i.log" 2>&1 & pids+=($!)
  done
  bad=0
  for p in "${pids[@]}"; do wait "$p" || bad=1; done
  tail -qn 1 "$SCRATCH/fuzz-1.log"
  result $bad "$J runs of damaged files under ASan and UBSan (logs: $SCRATCH/fuzz-*.log)"
  if [ "$LIBFUZZ" -gt 0 ]; then
    # -jobs, not -fork: libFuzzer's fork driver itself crashes here (clang 23), with nothing of ours on the stack
    mkdir -p "$SCRATCH/corpus" "$SCRATCH/libfuzzer"
    ( cd "$SCRATCH/libfuzzer" && "$SCRATCH/build-san/fuzz_libfuzzer" -max_total_time="$LIBFUZZ" -jobs="$J" -workers="$J" -max_len=512 -rss_limit_mb=4096 \
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
