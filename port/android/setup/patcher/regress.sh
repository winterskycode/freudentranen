#!/bin/bash
# The C rewriter against the Python one, DM_TARGET=android DM_X18=native. Run it after any change to patch_nso.py or this
# folder, and after an upstream sync.
#   port/android/setup/patcher/regress.sh [--scratch DIR] [--work DIR] [--synth N] [--mutants N] [--fuzz N] [--no-decode] [--no-fuzz]
# Reads game files from work/ (a worktree has none: --work or the main checkout's) and writes only to the scratch folder,
# which is a new temporary folder unless given. Steps, each a line of PASS or FAIL:
#   - every extracted game version: both rewriters, every output file compared; 1.4.3 also against work/android/v143/rewritten
#   - made-up modules and mutated real modules: same accept or refuse, same bytes
#   - Capstone built here decodes as the Python binding does (--no-decode skips it, about a minute)
#   - damaged modules under ASan and UBSan (--no-fuzz skips it)
# Exit status: 0 only when nothing failed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../../.." && pwd)
WORK=$ROOT/work; SCRATCH=; SYNTH=400; MUTANTS=48; FUZZ=2000; DECODE=1; DOFUZZ=1
while [ $# -gt 0 ]; do
  case $1 in
    --scratch) SCRATCH=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --synth) SYNTH=$2; shift 2 ;;
    --mutants) MUTANTS=$2; shift 2 ;;
    --fuzz) FUZZ=$2; shift 2 ;;
    --no-decode) DECODE=0; shift ;;
    --no-fuzz) DOFUZZ=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [ ! -d "$WORK/totk/extracted" ]; then
  COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
  WORK=${COMMON%/.git}/work
fi
[ -d "$WORK/totk/extracted" ] || { echo "no game files: give --work" >&2; exit 2; }
[ -n "$SCRATCH" ] || SCRATCH=$(mktemp -d)
mkdir -p "$SCRATCH"; SCRATCH=$(cd "$SCRATCH" && pwd)
PY=$WORK/venv/bin/python; TOOL=$ROOT/port/rewriter/d2r/patch_nso.py; J=$(nproc); FAILS=0
export PYTHONDONTWRITEBYTECODE=1 DM_TARGET=android DM_X18=native
echo "scratch: $SCRATCH"
result() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; FAILS=$((FAILS + 1)); fi; }

CAP=$ROOT/port/android/deps/src/capstone
[ -f "$CAP/cs.c" ] || { CAP=$SCRATCH/capstone; bash "$HERE/fetch_capstone.sh" "$CAP" || exit 2; }
for b in build:OFF build-san:ON; do
  dir=$SCRATCH/${b%:*}
  cmake -G Ninja -S "$HERE" -B "$dir" -DCAPSTONE_SRC="$CAP" -DTOKP_SANITIZE="${b#*:}" > "$dir.log" 2>&1 && cmake --build "$dir" -j "$J" >> "$dir.log" 2>&1 ||
    { echo "the build failed: $dir.log" >&2; exit 2; }
done
CLI=$SCRATCH/build/patcher_cli

want=$(grep -o 'TOKP_PY_SHA256 "[0-9a-f]*"' "$HERE/patcher.h" | cut -d'"' -f2)
have=$(sha256sum "$TOOL" | cut -d' ' -f1)
[ "$want" = "$have" ]; result $? "patcher.h names this patch_nso.py ($have)"

# Same files with the same bytes in two folders, ignoring the Python tool's log.
same_dirs() {
  local a=$1 b=$2 bad=0 f n
  for f in "$a"/*; do
    n=${f##*/}
    case $n in *.log) continue ;; esac
    cmp -s "$f" "$b/$n" || { echo "  differs: $n"; bad=1; }
  done
  for f in "$b"/*; do
    n=${f##*/}
    case $n in *.log) continue ;; esac
    [ -e "$a/$n" ] || { echo "  only in $b: $n"; bad=1; }
  done
  return $bad
}

# ---- the game versions on this machine ----
pids=()
for ex in "$WORK"/totk/extracted/*/exefs; do
  v=$(basename "$(dirname "$ex")")
  mkdir -p "$SCRATCH/py/$v" "$SCRATCH/c/$v"
  for m in main sdk subsdk0; do
    [ -f "$ex/$m" ] || continue
    ( cd "$SCRATCH" && "$PY" "$TOOL" "$ex/$m" "$SCRATCH/py/$v" "$m" > "$SCRATCH/py/$v/$m.log" 2>&1 ) & pids+=($!)
  done
done
for p in "${pids[@]}"; do wait "$p"; done
printf '%-10s %-8s %8s %12s\n' version module 'C ms' 'image bytes'
for ex in "$WORK"/totk/extracted/*/exefs; do
  v=$(basename "$(dirname "$ex")")
  for m in main sdk subsdk0; do
    [ -f "$ex/$m" ] || continue
    best=
    for i in 1 2 3 4 5; do
      t=$("$CLI" --time "$ex/$m" "$SCRATCH/c/$v" "$m" 2>&1 > "$SCRATCH/c/$v/$m.log" | sed -n 's/.*: \([0-9.]*\) ms$/\1/p')
      [ -z "$best" ] || [ "$(echo "$t < $best" | bc)" = 1 ] && best=$t
    done
    printf '%-10s %-8s %8s %12s\n' "$v" "$m" "$best" "$(stat -c %s "$SCRATCH/c/$v/$m.patched.bin")"
    grep -qxF "$(cat "$SCRATCH/c/$v/$m.log")" "$SCRATCH/py/$v/$m.log"; result $? "$v $m: summary line"
  done
  same_dirs "$SCRATCH/py/$v" "$SCRATCH/c/$v"; result $? "$v: Python and C, every file"
