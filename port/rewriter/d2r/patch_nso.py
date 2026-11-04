# 通用 NSO 改写器：Switch aarch64 模块 → 可在苹果系统原生执行的映像。
# 用法：patch_nso.py <NSO 文件> <输出目录> <模块名>
# 改写（苹果内核会改 x18 与 tpidr_el0；Horizon 的线程区、svc、计时器在苹果上没有对应物）：
#   x18 读写            → 线程槽 K18（TSD，经 tpidrro_el0）
#   mrs tpidr_el0       → 读线程槽 KTLS；msr tpidr_el0 → 写 KTLS
#   mrs tpidrro_el0     → 读线程槽 KTLR（Horizon ThreadLocalRegion 指针）
#   svc #n、mrs cntpct_el0 → 宿主门（门地址取自线程槽 KGATE，进 C 后按返回点查 gates.tsv）
#   已登记剪口（CUTS，按 NSO 构建号限定）：改写前逐条断言原字节，不符即中止；结果列在 <名>.cuts.tsv
#   已登记观测门（OBS，按构建号限定）：入口一条指令换成「进宿主观测回调 → 执行原指令 → 跳回」，诊断零运行时改代码；列在 <名>.obs.tsv
# 键页只在数据段末尾保存“实际键号 × 8”，补丁代码通过键页间接取线程槽：装载器可使用任意键号。
# 输出：<名>.patched.bin（键页与补丁区接在映像后 PATCH_OFF）、<名>.fixups.tsv（布局与键页下标表头）、<名>.gates.tsv、<名>.sites.tsv、<名>.cuts.tsv、<名>.obs.tsv
# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   Registered cuts (CUTS, restricted by NSO build ID): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Registered observation gates (OBS, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv
import struct, re, sys, os
from itertools import combinations
import lz4.block
from capstone import *
from capstone.arm64 import ARM64_OP_MEM
from call_window import BLR_X18, BLR_X30, movable_store, prepare_call
from relr import decode_relr

# Android 通过保留寄存器的宿主入口取得上下文；苹果保持原有 pthread TSD 布局。
# Android gets its context through a register-preserving host entry; Apple keeps pthread TSD.
TARGET = os.environ.get('DM_TARGET', 'apple')
if TARGET not in ('apple', 'android'):
    sys.exit('DM_TARGET must be apple or android')
# 仅供验证器反例测试：下调指定的 fail-closed 闸门，让“坏产物”能被生成，供 structck/equiv 证明抓得到。
# 生产与 structck/equiv 永不读它。DM_UNSAFE 是逗号分隔的 token 集合。
# Test-only: downgrade named fail-closed gates so a "bad product" can be generated for structck/equiv to prove they catch it.
# Production and structck/equiv never read this. DM_UNSAFE is a comma-separated token set.
UNSAFE = set(filter(None, os.environ.get('DM_UNSAFE', '').split(',')))
# DM_X18=native (Android only): leave every x18 instruction as it is. On Android nothing that runs on a game thread touches
# x18 -- bionic, libc++, the Vulkan loader, Turnip and our runtime are built with x18 reserved, the vDSO never uses it, the
# kernel saves and restores it like any register -- so the game keeps the register and the K18 slot, its trampolines
# (43,488 sites in 1.4.3 main, 7.6% of the game threads' CPU in play and 17.5% on a loading screen, 2026-11-02) and the
# x18 windows and jump-table / call-window rewrites are not needed. Apple needs them: its kernel clears x18.
X18 = os.environ.get('DM_X18', 'slot')
if X18 not in ('slot', 'native') or (X18 == 'native' and TARGET != 'android'):
    sys.exit('DM_X18 must be slot or native (native: Android only)')

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack_from('<I', d, 0xc)[0]
segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
bss = struct.unpack_from('<I', d, 0x3c)[0]
csz = struct.unpack_from('<III', d, 0x60)
IMG = bytearray(segs[2][1] + segs[2][2] + bss)
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
    IMG[mo:mo + sz] = raw
TEXT_END = segs[0][2]
RO_OFF, DATA_OFF = segs[1][1], segs[2][1]
KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
IMG.extend(bytes(0x4000))
PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF

# Registered cuts {NSO build ID: [(offset, original bytes, new bytes, reason)]}: none for TOTK (upstream's are Diablo II's).
CUTS = {}
# Registered observation gates {NSO build ID: [(offset, number, original word, reason)]}: none for TOTK.
OBS = {}
BUILD_ID = d[0x40:0x50].hex()
cuts = list(CUTS.get(BUILD_ID, []))
cuts = [c if len(c) == 5 else (*c, '全') for c in cuts]          # 第 5 项作用域：全 | 仅宿主（仅宿主 = 真值模拟器不应用） | 5th item is the scope: 全 (all) | 仅宿主 (host only) (仅宿主 = not applied by the ground-truth emulator)
for off, ob, nb, why, scope in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    # 显式允许已应用的同一登记补丁；未知字节仍拒绝。供自备离线修复程序使用。
    # Explicitly allow the same registered patch already applied; still reject unknown bytes.
    actual = bytes(IMG[off:off + len(ob)])
    already = os.environ.get('DM_ALLOW_PREPATCHED') == '1' and actual == nb
    assert len(ob) == len(nb) and (actual == ob or already), f'剪口原字节不符 {off:#x}：{actual.hex()}'
    IMG[off:off + len(nb)] = nb
