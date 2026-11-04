# 改写结构核对：不跑代码，逐字核对改写器产物。分母用独立反汇编器（llvm-objdump）重数，不依赖改写器自己的 capstone 扫描。
# 用法：structck.py <改写产物目录> <模块> <NSO>     输出末行 STRUCT <模块> OK|FAIL ...，退出码 0/1
# 核对项：
#  ⓪ 已登记剪口（cuts.tsv）：构建号对得上，原映像该处 == 原字节、改写映像 == 新字节、理由非空，不与改写点 / 窗口重叠；剪口引入的宿主调用门（svc #N≥0x80）按 hle.h 登记行核号、偏移与形态（入口形 svc;ret 在函数入口 / 调用点形原指令是 bl 且目标符登记）
#  ① 非 text 段除剪口外与原映像逐字节相同，映像尾到补丁区之间全 0
#  ② text 改动的字 == 改写点头（sites.tsv 非 J 行 + 窗口起点）∪ text 内剪口，改写点头都是跳进补丁区的 B
#  ③ 独立分母：llvm-objdump 读原 text，x18/w18 操作数、tpidr_el0 读写、tpidrro_el0 读、svc、cntpct/cntvct
#     全部落在改写点或窗口内；反向，每个改写点都在独立集合里（窗口起点除外）
#  ④ 补丁区里不再有 x18/w18、tpidr_el0、svc、cntpct/cntvct
#  ⑤ 宿主门（svc / 计时）块逐字等于模板，gates.tsv 一一对上（返回点、种类、svc 号 / 目标寄存器、原地址）
#  ⑧ 参数门（params.tsv）：原两字为 64 位 movz/movk 造常数、新两字 = mrs tpidrro / ldr 键槽、中间不碰该寄存器、区间无分支目标、键号登记、同键原值唯一
#  ⑦ 已登记观测门（obs.tsv）：原映像该处 == 登记原指令字；原指令可搬（非 PC 相对 / 非分支 / 不涉 x18 / 不读写系统寄存器，capstone 判）；
#     可在函数中间（门 ABI 全存回，gatefn 核）；不与改写点 / 窗口 / 剪口重叠；门块逐字 = 模板（svc 门同构前半 + 原指令 + b 回 off+4），gates.tsv 记 obs 行
#  ⑩ 跳板出口：每个跳进补丁区的 b，补丁块回 text 的出口只能是 原处+4 / 吸收段尾 / 窗口尾 / 段内原指令的分支目标（已登记剪口放行并列出）
#  ⑪ 剪口引入的分支门（hle.h 登记「分支门」行）：块逐字 = 模板、两路恢复 x8、gates.tsv 对上
#  ⑥ 线程槽：表头四个键号互不相同且 < 512；补丁区每条 mrs xN, tpidrro_el0 之后（跳过不写 xN 的 adrp/add）都是 ldr/str xM,[xN,#键号×8]
# Rewrite structural check: no code is run, the rewriter's build output is checked word by word. Denominators are recounted with an independent disassembler (llvm-objdump), not relying on the rewriter's own capstone scan.
# Usage: structck.py <rewrite build output dir> <module> <NSO>     last output line STRUCT <module> OK|FAIL ..., exit code 0/1
# Checks:
#  ⓪ Registered cuts (cuts.tsv): build ID matches, original image at that site == original bytes, rewritten image == new bytes, reason non-empty, no overlap with rewrite sites / windows; host-call gates introduced by cuts (svc #N≥0x80) have number, offset and form checked against the hle.h registry row (entry form svc;ret at a function entry / call-site form where the original instruction is bl and the target matches the registration)
#  ① Non-text segments are byte-for-byte identical to the original image except for cuts; everything from the end of the image to the patch area is 0
#  ② Changed words in text == rewrite site heads (non-J rows of sites.tsv + window starts) ∪ cuts inside text; all rewrite site heads are a B into the patch area
#  ③ Independent denominator: llvm-objdump reads the original text; x18/w18 operands, tpidr_el0 reads/writes, tpidrro_el0 reads, svc, cntpct/cntvct
#     all fall within rewrite sites or windows; conversely, every rewrite site is in the independent set (except window starts)
#  ④ The patch area no longer contains x18/w18, tpidr_el0, svc, cntpct/cntvct
#  ⑤ Host gate (svc / timer) blocks equal the template word for word, and match gates.tsv one to one (return point, kind, svc number / destination register, original address)
#  ⑧ Parameter gates (params.tsv): the two original words build a constant with 64-bit movz/movk, the two new words = mrs tpidrro / ldr key slot, the register is not touched in between, no branch target in the range, key number registered, a unique original value per key
#  ⑦ Registered observation gates (obs.tsv): original image at that site == the registered original instruction word; the original instruction is relocatable (not PC-relative / not a branch / does not involve x18 / does not read or write system registers, judged by capstone);
#     may be in the middle of a function (the gate ABI saves and restores everything, checked by gatefn); no overlap with rewrite sites / windows / cuts; the gate block = the template word for word (the same first half as an svc gate + original instruction + b back to off+4), gates.tsv records an obs row
#  ⑩ Trampoline exits: for every b jumping into the patch area, the patch block's exits back to text may only be original site+4 / end of the absorbed span / end of the window / a branch target of an original instruction within the span (registered cuts are let through and listed)
#  ⑪ Branch gates introduced by cuts (hle.h registry 「分支门」(branch gate) rows): block = template word for word, x8 restored on both paths, matches gates.tsv
#  ⑥ Thread slots: the four key numbers in the header are mutually distinct and < 512; every mrs xN, tpidrro_el0 in the patch area is followed (skipping adrp/add that do not write xN) by ldr/str xM,[xN,#key number×8]
import sys, os, re, struct, subprocess, tempfile
sys.path.insert(0, os.path.dirname(__file__))
import nsoimg
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作区根目录（游戏文件、产物、运行现场都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)

LLVM = os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm@23/bin')
out_dir, mod, nso = sys.argv[1:4]
# Android 目标自动识别（产物带 <mod>.target）；也可用第 4 参 --target 覆盖。
# Android target auto-detection (build output carries <mod>.target); the 4th arg --target can override.
TARGET = None   # None = 自动识别；显式 --target 优先（含 --target apple，不被 <mod>.target 覆盖 M7） | None = auto-detect; an explicit --target wins (including --target apple, not overridden by <mod>.target, M7)
_args = sys.argv[4:]
for i, a in enumerate(_args):
    if a == '--target': TARGET = _args[i + 1]
    elif a.startswith('--target='): TARGET = a.split('=', 1)[1]
if TARGET is None:
    TARGET = 'android' if (os.path.exists(f'{out_dir}/{mod}.target') and open(f'{out_dir}/{mod}.target').read().strip() == 'android-context-v1') else 'apple'
ANDROID = TARGET == 'android'
orig = nsoimg.load(nso)
h = nsoimg.header(out_dir, mod)
PO, TE = h['PATCH_OFF'], h['TEXT_END']
pat = open(f'{out_dir}/{mod}.patched.bin', 'rb').read()
W = lambda b, o: struct.unpack_from('<I', b, o)[0]
fails = []
def bad(msg):
    fails.append(msg)
    if len(fails) <= 30: print('FAIL', msg)

def llvm_dis(blob, base):
    """按原始字节反汇编，返回相关行 {偏移: 小写指令文本}。 | Disassemble from raw bytes and return the relevant lines {offset: lowercase instruction text}."""
    with tempfile.TemporaryDirectory() as td:
        b, e = os.path.join(td, 'x.bin'), os.path.join(td, 'x.elf')
        open(b, 'wb').write(blob)
        subprocess.run([f'{LLVM}/llvm-objcopy', '-I', 'binary', '-O', 'elf64-littleaarch64',
                        '--rename-section', '.data=.text,code', b, e], check=True)
        # 只留可能相关的行（x18/w18、tpidr、svc、cnt?ct），几百万行的筛选交给 grep
        # Keep only potentially relevant lines (x18/w18, tpidr, svc, cnt?ct); filtering millions of lines is left to grep
        od = subprocess.Popen([f'{LLVM}/llvm-objdump', '-d', '--no-show-raw-insn', e], stdout=subprocess.PIPE)
        r = subprocess.run(['/usr/bin/grep', '-iE', r'[xw]18|tpidr|svc|cnt[pv]ct'], stdin=od.stdout, capture_output=True, text=True)
        od.stdout.close(); assert od.wait() == 0
    out = {}
    for line in r.stdout.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)$', line)
        if m: out[base + int(m.group(1), 16)] = m.group(2).split('//')[0].split('<')[0].strip().lower()
    return out

RX18 = re.compile(r'\b[xw]18\b')
def sysk(t):
    if re.match(r'svc\s', t): return 'svc'
    if re.match(r'mrs\s', t) and 'tpidr_el0' in t: return 'tpidr_r'
    if re.match(r'msr\s', t) and 'tpidr_el0' in t: return 'tpidr_w'
    if re.match(r'mrs\s', t) and 'tpidrro_el0' in t: return 'tpidrro_r'
    if re.match(r'mrs\s', t) and 'cntpct_el0' in t: return 'cntpct'
    if re.match(r'mrs\s', t) and 'cntvct_el0' in t: return 'cntvct'
    return None

# ---- sites.tsv ----
sites = {}; wins = []; jumps = 0; calls = {}; jt_heads = set()
for l in open(f'{out_dir}/{mod}.sites.tsv'):
    f = l.split(); a = int(f[0], 16)
    if f[1] == 'J': jumps += 1; jt_heads.add(a); continue     # 跳转表头：⑫ 把它当「已解析的 br」，不保守当提交点 | jump-table head: ⑫ treats it as a resolved br, not conservatively a commit point
    if f[1] == 'W': wins.append((a, int(f[2], 16))); sites[a] = 'W'
    elif f[1] == 'C':
        end = int(f[2], 16)
        if end != a + 8: bad(f'call window must have two words {a:#x}')
        calls[a] = end - 4; wins.append((a, end)); sites[a] = 'C'
    else: sites[a] = f[1]
inwin = set(o for lo, hi in wins for o in range(lo, hi, 4))
atomic_groups = nsoimg.exclusive_groups(orig, [(lo, hi) for lo, hi in wins if lo not in calls])

# ⓪ 已登记剪口
# ⓪ Registered cuts
cuts = []; bid = None
for l in open(f'{out_dir}/{mod}.cuts.tsv'):
    if l.startswith('# BUILD_ID'): bid = l.split()[-1]; continue
    f = l.rstrip('\n').split('\t')
    if len(f) == 4: f = f[:3] + ['全'] + f[3:]                      # 旧 4 列格式：作用域缺省「全」 | Old 4-column format: scope defaults to 「全」(all)
    off, ob, nb, scope, why = f[:5]
    if scope not in ('全', '仅宿主'): bad(f'剪口 {off} 作用域不认识：{scope}')
    cuts.append((int(off, 16), bytes.fromhex(ob), bytes.fromhex(nb), why))
