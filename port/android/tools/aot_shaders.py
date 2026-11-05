#!/usr/bin/env python3.14
"""Shaders translated ahead of time on the PC, so the game does not stop to translate them while it plays.

    work/venv/bin/python port/android/tools/aot_shaders.py --profile <request-profile.json> [--jobs N]
        -> work/android/v143/shader-aot/<profile digest>/ (then tools/push_shaders.sh puts it on the device)

Every shader in the RomFS and the game modules (MacNativeGames' prepare_shader_seed.py inventory), translated for the
default bind state, and every bind state MacNativeGames' shared shader list (assets/shader_list.tsv, V rows) names for
it, and with --learned every variant the device's own cache holds (its .variants and .deps: the vertex input and colour
output types a shader is drawn with come from the bound buffers and targets, which the shader alone does not tell). The
list is made mostly on other game versions, whose shaders differ: for 1.4.3 it names about 100 shaders. The requests are the device's (vk/nvn_vulkan_totk_shader.inc): the same fields, and the device-dependent ones
(depth mode, subgroup sizes) from the request-profile.json the device writes into its shader cache, so a variant here is
the one the device would make. The device finds variants by their .deps, not by name. Each variant also gets
<key>.aot, which the session warm-up (vk/nvn_vulkan_warm.inc) skips until the game first uses the variant.

The output is made from the player's own game files: it stays in work/, never in the repository.
"""
import argparse, hashlib, json, re, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT/'MacNativeGames/games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
import prepare_shader_seed as seed   # noqa: E402
from shader_list import ShaderList   # noqa: E402

# the device's --chain (totk_service_start)
CHAIN = 'android-vk1'
VERSION = 'v143'
# an unbound slot's answer (totk_deps_match)
DEFAULT = dict(tex='2d', attr='float', fout='float')


def answers_text(answers):
    """The answers as the device writes them into a request (totk_answers_json): tex, img, attr, fout, slots in order."""
    parts = []
    for kind in ('tex', 'img', 'attr', 'fout'):
        slots = sorted(answers.get(kind, {}).items(), key=lambda kv: int(kv[0]))
        parts.append(f'"{kind}": ' + json.dumps(dict(slots)))
    return ', '.join(parts)


def learned_answers(deps_text):
    """A device .deps file ("tex 3 2d", "img 1 2d R8G8B8A8Unorm", "attr 0 sint", "fout 0 uint") as the answers it was
    translated with: the bind state that makes the same variant."""
    answers = dict(tex={}, img={}, attr={}, fout={})
    for line in deps_text.splitlines():
        f = line.split()
        if len(f) >= 3 and f[0] in ('tex', 'attr', 'fout'):
            answers[f[0]][f[1]] = f[2]
        elif len(f) == 4 and f[0] == 'img':
            answers['img'][f[1]] = {k: v for k, v in (('type', f[2]), ('fmt', f[3])) if v != '-'}
    return answers


def serves(deps, answers):
    """Whether a variant with these deps is the one the device would pick for these answers (totk_deps_match)."""
    for kind, slots in deps.items():
        for slot, want in slots.items():
            have = answers.get(kind, {}).get(slot)
            if kind == 'img':
                if (have or {}) != want:
                    return False
            elif (have or DEFAULT[kind]) != want:
                return False
    return True