# 键页下标固定；键页内容由装载器写入“实际键号 × 8”。
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511, 'KRES0': 506, 'KRES1': 507, 'KASPECT': 505, 'KASPECTUI': 504}   # KRES*：分辨率参数位；KASPECT：画面宽高比上限（见 PARAMS） | KRES*: resolution parameter slots; KASPECT: picture aspect ratio cap (see PARAMS)
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_OFF + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- 编码器 ----
# ---- Encoders ----
def B(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)
def STP(t, t2, n, off): return 0xA9000000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP(t, t2, n, off): return 0xA9400000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STP_PRE(t, t2, n, off): return 0xA9800000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP_POST(t, t2, n, off): return 0xA8C00000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STUR(t, n, off): return 0xF8000000 | ((off & 0x1FF) << 12) | (n << 5) | t   # stur xt,[xn,#off]（off 可负） | stur xt,[xn,#off] (off may be negative)
def LDUR(t, n, off): return 0xF8400000 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldur xt,[xn,#off]
def LDRX(t, n, imm12=0): return 0xF9400000 | (imm12 << 10) | (n << 5) | t
def STRX(t, n, imm12=0): return 0xF9000000 | (imm12 << 10) | (n << 5) | t
def thread_base(t):
    if TARGET == 'apple':
        emit(0xD53BD060 | t)
        return
    # Android（M1b）：第 8 个键保存宿主上下文入口（只改 x16，保 NZCV / 浮点状态）。Linux/Android 不保证 SP 下方的红区
    # （没设 SA_ONSTACK 的异步信号会把信号帧压在 SP 下方），所以这里只用「先减 SP 再存」的压栈：
    # str/stp 前变址一条指令内完成分配与保存，ldr/ldp 后变址一条指令内完成读回与释放，任何时刻都不访问 SP 以下的内存。
    # 三种形状（t = 目的寄存器）：t=16 只需保 x30；t=30 只需保 x16；其余保 x16/x30 并 mov xt, x16。
    # Android (M1b): key 8 holds the host context entry (changes only x16, preserves NZCV / FP state). Linux/Android does not
    # guarantee a red zone below SP (an async signal without SA_ONSTACK pushes its frame below SP), so only "decrement SP, then
    # store" pushes are used: a pre-index str/stp allocates and saves in one instruction, a post-index ldr/ldp reloads and frees
    # in one instruction; memory below SP is never accessed. Three shapes (t = destination): t=16 saves only x30; t=30 saves
    # only x16; any other t saves x16/x30 and does mov xt, x16.
    if t == 16:
        emit(STR_PRE(30, 31, -16)); emit(ADRP(16, here(), KEY_PAGE_OFF)); emit(LDRX(16, 16, 8)); emit(BLR(16))
        emit(LDR_POST(30, 31, 16))
    elif t == 30:
        emit(STR_PRE(16, 31, -16)); emit(ADRP(16, here(), KEY_PAGE_OFF)); emit(LDRX(16, 16, 8)); emit(BLR(16))
        emit(MOVR(30, 16)); emit(LDR_POST(16, 31, 16))
    else:
        assert 0 <= t < 31 and t != 18, f'thread_base x{t}'
        emit(STP_PRE(16, 30, 31, -16)); emit(ADRP(16, here(), KEY_PAGE_OFF)); emit(LDRX(16, 16, 8)); emit(BLR(16))
        emit(MOVR(t, 16)); emit(LDP_POST(16, 30, 31, 16))
def STR_PRE(t, n, off): return 0xF8000C00 | ((off & 0x1FF) << 12) | (n << 5) | t    # str xt,[xn,#off]!
def LDR_POST(t, n, off): return 0xF8400400 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldr xt,[xn],#off
def MOVR(dd, m): return 0xAA0003E0 | (m << 16) | dd                                  # mov xd, xm（orr，不改标志 | orr, flags untouched）
def ADRP(dd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12; assert -(1 << 20) <= imm < (1 << 20)
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | dd
def ADDI(dd, n, imm12, sh=0): return 0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def ADDR(dd, n, m): return 0x8B000000 | (m << 16) | (n << 5) | dd
def SUBI(dd, n, imm12, sh=0): return 0xD1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def BR(n): return 0xD61F0000 | (n << 5)
def BLR(n): return 0xD63F0000 | (n << 5)
NOP = 0xD503201F

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS：只改 4 个寄存器字段位置，用反汇编文本核对。 | x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- 各类改写块 ----
# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """指令对 sp 的写回量（前变址 [sp, #i]! / 后变址 [sp], #i）；不写回 sp 返回 0。 | The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

# ---- Android（M1b）：栈安全暂存。Linux/Android 没有 SP 下方的红区，所以暂存一律「先 sub sp 开帧、存正偏移、恢复后 add sp」，
# 任何时刻都不写当前 SP 下方。pick3 多取一个寄存器给「原始 SP」方案用。 ----
# ---- Android (M1b): SP-safe scratch. Linux/Android has no red zone below SP, so all scratch uses "sub sp to open a frame,
# save at positive offsets, restore, then add sp"; nothing is ever written below the live SP. pick3 takes one extra register
# for the saved-original-SP scheme. ----
def pick3(avoid):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:3]; assert len(out) == 3, f'pick3 {avoid}'; return out

def sp_cat(ins):
    """原指令对 SP 的使用类别（Android 补偿用）：none / base_imm / alu_imm / reg_off / writeback。
    | How the original instruction uses SP (for Android compensation): none / base_imm / alu_imm / reg_off / writeback."""
    s = ins.op_str
    if not re.search(r'\b(w?sp)\b', s): return 'none'
    assert not re.match(r'(sp|wsp),', s), f'writes sp (unsupported) {ins.address:x} {ins.op_str}'
    if re.search(r'\]!', s) or re.search(r'\[sp\], #', s): return 'writeback'
    if re.search(r'\[sp, [wx]\w+', s): return 'reg_off'      # [sp, xM] / [sp, xM, lsl #n] 寄存器偏移 | register offset
    if '[sp' in s: return 'base_imm'                          # [sp] / [sp, #imm]
    return 'alu_imm'                                          # add/sub/mov xd, sp(, #imm)

def base_swap(w, addr, K):
    """把指令里作为 SP 的基址寄存器（Rn，位 5..9 = 31）换成 xK；用反汇编核对 'sp'→'xK' 且其余不变。
    | Replace the base register used as SP (Rn, bits 5..9 = 31) with xK; verify via disassembly that 'sp'→'xK' and nothing else changes."""
    orig = dis(w, addr)
    assert (w >> 5) & 31 == 31, f'base_swap: Rn 不是 sp {addr:x} {orig.mnemonic} {orig.op_str}'
    nw = (w & ~(31 << 5)) | (K << 5)
    dd = dis(nw, addr)
    want = re.sub(r'\bsp\b', f'x{K}', orig.op_str)
    # 比较前把 'mov xd, sp'（= add xd, sp, #0）与 'mov xd, xK' 的别名差异抹平 | normalize the mov/add aliasing between 'mov xd, sp' and 'mov xd, xK'
    if orig.mnemonic == 'mov' and dd.mnemonic == 'add' and dd.op_str == f'{orig.op_str.split(",")[0]}, x{K}, #0'.replace('sp', f'x{K}'):
        return nw
    assert dd.mnemonic == orig.mnemonic and dd.op_str == want, f'base_swap 失败 {addr:x}: {orig.mnemonic} {orig.op_str} -> {dd.mnemonic} {dd.op_str}'
    return nw

def site_plain(ins):
    if TARGET == 'android': return site_plain_android(ins)
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # 改 sp 的指令会写到 sp 下方，暂存放到保留区深处 | Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # 一律写回：capstone 会漏报写入（adds w18 实例） | Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(B(here(), ins.address + 4))
    return start

def site_plain_android(ins):
    """Android 普通块：开帧暂存（不写 SP 下方），把原指令对 SP 的引用改成「保存的原始 SP」寄存器 K。
    写回（前/后变址 push/pop）拆成经 K 的存取 + 收帧后调整真实 SP，永远不写当前 SP 下方。
    | Android plain block: open a frame for scratch (never writing below SP), and redirect the original instruction's SP use to
    the saved-original-SP register K. Writeback (pre/post-index push/pop) is decomposed into K-relative accesses plus a real-SP
    adjustment after the frame is closed, so nothing is ever written below the live SP."""
    # 寄存器后变址 `[sp], xN`（SIMD ld1/st1 族）会写回 SP，但 sp_cat 只认立即数后变址，会把它当 base_imm、base_swap 后 SP 写回丢失
    # （Opus r2.2：harness 实测 equivalent=False）。本版游戏 0 处，fail closed。
    # Register post-index `[sp], xN` (SIMD ld1/st1 family) writes SP back, but sp_cat only knows the immediate post-index and would
    # treat it as base_imm, losing the SP writeback after base_swap (Opus r2.2: equivalent=False in the harness). 0 sites in this game; fail closed.
    if re.search(r'\[w?sp\], [wx]\d+', ins.op_str) and 'wbwidth' not in UNSAFE:
        raise RuntimeError(f'{ins.address:x} 寄存器后变址 SP 写回无法建模，fail closed：{ins.mnemonic} {ins.op_str}')
    cat = sp_cat(ins)
    w18 = has18(ins)
    start = here()
    if cat == 'writeback':
        # 前/后变址 push/pop：拆成「开帧取 x18 值 / 槽址 → 经帧偏移 (= 原始 SP ± off) 存取 → 收帧并把真实 SP 调整到最终值」。
        # 帧暂存 S,T 占 [0,16)；存取落在帧顶（原始 SP 一侧）[N+acc, N+acc+8·len)，N 取到两者不重叠，任何写都在当时 SP 上方或等于新 SP。
        # Pre/post-index push/pop: open a frame to fetch the x18 value / slot addr, access via a frame offset (= original SP ± off),
        # then close the frame and move the real SP to its final value. Saved S,T occupy [0,16); accesses land at the frame top
        # (the original-SP side) [N+acc, N+acc+8·len), with N chosen so the two never overlap, so every write is at/above the live SP.
        rnames = [{'fp': 'x29', 'lr': 'x30'}.get(r, r) for r in re.findall(r'\b([wx]zr|[wx]\d+|sp|wsp|fp|lr|[qdshb]\d+)\b', ins.op_str.split('[')[0])]
        if 'wbwidth' not in UNSAFE and (ins.mnemonic not in ('ldr', 'str', 'ldp', 'stp') or not rnames or not all(re.fullmatch(r'x([0-9]|1[0-9]|2[0-9]|30)', r) for r in rnames)):
            raise RuntimeError(f'{ins.address:x} 写回形式不在白名单（只支持 X 通用寄存器的 ldr/str/ldp/stp），fail closed：{ins.mnemonic} {ins.op_str}')
        regs = [{'fp': 29, 'lr': 30}.get(ins.reg_name(op.reg), num(ins.reg_name(op.reg))) for op in ins.operands if op.type == 1]   # 1 = ARM64_OP_REG（mem 前的寄存器列表）；fp/lr 别名映射到 29/30，避免白名单接受 x29/x30 对后 num() 回 None 崩 TypeError（Codex r2 blocker）| register list before the mem operand; map fp/lr aliases to 29/30 so an accepted x29/x30 pair does not make num() return None and raise TypeError (Codex r2 blocker)
        imm = sp_delta(ins); pre = '!' in ins.op_str; is_load = ins.mnemonic.startswith('ld')
        acc = imm if pre else 0
        S, T = pick(set(regs) | regs_in(ins))
        N = 16
        while N + (acc if acc < 0 else 0) < 16: N += 16   # 帧顶的存取不得盖到底部 [0,16) 的 S,T 暂存 | frame-top accesses must not cover the S,T saves at [0,16)
        assert (N + imm) % 16 == 0 and 0 < (N + imm) < (1 << 12), f'写回量 {imm} 使最终 SP 调整不可编码 {ins.address:x}'
        emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0))
        thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))   # S = x18 值（pop 时随后被覆盖）；T = x18 槽址 | S = x18 value (overwritten on pop); T = x18 slot addr
        for i, r in enumerate(regs):
            foff = N + acc + 8 * i            # 帧偏移 = 原始 SP + acc + 8i | frame offset = original SP + acc + 8i
            assert foff % 8 == 0 and 0 <= foff // 8 < (1 << 12)
            rr = S if r == 18 else r
            emit((LDRX if is_load else STRX)(rr, 31, foff // 8))
        if is_load and 18 in regs: emit(STRX(S, T))      # 写回 x18 槽 | write back the x18 slot
        emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N + imm))   # 收帧并到最终 SP = 原始 SP + imm | close frame and reach final SP = original SP + imm
        emit(B(here(), ins.address + 4))
        return start
    if cat == 'none':
        S, T = pick(regs_in(ins)); N = 16
        emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0))
        thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
        emit(subst(ins, S))
        emit(STRX(S, T))
        emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N)); emit(B(here(), ins.address + 4))
        return start
    # base_imm / alu_imm / reg_off：原指令读 SP，用保存的原始 SP 寄存器 K 替换基址 | original reads SP, replace base with saved-original-SP K
    is_store = ins.mnemonic.startswith('st')
    if cat == 'reg_off' and is_store: raise RuntimeError(f'{ins.address:x} reg-offset SP store 无法静态证明在 SP 上方，fail closed：{ins.mnemonic} {ins.op_str}')
    # reg-offset SP load 同样 fail closed：索引寄存器为负时会读进暂存帧（K / S,T / thread_base 压栈），无法静态排除（round-1 I1、Opus r2）；本版游戏 0 处。
    # A reg-offset SP load is also fail-closed: a negative index would read into the scratch frame (K / S,T / thread_base push), which cannot be ruled out statically (round-1 I1, Opus r2); 0 sites in this game.
    if cat == 'reg_off' and 'negload' not in UNSAFE: raise RuntimeError(f'{ins.address:x} reg-offset SP load 索引符号不可静态证明，fail closed：{ins.mnemonic} {ins.op_str}')
    S, T, K = pick3(regs_in(ins)); N = 32
    emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0)); emit(STRX(K, 31, 2)); emit(ADDI(K, 31, N))
    thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    nw = subst(ins, S) if w18 else struct.unpack('<I', ins.bytes)[0]
    if cat == 'base_imm':
        m = re.search(r'\[sp(?:, #(-?(?:0x)?[0-9a-f]+))?\]', ins.op_str)
        off = int(m.group(1), 0) if m and m.group(1) else 0
        # 经 K=原始 SP 访问 orig+off；orig = 当前 SP + N，帧占 [0,N)。off < 0 会落进帧（写会毁暂存、读会读到暂存而非原栈值），fail closed。
        # Accessed via K=original SP at orig+off; orig = live SP + N, frame occupies [0,N). off < 0 lands inside the frame (a store
        # corrupts scratch saves; a load reads scratch saves instead of the original stack slot), so fail closed.
        if off < 0 and (is_store or 'negload' not in UNSAFE):
            raise RuntimeError(f'{ins.address:x} base_imm SP {"store" if is_store else "load"} with negative offset off={off}，fail closed：{ins.mnemonic} {ins.op_str}')
    emit(base_swap(nw, ins.address, K))
    emit(STRX(S, T))
    emit(LDRX(K, 31, 2)); emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N)); emit(B(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    m = ins.mnemonic
    start = here()
    android = TARGET == 'android'; N = 16
    if android: emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0))
    else: emit(STP(S, T, 31, -16))
    thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    exlen = 3 if android else 2              # 每个出口：Android = ldp + add sp + b；Apple = ldp + b | per exit: Android = ldp + add sp + b; Apple = ldp + b
    taken = here() + 4 * (1 + exlen)
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    def exit_to(dest):
        if android: emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N)); emit(B(here(), dest))
        else: emit(LDP(S, T, 31, -16)); emit(B(here(), dest))
    exit_to(ins.address + 4)
    assert here() == taken
    exit_to(tgt)
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    android = TARGET == 'android'; N = 16
    if android: emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0))
    else: emit(STP(S, T, 31, -16))
    thread_base(T); emit_slot_addr(T, S, 'K18')   # S 先作键页偏移暂存，下一条再装目标值 | S first serves as a temporary for the key page offset; the next instruction then loads the target value
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    if android: emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N))
    else: emit(LDP(S, T, 31, -16))
    emit(B(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """跳转表 br x18：目标 = base + 4i。借用 x16/x17 → 等距落地垫 → 恢复 x16 → 原目标。
    Apple 把 x16 暂存红区；Android 没有红区，改用开帧（sub sp），尾部恢复 x16 并收帧后再跳原目标。
    | Jump table br x18: target = base + 4i. Borrow x16/x17 → equally spaced landing pads → restore x16 → original target.
    Apple stashes x16 in the red zone; Android has no red zone, so it opens a frame (sub sp) and each tail restores x16 and
    closes the frame before branching to the original target."""
    android = TARGET == 'android'; N = 16
    start = here()
    if android:
        emit(SUBI(31, 31, N)); emit(STRX(16, 31, 0)); emit(STRX(17, 31, 1))
        thread_base(16); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDRX(17, 31, 1))
    else:
        emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 是函数内可存活的临时寄存器，借用前存红区 | x16 / x17 are temporary registers that can stay live within a function; save them to the red zone before borrowing
        thread_base(16); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * (32 if android else 24)
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    assert here() == padbase, f'site_br {ins.address:#x}: 前导 {here()-start} 超出 padbase 预留'
    tstride = 12 if android else 8           # Android 尾部 3 条（ldr x16; add sp; b），Apple 2 条（ldur x16; b） | Android tail is 3 instrs, Apple 2
    tails = padbase + 4 * count
    for i in range(count): emit(B(here(), tails + tstride * i))
    for i in range(count):
        if android:
            emit(LDRX(16, 31, 0)); emit(ADDI(31, 31, N)); emit(B(here(), base + 4 * i))
        else:
            emit(0xF85F0000 | (31 << 5) | 16)      # ldur x16,[sp,#-16]
            emit(B(here(), base + 4 * i))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = 线程槽值。 | mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    android = TARGET == 'android'
    if N == 18:
        start = here()
        S, T = 0, 1
        if android:
            Nf = 32
            emit(SUBI(31, 31, Nf)); emit(STP(S, T, 31, 0)); thread_base(T)
            emit_slot_addr(T, S, kind); emit(LDRX(S, T)); emit(STRX(S, 31, 2))     # S = kind 槽值，暂存帧 [sp,#16] | S = kind slot value, stash at frame [sp,#16]
            thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, 31, 2)); emit(STRX(S, T))
            emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, Nf)); emit(B(here(), ins.address + 4))
            return start
        emit(STP(S, T, 31, -16)); thread_base(T)
        emit_slot_addr(T, S, kind); emit(LDRX(S, T)); emit(STUR(S, 31, -24))
        thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDUR(S, 31, -24)); emit(STRX(S, T))
        emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
        return start
    tmp = 16 if N != 16 else 17
    start = here()
    if android:
        Nf = 16
        emit(SUBI(31, 31, Nf)); emit(STRX(tmp, 31, 0)); thread_base(N); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDRX(tmp, 31, 0))
        emit(ADDI(31, 31, Nf)); emit(B(here(), ins.address + 4))
        return start
    emit(STUR(tmp, 31, -16)); thread_base(N); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))   # tmp 存红区借用 | tmp is saved by borrowing the red zone
    emit(B(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    (T,) = pick({N}, 1)
    android = TARGET == 'android'
    start = here()
    if android:
        tmp = next(x for x in (16, 17) if x not in (N, T))
        if N == 18:
            U = next(x for x in range(16) if x not in (T, tmp, N))
            Nf = 32
            emit(SUBI(31, 31, Nf)); emit(STP(T, tmp, 31, 0)); emit(STRX(U, 31, 2))
            thread_base(T); emit_slot_addr(T, tmp, 'KTLS')
            thread_base(tmp); emit_slot_addr(tmp, U, 'K18'); emit(LDRX(tmp, tmp)); emit(STRX(tmp, T))
            emit(LDP(T, tmp, 31, 0)); emit(LDRX(U, 31, 2)); emit(ADDI(31, 31, Nf)); emit(B(here(), ins.address + 4))
            return start
        Nf = 16
        emit(SUBI(31, 31, Nf)); emit(STP(T, tmp, 31, 0))
        thread_base(T); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T))
        emit(LDP(T, tmp, 31, 0)); emit(ADDI(31, 31, Nf)); emit(B(here(), ins.address + 4))
        return start
    emit(0xF81F0000 | (31 << 5) | T)           # stur xT,[sp,#-16]
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); thread_base(T); emit_slot_addr(T, tmp, 'KTLS')
    if N == 18:
        U = next(x for x in range(16) if x not in (T, tmp))
        emit(STUR(U, 31, -32)); thread_base(tmp); emit_slot_addr(tmp, U, 'K18')
        emit(LDRX(tmp, tmp)); emit(STRX(tmp, T)); emit(LDUR(U, 31, -32))
    else:
        emit(STRX(N, T))
    emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)           # ldur xT,[sp,#-16]
    emit(B(here(), ins.address + 4))
    return start