done
LIVE=$WORK/android/v143/rewritten
if [ -d "$LIVE" ]; then
  bad=0
  for f in "$SCRATCH"/c/v143/*.patched.bin; do cmp -s "$f" "$LIVE/${f##*/}" || { echo "  differs: ${f##*/}"; bad=1; }; done
  result $bad "v143: C and the live images in $LIVE"
fi

# ---- modules no game has ----
# one_case <module>: both rewriters must accept or refuse together; when they accept, every file must match.
one_case() {
  local f=$1 d n
  n=${f##*/}; d=$(dirname "$f")/out/$n
  mkdir -p "$d/py" "$d/c"
  "$PY" "$TOOL" "$f" "$d/py" m > "$d/py.log" 2>&1; local a=$?
  "$CLI" "$f" "$d/c" m > "$d/c.log" 2>&1; local b=$?
  if [ $((a != 0)) != $((b != 0)) ]; then echo "$n: Python $a, C $b"; return 1; fi
  if [ $a != 0 ]; then echo "$n: refused" ; rm -rf "$d"; return 0; fi
  same_dirs "$d/py" "$d/c" > "$d/diff.log" || { echo "$n: bytes differ"; return 1; }
  echo "$n: ok"; rm -rf "$d"
}
export -f one_case same_dirs
export PY TOOL CLI
# <dir> <label>
run_cases() {
  find "$1" -maxdepth 1 -type f | sort | xargs -P "$J" -I{} bash -c 'one_case {}' > "$1.results" 2>&1
  local ok refused bad
  ok=$(grep -c ': ok$' "$1.results"); refused=$(grep -c ': refused$' "$1.results"); bad=$(grep -vc -e ': ok$' -e ': refused$' "$1.results")
  [ "$bad" = 0 ] || grep -v -e ': ok$' -e ': refused$' "$1.results" | head -20
  [ "$ok" -gt 0 ] && [ "$bad" = 0 ]; result $? "$2: $ok same bytes, $refused refused by both, $bad disagreements"
}
if [ "$SYNTH" -gt 0 ]; then
  "$PY" -I "$HERE/tests/gen_synth.py" "$SCRATCH/synth" "$SYNTH" 1 && run_cases "$SCRATCH/synth" "made-up modules"
fi
SDK=$WORK/totk/extracted/v143/exefs/sdk
if [ "$MUTANTS" -gt 0 ] && [ -f "$SDK" ]; then
  "$PY" -I "$HERE/tests/gen_synth.py" --mutate "$SDK" "$SCRATCH/mutants" "$MUTANTS" 1 && run_cases "$SCRATCH/mutants" "mutated 1.4.3 sdk"
  rm -f "$SCRATCH"/mutants/mutant_*
fi

# ---- Capstone ----
if [ "$DECODE" = 1 ] && [ -f "$SCRATCH/py/v143/main.patched.bin" ]; then
  end=$(sed -n 's/^# TEXT_END\t//p' "$SCRATCH/py/v143/main.fixups.tsv")
  "$PY" -I "$HERE/tests/decode_check.py" "$SCRATCH/build/decode_dump" "$SCRATCH/py/v143/main.patched.bin" "$end"; result $? "Capstone: C and Python decode alike"
fi

# ---- damage ----
if [ "$DOFUZZ" = 1 ]; then
  FZ=$SCRATCH/build-san/fuzz_malformed; pids=()
  export ASAN_OPTIONS=allocator_may_return_null=1:detect_leaks=1
  for i in $(seq 1 $((J - 2))); do
    module=$SCRATCH/synth/case_shapes
    [ $((i % 2)) = 0 ] && module=$SCRATCH/synth/$(printf 'case_%05d' "$i")
    ( "$FZ" "$module" "$FUZZ" "$i" ) > "$SCRATCH/fuzz-made-up-$i.log" 2>&1 & pids+=($!)
  done
  for m in main sdk subsdk0; do
    ( "$FZ" "$WORK/totk/extracted/v143/exefs/$m" 60 7 ) > "$SCRATCH/fuzz-$m.log" 2>&1 & pids+=($!)
  done
  bad=0
  for p in "${pids[@]}"; do wait "$p" || bad=1; done
  tail -qn 1 "$SCRATCH"/fuzz-main.log "$SCRATCH"/fuzz-sdk.log "$SCRATCH"/fuzz-subsdk0.log "$SCRATCH"/fuzz-made-up-1.log "$SCRATCH"/fuzz-made-up-2.log
  result $bad "damaged modules under ASan and UBSan (logs: $SCRATCH/fuzz-*.log)"
fi

echo "failures: $FAILS"
[ "$FAILS" = 0 ]