if bid != open(nso, 'rb').read(0x50)[0x40:0x50].hex(): bad(f'cuts.tsv 构建号 {bid} 与 NSO 不符')
cutb = set()
for off, ob, nb, why in cuts:
    if orig[off:off + len(ob)] != ob: bad(f'剪口 {off:#x} 原字节不符')
    for k in range(0, len(nb), 4):                               # 新字节里的 svc 字随后被改写成门（头是 B），按改写点核，这里跳过 | The svc words in the new bytes are subsequently rewritten into gates (head is B) and checked as rewrite sites; skipped here
        if int.from_bytes(nb[k:k + 4], 'little') & 0xFFE0001F == 0xD4000001: continue
        if pat[off + k:off + k + len(nb[k:k + 4])] != nb[k:k + 4]: bad(f'剪口 {off:#x}+{k} 改写映像不是新字节'); break
    if not why.strip() or len(ob) != len(nb): bad(f'剪口 {off:#x} 理由为空或长度不等')
    cutb |= set(range(off, off + len(ob)))
# 剪口产生的宿主调用门：剪口新字节里解出 svc #N 的那个字（N ≥ 0x80，须在 hle.h 宿主调用号登记表 SVC_HOST_*）
# Host-call gates produced by cuts: the word in the cut's new bytes that decodes to svc #N (N ≥ 0x80, must be in the hle.h host-call number registry SVC_HOST_*)
_hostcall = {}
registry_path = os.environ.get('DM_HOST_REGISTRY', os.path.join(os.path.dirname(__file__), '..', 'host', 'hle.h'))
registry_text = open(registry_path).read() if os.path.isfile(registry_path) else ''
_reg = {int(v, 16) for v in re.findall(r'SVC_HOST_\w+\s*=\s*(0x[0-9a-fA-F]+)', registry_text)}
for off, ob, nb, why in cuts:
    for k in range(0, len(nb), 4):
        w = int.from_bytes(nb[k:k + 4], 'little')
        if w & 0xFFE0001F == 0xD4000001:
            n = (w >> 5) & 0xFFFF; a = off + k
            if n < 0x80: bad(f'剪口 {off:#x} 引入 svc #{n:#x}：0x80 以下是 Horizon 号段，剪口不得占用')
            elif n not in _reg: bad(f'剪口 {off:#x} 引入 svc #{n:#x}：未在 hle.h 宿主调用号登记表')
            elif sites.get(a) != 'svc': bad(f'剪口 {off:#x} 引入的 svc #{n:#x} 没被改写成宿主门（sites.tsv 无 {a:#x} svc）')
            _hostcall[a] = n
# 宿主调用门的形态按 hle.h 登记表那一行核（「号 … 剪口（main 偏移）」列）：
#   入口形（「…入口」）：新字节 = svc #N ; ret，偏移是 eh_frame 函数入口——门保存恢复 x30，ret 回原调用者；
#   调用点形（「bl 0x目标 …」）：原指令必须是 bl 且目标 = 登记目标，新字节 = 单条 svc #N，门返回 off+4（⑤ 模板的末条 b 核）。
#   调用点形的寄存器理由：bl 处按 AAPCS 被调函数本可破坏 x9..x17、x16/x17、x30 与 NZCV，所以门在这些上的副作用不改变调用方可见行为；
#   x0..x7 返回值与 x19..x28 / sp 由宿主门契约保持（gatefn 逐门核）。
# The form of a host-call gate is checked against its row in the hle.h registry (the 「号 … 剪口（main 偏移）」(number ... cut (main offset)) column):
#   entry form (「…入口」(... entry)): new bytes = svc #N ; ret, the offset is an eh_frame function entry - the gate saves and restores x30, ret returns to the original caller;
#   call-site form (「bl 0x目标 …」(bl 0x<target> ...)): the original instruction must be bl with target = the registered target, new bytes = a single svc #N, the gate returns to off+4 (checked by the final b of the ⑤ template).
#   Register rationale for the call-site form: at a bl, per AAPCS the callee could clobber x9..x17, x16/x17, x30 and NZCV anyway, so the gate's side effects on these do not change caller-visible behavior;
#   the x0..x7 return values and x19..x28 / sp are preserved by the host gate contract (checked gate by gate by gatefn).
_hreg = {}
for l in registry_text.splitlines():
    m = re.match(r'//\s+(0x[0-9a-fA-F]+)\s.*?\s(0x[0-9a-fA-F]+)（([^）]*)', l)
    if m: _hreg[int(m[1], 16)] = (int(m[2], 16), m[3])
def _bl(w, o): return o + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) if w & 0xFC000000 == 0x94000000 else None
for a, n in _hostcall.items():
    if n not in _hreg: bad(f'宿主调用 svc #{n:#x}：hle.h 登记表没有对应行'); continue
    roff, form = _hreg[n]
    if mod != 'main' or roff != a: bad(f'宿主调用 svc #{n:#x} 在 {mod} {a:#x}，登记表写 main {roff:#x}'); continue
    nbw = next(c[2] for c in cuts if c[0] <= a < c[0] + len(c[2]))
    mt = re.search(r'\bbl (0x[0-9a-fA-F]+)', form)
    if mt:
        t = _bl(W(orig, a), a)
        if t is None: bad(f'宿主调用 svc #{n:#x} {a:#x}：登记为调用点形，原指令 {W(orig, a):#010x} 不是 bl')
        elif t != int(mt[1], 16): bad(f'宿主调用 svc #{n:#x} {a:#x}：原 bl 目标 {t:#x} ≠ 登记 {mt[1]}')
        if len(nbw) != 4: bad(f'宿主调用 svc #{n:#x} {a:#x}：调用点形剪口只许换这一条指令')
    elif '入口' in form:
        if a not in set(nsoimg.func_starts(orig, TE)): bad(f'宿主调用 svc #{n:#x} {a:#x}：登记为入口形，不是 eh_frame 函数入口')
        if nbw[4:8] != (0xD65F03C0).to_bytes(4, 'little'): bad(f'宿主调用 svc #{n:#x} {a:#x}：入口形新字节不是 svc ; ret')
    else: bad(f'宿主调用 svc #{n:#x}：登记表形态认不出（须含「bl 0x目标」或「入口」）：{form}')
if cutb & {a + i for a in list(sites) + list(inwin) if a not in _hostcall for i in range(4)}: bad('剪口与改写点 / 窗口重叠')
ncut_code = sum(1 for c in cuts if c[0] < TE); ncut_data = len(cuts) - ncut_code
obs = []
obs_p = f'{out_dir}/{mod}.obs.tsv'
if os.path.exists(obs_p):
    for l in open(obs_p, encoding='utf-8'):
        if l.startswith('# BUILD_ID'):
            if l.split()[-1] != open(nso, 'rb').read(0x50)[0x40:0x50].hex(): bad('obs.tsv 构建号与 NSO 不符')
            continue
        off, oid, ow, why = l.rstrip('\n').split('\t', 3); obs.append((int(off, 16), int(oid), int(ow, 16), why))
obs_heads = {o for o, *_ in obs}
params = []                                                          # 参数门（params.tsv，r11 起）：(A 偏移, B 偏移, 寄存器号, 键名) | Parameter gates (params.tsv, since r11): (A offset, B offset, register number, key name)
par_p = f'{out_dir}/{mod}.params.tsv'
if os.path.exists(par_p):
    for l in open(par_p, encoding='utf-8'):
        if l.startswith('# BUILD_ID'):
            if l.split()[-1] != open(nso, 'rb').read(0x50)[0x40:0x50].hex(): bad('params.tsv 构建号与 NSO 不符')
            continue
        a, b, r, k, why = l.rstrip('\n').split('\t', 4)
        if not why.strip(): bad(f'参数门 {a} 理由为空')
        params.append((int(a, 16), int(b, 16), int(r.lstrip('x')), k))
par_words = {o for a, b, r, k in params for o in (a, b)}

# ① 非 text 段：先整块比，块不同再逐字节，差异只许落在剪口里（剪口字节已在 ⓪ 按应有值核过）
# ① Non-text segments: compare whole blocks first, then byte by byte where blocks differ; differences may only fall inside cuts (cut bytes were already checked against their expected values in ⓪)
nd = []
for blk in range(TE, len(orig), 4096):
    e = min(blk + 4096, len(orig))
    if pat[blk:e] != orig[blk:e]: nd += [o for o in range(blk, e) if pat[o] != orig[o] and o not in cutb]
if nd: bad(f'非 text 段与原映像不同（剪口以外）{len(nd)} 字节 例 {[hex(x) for x in nd[:5]]}')
if any(pat[len(orig):PO]): bad('映像尾到补丁区之间非 0')

# ② text 改动 == 改写点头
# ② text changes == rewrite site heads
diff = set()
for blk in range(0, TE & ~3, 4096):                    # 先整块比，块不同再逐字 | Compare whole blocks first, then word by word where blocks differ
    e = min(blk + 4096, TE & ~3)
    if orig[blk:e] != pat[blk:e]: diff |= {o for o in range(blk, e, 4) if W(orig, o) != W(pat, o)}
heads = set(sites)
cutw = {o & ~3 for o in cutb if o < TE}
_exp = (heads | set(calls.values()) | obs_heads | par_words) - cutw; _got = diff - cutw
if _got != _exp: bad(f'text 改动与改写点不一致：多改 {len(_got - _exp)} 漏改 {len(_exp - _got)} 例 {[hex(x) for x in sorted(_got ^ _exp)[:5]]}')
def btarget(o):
    w = W(pat, o)
    if w & 0xFC000000 != 0x14000000: return None
    imm = w & 0x3FFFFFF
    if imm & 0x2000000: imm -= 0x4000000
    return o + 4 * imm
nb = [a for a in heads | obs_heads if not (btarget(a) is not None and PO <= btarget(a) < len(pat))]
if nb: bad(f'改写点头不是跳进补丁区的 B：{len(nb)} 例 {[hex(x) for x in nb[:5]]}')

# ③ 独立分母
# ③ Independent denominator
def cached_dis(blob):
    """原映像是固定数据：相关行按 sha 缓存（llvm 换版本另起键），只在首次付反汇编的十来秒。 | The original image is fixed data: relevant lines are cached by sha (a new llvm version gets a new key), so the ten-odd seconds of disassembly are paid only the first time."""
    import hashlib, json
    cd = os.environ.get('DM_VCACHE', os.path.join(DM_ROOT, 'build/cache/dm/verify'))
    ver = os.path.basename(os.path.dirname(LLVM.rstrip('/')))
    p = os.path.join(cd, f'{mod}.{hashlib.sha256(blob).hexdigest()[:16]}.{ver}.json')
    if os.path.exists(p): return {int(k): v for k, v in json.load(open(p)).items()}
    d = llvm_dis(blob, 0)
    os.makedirs(cd, exist_ok=True); tmp = f'{p}.new.{os.getpid()}'; json.dump(d, open(tmp, 'w')); os.replace(tmp, p)
    return d