def emit_gate_call():
    """门块公共段（紧跟 stp x29,x30,[sp,#-16]! 之后、blr x30 之前）：x30 = 线程槽 KGATE 的门地址。
    键页取槽要借第二个寄存器，借 x29 后立即从栈上恢复：_dm_gate 存下的 x29 必须是客体原值——
    宿主按 [x29+8] 认调用者（技能绑定观测门 6、私有瞄准门 svc86）、崩溃回溯也走 x29 链
    （r24 方案 C 起借了不还，宿主拿到的 x29 是键页里的槽偏移，技能图标退回占位字）。
    Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be the guest's original value --
        the host identifies the caller by [x29+8] (skill binding observation gate 6, private aiming gate svc86), and the crash backtrace also walks the x29 chain
        (since r24 plan C it was borrowed and not returned, the x29 the host got was the slot offset from the key page, and skill icons fell back to placeholder text)."""
    thread_base(30); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → 宿主门。门地址取自线程槽 KGATE；门按返回点（本块 ldp 的地址）查 gates.tsv。 | svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """观测门：入口原指令 → 进宿主门（arg = 观测编号）→ 恢复 x29/x30 → 执行原指令 → 跳回 off+4。
    门块前半与 site_gate 逐字同构；宿主只读寄存器、不改（dm_gate 回写的 x0–x8 即原值）。
    Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; the host only reads registers and does not modify them (the x0–x8 written back by dm_gate are the original values)."""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'观测门原指令不可搬 {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(B(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- 独占窗口 ----
# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i + 1
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST + EXCL_LD: j += 1
        if j >= len(insl) or j - i >= 16 or insl[j].mnemonic not in EXCL_ST:
            # TOTK: the window ends at the first store-exclusive, and a second load-exclusive first means no window.
            # Out-of-line exclusive loads are grouped by linked_windows; the Android safety gate is the branch-following
            # reservation_committed_sites check after rewriting (a linear scan here wrongly rejected 1.4.3's grouped windows).
            continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def linked_windows(insl, windows):
    """Move an out-of-line LDXR retry together with the STXR it re-enters.

    The TSD/scratch prologue must precede LDXR. Entering a suffix trampoline
    between LDXR and STXR would insert stores into the reservation interval
    and can prevent the retry from making progress on real hardware.
    """
    groups = {}
    edges = set()
    original = list(windows)
    for j, ins in enumerate(insl):
        if not (ins.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.')):
            continue
        target = ins.operands[-1].imm
        owner = next((seg for seg in original if seg[0].address < target <= seg[-1].address), None)
        if owner is None or owner[0].address <= ins.address <= owner[-1].address:
            continue
        k = j
        while k >= max(0, j - 15) and insl[k].mnemonic not in EXCL_LD:
            k -= 1
        assert k >= max(0, j - 15), f'exclusive alternate entry lacks LDXR: {ins.address:x}'
        retry = insl[k:j + 1]
        assert all(x.mnemonic not in EXCL_ST and not x.mnemonic.startswith(('st', 'bl'))
                   and x.mnemonic not in ('b', 'br', 'ret')
                   and not x.mnemonic.startswith('b.') for x in retry[:-1]), f'unsafe exclusive retry: {ins.address:x}'
        assert all(not any(seg[0].address <= x.address <= seg[-1].address for seg in original)
                   for x in retry), f'overlapping exclusive retry: {ins.address:x}'
        group = groups.setdefault(owner[0].address, [owner])
        if not any(seg[0].address == retry[0].address for seg in group):
            group.append(retry); windows.append(retry)
        edges.add((ins.address, target))
    return list(groups.values()), edges

def sp_rebase(word, address, n):
    """Android windows run their copies with sp lowered by n. Return word with its SP-based immediate offset raised by n
    (load/store unsigned-offset, unscaled, or pair signed-offset forms, no writeback); any other SP use fails closed."""
    ins = dis(word, address)
    if not re.search(r'\b(w?sp)\b', ins.op_str): return word
    mem = [o for o in ins.operands if o.type == ARM64_OP_MEM]
    if len(mem) != 1 or ins.reg_name(mem[0].mem.base) != 'sp' or mem[0].mem.index or ins.writeback:
        raise RuntimeError(f'window copy {address:#x} {ins.mnemonic} {ins.op_str}: SP use cannot be rebased, Android fails closed')
    disp = mem[0].mem.disp
    if word & 0x3B000000 == 0x39000000:                 # LDR/STR (unsigned immediate), imm12 scaled
        scale = (word >> 30) & 3
        if word & (1 << 26) and (word >> 23) & 1: scale = 4    # 128-bit SIMD
        imm = ((word >> 10) & 0xFFF) + (n >> scale)
        if n % (1 << scale) or imm > 0xFFF: raise RuntimeError(f'window copy {address:#x}: rebased offset out of range')
        new = (word & ~(0xFFF << 10)) | (imm << 10)
    elif word & 0x3B200C00 == 0x38000000:               # LDUR/STUR (unscaled imm9)
        imm = disp + n
        if not -256 <= imm < 256: raise RuntimeError(f'window copy {address:#x}: rebased offset out of range')
        new = (word & ~(0x1FF << 12)) | ((imm & 0x1FF) << 12)
    elif word & 0x3B800000 == 0x29000000:               # LDP/STP (signed offset), imm7 scaled
        opc, v = (word >> 30) & 3, (word >> 26) & 1
        scale = (2 + opc) if v else (2 + (opc >> 1))
        imm = disp + n
        if imm % (1 << scale) or not -64 <= imm >> scale < 64: raise RuntimeError(f'window copy {address:#x}: rebased offset out of range')
        new = (word & ~(0x7F << 15)) | (((imm >> scale) & 0x7F) << 15)
    else:
        raise RuntimeError(f'window copy {address:#x} {ins.mnemonic} {ins.op_str}: unknown SP form, Android fails closed')
    chk = dis(new, address)
    m2 = [o for o in chk.operands if o.type == ARM64_OP_MEM]
    assert chk.mnemonic == ins.mnemonic and m2 and m2[0].mem.disp == disp + n and chk.reg_name(m2[0].mem.base) == 'sp', hex(address)
    return new

def site_window_group(segments):
    used = set()
    for seg in segments:
        for ins in seg: used |= regs_in(ins)
    S, T = pick(used)
    android = TARGET == 'android'; N = 16
    entries = {}; entry_branches = []
    for seg in segments:
        entries[seg[0].address] = here()
        if android: emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0)); thread_base(T)
        else: emit(STP(S, T, 31, -16)); thread_base(T)   # Apple: mrs xT, tpidrro_el0
        emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
        entry_branches.append((len(patch), seg[0].address)); emit(NOP)
    copied = {}; cursor = here()
    for seg in segments:
        for ins in seg: copied[ins.address] = cursor; cursor += 4
        cursor += 4  # explicit fallthrough from each fragment
    exits = []
    for seg in segments:
        exits.append(seg[-1].address + 4)
        for ins in seg:
            if ins.mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.'):
                target = ins.operands[-1].imm
                if target not in copied and target not in exits: exits.append(target)
    exits = list(dict.fromkeys(exits))
    exit_addresses = {target: cursor + 16 * i for i, target in enumerate(exits)}
    for offset, target in entry_branches:
        struct.pack_into('<I', patch, offset, B(PATCH_OFF + offset, copied[target]))
    for seg in segments:
        for ins in seg:
            assert here() == copied[ins.address]
            word = subst(ins, S) if has18(ins) else struct.unpack('<I', ins.bytes)[0]
            if android: word = sp_rebase(word, ins.address, N)   # the copy runs with sp lowered by N
            mnemonic = ins.mnemonic
            if mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or mnemonic.startswith('b.'):
                target = ins.operands[-1].imm
                destination = copied.get(target, exit_addresses.get(target))
                delta = (destination - here()) >> 2
                if mnemonic == 'b': word = B(here(), destination)
                elif mnemonic in ('tbz', 'tbnz'): word = (word & 0xFFF8001F) | ((delta & 0x3FFF) << 5)
                else: word = (word & 0xFF00001F) | ((delta & 0x7FFFF) << 5)
                assert dis(word, here()).operands[-1].imm == destination
            else:
                assert mnemonic not in ('adr', 'adrp', 'bl', 'br', 'blr', 'ret') and not (mnemonic == 'ldr' and '[' not in ins.op_str)
            emit(word)
        emit(B(here(), copied.get(seg[-1].address + 4, exit_addresses[seg[-1].address + 4])))
    for target in exits:
        assert here() == exit_addresses[target]
        if android: emit(STRX(S, T)); emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N)); emit(B(here(), target))   # 16 bytes, same stride
        else: emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), target)); emit(NOP)
    return entries

