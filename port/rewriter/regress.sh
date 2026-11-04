#!/bin/sh
# Rewriter regression: the merged rewriter with DM_TARGET=apple must reproduce every pinned TOTK hash (all versions with an
# extracted ExeFS), all modules at once. usage: port/rewriter/regress.sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd); PY=$ROOT/work/venv/bin/python; OUT=$ROOT/work/regress
for v in v0 v112 v121 v143; do
  EX=$ROOT/work/totk/extracted/$v/exefs; [ -d "$EX" ] || continue; mkdir -p $OUT/$v
  for m in main sdk subsdk0; do
    (cd $ROOT/port/rewriter/d2r && DM_TARGET=apple $PY patch_nso.py $EX/$m $OUT/$v $m > $OUT/$v/$m.log 2>&1) &
  done
done
wait
python3 -I - "$ROOT" <<'PY'
import hashlib, json, sys, pathlib
root = pathlib.Path(sys.argv[1]); bad = 0
names = {'v0': '1.0.0', 'v112': '1.1.2', 'v121': '1.2.1', 'v143': '1.4.3'}
for v, n in names.items():
    d = root / 'work/regress' / v
    if not d.exists(): continue
    e = json.loads((root / f'MacNativeGames/games/totk/assets/expected-v{n}.json').read_text())['rewritten_sha256']
    for m in ('main', 'sdk', 'subsdk0'):
        p = d / f'{m}.patched.bin'
        ok = p.exists() and hashlib.sha256(p.read_bytes()).hexdigest() == e[f'{m}.patched.bin']
        bad += not ok; print(f'{v} {m}: {"MATCH" if ok else "DIFF"}')
sys.exit(1 if bad else 0)
PY