D = cached_dis(orig[:TE])
ind = {}
for o, t in D.items():
    if RX18.search(t): ind[o] = 'x18'
    k = sysk(t)
    if k: ind[o] = k
miss = sorted(o for o in ind if o not in sites and o not in inwin)
if miss: bad(f'独立反汇编有、改写点没覆盖：{len(miss)} 例 {[(hex(x), D[x]) for x in miss[:5]]}')
extra = sorted(a for a, k in sites.items() if k not in ('W', 'C') and a not in ind and a not in _hostcall)
if extra: bad(f'改写点不在独立集合里：{len(extra)} 例 {[(hex(x), D.get(x)) for x in extra[:5]]}')
kmis = sorted(a for a, k in sites.items() if k not in ('W', 'C') and a in ind and ind[a] != k)
if kmis: bad(f'改写点种类与独立判读不同：{len(kmis)} 例 {[(hex(x), sites[x], D[x]) for x in kmis[:5]]}')
nwin_ref = sum(1 for o in ind if o in inwin)

# ④ 补丁区干净
# ④ Patch area is clean
P = llvm_dis(pat[PO:], PO)
# Android 补丁区不得有任何系统寄存器访问（含 mrs tpidrro_el0）：Android 上 TPIDRRO_EL0 不是上下文基址（I3）。
# Apple 的键页序列用 mrs tpidrro_el0 取 TSD，故 Apple 允许 tpidrro_r。
# The Android patch area must contain no system-register access (including mrs tpidrro_el0): on Android TPIDRRO_EL0 is not the context base (I3).
# Apple's key-page sequence uses mrs tpidrro_el0 to fetch the TSD, so Apple allows tpidrro_r.
_allowed_sys = (None,) if ANDROID else (None, 'tpidrro_r')
left = sorted(o for o, t in P.items() if RX18.search(t) or sysk(t) not in _allowed_sys)
if left: bad(f'补丁区残留 x18/系统寄存器/svc：{len(left)} 例 {[(hex(x), P[x]) for x in left[:5]]}')

# ⑤ 宿主门
# ⑤ Host gates
KEY = {k[4:-4]: v for k, v in h.items() if k.startswith('KEY_') and k.endswith('_IDX')}
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
_md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); _md.detail = True

# ⑮ thread_base 宿主入口逐字核对（Android，M1b B4a / Codex C6）：补丁区每处宿主上下文入口都必须是三种固定形状之一，
# 且上下文经 blr x16 产出后由 mov xt,x16 搬给目的（不是别的源寄存器）。这封死「thread_base(30) 把 mov x30,x16 写成 mov x30,x0」这类错误，
# keyseq 只核 add Xt,Xt,Xk、核不到入口形状的盲点。PC 无关字固定：ldr x16,[x16,#0x40]=0xF9402210、blr x16=0xD63F0200、
# 压栈 str x30=0xF81F0FFE / str x16=0xF81F0FF0 / stp x16,x30=0xA9BF7BF0、mov x30,x16=0xAA1003FE / mov xt,x16=0xAA1003E0|t、
# 出栈 ldr x30=0xF84107FE / ldr x16=0xF84107F0 / ldp x16,x30=0xA8C17BF0；adrp x16,KEY 用 capstone 核。
# ⑮ Word-for-word check of every thread_base host entry (Android, M1b B4a / Codex C6): each host-context entry in the patch area must be one of the three fixed shapes,
# with the context produced by `blr x16` moved to the destination by `mov xt,x16` (not some other source register). This closes errors like "thread_base(30) writes mov x30,x0 instead of mov x30,x16",
# which keyseq (checking only add Xt,Xt,Xk) cannot see.
if ANDROID:
    _tbbad = []; _tbn = 0
    for o in range(PO, len(pat) - 12, 4):
        if W(pat, o) != 0xF9402210 or W(pat, o + 4) != 0xD63F0200: continue   # ldr x16,[x16,#0x40]; blr x16
        _tbn += 1
        ad = next(_md.disasm(pat[o - 4:o], o - 4), None)                      # 前一条须是 adrp x16, KEY | preceding must be adrp x16, KEY
        if not (ad and ad.mnemonic == 'adrp' and _md.reg_name(ad.operands[0].reg) == 'x16' and ad.operands[1].imm == h['KEY_PAGE_OFF']):
            _tbbad.append((o, 'no adrp x16,KEY')); continue
        push = W(pat, o - 8)
        if push == 0xF81F0FFE:                                               # 形状 t=16：str x30,[sp,#-16]! → ... → ldr x30,[sp],#16（无 mov）| shape t=16
            if W(pat, o + 8) != 0xF84107FE: _tbbad.append((o, f't16 pop {W(pat, o + 8):08x}'))
        elif push == 0xF81F0FF0:                                             # 形状 t=30：str x16 → mov x30,x16 → ldr x16 | shape t=30
            if W(pat, o + 8) != 0xAA1003FE: _tbbad.append((o, f't30 mov {W(pat, o + 8):08x}≠mov x30,x16'))
            elif W(pat, o + 12) != 0xF84107F0: _tbbad.append((o, f't30 pop {W(pat, o + 12):08x}'))
        elif push == 0xA9BF7BF0:                                             # 形状 t=其它：stp x16,x30 → mov xt,x16 → ldp x16,x30 | shape t=other
            mv = W(pat, o + 8)
            if mv & 0xFFFFFFE0 != 0xAA1003E0: _tbbad.append((o, f'tN mov {mv:08x}≠mov x?,x16'))
            elif (mv & 31) in (16, 18, 30, 31):                              # 目的若是 x16/x30 会被随后的 ldp 还原掉（上下文丢失），x18/xzr 不合法（Opus r2 N1）| dest x16/x30 is undone by the following ldp (context lost); x18/xzr illegal (Opus r2 N1)
                _tbbad.append((o, f'tN mov dest x{mv & 31} not allowed'))
            elif W(pat, o + 12) != 0xA8C17BF0: _tbbad.append((o, f'tN pop {W(pat, o + 12):08x}'))
        else:
            _tbbad.append((o, f'push {push:08x} 非三种压栈形状 | not one of the three push shapes'))
    # 经 x16 以外的寄存器取键页第 8 项并 blr 的「旁路入口」：ldr xN,[xN,#0x40]; blr xN（N≠16）一律判红，⑮ 的逐字核对才不会被绕过（Opus r2 N1）。
    # A "side entry" through a register other than x16 — ldr xN,[xN,#0x40]; blr xN (N!=16) — is flagged, so ⑮'s word check cannot be bypassed (Opus r2 N1).
    for o in range(PO, len(pat) - 8, 4):
        w = W(pat, o)
        if w & 0xFFFFFC00 == 0xF9402000 and (w & 31) == ((w >> 5) & 31) and (w & 31) != 16 and W(pat, o + 4) == (0xD63F0000 | ((w & 31) << 5)):
            _tbbad.append((o, f'host entry via x{w & 31} (only x16 allowed)'))
    if _tbbad: bad(f'thread_base 宿主入口逐字核对：{len(_tbbad)} 处不符三种固定形状（含上下文搬寄存器错）：例 ' + ', '.join(f'{x[0]:#x}:{x[1]}' for x in _tbbad[:5]))
    print(f'TBASE {mod} 宿主入口={_tbn} 不符={len(_tbbad)}')