def site_window(seg):
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    android = TARGET == 'android'; N = 16
    # Android 整块相对化后，副本指令在 sub sp 之后执行；若任何原指令引用 SP，补偿语义会改变锁字 fail-closed。
    # Android: after whole-window relocation the copy runs with sp lowered; if any original instruction references SP, fail closed.
    start = here()
    if android: emit(SUBI(31, 31, N)); emit(STP(S, T, 31, 0))
    else: emit(STP(S, T, 31, -16))
    thread_base(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    exits = copy0 + 4 * n
    # The copied body falls through to the first exit stub. That stub MUST be
    # hi; placing an external conditional target first misroutes the untaken
    # edge (TOTK main+0x72afdc and main+0x7aa630).
    ex_targets = [hi]; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        if android: w = sp_rebase(w, x.address, N)   # the copy runs with sp lowered by N
        m = x.mnemonic
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * (len(ex_targets) - 1)
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    for t in ex_targets:   # ex_targets[0] is hi (TOTK's fix): the copy falls through into it
        if android: emit(STRX(S, T)); emit(LDP(S, T, 31, 0)); emit(ADDI(31, 31, N)); emit(B(here(), t))   # 4 instrs = 16 bytes, same stride as Apple
        else: emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), t)); emit(NOP)
    return start, lo, hi

# ---- 函数边界（eh_frame_hdr）----
# ---- Function boundaries (eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs = m0 + struct.unpack_from('<i', IMG, m0 + 16)[0]
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    v, sz = {0x3: (struct.unpack_from('<I', IMG, o)[0], 4), 0xB: (struct.unpack_from('<i', IMG, o)[0], 4),
             0x4: (struct.unpack_from('<Q', IMG, o)[0], 8)}[fmt]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
_, penc, cenc, tenc = IMG[ehs:ehs + 4]
o = ehs + 4
_, s = rd(o, penc); o += s
nfde, s = rd(o, cenc); o += s
starts = set()
for _ in range(nfde):
    a, s = rd(o, tenc); o += s
    _, s = rd(o, tenc); o += s
    starts.add(a)
starts = sorted(x for x in starts if x < TEXT_END)

# ---- 线性扫描找全部改写点（不靠寄存器读写报告）----
# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
kinds = {}
for o in range(0, TEXT_END, 4):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    # x18/w18 只会占这些寄存器字段；无匹配的字无需反汇编 | x18/w18 can only occupy these register fields; skip words without a match
    # x18/w18 can only occupy these register fields; skip words without a match
    if X18 == 'native' or not any((w >> p) & 31 == 18 for p in (0, 5, 10, 16)):
        continue
    for ins in md.disasm(IMG[o:o + 4], o):
        if has18(ins): kinds[o] = ('x18', None)