class Translator:
    """One translator service (totk_xlate --service --spirv-only), as the device runs it."""
    def __init__(self, exe, out):
        self.p = subprocess.Popen([str(exe), '--service', '--spirv-only', '--out', str(out), '--chain', CHAIN],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(self, request):
        self.p.stdin.write(json.dumps(request) + '\n'); self.p.stdin.flush()
        reply = self.p.stdout.readline()
        if not reply:
            raise RuntimeError('translator exited')
        return json.loads(reply)

    def close(self):
        self.p.stdin.close(); self.p.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--profile', required=True, type=Path, help="the device's <shader cache>/request-profile.json")
    ap.add_argument('--romfs', type=Path, default=ROOT/f'work/totk/extracted/{VERSION}/romfs')
    ap.add_argument('--modules', type=Path, default=ROOT/f'work/android/{VERSION}/rewritten')
    ap.add_argument('--list', type=Path, default=ROOT/'MacNativeGames/games/totk/assets/shader_list.tsv')
    ap.add_argument('--translator', type=Path, default=ROOT/'port/xlate/linux-x64/totk_xlate')
    ap.add_argument('--learned', type=Path, help="a copy of the device's shader cache (its .variants and .deps files)")
    ap.add_argument('--jobs', type=int, default=16)
    ap.add_argument('--limit', type=int, default=0, help='only the first N shaders (a trial run)')
    args = ap.parse_args()

    profile = json.loads(args.profile.read_text())
    if profile.get('chain') != CHAIN:
        sys.exit(f'{args.profile}: chain {profile.get("chain")!r}, this tool makes {CHAIN!r}')
    digest = hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest()[:12]
    out = ROOT/f'work/android/{VERSION}/shader-aot/{digest}'
    out.mkdir(parents=True, exist_ok=True)
    (out/'request-profile.json').write_text(json.dumps(profile, indent=1) + '\n')
    # the translator here has every fix the device's cache revisions stand for (host/app.c SHADER_CACHE_REVISION): without
    # the file the app would drop translations from this cache as if an older translator had made them
    revision = int(re.search(r'#define SHADER_CACHE_REVISION (\d+)', (ROOT/'port/android/host/app.c').read_text())[1])
    (out/'xlate-revision.txt').write_text(f'{revision}\n')

    # every shader of the game: the RomFS packages and modules, and what the shared list locates elsewhere
    identity = dict(schema=seed.INVENTORY_SCHEMA, game='totk',
                    source_romfs_sha256=json.loads((args.romfs/'index.json').read_text())['romfs_sha256'],
                    modules={p.name: seed.digest(p.read_bytes()) for p in sorted(args.modules.glob('*.patched.bin'))},
                    preparation_sha256=seed.digest((SCRIPTS/'prepare_shader_seed.py').read_bytes()))
    _, shaders, startup, _ = seed.cached_inventory(args.romfs, args.modules, 'totk', out.parent/'inventory', identity)
    shared = ShaderList.load(args.list)
    extra = seed.located({w for w in shared.locators if w[0] not in shaders}, seed.source_reader(args.romfs, args.modules), 'totk')
    shaders.update(extra)
    jobs = seed.plan(shaders, startup, shared.variants, all_default=True)
    by_shader = {}
    for key, answers, _, why in jobs:
        a = {k: answers.get(k, {}) for k in ('tex', 'img', 'attr', 'fout')}
        by_shader.setdefault(key, []).append((a, why))
    learned = 0
    if args.learned:
        for vf in sorted(args.learned.glob('*.variants')):
            if vf.stem not in shaders:
                continue
            for variant in vf.read_text().split():
                d = args.learned/f'{variant}.deps'
                if d.is_file():
                    by_shader.setdefault(vf.stem, []).append((learned_answers(d.read_text()), 'device')); learned += 1
    keys = sorted(by_shader)[:args.limit or None]
    print(f'{len(shaders)} shaders ({len(extra)} located by the list), {learned} variants the device made, '
          f'{sum(len(by_shader[k]) for k in keys)} jobs for '
          f'{len(keys)} of them; output {out}', flush=True)

    lock = threading.Lock()
    pending = iter(keys)
    tally = dict(translated=0, reused=0, served=0, same_deps=0, failed=0)
    failures = []
    started = time.monotonic()

    def worker():
        service = None
        try:
            while True:
                with lock:
                    key = next(pending, None)
                if key is None:
                    return
                stage, control, code, _ = shaders[key]
                # (variant, deps) of this shader, in job order
                made = []
                for answers, why in by_shader[key]:
                    if any(serves(deps, answers) for _, deps in made):
                        with lock: tally['served'] += 1
                        continue
                    text = answers_text(answers)
                    variant = hashlib.sha256((key + text).encode()).hexdigest()[:16]
                    v_json = out/f'{variant}_v.json'
                    if not (v_json.exists() and (out/f'{variant}.spv').exists() and (out/f'{variant}.deps').exists()):
                        code_path = out/f'{variant}.code'
                        request = seed.translation_request(variant, stage, control, code, answers, code_path)
                        # the device adds the mode itself (spirv_add_early_tests)
                        request.pop('early_fragment_tests', None)
                        request.update(profile[stage])
                        if service is None:
                            service = Translator(args.translator, out)
                        reply = service.ask(request)
                        code_path.unlink(missing_ok=True)
                        if not reply.get('ok'):
                            with lock:
                                tally['failed'] += 1; failures.append(f'{key} {stage} {why} {variant}: {reply}')
                            continue
                        with lock: tally['translated'] += 1
                    else:
                        with lock: tally['reused'] += 1
                    deps = json.loads(v_json.read_text())['deps']
                    # the translator's deps show it is a variant this shader has
                    if any(deps == d for _, d in made):
                        for suffix in ('.spv', '.meta', '.deps', '.res.json', '_v.json', '.aot'):
                            (out/f'{variant}{suffix}').unlink(missing_ok=True)
                        with lock: tally['same_deps'] += 1
                        continue
                    (out/f'{variant}.aot').touch()
                    made.append((variant, deps))
                (out/f'{key}.variants').write_text(''.join(v + '\n' for v, _ in made))
                with lock:
                    done = sum(tally.values())
                    if done and done % 500 < len(by_shader[key]):
                        print(f'  {done} jobs, {time.monotonic() - started:.0f} s: {tally}', flush=True)
        finally:
            if service:
                service.close()

    threads = [threading.Thread(target=worker) for _ in range(args.jobs)]
    for t in threads: t.start()
    for t in threads: t.join()
    (out/'failures.txt').write_text(''.join(f + '\n' for f in failures))
    size = sum(p.stat().st_size for p in out.iterdir() if p.is_file())
    print(f'done in {time.monotonic() - started:.0f} s: {tally}; {size >> 20} MB; failures in {out/"failures.txt"}')


if __name__ == '__main__':
    main()