def keyseq(at):
    """槽访问序列。尾部两边一致：adrp Xk,KEY ; ldr Xk,[Xk,#idx] ; add Xt,Xt,Xk。
    基址取得：Apple = mrs Xt,tpidrro_el0（1 条）；Android = 键页第 8 项 blr 宿主入口序列（含 adrp x16,KEY; ldr x16,[x16,#64]; blr x16）。
    返回 (add 之后的偏移, 槽下标 idx)。 | Slot-access sequence. The tail is identical on both targets: adrp Xk,KEY; ldr Xk,[Xk,#idx]; add Xt,Xt,Xk.
    Base acquisition: Apple = mrs Xt,tpidrro_el0 (1 instr); Android = the key-page[8] blr host-entry sequence. Returns (offset after the add, slot idx)."""
    q = list(_md.disasm(pat[at:at + 56], at))
    for i in range(len(q) - 2):
        a0, a1, a2 = q[i], q[i + 1], q[i + 2]
        if a0.mnemonic == 'adrp' and a0.operands[1].imm == h['KEY_PAGE_OFF'] \
           and a1.mnemonic == 'ldr' and a1.operands[1].mem.base == a0.operands[0].reg and a1.operands[0].reg == a0.operands[0].reg \
           and a2.mnemonic == 'add' and a2.operands[1].reg == a2.operands[0].reg and a2.operands[2].reg == a0.operands[0].reg:
            d = a1.operands[1].mem.disp
            if d % 8 != 0 or not (0 <= d // 8 < 8): continue
            if not ANDROID:
                if not (i == 1 and q[0].mnemonic == 'mrs' and 'tpidrro_el0' in q[0].op_str and q[0].operands[0].reg == a2.operands[0].reg): return None
            else:
                # 基址须由宿主入口产出：前导含 adrp x16,KEY; ldr x16,[x16,#64]; blr x16（键页第 8 项）
                # Base must come from the host entry: the preamble has adrp x16,KEY; ldr x16,[x16,#64]; blr x16 (key page index 8)
                pre = q[:i]
                if not any(x.mnemonic == 'blr' for x in pre): return None
                if not any(pre[j].mnemonic == 'ldr' and pre[j].operands[1].mem.disp == 8 * 8
                           and pre[j - 1].mnemonic == 'adrp' and pre[j - 1].operands[1].imm == h['KEY_PAGE_OFF']
                           for j in range(1, len(pre))): return None
            return (a2.address + 4, d // 8)
    return None
def paramseq_android(at, r):
    """Android 参数门跳板：sub sp,#16; str xtmp,[sp,#0]; <宿主入口>; adrp Xk,KEY; ldr Xk,[Xk,#idx]; add xr,xr,Xk; ldr xr,[xr]; ldr xtmp,[sp,#0]; add sp,#16; b。
    返回 (b 之后的偏移, 槽下标)。 | Android parameter trampoline. Returns (offset after b, slot idx)."""
    tmp = 17 if r == 16 else 16
    q = list(_md.disasm(pat[at:at + 72], at))
    if len(q) < 9 or q[0].mnemonic != 'sub' or q[0].op_str.split(',')[0] != 'sp' \
            or q[1].mnemonic != 'str' or q[1].reg_name(q[1].operands[0].reg) != f'x{tmp}' \
            or q[1].operands[1].mem.base != capstone.arm64_const.ARM64_REG_SP or q[1].operands[1].mem.disp != 0:
        return None
    for i in range(2, len(q) - 3):
        a0, a1, a2, a3 = q[i], q[i + 1], q[i + 2], q[i + 3]
        if a0.mnemonic == 'adrp' and a0.operands[1].imm == h['KEY_PAGE_OFF'] \
           and a1.mnemonic == 'ldr' and a1.operands[1].mem.base == a0.operands[0].reg and a1.operands[0].reg == a0.operands[0].reg \
           and a2.mnemonic == 'add' and a2.reg_name(a2.operands[0].reg) == f'x{r}' and a2.operands[1].reg == a2.operands[0].reg and a2.operands[2].reg == a0.operands[0].reg \
           and a3.mnemonic == 'ldr' and a3.reg_name(a3.operands[0].reg) == f'x{r}' and a3.operands[1].mem.base == a2.operands[0].reg and a3.operands[1].mem.disp == 0:
            pre = q[2:i]
            if not any(x.mnemonic == 'blr' for x in pre): return None
            d = a1.operands[1].mem.disp
            if d % 8 != 0 or not (0 <= d // 8 < 8): return None
            # 尾段必核（Codex C5：缺 add sp 会让 SP 偏 16 却仍通过）：ldr xtmp,[sp,#0]; add sp,sp,#16; b。
            # The tail must be checked (Codex C5: a missing add sp leaves SP off by 16 yet still passed): ldr xtmp,[sp,#0]; add sp,sp,#16; b.
            if i + 6 >= len(q): return None
            t0, t1, t2 = q[i + 4], q[i + 5], q[i + 6]
            if not (t0.mnemonic == 'ldr' and t0.reg_name(t0.operands[0].reg) == f'x{tmp}'
                    and t0.operands[1].mem.base == capstone.arm64_const.ARM64_REG_SP and t0.operands[1].mem.disp == 0): return None
            if W(pat, t1.address) != 0x910043FF: return None     # add sp, sp, #16
            if t2.mnemonic != 'b': return None
            return (t2.address + 4, d // 8)
    return None
# C windows intentionally change the call word as well as the preceding head.
# Inspect the actual decoded sequence, including the original link-producing PC.
def call_window_android(prefix, call, start):
    """prefix; str x16,[sp,#-16]!; adrp x16,KEY; ldr x16,[x16,#64]; blr x16; mov x30,x16; ldr x16,[sp],#16 (thread_base(30));
    str x16,[sp,#-16]!; adrp x16,KEY; ldr x16,[x16,#K18]; add x30,x30,x16; ldr x30,[x30]; ldr x16,[sp],#16; b call"""
    q = list(_md.disasm(pat[start:start + 56], start))
    if len(q) != 14: return False
    def adrp16(i): return q[i].mnemonic == 'adrp' and q[i].operands[1].imm == h['KEY_PAGE_OFF'] and q[i].reg_name(q[i].operands[0].reg) == 'x16'
    if W(pat, start) != W(orig, prefix): return False
    if W(pat, start + 4) != 0xF81F0FF0 or not adrp16(2) or W(pat, start + 12) != 0xF9402210 or W(pat, start + 16) != 0xD63F0200 \
            or W(pat, start + 20) != 0xAA1003FE or W(pat, start + 24) != 0xF84107F0: return False
    if W(pat, start + 28) != 0xF81F0FF0 or not adrp16(8): return False
    if q[9].mnemonic != 'ldr' or q[9].reg_name(q[9].operands[0].reg) != 'x16' or q[9].reg_name(q[9].operands[1].mem.base) != 'x16' \
            or q[9].operands[1].mem.disp != 8 * KEY['K18']: return False
    return W(pat, start + 40) == 0x8B1003DE and W(pat, start + 44) == 0xF94003DE and W(pat, start + 48) == 0xF84107F0 \
        and btarget(start + 52) == call
for prefix, call in calls.items():
    start = btarget(prefix)
    if start is None or not PO <= start < len(pat):
        bad(f'call preparation has no trampoline {prefix:#x}'); continue
    if ANDROID:
        first = next(_md.disasm(orig[prefix:prefix + 4], prefix), None)
        safe = first and first.mnemonic in ('str', 'stp', 'stur', 'ldr', 'ldp', 'ldur') and not first.writeback \
            and first.reg_name(first.operands[-1].mem.base) in ('sp', 'fp') and not first.operands[-1].mem.index \
            and (first.reg_name(first.operands[-1].mem.base) != 'sp' or first.operands[-1].mem.disp >= 0) and not RX18.search(first.op_str)
        if not safe or W(orig, call) != 0xD63F0240 or W(pat, call) != 0xD63F03C0 or not call_window_android(prefix, call, start):
            bad(f'android call window does not preserve store/scratch/link PC {prefix:#x}')
        continue
    decoded = list(_md.disasm(pat[start:start + 36], start))
    first = next(_md.disasm(orig[prefix:prefix + 4], prefix), None)
    safe = first and first.mnemonic in ('str', 'stp', 'stur', 'ldr', 'ldp', 'ldur') and not first.writeback \
        and first.reg_name(first.operands[-1].mem.base) in ('sp', 'fp') and not first.operands[-1].mem.index \
        and (first.reg_name(first.operands[-1].mem.base) != 'sp' or first.operands[-1].mem.disp >= 0) and not RX18.search(first.op_str)
    slot = keyseq(start + 8)
    if len(decoded) != 9 or not safe or W(pat, start) != W(orig, prefix) \
            or W(orig, call) != 0xD63F0240 or W(pat, call) != 0xD63F03C0 \
            or W(pat, start + 4) != 0xF81F03E0 or W(pat, start + 8) != 0xD53BD07E \
            or not slot or slot[1] != KEY['K18'] \
            or W(pat, start + 20) != 0x8B0003DE or W(pat, start + 24) != 0xF94003DE \
            or W(pat, start + 28) != 0xF85F03E0 or btarget(start + 32) != call:
        bad(f'call window does not preserve store/scratch/link PC {prefix:#x}')
    elif decoded[3].reg_name(decoded[3].operands[0].reg) != 'x0' \
            or decoded[4].reg_name(decoded[4].operands[0].reg) != 'x0':
        bad(f'call window key scratch mismatch {prefix:#x}')

def paramseq(at, r):
    if ANDROID: return paramseq_android(at, r)
    q = list(_md.disasm(pat[at:at + 40], at))
    tmp = 17 if r == 16 else 16
    rn = lambda ins, i: ins.reg_name(ins.operands[i].reg)
    if len(q) < 8 or q[0].mnemonic not in ('stur', 'str') or rn(q[0], 0) != f'x{tmp}' \
            or q[0].operands[1].mem.base != capstone.arm64_const.ARM64_REG_SP or q[0].operands[1].mem.disp not in (-16, -24) \
            or q[1].mnemonic != 'mrs' or rn(q[1], 0) != f'x{r}' \
            or 'tpidrro_el0' not in q[1].op_str or q[2].mnemonic != 'adrp' \
            or q[2].operands[1].imm != h['KEY_PAGE_OFF'] or q[3].mnemonic != 'ldr' \
            or q[3].operands[1].mem.base != q[2].operands[0].reg or q[4].mnemonic != 'add' \
            or q[4].operands[0].reg != q[1].operands[0].reg or q[4].operands[1].reg != q[1].operands[0].reg \
            or q[5].mnemonic != 'ldr' or q[5].operands[0].reg != q[1].operands[0].reg \
            or q[5].operands[1].mem.base != q[4].operands[0].reg or q[6].mnemonic not in ('ldur', 'ldr') \
            or rn(q[6], 0) != f'x{tmp}' \
            or q[6].operands[1].mem.base != capstone.arm64_const.ARM64_REG_SP \
            or q[6].operands[1].mem.disp != q[0].operands[1].mem.disp or q[7].mnemonic != 'b':
        return None
    d = q[3].operands[1].mem.disp
    return (q[6].address + 4, d // 8)
gates = {}
for l in open(f'{out_dir}/{mod}.gates.tsv'):
    r, k, arg, oa = l.split(); gates[int(r, 16)] = (k, int(arg), int(oa, 16))
def B(frm, to): return 0x14000000 | (((to - frm) >> 2) & 0x3FFFFFF)
# 门块 blr 前须有 ldr x29,[sp]：键页取槽借用的 x29 还原成客体原值，宿主读到的 x29 才是真帧指针（r26，见 patch_nso emit_gate_call）
# The gate block must have ldr x29,[sp] before blr: the x29 borrowed for fetching the slot via the key page is restored to the guest's original value, so the x29 the host reads is the real frame pointer (r26, see patch_nso emit_gate_call)
LDR_X29_SP = 0xF94003FD
gsites = sorted(a for a, k in sites.items() if k in ('svc', 'cntpct', 'cntvct'))
seen = set()
for a in gsites:
    s = btarget(a); ow = W(orig, a) if a not in _hostcall else (0xD4000001 | _hostcall[a] << 5)
    ks = keyseq(s + 4)
    if W(pat, s) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') \
            or W(pat, ks[0]) & 0xFFC00000 != 0xF9400000 or W(pat, ks[0] + 4) != LDR_X29_SP or W(pat, ks[0] + 8) != 0xD63F03C0 \
            or W(pat, ks[0] + 12) != 0xA8C17BFD or W(pat, ks[0] + 16) != B(ks[0] + 16, a + 4):
        bad(f'门块不符键页模板 {a:#x}'); continue
    arg = (ow >> 5) & 0xFFFF if sites[a] == 'svc' else ow & 31
    g = gates.get(ks[0] + 12)
    if g != (sites[a], arg, a): bad(f'gates.tsv 不符 {a:#x}：表 {g} 期望 {(sites[a], arg, a)}')
    seen.add(ks[0] + 12)
# ⑦ 观测门
# ⑦ Observation gates
_starts = set(nsoimg.func_starts(orig, TE))
_busy = {a + i for a in list(sites) + list(inwin) for i in range(4)} | cutb
for off, oid, ow, why in obs:
    if W(orig, off) != ow: bad(f'观测门 {off:#x} 原指令字不符：映像 {W(orig, off):08x} 登记 {ow:08x}'); continue
    ins = next(_md.disasm(struct.pack('<I', ow), off), None)
    m = ins.mnemonic if ins else '?'
    if ins is None or m in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz', 'svc', 'mrs', 'msr', 'eret') or m.startswith('b.') \
       or (m.startswith('ldr') and '[' not in ins.op_str) or RX18.search(ins.op_str or ''):
        bad(f'观测门 {off:#x} 原指令不可搬：{m} {ins.op_str if ins else ""}')
    # 不再要求函数入口：_dm_gate 存回 x0–x17、x19–x30、q0–q31 与 NZCV/FPSR/FPCR，函数中间的活寄存器不受影响；
    # 这条 ABI 由 gatefn 功能核（桩故意破坏 x9–x17 与 NZCV，核全部恢复），不在这里按位置近似
    # Function entry is no longer required: _dm_gate saves and restores x0–x17, x19–x30, q0–q31 and NZCV/FPSR/FPCR, so live registers in the middle of a function are unaffected;
    # this ABI is checked functionally by gatefn (the stub deliberately clobbers x9–x17 and NZCV, and full restoration is checked), not approximated here by position
    if set(range(off, off + 4)) & _busy: bad(f'观测门 {off:#x} 与改写点 / 窗口 / 剪口重叠')
    s_ = btarget(off)
    if s_ is None or not (PO <= s_ < len(pat)): bad(f'观测门头 {off:#x} 不是跳进补丁区的 B'); continue
    ks = keyseq(s_ + 4)
    if W(pat, s_) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') \
            or W(pat, ks[0]) & 0xFFC00000 != 0xF9400000 or W(pat, ks[0] + 4) != LDR_X29_SP or W(pat, ks[0] + 8) != 0xD63F03C0 \
            or W(pat, ks[0] + 12) != 0xA8C17BFD or W(pat, ks[0] + 16) != ow \
            or W(pat, ks[0] + 20) != B(ks[0] + 20, off + 4):
        bad(f'观测门块不符键页模板 {off:#x}'); continue
    if gates.get(ks[0] + 12) != ('obs', oid, off): bad(f'观测门行不符 {off:#x}：表 {gates.get(ks[0] + 12)}')
    seen.add(ks[0] + 12)
# ⑪ 剪口引入的分支门（hle.h 登记表形态「分支门：x8≠0 跳 0x目标，否则执行原指令回 0x原处+4」）：原处换成 B 进补丁区，块逐字 =
#   stp x8,xzr,[sp,#-16]! ; stp x29,x30,[sp,#-16]! ; 键页取 KGATE ; ldr x30,[x30] ; ldr x29,[sp] ; blr x30 ; ldp x29,x30 ; cbz w8,+12 ;
#   ldp x8,xzr ; b 目标 ; ldp x8,xzr ; 原指令 ; b 原处+4 —— x8 两路都恢复为原值，宿主结果只用于选路；gates.tsv 记 (svc, 号, 原处)
# ⑪ Branch gates introduced by cuts (hle.h registry form 「分支门：x8≠0 跳 0x目标，否则执行原指令回 0x原处+4」(branch gate: x8≠0 jumps to 0x<target>, otherwise executes the original instruction and returns to 0x<original site>+4)): the original site is replaced with a B into the patch area, and the block equals word for word
#   stp x8,xzr,[sp,#-16]! ; stp x29,x30,[sp,#-16]! ; fetch KGATE via key page ; ldr x30,[x30] ; ldr x29,[sp] ; blr x30 ; ldp x29,x30 ; cbz w8,+12 ;
#   ldp x8,xzr ; b target ; ldp x8,xzr ; original instruction ; b original site+4 - x8 is restored to its original value on both paths, the host result is only used to pick the path; gates.tsv records (svc, number, original site)
nbranch = 0
for n, (roff, form) in _hreg.items():
    if not form.startswith('分支门'): continue
    cut = next((c for c in cuts if c[0] == roff), None)
    if cut is None: continue                                       # 本产物没开这扇私有门 | This build output does not enable this private gate
    mt, mb = re.search(r'跳 (0x[0-9a-fA-F]+)', form), re.search(r'回 (0x[0-9a-fA-F]+)', form)
    if not mt or not mb or int(mb[1], 16) != roff + 4: bad(f'分支门 svc #{n:#x}：登记表形态缺跳转目标或回点 ≠ 原处+4：{form}'); continue
    tgt = int(mt[1], 16); s_ = btarget(roff); ks = keyseq(s_ + 8) if s_ is not None else None
    if s_ is None or not (PO <= s_ < len(pat)) or W(pat, s_) != 0xA9BF7FE8 or W(pat, s_ + 4) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') \
            or W(pat, ks[0]) & 0xFFC00000 != 0xF9400000 or W(pat, ks[0] + 4) != LDR_X29_SP or W(pat, ks[0] + 8) != 0xD63F03C0 or W(pat, ks[0] + 12) != 0xA8C17BFD \
            or W(pat, ks[0] + 16) != 0x34000068 or W(pat, ks[0] + 20) != 0xA8C17FE8 or W(pat, ks[0] + 24) != B(ks[0] + 24, tgt) \
            or W(pat, ks[0] + 28) != 0xA8C17FE8 or W(pat, ks[0] + 32) != W(orig, roff) or W(pat, ks[0] + 36) != B(ks[0] + 36, roff + 4):
        bad(f'分支门 svc #{n:#x} {roff:#x}：块不符模板'); continue
    if gates.get(ks[0] + 12) != ('svc', n, roff): bad(f'分支门 svc #{n:#x} gates.tsv 行不符：{gates.get(ks[0] + 12)}')
    seen.add(ks[0] + 12); nbranch += 1
if set(gates) != seen: bad(f'gates.tsv 有多余行 {len(set(gates) - seen)}')

# ⑥ 线程槽
# ⑥ Thread slots
_base = {'K18', 'KTLS', 'KTLR', 'KGATE'}; _extra = set(KEY) - _base   # 额外键 = 参数门键（KRES* / KASPECT* …；改写器全局键表，三模块表头都列）：须是 K 开头大写名、键号低于基础四键 | Extra keys = parameter-gate keys (KRES* / KASPECT* ...; the rewriter's global key table, listed in the headers of all three modules): must be uppercase names starting with K, with key numbers below the four base keys
if set(KEY) != {'K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'} \
        or not {k for *_, k in params} <= set(KEY) or set(KEY.values()) != set(range(8)):
    bad(f'表头键页下标不合规：{KEY}（须含八个键且下标恰为 0..7）')
kv = {v: k for k, v in KEY.items()}; nk = {}
if not ANDROID:
    for o in range(PO, len(pat) - 4, 4):
        w = W(pat, o)
        if w & 0xFFFFFFE0 != 0xD53BD060: continue           # mrs xN, tpidrro_el0
        n = w & 31; ks = keyseq(o); k = kv.get(ks[1]) if ks else None
        if not ks or k is None:
            bad(f'补丁区 {o:#x} 读 tpidrro 后不是键页间接槽序列'); continue
        nk[k] = nk.get(k, 0) + 1
else:
    # Android：补丁区没有 mrs tpidrro；槽访问的不变尾部是 adrp Xk,KEY; ldr Xk,[Xk,#idx*8]（idx 0..7）; add Xt,Xt,Xk。
    # 键页第 8 项（宿主入口）是 ldr [#64] 后接 blr、不接 add Xt,Xt,Xk，自然排除。每条这样的尾部核对其前有宿主入口序列。
    # Android: no mrs tpidrro in the patch area; the invariant tail of a slot access is adrp Xk,KEY; ldr Xk,[Xk,#idx*8] (idx 0..7); add Xt,Xt,Xk.
    # Key page index 8 (host entry) is a ldr [#64] followed by blr (not add Xt,Xt,Xk), so it is naturally excluded. Each such tail is checked to be preceded by the host-entry sequence.
    _kp = h['KEY_PAGE_OFF']
    def _adrp_key(o):   # raw-word prefilter (speed only): adrp whose page is the key page; every hit gets the full check below
        w = W(pat, o)
        if w & 0x9F000000 != 0x90000000: return False
        imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
        return (o & ~0xFFF) + (((imm ^ (1 << 20)) - (1 << 20)) << 12) == _kp
    for o in range(PO, len(pat) - 12, 4):
        if not _adrp_key(o): continue
        a0 = list(_md.disasm(pat[o:o + 12], o))
        if len(a0) != 3: continue
        q0, q1, q2 = a0
        if q0.mnemonic == 'adrp' and q0.operands[1].imm == h['KEY_PAGE_OFF'] \
           and q1.mnemonic == 'ldr' and q1.operands[1].mem.base == q0.operands[0].reg and q1.operands[0].reg == q0.operands[0].reg \
           and q2.mnemonic == 'add' and q2.operands[1].reg == q2.operands[0].reg and q2.operands[2].reg == q0.operands[0].reg \
           and q1.operands[1].mem.disp % 8 == 0 and 0 <= q1.operands[1].mem.disp // 8 < 8:
            # 回看：这段 add 之前（同一改写块内）须有宿主入口；用 keyseq 从块起点难定位，这里用 o 往前一小段确认有 blr + key8 入口
            # Look back: before this add (within the same block) there must be the host entry; confirm a blr + key-8 entry a short distance before o.
            win = list(_md.disasm(pat[max(PO, o - 48):o], max(PO, o - 48)))
            if not (any(x.mnemonic == 'blr' for x in win) and any(win[j].mnemonic == 'ldr' and win[j].operands[1].mem.disp == 8 * 8 and win[j - 1].mnemonic == 'adrp' and win[j - 1].operands[1].imm == h['KEY_PAGE_OFF'] for j in range(1, len(win)))):
                bad(f'补丁区 {o:#x} 槽访问尾部前无宿主入口序列'); continue
            k = kv.get(q1.operands[1].mem.disp // 8)
            if k is None: bad(f'补丁区 {o:#x} 槽下标未登记'); continue
            nk[k] = nk.get(k, 0) + 1
expected = {}
for kind in sites.values():
    key = {'x18': 'K18', 'W': 'K18', 'C': 'K18', 'tpidr_r': 'KTLS', 'tpidr_w': 'KTLS', 'tpidrro_r': 'KTLR'}.get(kind)
    if key: expected[key] = expected.get(key, 0) + 1
expected['KGATE'] = len(gsites) + len(obs) + nbranch
for _, _, _, key in params: expected[key] = expected.get(key, 0) + 1
if any(nk.get(k, 0) != v for k, v in expected.items()) or any(k not in expected and v for k, v in nk.items()):
    bad(f'键页槽访问计数不符：期望 {expected} 实际 {nk}')
# Android：每个 thread_base 宿主入口后紧跟一次键页槽访问，两边计数必须 1:1（多出的入口 / 槽访问 = 有模板绕过了 ⑮ 或 ⑥，Opus r2 N1）。
# Android: every thread_base host entry is followed by exactly one key-page slot access, so the counts must match 1:1 (an extra entry / slot access means a template bypassed ⑮ or ⑥, Opus r2 N1).
if ANDROID and _tbn != sum(nk.values()):
    bad(f'宿主入口数 {_tbn} ≠ 键页槽访问数 {sum(nk.values())}（thread_base 与槽访问应一一对应）')

# ⑧ 参数门（PARAMS，仅宿主）：原处 movz xR,#lo / movk xR,#hi,lsl #32 造的 64 位常数，等长换成 mrs xR,tpidrro_el0 / ldr xR,[xR,#键*8]
#   核：A 原字是 64 位 movz（hw=0）写 xR、B 原字是 64 位 movk（hw=2）写 xR；新字逐位 = 模板；A、B 之间的指令不读写 xR；
#   (A, B] 不是任何直接分支的目标、也不是函数入口（否则从中间进来的路径会拿到槽指针而不是常数）；键号在表头登记；不与改写点 / 剪口 / 观测门重叠。
#   宿主给槽原值（宽高打包 = 下列常数）时语义与原两条逐位等价：B 之后 xR 同值，A、B 之间无人读 xR。
# ⑧ Parameter gates (PARAMS, host only): a 64-bit constant originally built by movz xR,#lo / movk xR,#hi,lsl #32 is replaced at equal length by mrs xR,tpidrro_el0 / ldr xR,[xR,#key*8]
#   Checks: original word A is a 64-bit movz (hw=0) writing xR, original word B is a 64-bit movk (hw=2) writing xR; the new words = the template bit for bit; instructions between A and B do not read or write xR;
#   (A, B] is not the target of any direct branch nor a function entry (otherwise a path entering in the middle would get the slot pointer instead of the constant); the key number is registered in the header; no overlap with rewrite sites / cuts / observation gates.
#   When the host gives the slot the original value (packed width/height = the constants below), the semantics are bit-for-bit equivalent to the original two instructions: xR has the same value after B, and nobody reads xR between A and B.
par_const = {}
if params:
    import capstone
    _cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); _cs.detail = True
    _tg = set()
    for o in range(0, TE & ~3, 4):                                  # 全 text 直接分支目标 | Direct branch targets across all of text
        w = W(orig, o)
        if w & 0x7C000000 == 0x14000000: d = ((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000; _tg.add(o + 4 * d)
        elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: d = (((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000; _tg.add(o + 4 * d)
        elif w & 0x7E000000 == 0x36000000: d = (((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000; _tg.add(o + 4 * d)
    _fs = set(nsoimg.func_starts(orig, TE))
    _busy8 = {a for a in sites} | set(inwin) | obs_heads | {o & ~3 for o in cutb}
    for a, b, r, k in params:
        wa, wb = W(orig, a), W(orig, b)
        # 两种原指令对：64 位 movz xR,#lo / movk xR,#hi,lsl #32（KRES*）；32 位 movz wR,#lo / movk wR,#hi,lsl #16（KASPECT*，原值 = hi<<16|lo，
        # 写 w 寄存器高 32 位清零 ⇒ 宿主槽须按 u32 位模式存、高 32 位为 0（运行期约束，静态核不到，见 platform thread.c）
        # Two kinds of original instruction pair: 64-bit movz xR,#lo / movk xR,#hi,lsl #32 (KRES*); 32-bit movz wR,#lo / movk wR,#hi,lsl #16 (KASPECT*, original value = hi<<16|lo,
        # writing a w register zeroes the upper 32 bits ⇒ the host slot must be stored as a u32 bit pattern with the upper 32 bits 0 (a runtime constraint that cannot be checked statically, see platform thread.c)
        w64 = wa & 0xFFE00000 == 0xD2800000 and wb & 0xFFE00000 == 0xF2C00000
        w32 = wa & 0xFFE00000 == 0x52800000 and wb & 0xFFE00000 == 0x72A00000
        if not (w64 or w32) or wa & 31 != r or wb & 31 != r:
            bad(f'参数门 {a:#x}/{b:#x}：原指令 {wa:#010x}/{wb:#010x} 不是 movz/movk 造常数对（64 位 lsl #32 或 32 位 lsl #16）写 x{r}')
        if not a < b: bad(f'参数门 {a:#x}/{b:#x} 顺序不对')
        if k not in KEY: bad(f'参数门 {a:#x} 键 {k} 未在表头登记'); continue
        s = btarget(a); ks = paramseq(s, r) if s is not None else None
        ok = ks and ks[1] == KEY[k] and W(pat, b) == 0xD503201F
        if not ok: bad(f'参数门 {a:#x}/{b:#x}：键页间接模板不符')
        for o in range(a + 4, b, 4):
            ins = next(_cs.disasm(orig[o:o + 4], o), None)
            if ins is None: bad(f'参数门 {a:#x}：中间 {o:#x} 解不出'); continue
            rr, ww = ins.regs_access()
            names = {ins.reg_name(x) for x in rr + ww}
            if f'x{r}' in names or f'w{r}' in names: bad(f'参数门 {a:#x}：中间 {o:#x} {ins.mnemonic} {ins.op_str} 读写 x{r}')
        bt = sorted(t for t in _tg if a < t <= b) + sorted(t for t in _fs if a < t <= b)
        if bt: bad(f'参数门 {a:#x}..{b:#x}：区间内有分支目标 / 函数入口 {[hex(x) for x in bt[:4]]}')
        if {a, b} & _busy8: bad(f'参数门 {a:#x}/{b:#x} 与改写点 / 窗口 / 观测门 / 剪口重叠')
        par_const[(a, k)] = ((wb >> 5) & 0xFFFF) << (16 if w32 else 32) | ((wa >> 5) & 0xFFFF)
    _kc = {}
    for (a, k), v in par_const.items(): _kc.setdefault(k, set()).add(v)
    for k, vs in _kc.items():
        if len(vs) != 1: bad(f'参数门键 {k} 对应多个原常数 {[hex(v) for v in vs]}（同一槽给不出唯一原值）')
    print('参数门 ' + ', '.join(f'{a:#x}={k}:原值 {v:#x}' for (a, k), v in par_const.items()))
    # 参数寄存器的下游不得再被常数掩码：编译器会把「值只可能是这几个常数」折进后续指令——运行模式通知处理 0xe10e48 的
    # mov w9,#0x740; and x9,x25,x9 就是 1280|1600 推出的掩码，换成参数后宽度被掩成 768 / 64。从每个参数位起按函数内线性顺序做污点传播
    # （mov / csel 类传递，其余写入清除；不跟分支，属保守近似），被污染的寄存器遇到 and / ands / bic 且另一操作数是立即数或 mov 立即数造的寄存器即报
    # Downstream of a parameter register there must be no further masking by a constant: the compiler folds "the value can only be one of these constants" into later instructions - in the operation mode notification handler at 0xe10e48,
    # mov w9,#0x740; and x9,x25,x9 is exactly a mask derived from 1280|1600, and once replaced by a parameter the width gets masked to 768 / 64. From each parameter site, do taint propagation in linear order within the function
    # (mov / csel kinds propagate, other writes clear; branches are not followed, a conservative approximation); report when a tainted register meets and / ands / bic whose other operand is an immediate or a register built by a mov immediate
    _fsl = sorted(_fs)
    def _rn(ins, x): return re.sub(r'^[wx]', '', ins.reg_name(x)) if re.match(r'^[wx]\d+$', ins.reg_name(x) or '') else None
    _donef = set()
    for a, b, r, k in params:
        import bisect as _bi
        i = _bi.bisect_right(_fsl, a); f0 = _fsl[i - 1]; f1 = _fsl[i] if i < len(_fsl) else TE
        if f0 in _donef: continue
        _donef.add(f0)
        seeds = {(pa, str(pr)) for pa, pb, pr, pk in params if f0 <= pa < f1}
        taint, const = set(), {}
        for o in range(f0, f1, 4):
            for pa, pr in seeds:
                if o == pa: taint.add(pr)
            if any(o == pa for pa, _ in seeds): continue
            ins = next(_cs.disasm(pat[o:o + 4], o), None)
            if ins is None: continue
            rr, ww = ins.regs_access(); rd = {x for x in (_rn(ins, q) for q in rr) if x}; wr = {x for x in (_rn(ins, q) for q in ww) if x}
            m = ins.mnemonic
            if m in ('and', 'ands', 'bic', 'bics') and rd & taint:
                imm = any(op.type == capstone.arm64.ARM64_OP_IMM for op in ins.operands)
                if imm or (rd - taint) & set(const): bad(f'参数门下游常数掩码 {o:#x}：{m} {ins.op_str}（函数 {f0:#x}，被参数寄存器污染的值再被常数掩码）')
            if m in ('mov', 'csel', 'csinc', 'csinv', 'csneg') and rd & taint: taint |= wr; [const.pop(x, None) for x in wr]
            else:
                taint -= wr
                isimm = m in ('mov', 'movz') and len(ins.operands) == 2 and ins.operands[1].type == capstone.arm64.ARM64_OP_IMM
                for x in wr:
                    if isimm: const[x] = ins.operands[1].imm
                    else: const.pop(x, None)

# ⑨ 临时寄存器静态检查：每个补丁块的非原指令目的寄存器必须有同偏移红区存取配对。
# ⑨ Static check of temporary registers: every non-original-instruction destination register in a patch block must have a matching red-zone save/restore pair at the same offset.
import capstone
def _writes(ins):
    _, ww = ins.regs_access()
    return {'x29' if ins.reg_name(r) == 'fp' else 'x30' if ins.reg_name(r) == 'lr' else re.sub(r'^[wx]', 'x', ins.reg_name(r))
            for r in ww if ins.reg_name(r) not in ('sp', 'pc')}
def _mem_sp(ins, load):
    if (load and not ins.mnemonic.startswith('ld')) or (not load and not ins.mnemonic.startswith('st')):
        return None
    for op in ins.operands:
        if op.type == capstone.arm64_const.ARM64_OP_MEM and ins.reg_name(op.mem.base) == 'sp':
            if ANDROID: return op.mem.disp          # Android：开帧暂存在正偏移（含前/后变址压栈）；配对不比偏移，仅要求存在存/取 | Android: framed scratch uses positive offsets (and pre/post-index pushes); pairing does not match offsets, only presence
            if op.mem.disp < 0: return op.mem.disp
            if load and ins.mnemonic == 'ldp' and op.mem.disp == 0: return -16
    return None
def _block(start):
    out = []
    for ins in _md.disasm(pat[start:start + 256], start):
        out.append(ins)
        if ins.mnemonic == 'b':
            t = ins.operands[0].imm
            if not (PO <= t < len(pat)): break
    return out
static_entries = [(a, set()) for a in sites]
for a, b, r, _ in params:
    static_entries.append((a, {f'x{r}'}))
static_seen = 0
for a, allowed in static_entries:
    s = btarget(a)
    if s is None: continue
    block = _block(s); static_seen += 1
    if not allowed:
        end = next((hi for lo, hi in wins if lo == a), a + 4)
        for lo, hi in atomic_groups.get(a, [(a, end)]):
            for ins in _md.disasm(orig[lo:hi], lo): allowed |= _writes(ins)
        if next(_md.disasm(orig[a:a + 4], a), None).mnemonic in ('stp', 'ldp'):
            allowed.update(('x0', 'x1'))
    saved = {}
    restored = {}
    for i, ins in enumerate(block):
        d = _mem_sp(ins, False)
        if d is not None:
            regs = [op.reg for op in ins.operands[:2] if op.type == capstone.arm64_const.ARM64_OP_REG]
            for reg in regs:
                name = ins.reg_name(reg); name = 'x29' if name == 'fp' else 'x30' if name == 'lr' else re.sub(r'^[wx]', 'x', name)
                saved.setdefault(name, []).append((d, i))
        d = _mem_sp(ins, True)
        if d is not None:
            regs = [op.reg for op in ins.operands[:2] if op.type == capstone.arm64_const.ARM64_OP_REG]
            for reg in regs:
                name = ins.reg_name(reg); name = 'x29' if name == 'fp' else 'x30' if name == 'lr' else re.sub(r'^[wx]', 'x', name)
                restored.setdefault(name, []).append((d, i))
    writes = set().union(*(_writes(ins) for ins in block)) if block else set()
    for reg in sorted(writes - allowed):
        wi = [i for i, ins in enumerate(block) if reg in _writes(ins) and not (i > 0 and _mem_sp(ins, True) is not None)]
        dead = any(ins.mnemonic in ('br', 'blr') and ins.operands and ins.reg_name(ins.operands[0].reg) in (reg, reg[1:]) for ins in block)
        if ANDROID:   # 偏移跨前/后变址压栈无法逐位对齐；要求有存（首次写前）且有取（末次写后），精确偏移由 equiv 动态保证 | offsets across pre/post-index pushes do not line up; require a save before the first write and a restore after the last, with exact offsets guaranteed dynamically by equiv
            ok = dead or (any(si < min(wi, default=10**9) for _, si in saved.get(reg, [])) and any(ri > max(wi, default=-1) for _, ri in restored.get(reg, [])))
        else:
            ok = dead or any(sd == rd and si < min(wi, default=10**9) and ri > max(wi, default=-1)
                     for sd, si in saved.get(reg, []) for rd, ri in restored.get(reg, []))
        if not ok: bad(f'临时寄存器 {a:#x}：块写 {reg}，原目的寄存器外且无存取配对')
print(f'STATIC {mod} scanned={static_seen} violations={sum(1 for x in fails if x.startswith("临时寄存器"))}')

# ⑩ 跳板出口（切掌机崩溃：参数门跳板回到 B+4，越过 A、B 之间未改的原指令和另一个跳板头）：
#   text 里每条跳进补丁区的 b（跳板头 a）沿补丁块走遍全部回 text 的出口（顺序 b 与条件分支，bl 视为返回，ret / br 不算回 text）。
#   合法出口：a+4；越过的字全是被改写吸收的字（≠ 原字且不是别的跳板头，如参数门 B 处的 nop）后的地址；
#   窗口（W 行 [lo, hi)）头：hi 与窗口内原指令自己的分支目标；非窗口头：连续吸收段内原指令自己的分支目标。
# ⑩ Trampoline exits (the handheld-switch crash: the parameter-gate trampoline returned to B+4, skipping the unmodified original instructions between A and B and another trampoline head):
#   for every b in text that jumps into the patch area (trampoline head a), follow the patch block through all exits back to text (sequential b and conditional branches; bl is treated as returning; ret / br do not count as returning to text).
#   Legal exits: a+4; the address after skipped words that are all words absorbed by the rewrite (≠ the original word and not another trampoline head, e.g. the nop at parameter-gate B);
#   window (W row [lo, hi)) heads: hi and the branch targets of the original instructions inside the window; non-window heads: the branch targets of the original instructions inside the contiguous absorbed span.
_heads = {o for o in range(0, TE & ~3, 4) if W(pat, o) & 0xFC000000 == 0x14000000 and btarget(o) is not None and btarget(o) >= PO}
_wend = dict(wins)
def _bt(w, o):
    if w & 0x7C000000 == 0x14000000: return o + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: return o + 4 * ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000)
    if w & 0x7E000000 == 0x36000000: return o + 4 * ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000)
    return None
if calls:
    second_words = set(calls.values())
    for address in range(0, TE, 4):
        if _bt(W(orig, address), address) in second_words:
            bad(f'known branch enters call without preparation {address:#x}')
    if second_words & set(nsoimg.func_starts(orig, TE)):
        bad('function entry enters call without preparation')
    for _, typ, _, value, addend in nsoimg.relocs(orig):
        if (typ == 1027 and addend in second_words) or value in second_words:
            bad('relocation enters call without preparation')

def _exits(start):
    ex, seen, st = set(), set(), [start]
    while st:
        o = st.pop()
        while o not in seen and o + 4 <= len(pat):
            seen.add(o); w = W(pat, o)
            if w & 0xFC000000 == 0x14000000:
                t = _bt(w, o)
                if t < PO: ex.add(t); break
                o = t; continue
            if w & 0xFC000000 == 0x94000000: o += 4; continue
            if w == 0xD65F03C0 or w & 0xFFFFFC1F == 0xD61F0000: break
            t = _bt(w, o)
            if t is not None:
                if t < PO: ex.add(t)
                else: st.append(t)
            o += 4
    return ex
nexit = 0; _cutoff = {c[0] for c in cuts}; _cutex = []
for a in sorted(_heads):
    if a in _wend:
        group = atomic_groups.get(a, [(a, _wend[a])])
        seg = [o for lo, hi in group for o in range(lo, hi, 4)]
        legal = {calls[a]} if a in calls else {hi for lo, hi in group}
    else:
        e = a + 4
        while e < TE and W(pat, e) != W(orig, e) and e not in _heads: e += 4
        seg = range(a, e, 4); legal = {a + 4 * k for k in range(1, len(seg) + 1)}
    legal |= {t for o in seg for t in [_bt(W(orig, o), o)] if t is not None}
    for t in _exits(btarget(a)):
        nexit += 1
        if t not in legal and a in _cutoff: _cutex.append(f'{a:#x}→{t:#x}'); continue   # 已登记剪口（带理由，故意改控制流）：放行并列出 | Registered cuts (with a reason, deliberately changing control flow): let through and listed
        if t not in legal: bad(f'跳板出口 {a:#x} → {t:#x}：既不是 {a + 4:#x} / 吸收段尾 / 窗口尾，也不是段内原指令的分支目标（越过未改原指令或别的跳板头）')
print(f'EXITS {mod} 跳板头={len(_heads)} 出口={nexit} 已登记剪口改控制流出口={_cutex}')

# ⑭ SP 平衡静态走访（Android，M1b B4 / Codex C5 / Opus I4）：对每个「非门」跳板（普通 x18 / 窗口 / 条件 / pcrel / sysreg / tpidr / 参数门），
# 从补丁块走遍全部回 text 的出口，累计 SP 增量（sub/add sp 立即数、前/后变址压栈写回），要求每个出口的净增量 == 该跳板吸收的原指令自身的 SP 写回量
# （窗口与参数门 = 0；写回类 x18 站点 = 原指令的 ±N；普通 / 条件 / pcrel / sysreg / tpidr = 0）。缺收栈 / 偏 8 / 少 add sp 都会被判出。
# 门 / 观测门 / 剪口分支门的 SP 由 ⑤ / ⑦ / ⑪ 的逐字模板核对保证，不在此列。
# ⑭ Static SP-balance walk (Android, M1b B4 / Codex C5 / Opus I4): for every non-gate trampoline (plain x18 / window / cond / pcrel / sysreg / tpidr / param gate),
# walk the patch block to all exits back to text, accumulating the SP delta (sub/add sp immediates, pre/post-index writeback), and require each exit's net delta == the SP writeback of the
# original instruction the trampoline absorbed (windows and param gates = 0; writeback-class x18 sites = the original's ±N; plain / cond / pcrel / sysreg / tpidr = 0). A missing frame close / off-by-8 / missing add sp is caught.
# Gate / observation-gate / branch-gate SP balance is guaranteed by the word-for-word template checks ⑤ / ⑦ / ⑪ and is not covered here.
if ANDROID:
    _OPAQUE = 'OPAQUE'
    def _spd(w):
        """指令对 SP 的净写回量（sub/add sp,sp,#imm、前/后变址 [sp,#i]!、[sp],#i）；不动 SP 返回 0；
        任何「把 SP 设成别的来源」的写（add/sub sp,xN,#imm 的 xN≠sp、mov sp,xN、寄存器形 add sp,sp,xN 等）返回 _OPAQUE，让走访判红
        （Codex r2 blocker 2：原本只核目的 SP + 立即数，add sp,x0,#16 也得 +16 却把 SP 设成 x0+16）。
        | Net SP writeback (sub/add sp,sp,#imm, pre/post-index); 0 if SP untouched; any write that sets SP from another source
        (add/sub sp,xN,#imm with xN!=sp, mov sp,xN, register-form add sp,sp,xN, ...) returns _OPAQUE so the walk flags it
        (Codex r2 blocker 2: the old code checked only the dest SP + immediate, so add sp,x0,#16 also yielded +16 while setting SP = x0+16)."""
        ins = next(_md.disasm(struct.pack('<I', w), 0), None)
        if ins is None: return 0
        s = ins.op_str
        m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-fA-F]+)\]!', s)
        if m: return int(m.group(1), 0)
        m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-fA-F]+)', s)
        if m: return int(m.group(1), 0)
        ops = ins.operands
        # 唯一允许的直接 SP 调整：add/sub sp, sp, #imm —— 目的与来源都恰为 64 位 sp（wsp 会零扩展清高 32 位），立即数不带移位
        # （`sub sp,sp,#16,lsl #12` 实际 −65536，旧版当 −16 放过，Opus r2.2 B1）。改写器只发 sh=0 的 SP 调整。
        # The only allowed direct SP adjust: add/sub sp, sp, #imm — dest and source exactly 64-bit sp (wsp zero-extends), immediate unshifted
        # (`sub sp,sp,#16,lsl #12` is really -65536 but the old code took it as -16, Opus r2.2 B1). The rewriter only emits sh=0 SP adjusts.
        if ins.mnemonic in ('add', 'sub') and len(ops) == 3 \
           and ops[0].type == capstone.arm64_const.ARM64_OP_REG and _md.reg_name(ops[0].reg) == 'sp' \
           and ops[1].type == capstone.arm64_const.ARM64_OP_REG and _md.reg_name(ops[1].reg) == 'sp' \
           and ops[2].type == capstone.arm64_const.ARM64_OP_IMM and ops[2].shift.type == 0:
            return (-ops[2].imm if ins.mnemonic == 'sub' else ops[2].imm)
        # 兜底：任何其它会写 SP 的指令（目的为 sp/wsp、寄存器后变址 [sp],xN、带移位的 add sp 等）→ _OPAQUE，⑭ 判红。
        # Fallback: any other instruction that writes SP (dest sp/wsp, register post-index [sp],xN, shifted add sp, ...) -> _OPAQUE, ⑭ flags it.
        try:
            _r, _w = ins.regs_access()
            if any(_md.reg_name(x) in ('sp', 'wsp') for x in _w): return _OPAQUE
        except capstone.CsError:
            pass
        if ops and ops[0].type == capstone.arm64_const.ARM64_OP_REG and _md.reg_name(ops[0].reg) in ('sp', 'wsp'): return _OPAQUE
        return 0
    def _sp_exits(start):
        """返回 ({回 text 的净 SP 增量集合}, 是否遇到无法建模的 SP 写 opaque)；沿顺序 b / 条件分支走（bl 中性，ret / br 终止）。
        | Returns ({net SP deltas at exits back to text}, whether an unmodelable SP write (opaque) was seen); follow sequential b / conditional branches (bl neutral, ret / br terminal)."""
        out = set(); seen = set(); stk = [(start, 0)]; opaque = False
        while stk:
            o, d = stk.pop()
            while PO <= o < len(pat) - 3 and (o, d) not in seen:
                seen.add((o, d)); w = W(pat, o); delta = _spd(w)
                if delta == _OPAQUE: opaque = True; break
                d += delta
                if w & 0xFC000000 == 0x14000000:                      # b
                    t = _bt(w, o)
                    if t is None: break
                    if t < PO: out.add(d); break
                    o = t; continue
                if w & 0xFC000000 == 0x94000000: o += 4; continue     # bl：中性 | bl: neutral
                if w == 0xD65F03C0 or w & 0xFFFFFC1F == 0xD61F0000: break   # ret / br
                t = _bt(w, o)                                          # 条件分支：两路 | conditional: both paths
                if t is not None:
                    if t < PO: out.add(d)
                    else: stk.append((t, d))
                o += 4
        return out, opaque
    _par_heads = {a for a, b, r, k in params}
    _spbad = []; _spn = 0
    _nontemplate = {a for a, k in sites.items() if k in ('x18', 'W', 'tpidr_r', 'tpidr_w', 'tpidrro_r')} | _par_heads   # 门（svc/cntpct/cntvct）与观测门由 ⑤/⑦ 逐字模板核，不在 SP 走访 | gates and obs gates are word-for-word template-checked by ⑤/⑦, excluded from the SP walk
    for a in sorted(h2 for h2 in _heads if h2 in _nontemplate):
        exp = 0 if (a in _wend or a in _par_heads) else _spd(W(orig, a))   # 吸收的原指令自身的 SP 写回（窗口 / 参数门 = 0）| SP writeback of the absorbed original (windows / param gates = 0)
        if exp == _OPAQUE:           # 吸收的原指令本身就是无法建模的 SP 写：直接判红，不当 0（Opus r2.2）| the absorbed original is itself an unmodelable SP write: flag it, do not treat as 0 (Opus r2.2)
            _spbad.append((a, hex(a), 'OPAQUE-original', 'modelled')); _spn += 1; continue
        _spn += 1
        exits, opaque = _sp_exits(btarget(a))
        if opaque: _spbad.append((a, hex(a), 'OPAQUE-sp-write', exp))   # 块内把 SP 设成别处（add sp,xN,#imm / mov sp,xN 等）| block sets SP from elsewhere
        for d in exits:
            if d != exp: _spbad.append((a, hex(a), d, exp))
    if _spbad: bad(f'SP 平衡走访：{len(_spbad)} 个跳板出口净 SP 增量 ≠ 预期（缺收栈 / 少 add sp / 偏移错 / 把 SP 设成别处）：例 ' + ', '.join(f'{x[1]}净{x[2]}≠{x[3]}' for x in _spbad[:5]))
    print(f'SPBAL {mod} 走访跳板={_spn} 不平衡={len(_spbad)}')

# ⑫ LL/SC 独占区规则（Android 要点 3）：分支跟随求独占区（独占加载前向可达到匹配独占存，再与能提交 stxr 反向取交），
# 独占区内的改写点必须属于被整块相对化的窗口（sites.tsv 'W'）；否则逐点改写块里的 blr / 访存会清掉独占监视器 → stxr 永远失败。
# 线性普查会把 ldxr→下一条文本 stxr 跨函数 / 跨循环退出全算进去，这里用分支跟随排除那些假阳性（如 sdk 0x345a4c / 0x34f390、main 0x179998c 的 tpidr 读）。
# ⑫ LL/SC reservation rule (Android item 3): branch-following reservation set (forward-reachable from an exclusive-load to the
# matching exclusive-store, intersected with "can commit the stxr"); any rewrite site inside it must belong to a wholly relocated
# window (sites.tsv 'W'), otherwise the per-site block's blr / memory accesses clear the monitor and the stxr fails forever.
# A linear census counts everything between an ldxr and the next textual stxr (across functions / loop exits); branch-following
# excludes those false positives (e.g. the tpidr reads near sdk 0x345a4c / 0x34f390, main 0x179998c).
res_sites = 0
if True:
    import collections as _col
    _starts2 = nsoimg.func_starts(orig, TE)
    _w2 = struct.unpack_from(f'<{TE // 4}I', orig, 0)
    def _xld(w): return w & 0x3F800000 == 0x08000000 and (w >> 22 & 1) and not (w >> 31 == 0 and w >> 21 & 1)
    def _xst(w): return w & 0x3F800000 == 0x08000000 and not (w >> 22 & 1) and not (w >> 31 == 0 and w >> 21 & 1)
    def _clr(w): return w & 0xFFFFF0FF == 0xD503305F
    def _sxn(v, b): return v - (1 << b) if v >> (b - 1) & 1 else v
    def _sc(o):
        w = _w2[o // 4]
        if w & 0xFC000000 == 0x14000000: return (o + 4 * _sxn(w & 0x3FFFFFF, 26),)
        if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: return (o + 4, o + 4 * _sxn((w >> 5) & 0x7FFFF, 19))
        if w & 0x7E000000 == 0x36000000: return (o + 4, o + 4 * _sxn((w >> 5) & 0x3FFF, 14))
        if w & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000): return ()
        return (o + 4,)
    _rbad = []
    for fa, fb in zip([0] + _starts2, _starts2 + [TE]):
        lds = [o for o in range(fa, fb, 4) if _xld(_w2[o // 4])]
        for L in lds:
            F = set(); stk = list(_sc(L))
            while stk:
                n = stk.pop()
                if not (fa <= n < fb) or n in F: continue
                F.add(n); w = _w2[n // 4]
                if _xld(w) or _xst(w) or _clr(w): continue
                stk += list(_sc(n))
            pred = _col.defaultdict(list)
            for n in F:
                if _xld(_w2[n // 4]) or _xst(_w2[n // 4]) or _clr(_w2[n // 4]): continue
                for s in _sc(n):
                    if s in F: pred[s].append(n)
            # 提交点种子：独占存，加上无法解析的间接 br（非跳转表头）—— 它可能跳回独占存提交（保守，Codex C1）。ret 不算（只到 ret 的路径不提交）。
            # Commit seeds: store-exclusive, plus unresolvable indirect br (not a jump-table head) which could jump back to the store and commit (conservative, Codex C1). ret does not count (a path reaching only ret never commits).
            # Same rule as the rewriter: an untracked br ends the region (DM_RC_BR_COMMITS=1 restores the old rule).
            br_commits = os.environ.get('DM_RC_BR_COMMITS') == '1'
            B = {n for n in F if _xst(_w2[n // 4]) or (br_commits and _w2[n // 4] & 0xFFFFFC1F == 0xD61F0000 and n not in jt_heads)}; stk = list(B)
            while stk:
                n = stk.pop()
                for p in pred[n]:
                    if p not in B: B.add(p); stk.append(p)
            if B: B.add(L)                      # 独占加载本身也在保留区内（它可能就是 x18 站点，如 ldaxp x18,...）| the exclusive-load itself is in the reservation (it may be the x18 site, e.g. ldaxp x18,...)
            for n in B:
                if n in ind:                    # ind = 独立反汇编认定的全部改写点（x18/tpidr/tpidrro/svc/cnt），含窗口内的 | ind = all rewrite points per the independent disassembler, including window-internal ones
                    res_sites += 1
                    if n not in inwin: _rbad.append((n, ind[n], L))
    if _rbad: bad(f'LL/SC 独占区内有未整块相对化的改写点（逐点改写会清掉独占监视器 → stxr 永久失败）：{len(_rbad)} 例 ' + ', '.join(f'{n:#x}({k},独占加载{L:#x})' for n, k, L in _rbad[:5]))
    print(f'RESV {mod} 独占区内改写点={res_sites}(应全在窗口 {len(wins)} 内) 违规={len(_rbad)}')

# ⑬ Android 红区静态规则（M1b 核心）：补丁区里不得有「写回缺省、落在 [sp,#<负偏移>] 的普通 store」——这正是把暂存写到 SP 下方的紅区写。
# 不可被抽样蒙混的精确静态检查（与 equiv 的动态逐写 SP 检查互补）。预筛 Rn==sp 再反组譯。
# ⑬ Android red-zone static rule (M1b core): the patch area must contain no plain store (no writeback) to [sp, #<negative>] — exactly a scratch write below SP.
# An exact static check that sampling cannot fool (complements equiv's dynamic per-write SP check). Prefilter Rn==sp, then disassemble.
if ANDROID:
    rz = []
    for o in range(PO, len(pat) - 4, 4):
        w = W(pat, o)
        if (w >> 5) & 31 != 31: continue          # 基址寄存器 Rn 非 sp，跳过 | base register Rn != sp, skip
        # Speed only: every store form that can take [sp, #-imm] (STR/STUR/STP/STNP/STLUR, GPR and SIMD) is in the
        # load/store class (bit 27 = 1, bit 25 = 0) with bit 22 = 0; everything else is skipped before decoding.
        if w & 0x0A000000 != 0x08000000 or (w >> 22) & 1: continue
        for i in _md.disasm(pat[o:o + 4], o):
            if not i.mnemonic.startswith('st'): continue
            m = re.search(r'\[sp(?:,\s*#(-?(?:0x)?[0-9a-fA-F]+))?\](!)?', i.op_str)
            if m and not m.group(2) and m.group(1) and int(m.group(1), 0) < 0:
                rz.append((o, i.mnemonic, i.op_str))
    if rz: bad(f'Android 补丁区有 {len(rz)} 条 SP 下方的普通 store（红区写，Linux/Android 无红区）：例 {[(hex(a), mn, s) for a, mn, s in rz[:5]]}')
    print(f'REDZONE {mod} 补丁区 SP 下方普通 store={len(rz)}（应为 0）')

cnt = {}
if _hostcall: print(f'剪口引入宿主调用门 {len(_hostcall)} 个：' + ', '.join(f"{a:#x}=svc#{n:#x}" for a, n in _hostcall.items()))
for k in sites.values(): cnt[k] = cnt.get(k, 0) + 1
print(f'STRUCT {mod} {"OK" if not fails else "FAIL"} 改写点={len(sites)} {cnt} 跳转表={jumps} 窗口内x18引用={nwin_ref} '
      f'已登记剪口={len(cuts)}(代码{ncut_code}/数据{ncut_data}) 观测门={len(obs)} 参数门={len(params)} '
      f'独立分母={len(ind)} text改动字={len(diff)} 门={len(gsites)} 槽访问={nk} 失败项={len(fails)}')
sys.exit(1 if fails else 0)