# 按函数取指令表，找窗口与跳转表
# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END) if i >= 0 else (a, a + 4)
done = set(); windows = []; window_groups = []; jumps = {}; calls = {}
# Signed 32-bit offset tables added in TOTK 1.4.3. Validate both the dispatch
# instructions and every table entry before using a bounded landing-pad span.
# Each tuple is (ADR base, table address, expected offsets, dispatch bytes).
SIGNED_TABLES = {'277178b7dba1b6d4949a84778d4abc58': {
    0x393bb0: (0x393ba0, 0x2d8626c, (764,260,248,20,80,1028), '12000010217aa0b85202018b2001271e40021fd6'),
    0x596d30: (0x596d20, 0x2d89154, (536,544,20,552,560,568,576,-3136,-3304), '12000010207ab0b85202008bfe3740f940021fd6'),
    0x598ef4: (0x598ee4, 0x2d89038, (680,688,20,696,704,712,720,-5688,-2640), '12000010207ab0b85202008bdef747f940021fd6'),
    0x5f2a9c: (0x5f2a90, 0x2d892e0, (884,892,16,900,-6516), 'f0030f2a12000010207ab0b85202008b40021fd6'),
    0x6397d8: (0x6397cc, 0x2d88e7c, (1376,1384,16,1392,1400,1408,-7264), 'fb0301aa12000010207ab0b85202008b40021fd6'),
}}
FLAG_SETTERS = ('cmp', 'cmn', 'tst', 'adds', 'subs', 'ands', 'bics', 'adcs', 'sbcs', 'ccmp', 'ccmn', 'fcmp', 'fcmpe', 'fccmp', 'fccmpe', 'negs', 'ngcs')
def ldrsw_table(insl, j, fa, fb):
    """Breath of the Wild 1.6.0 (older clang): a table of signed 32-bit offsets from its own base,
        cmp wK, #n-1 ; b.hi default ; [mov wI, wK] ; adrp xT, page ; add xT, xT, #lo ; ldrsw xE, [xT, xI, lsl #2] ; add x18, xE, xT ; br x18
    (T, E, I may be x18). The table is read from the image; returns (base, count) covering every target, each one checked to be an
    instruction of this function, or None when the dispatch does not have exactly this shape."""
    ops = lambda p: [o.strip() for o in p.op_str.replace('[', '').replace(']', '').split(',')]
    ctx = insl[max(0, j - 48):j]
    writes = lambda p, r: r is not None and p.mnemonic not in ('str', 'stur', 'stp', 'strb', 'strh', 'cmp', 'cmn', 'tst', 'b', 'bl', 'br', 'blr', 'ret',
                                                               'cbz', 'cbnz', 'tbz', 'tbnz') \
        and not p.mnemonic.startswith('b.') and ops(p)[0] in (f'x{r}', f'w{r}')
    k = len(ctx) - 1
    if k < 0 or ctx[k].mnemonic != 'add' or len(ops(ctx[k])) != 3 or ops(ctx[k])[0] != 'x18':
        return None
    a, b = ops(ctx[k])[1:]
    for k2 in range(k - 1, -1, -1):              # the load of the entry: ldrsw xE, [xT, xI, lsl #2] with {E, T} = {a, b}
        p = ctx[k2]
        if p.mnemonic == 'ldrsw' and 'lsl #2' in p.op_str:
            o = ops(p)
            if len(o) == 4 and {o[0], o[1]} == {a, b} and o[0] != o[1]:
                break
        if writes(p, num(a)) or writes(p, num(b)):
            return None
    else:
        return None
    E, T, I = num(o[0]), num(o[1]), num(o[2])
    if None in (E, T, I) or any(writes(p, T) or writes(p, I) for p in ctx[k2 + 1:k]):
        return None
    k3 = k2 - 1                                  # the table base: adrp xT, page ; add xT, xT, #lo
    while k3 >= 0 and not writes(ctx[k3], T): k3 -= 1
    if k3 < 1 or ctx[k3].mnemonic != 'add' or ops(ctx[k3])[:2] != [f'x{T}', f'x{T}'] or not ops(ctx[k3])[2].startswith('#'):
        return None
    lo = int(ops(ctx[k3])[2][1:], 0)
    k4 = k3 - 1
    while k4 >= 0 and not writes(ctx[k4], T): k4 -= 1
    if k4 < 0 or ctx[k4].mnemonic != 'adrp' or ops(ctx[k4])[0] != f'x{T}':
        return None
    table = ctx[k4].operands[1].imm + lo
    src = {I}; k5 = k2 - 1                      # the bound: b.hi / b.hs after a cmp on the index or on the register it was copied from
    while k5 >= 0 and ctx[k5].mnemonic not in ('b.hi', 'b.hs'):
        p = ctx[k5]
        if p.mnemonic in FLAG_SETTERS or p.mnemonic in ('b', 'br', 'ret') or p.mnemonic.startswith('b.'):
            return None
        if p.mnemonic == 'mov' and len(ops(p)) == 2 and num(ops(p)[0]) in src and num(ops(p)[1]) is not None:
            src = {num(ops(p)[1])}
        elif any(writes(p, r) for r in src):
            return None
        k5 -= 1
    if k5 < 1:
        return None
    k6 = k5 - 1
    while k6 >= 0 and ctx[k6].mnemonic not in FLAG_SETTERS:
        if any(writes(ctx[k6], r) for r in src) or ctx[k6].mnemonic.startswith('b'):
            return None
        k6 -= 1
    if k6 < 0 or ctx[k6].mnemonic != 'cmp' or len(ops(ctx[k6])) != 2 or num(ops(ctx[k6])[0]) not in src or not ops(ctx[k6])[1].startswith('#'):
        return None
    n = int(ops(ctx[k6])[1][1:], 0) + (1 if ctx[k5].mnemonic == 'b.hi' else 0)
    if not 0 < n <= 4096:
        return None
    entries = struct.unpack_from('<' + 'i' * n, IMG, table)
    targets = [table + e for e in entries]
    assert all(fa <= t < fb and t % 4 == 0 for t in targets), f'ldrsw table {insl[j].address:x}: target outside its function'
    return min(targets), (max(targets) - min(targets)) // 4 + 1

funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
for fa, fb in funcs:
    insl = list(md.disasm(bytes(IMG[fa:fb]), fa))
    ws = find_windows(insl)
    groups, linked_edges = linked_windows(insl, ws)
    window_groups.extend(groups)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            ctx = insl[max(0, j - 32):j]  # TOTK: arithmetic may follow the table-address setup.
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    idx = num(p.reg_name(p.operands[2].reg))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = 65536
                        if width: break
                    break
            if not width and ins.address in SIGNED_TABLES.get(BUILD_ID, {}):
                adrbase, table, offsets, dispatch = SIGNED_TABLES[BUILD_ID][ins.address]
                assert IMG[ins.address - 16:ins.address + 4].hex() == dispatch, f'signed-table dispatch {ins.address:x}'
                assert struct.unpack_from('<' + 'i' * len(offsets), IMG, table) == offsets, f'signed-table entries {ins.address:x}'
                targets = [adrbase + offset for offset in offsets]
                assert all(0 <= target < TEXT_END and target % 4 == 0 for target in targets)
                base, width = min(targets), (max(targets) - min(targets)) // 4 + 1
            if not width:
                found = ldrsw_table(insl, j, fa, fb)
                if found:
                    base, width = found; stat('ldrsw_table')
            assert base and width, f'jump table {ins.address:x}'
            jumps[ins.address] = (base, width)
        if ins.mnemonic == 'blr' and ins.op_str == 'x18':
            assert j > 0, f'call without prefix {ins.address:x}'
            previous = insl[j - 1]
            assert previous.address + 4 == ins.address
            calls[previous.address] = (ins.address, struct.unpack('<I', previous.bytes)[0])
    # 不许有跳转落进窗口中间
    # No jump may land in the middle of a window
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            assert not (t in inwin and t not in heads and (x.address, t) not in linked_edges), f'branch into window {x.address:x}'

# A call must always execute its preparation word; reject known alternate entry.
if calls:
    call_targets = {a for a, _ in calls.values()}
    assert not call_targets & set(starts), 'function entry into call-window second word'
    for address in range(0, TEXT_END, 4):
        word = struct.unpack_from('<I', IMG, address)[0]
        target = None
        if word & 0x7C000000 == 0x14000000:
            target = address + 4 * (((word & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
        elif word & 0xFF000010 == 0x54000000 or word & 0x7E000000 == 0x34000000:
            target = address + 4 * ((((word >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000)
        elif word & 0x7E000000 == 0x36000000:
            target = address + 4 * ((((word >> 5) & 0x3FFF) ^ 0x2000) - 0x2000)
        assert target not in call_targets, f'branch into call without preparation {address:x}'
    for prefix, (call, word) in calls.items():
        assert prefix not in kinds and prefix not in done and call not in done, f'overlapping call window {call:x}'
    # Check dynamic definitions and RELA pointer targets as well as direct branches.
    dyn = m0 + struct.unpack_from('<i', IMG, m0 + 4)[0]
    tags = {}
    while True:
        tag, value = struct.unpack_from('<qQ', IMG, dyn); dyn += 16
        if tag == 0: break
        tags[tag] = value
    count = struct.unpack_from('<I', IMG, tags[4] + 4)[0]
    for where, addend in decode_relr(IMG, tags, DATA_OFF, KEY_PAGE_OFF):
        assert addend not in call_targets, f'RELR pointer enters unprepared call: {where:x}'
    for i in range(count):
        _, _, _, section, value, _ = struct.unpack_from('<IBBHQQ', IMG, tags[6] + 24 * i)
        assert not section or value not in call_targets, 'dynamic symbol enters unprepared call'
    for table, size in ((tags.get(7), tags.get(8, 0)), (tags.get(23), tags.get(2, 0))):
        if table:
            for off in range(table, table + size, 24):
                _, info, addend = struct.unpack_from('<QQq', IMG, off)
                assert info & 0xFFFFFFFF != 1027 or addend not in call_targets, 'relative pointer enters unprepared call'

EXCL_LDW = (0x08400000, 0x3F400000)   # 独占加载位型（含 ldar? 否——仅 ldxr/ldaxr/ldxp/ldaxp 系） | exclusive-load bit shape
def _is_ldx(w): return w & 0x3F800000 == 0x08000000 and (w >> 22 & 1) and not (w >> 31 == 0 and w >> 21 & 1)
def _is_stx(w): return w & 0x3F800000 == 0x08000000 and not (w >> 22 & 1) and not (w >> 31 == 0 and w >> 21 & 1)
def _is_clrex(w): return w & 0xFFFFF0FF == 0xD503305F
def _sx(v, b): return v - (1 << b) if v >> (b - 1) & 1 else v
def _succ(o):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w & 0xFC000000 == 0x14000000: return (o + 4 * _sx(w & 0x3FFFFFF, 26),)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: return (o + 4, o + 4 * _sx((w >> 5) & 0x7FFFF, 19))
    if w & 0x7E000000 == 0x36000000: return (o + 4, o + 4 * _sx((w >> 5) & 0x3FFF, 14))
    if w & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000): return ()         # ret / br：不跟 | ret / br: not followed
    return (o + 4,)
def reservation_committed_sites():
    """分支跟随（非线性）求「独占区」：从每条独占加载出发前向可达、遇独占存/独占载/clrex 为界，再与「能到达本函数匹配独占存」反向求交 =
    真正会被独占监视器守护、且该路径会提交 stxr 的指令集合。线性普查会把 ldxr→下一条文本 stxr 之间（可能跨函数 / 跨循环退出）全算进去，这里排除那些假阳性。
    | Branch-following (not linear) reservation set: from each exclusive-load, forward-reachable up to an exclusive-store / exclusive-load / clrex barrier, intersected with "can reach the matching exclusive-store" =
    exactly the instructions guarded by the monitor on a path that will commit the stxr. A linear census counts everything between an ldxr and the next textual stxr (possibly across functions / loop exits); those false positives are excluded here."""
    import collections
    bounds = list(zip([0] + starts, starts + [TEXT_END]))
    hits = {}
    for fa, fb in bounds:
        lds = [o for o in range(fa, fb, 4) if _is_ldx(struct.unpack_from('<I', IMG, o)[0])]
        for L in lds:
            F = set(); st = list(_succ(L))
            while st:
                n = st.pop()
                if not (fa <= n < fb) or n in F: continue
                F.add(n); w = struct.unpack_from('<I', IMG, n)[0]
                if _is_ldx(w) or _is_stx(w) or _is_clrex(w): continue    # 界：监视器被刷新 / 提交 / 清除 | barrier: monitor renewed / committed / cleared
                st += list(_succ(n))
            pred = collections.defaultdict(list)
            for n in F:
                if _is_ldx(struct.unpack_from('<I', IMG, n)[0]) or _is_stx(struct.unpack_from('<I', IMG, n)[0]) or _is_clrex(struct.unpack_from('<I', IMG, n)[0]): continue
                for s in _succ(n):
                    if s in F: pred[s].append(n)
            # TOTK: an untracked br (jump tables other than the x18 ones in `jumps`, tail calls) ends the region instead of
            # counting as a possible commit. Compiled LL/SC never spans an indirect branch; treating it as one marked whole
            # fail paths of TOTK's compare-and-swap loops (main 0xb45d18, 0xc86680). DM_RC_BR_COMMITS=1 restores the old rule.
            br_commits = os.environ.get('DM_RC_BR_COMMITS') == '1'
            B = {n for n in F for w in (struct.unpack_from('<I', IMG, n)[0],) if _is_stx(w) or (br_commits and w & 0xFFFFFC1F == 0xD61F0000 and n not in jumps)}; st = list(B)
            while st:
                n = st.pop()
                for p in pred[n]:
                    if p not in B: B.add(p); st.append(p)
            if B: B.add(L)
            for n in B:
                if n in kinds: hits.setdefault(n, L)
    return hits
if TARGET == 'android':
    # 独占区安全闸（M1b 要点 3）：独占区内的改写点只能是被整块相对化的 x18 窗口；
    # 其余种类（tpidr/tpidrro/svc/cnt）出现在独占区内无法安全逐点改写（改写块里的 blr / 访存会清掉监视器 → stxr 永远失败）——fail closed。
    # Exclusive-region safety gate (M1b item 3): the only rewrite sites allowed inside a reservation are x18 sites in a wholly
    # relocated window; any other kind inside a reservation cannot be safely rewritten per-site (the rewrite block's blr / memory
    # accesses clear the monitor → the stxr fails forever) — fail closed.
    _rc = reservation_committed_sites()
    _bad = []
    for a, L in sorted(_rc.items()):
        k = kinds[a][0]
        if k != 'x18' or a not in done:
            _bad.append((a, k, L, 'relocated' if a in done else 'per-site'))
    if _bad and 'llsc' not in UNSAFE:
        raise SystemExit('【红】LL/SC 独占区内有未整块相对化的改写点（Android fail closed）：' +
                         '; '.join(f'{a:#x} {k}（独占加载 {L:#x}，处置 {d}）' for a, k, L, d in _bad))

def put_branch(at, to): struct.pack_into('<I', IMG, at, B(at, to))
grouped_heads = set()
for group in window_groups:
    for head, entry in site_window_group(group).items():
        grouped_heads.add(head); put_branch(head, entry); stat('window')
for wseg in windows:
    if wseg[0].address in grouped_heads: continue
    st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for prefix, (call, word) in sorted(calls.items()):
    st = here()
    if TARGET == 'android':
        # Android C window: no red zone and no tpidrro_el0. Built only from the standard thread_base(30) shape and a
        # standard slot-address tail, so structck's host-entry shape check covers it. x30 is free here: the blr x30 at
        # the original call address writes the link register. x16 is preserved with pre/post-index pushes.
        # prefix; thread_base(30); str x16,[sp,#-16]!; adrp x16,KEY; ldr x16,[x16,#K18]; add x30,x30,x16; ldr x30,[x30];
        # ldr x16,[sp],#16; b call
        movable_store(word, prefix)
        emit(word); thread_base(30)
        emit(STR_PRE(16, 31, -16)); emit_key_addr(16, 'K18'); emit(ADDR(30, 30, 16)); emit(LDRX(30, 30))
        emit(LDR_POST(16, 31, 16)); emit(B(here(), call))
    else:
        block = prepare_call(word, prefix, call, st, KEY_PAGE_OFF, KEY_INDEX['K18'])
        fix.append((len(patch) + 12, 'K18'))
        patch.extend(block)
    put_branch(prefix, st)
    assert struct.unpack_from('<I', IMG, call)[0] == BLR_X18
    struct.pack_into('<I', IMG, call, BLR_X30)
    done.update((prefix, call)); stat('call_window')
for a in sorted(kinds):
    if a in done: continue
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br': st = site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m in ('blr', 'ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    put_branch(a, st)


obs = list(OBS.get(BUILD_ID, []))
for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'观测门原指令不符 {off:#x}：{w:08x}'
    # 只许函数入口：dm_gate 只回写 x0–x8 / x30，宿主 C 代码可能改 x9–x17；入口处这些按 AAPCS64 不承载值（参数只在 x0–x7 / x8）
    # Function entries only: dm_gate writes back only x0–x8 / x30, and host C code may modify x9–x17; at entry these carry no values under AAPCS64 (arguments are only in x0–x7 / x8)
    assert off in set(starts) or (oid == 6 and off == 0x43ecc8), f'private obs point not approved {off:#x}'
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'观测门与改写点冲突 {off:#x}'
    put_branch(off, site_obs(off, oid, w)); stat('obs')



# Parameter slots {NSO build ID: [(offset A, word A, offset B, word B, register, key, reason)]}: none for TOTK.
PARAMS = {}
params = PARAMS.get(BUILD_ID, [])
for oa, wa, ob_, wb, r, key, why in params:
    for o, w0 in ((oa, wa), (ob_, wb)):
        w = struct.unpack_from('<I', IMG, o)[0]
        assert f'{w:08x}' == w0, f'参数位原指令不符 {o:#x}：{w:08x}'
        assert o not in kinds and not any(lo <= o < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'参数位与改写点冲突 {o:#x}'
    for o in range(oa + 4, ob_, 4):
        ins = dis(struct.unpack_from('<I', IMG, o)[0], o)
        assert r not in regs_in(ins), f'参数位中间用到 x{r}：{o:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    tmp = 16 if r != 16 else 17
    if TARGET == 'android':
        Nf = 16
        emit(SUBI(31, 31, Nf)); emit(STRX(tmp, 31, 0)); thread_base(r); emit_slot_addr(r, tmp, key); emit(LDRX(r, r)); emit(LDRX(tmp, 31, 0)); emit(ADDI(31, 31, Nf)); emit(B(here(), oa + 4))
    else:
        emit(STUR(tmp, 31, -16)); thread_base(r); emit_slot_addr(r, tmp, key); emit(LDRX(r, r)); emit(LDUR(tmp, 31, -16)); emit(B(here(), oa + 4))
    struct.pack_into('<I', IMG, oa, B(oa, start)); struct.pack_into('<I', IMG, ob_, NOP); stat('param')


os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
if TARGET == 'android':
    open(P('target'), 'w').write('android-context-v1\n')
elif os.path.exists(P('target')):
    os.unlink(P('target'))
import hashlib
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:
    f.write(hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest() + '  patch_nso.py\n')
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for prefix, (call, _) in sorted(calls.items()): f.write(f'{prefix:#x}\tC\t{call + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why, scope in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{scope}\t{why}\n')   # 列：偏移 原字节 新字节 作用域 理由 | Columns: offset, original bytes, new bytes, scope, reason
with open(P('params.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for oa, wa, ob_, wb, r, key, why in params: f.write(f'{oa:#x}\t{ob_:#x}\tx{r}\t{key}\t{why}\n')
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch), 'PATCH_OFF', hex(PATCH_OFF), 'slot_refs', len(fix), 'gates', len(gates))
# 启动块清单随 main 出：取仓库版本化数据 dm/data/bootlist_<main 构建号>.tsv 原样拷进产物目录并写 sha256（清单入库、不每版重录；
# 记录本身有竞态——同产物三次记录块集合 5292 / 5114 / 5281、发起线程亦有不同——重录只在构建号变化或主动改进时做，提交附 mini A/B）
# The boot block list ships with main: take the repository's versioned data dm/data/bootlist_<main build ID>.tsv, copy it as is into the build output directory and write its sha256 (the list is checked in, not re-recorded for every version;
# the recording itself is racy -- three recordings of the same build output gave block sets of 5292 / 5114 / 5281, and the initiating threads also differed -- re-record only when the build ID changes or for a deliberate improvement, and attach an A/B from the second Mac to the commit)
if NAME == 'main':
    import hashlib, shutil
    BL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', f'bootlist_{BUILD_ID}.tsv')
    if not os.path.isfile(BL):   # 公开仓库不带这份清单（它每行带原版资源块的开头字节）：没有就不预取，启动时少一步提前解码，别的不受影响 | the public repository does not ship this list (each line carries the first bytes of an original resource block): without it there is no prefetch, so launch skips one ahead-of-time decode step and nothing else is affected
        print('bootlist 没有启动块清单，跳过（不预取）。要用的话自己录一份：dm/tools/tact_bootlist.py')
        for n in ('bootlist.tsv', 'bootlist.sha256'):   # 产物目录里以前留下的也去掉：留着会被当成这一次的产物带进安装包和壳的清单 | Leftovers from earlier runs in the build output directory are removed too: if kept they would be taken as outputs of this run and carried into the install package and the shell manifest
            if os.path.exists(os.path.join(outdir, n)): os.remove(os.path.join(outdir, n))
    else:
        hd = open(BL, encoding='utf-8').readline().split()
        if hd[-1:] != [BUILD_ID]: sys.exit(f'【红】{BL} 头行构建号 {hd[-1:]} 与 main {BUILD_ID} 不符')
        shutil.copyfile(BL, os.path.join(outdir, 'bootlist.tsv'))
        sha = hashlib.sha256(open(BL, 'rb').read()).hexdigest()
        open(os.path.join(outdir, 'bootlist.sha256'), 'w').write(f'{sha}  bootlist.tsv\n')
        print('bootlist', os.path.relpath(BL), sha[:12])
