# Per-site equivalence verification: original instruction (original image, x18 is a real register) vs patch block (rewritten image, x18 lives in a TSD slot).
# Several random states per site; compares x0-x30 (x18 compared against the slot value), sp, nzcv, v0-v31, all memory writes, final PC, memory access faults.
# Usage: equiv.py <rewrite build output dir> <module> <NSO> [--seed 1] [--jobs N] [--min-eff 2] [--max-extra 16] [--mutate 300] [--tsv file]
# Coverage: a trial where both sides take a memory access fault only compares the fault address and does not count as "effective"; each site needs at least min-eff effective trials, extra draws are made if short (≤max-extra), and sites still short are listed by name.
# Mutation self-check: draw mutate ordinary rewrite blocks; in half the replaced instruction is changed to nop, in half the target of the trampoline exit b gets +4; all must be judged divergent (a negative control showing the criterion can discriminate).
# Host gate (svc / cntpct / cntvct) semantics are defined by the host and are not run here, structck.py does the structural check; registered cuts (cuts.tsv) are intentional semantic changes and likewise only have their bytes checked by structck.
import sys, os, struct, random, argparse, multiprocessing as mp
from unicorn import *
from unicorn.arm64_const import *
from capstone import *
sys.path.insert(0, os.path.dirname(__file__))
import nsoimg

ap = argparse.ArgumentParser()
ap.add_argument('dir'); ap.add_argument('mod'); ap.add_argument('nso')
ap.add_argument('--seed', type=int, default=1); ap.add_argument('--jobs', type=int, default=os.cpu_count())
ap.add_argument('--min-eff', type=int, default=2); ap.add_argument('--max-extra', type=int, default=16)
ap.add_argument('--mutate', type=int, default=300); ap.add_argument('--tsv')
ap.add_argument('--target', choices=('apple', 'android'), default=None)
A = ap.parse_args()

# An explicit --target wins, --target apple included; <mod>.target decides only when unspecified.
TARGET = A.target
if TARGET is None:
    TARGET = 'android' if (os.path.exists(f'{A.dir}/{A.mod}.target') and open(f'{A.dir}/{A.mod}.target').read().strip() == 'android-context-v1') else 'apple'

IB = 0x100000000
# low 16 MB readable/writable random area
ARENA = 0x0; ARENA_SZ = 0x1000000
TSD = 0x2000000
# Android host-context entry stub (key page index 8 points here; after blr, x16 = context base = TSD)
ENTRY = 0x3000000
SP0 = 0x800000
# Android patched-side host-register poison: real TPIDRRO_EL0 / TPIDR_EL0 are separated from the context / KTLS slot and sit at
# unmapped high addresses (not 0: ARENA is mapped at 0, so a stray `mrs tpidrro; ldr` near 0 would read valid memory and slip through).
# The context reaches the patch area only via the key-page[8] entry stub.
POISON_RO = 0xE000000000
POISON_TP = 0xF000000000
DIR, MOD = A.dir, A.mod
# header: segment boundaries and the thread slot numbers hard-coded into instructions
hdr = nsoimg.header(DIR, MOD)
PATCH_OFF = hdr['PATCH_OFF']
KEY_IDX = {k[4:-4]: v for k, v in hdr.items() if k.startswith('KEY_') and k.endswith('_IDX')}
KEY_SLOT = {k: 300 + i for i, k in enumerate(sorted(KEY_IDX, key=KEY_IDX.get))}
SLOT18, SLOTTLS, SLOTTLR = (KEY_SLOT[k] for k in ('K18', 'KTLS', 'KTLR'))
KEY_PAGE_OFF = hdr['KEY_PAGE_OFF']
pat = bytearray(open(f'{DIR}/{MOD}.patched.bin', 'rb').read())
orig = nsoimg.load(A.nso)
ncut = sum(1 for l in open(f'{DIR}/{MOD}.cuts.tsv') if not l.startswith('#'))
nobs = sum(1 for l in open(f'{DIR}/{MOD}.obs.tsv') if not l.startswith('#')) if os.path.exists(f'{DIR}/{MOD}.obs.tsv') else 0
def deexcl(b):
    # Test only: all exclusive loads/stores (every width, single / pair) → plain ldr/str/ldp/stp; the status cbz/cbnz right after a store → nop. Both images are processed the same way.
    LD = {0: 0x39400000, 1: 0x79400000, 2: 0xB9400000, 3: 0xF9400000}
    ST = {0: 0x39000000, 1: 0x79000000, 2: 0xB9000000, 3: 0xF9000000}
    b = bytearray(b); n = 0
    for o in range(0, len(b) - 4, 4):
        w = struct.unpack_from('<I', b, o)[0]
        if w & 0x3F800000 != 0x08000000: continue
        size, L, pair = w >> 30, (w >> 22) & 1, (w >> 21) & 1
        rs, rt2, rn, rt = (w >> 16) & 31, (w >> 10) & 31, (w >> 5) & 31, w & 31
        if pair:
            if size < 2: continue
            nw = ((0xA9400000 if L else 0xA9000000) if size == 3 else (0x29400000 if L else 0x29000000)) | (rt2 << 10) | (rn << 5) | rt
        else:
            nw = (LD if L else ST)[size] | (rn << 5) | rt
        struct.pack_into('<I', b, o, nw); n += 1
        if not L:
            nx = struct.unpack_from('<I', b, o + 4)[0]
            if nx & 0x7E000000 == 0x34000000 and nx & 31 == rs: struct.pack_into('<I', b, o + 4, 0xD503201F)
    return bytes(b), n
orig, _ = deexcl(orig); pat, _ = deexcl(pat)

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
sites = []; winend = {}; brinfo = {}; callinfo = {}; ngate = 0
for l in open(f'{DIR}/{MOD}.sites.tsv'):
    f = l.split(); a = int(f[0], 16)
    if f[1] == 'W': winend[a] = int(f[2], 16); sites.append(a)
    elif f[1] == 'C': winend[a] = int(f[2], 16); callinfo[a] = winend[a] - 4; sites.append(a)
    elif f[1] == 'J': brinfo[a] = (int(f[2], 16), int(f[3]))
    # host gate: semantics are defined by the host, only a structural check is done
    elif f[1] in ('svc', 'cntpct', 'cntvct'): ngate += 1
    else: sites.append(a)
atomic_groups = nsoimg.exclusive_groups(orig, [(lo, hi) for lo, hi in winend.items() if lo not in callinfo])

def btarget(b, o):
    w = struct.unpack_from('<I', b, o)[0]; imm = w & 0x3FFFFFF
    if imm & 0x2000000: imm -= 0x4000000
    return o + 4 * imm
def plain_block_apple(a):
    """Ordinary rewrite block (stp; indirect slot read via key page; replaced instruction; indirect slot write via key page; ...) → offset of the replaced instruction."""
    s = btarget(pat, a); w0 = struct.unpack_from('<I', pat, s)[0]
    # stp xS, xT, [sp, #off]
    if w0 & 0xFFC003E0 != 0xA90003E0: return None
    ins = list(md.disasm(bytes(pat[s:s + 36]), s))
    if len(ins) < 8 or ins[1].mnemonic != 'mrs' or 'tpidrro_el0' not in ins[1].op_str:
        return None
    if ins[2].mnemonic != 'adrp' or ins[2].operands[1].imm != KEY_PAGE_OFF: return None
    if ins[3].mnemonic != 'ldr' or ins[3].operands[1].mem.base != ins[2].operands[0].reg: return None
    T = ins[1].operands[0].reg
    if ins[4].mnemonic != 'add' or ins[4].operands[0].reg != T or ins[4].operands[1].reg != T: return None
    # ldr xS,[xT]: read the slot
    if ins[5].mnemonic != 'ldr' or ins[5].operands[1].mem.base != T: return None
    # str xS,[xT]: write back to the slot; only ordinary blocks have it, conditional blocks do not
    if ins[7].mnemonic != 'str' or ins[7].operands[1].mem.base != T: return None
    return ins[6].address

def plain_block_android(a):
    """Android plain block: has the blr host entry, and the slot read/writeback pair add Xt,Xt,Xs; ldr Xs,[Xt]; <replaced>; str Xs,[Xt] (only 'none'/base_imm plain blocks, with exactly one replaced instruction in between). → replaced instruction offset."""
    s = btarget(pat, a); w0 = struct.unpack_from('<I', pat, s)[0]
    # sub sp, sp, #N
    if w0 & 0xFFC003FF != 0xD10003FF: return None
    full = list(md.disasm(bytes(pat[s:s + 200]), s))
    # this block only: up to the first b back to text, no spill into the adjacent block
    ins = []
    for x in full:
        ins.append(x)
        if x.mnemonic == 'b' and x.operands and x.operands[0].imm < PATCH_OFF: break
    if not any(x.mnemonic == 'blr' for x in ins): return None
    # ldr/str xS,[xT] with no offset/index → (S, T) else None
    def ldst_base(x):
        if len(x.operands) != 2 or x.op_str.endswith('!') or ',' in x.op_str.split('[')[-1]: return None
        try: return (x.operands[0].reg, x.operands[1].mem.base) if x.operands[1].mem.disp == 0 else None
        except Exception: return None
    for i in range(1, len(ins) - 2):
        p, q = ins[i - 1], ins[i]
        if q.mnemonic != 'ldr' or p.mnemonic != 'add': continue
        lb = ldst_base(q)
        if not lb: continue
        S_, T_ = lb
        # add Xt, Xt, Xs
        if p.operands[0].reg != T_ or p.operands[0].reg != p.operands[1].reg: continue
        if i + 2 >= len(ins) or ins[i + 2].mnemonic != 'str': continue
        sb = ldst_base(ins[i + 2])
        if sb == (S_, T_): return ins[i + 1].address
    return None

def plain_block(a): return plain_block_android(a) if TARGET == 'android' else plain_block_apple(a)
# Conditional branches inside an exclusive window (the status cbz/cbnz has already been turned into nop by deexcl and is not included): a random state almost never draws the "compare equal" direction (2^-32),
# so each branch is forced through both directions once -- in both images the same branch is changed to nop (not taken) / an unconditional b (taken) at the same time, and the rest is compared as usual.
# Random trials alone stay green on a compare-and-swap window whose sequential fall-out exit is placed after the external exits: after a successful write it falls into the "compare not equal" exit.
def cond_target(w, pc):
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: imm = (w >> 5) & 0x7FFFF; imm -= (imm & 0x40000) << 1; return pc + 4 * imm
    if w & 0x7E000000 == 0x36000000: imm = (w >> 5) & 0x3FFF; imm -= (imm & 0x2000) << 1; return pc + 4 * imm
    return None
win_br = {}
for a0, hi0 in winend.items():
    gate = btarget(pat, a0); pre = list(md.disasm(bytes(pat[gate:gate + 80]), gate))
    if TARGET == 'android':
        # Prologue: sub sp; stp; <host-entry blr sequence>; adrp; ldr; add Xt,Xt,Xs; ldr Xs,[Xt]. The copy starts after the slot read.
        copy0 = None
        for i in range(1, len(pre)):
            if pre[i].mnemonic == 'ldr' and pre[i - 1].mnemonic == 'add' and '[' in pre[i].op_str and ']' == pre[i].op_str[-1]:
                copy0 = pre[i].address + 4; break
        assert copy0 is not None and any(x.mnemonic == 'blr' for x in pre), f'Android 窗口块前导不是预期形状 {a0:#x}'
    else:
        assert [x.mnemonic for x in pre[:6]] == ['stp', 'mrs', 'adrp', 'ldr', 'add', 'ldr'], f'窗口块前导不是预期形状 {a0:#x}'
        copy0 = gate + 24
    # TOTK grouped windows (site_window_group): each fragment's entry ends in a b to that fragment's contiguous copy.
    wc = struct.unpack_from('<I', pat, copy0)[0]; w0 = struct.unpack_from('<I', orig, a0)[0]
    if wc & 0xFC000000 == 0x14000000 and w0 & 0xFC000000 != 0x14000000: copy0 = btarget(pat, copy0)
    lst = []
    for c in range(a0, hi0, 4):
        wo = struct.unpack_from('<I', orig, c)[0]; to = cond_target(wo, c)
        if to is None: continue
        cp = copy0 + (c - a0); wp = struct.unpack_from('<I', pat, cp)[0]; tp_ = cond_target(wp, cp)
        assert tp_ is not None, f'窗口副本里找不到对应的条件分支 {c:#x}'
        lst.append((c, wo, to, cp, wp, tp_))
    if lst: win_br[a0] = lst
def enc_b(pc, tgt): return 0x14000000 | (((tgt - pc) >> 2) & 0x3FFFFFF)
mut_sites = []
if A.mutate:
    cand = [a for a in sites if a not in winend and a not in brinfo and plain_block(a)]
    mut_sites = sorted(random.Random(A.seed).sample(cand, min(A.mutate, len(cand))))
    pat_mut = bytearray(pat)
    # the b by which a rewrite block returns to text (the first unconditional b in the block whose target lands back in
    # text)
    def exit_b(a):
        w = struct.unpack_from('<I', pat, a)[0]; o = a + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
        for k in range(o, o + 4 * 24, 4):
            w = struct.unpack_from('<I', pat, k)[0]
            if w & 0xFC000000 == 0x14000000 and k + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) < PATCH_OFF: return k, w
        return None
    def scratch_pair(a):
        """Addresses of the paired framed-scratch stp/ldp xS,xT,[sp,#0] (base sp, disp 0, no writeback) in the block, for the spdown mutation to shift them to [sp,#-32]."""
        s = btarget(pat, a); out = []
        for ins in md.disasm(bytes(pat[s:s + 256]), s):
            if ins.mnemonic == 'b' and ins.operands and ins.operands[0].imm < PATCH_OFF:
                # stop at the return-to-text, no spill into the adjacent block
                break
            # Pick only non-indexed framed scratch stp/ldp xS,xT,[sp,#0] (neither pre-index `!` nor post-index [sp],#i):
            # the post-index thread_base pop ldp x16,x30,[sp],#16, if shifted as scratch, would move SP and be caught by ordinary state divergence (not spviol), contaminating the control.
            # Select by encoding: 64-bit signed-offset stp/ldp (0xA9000000 / 0xA9400000, no writeback), Rn=sp, imm7=0.
            w = struct.unpack_from('<I', pat, ins.address)[0]
            if w & 0xFFC00000 in (0xA9000000, 0xA9400000) and (w >> 5) & 31 == 31 and (w >> 15) & 0x7F == 0:
                out.append(ins.address)
        return out
    # Three mutation kinds, ~a third each: nop (replaced insn → nop), exit+4 (trampoline exit b target +4), spdown (shift the framed-scratch
    # stp/ldp 32 bytes below the live SP, clear of the thread_base push slot — value / SP / registers all unchanged, so ONLY spviol can tell it apart: the negative control for the
    # per-write live-SP criterion; a site without a scratch pair falls back to nop to keep counts stable).
    mut_kind = {}
    for i, a in enumerate(mut_sites):
        if i % 3 == 1:
            eb = exit_b(a)
            if eb: k, w = eb; struct.pack_into('<I', pat_mut, k, (w & 0xFC000000) | ((w + 1) & 0x3FFFFFF)); mut_kind[a] = 'exit+4'; continue
        if i % 3 == 2:
            pair = scratch_pair(a)
            if len(pair) >= 2:
                for pa in pair:
                    w = struct.unpack_from('<I', pat_mut, pa)[0]
                    # [sp,#0] → [sp,#-32]: clear of the thread_base push slot [sp-16, sp) (a 16-byte shift is overwritten by stp x16,x30,[sp,#-16]! → register divergence, not spviol).
                    struct.pack_into('<I', pat_mut, pa, (w & ~(0x7F << 15)) | ((-4 & 0x7F) << 15))
                mut_kind[a] = 'spdown'; continue
        struct.pack_into('<I', pat_mut, plain_block(a), 0xD503201F); mut_kind[a] = 'nop'
    pat_mut = bytes(pat_mut)

GP = [getattr(sys.modules['unicorn.arm64_const'], f'UC_ARM64_REG_X{i}') for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
VR = [getattr(sys.modules['unicorn.arm64_const'], f'UC_ARM64_REG_Q{i}') for i in range(32)]

def mk(image, arena0):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    sz = (len(image) + 0xFFFF) & ~0xFFFF
    u.mem_map(IB, sz, UC_PROT_ALL); u.mem_write(IB, bytes(image))
    u.mem_map(ARENA, ARENA_SZ); u.mem_write(ARENA, arena0)
    u.mem_map(TSD, 0x10000)
    if TARGET == 'android':
        # ret: host context entry stub; x16 set to TSD by the code hook
        u.mem_map(ENTRY, 0x1000); u.mem_write(ENTRY, struct.pack('<I', 0xD65F03C0))
    u.reg_write(UC_ARM64_REG_TPIDRRO_EL0, TSD)
    st = {'w': [], 'fault': None, 'arena': arena0, 'spb': SP0, 'pend': [], 'maxlo': {}}
    def wr(uc, acc, addr, size, val, d):
        st['w'].append((addr, size, val & ((1 << (8 * size)) - 1)))
        # Android SP-safety: record each write's address and the SP at that instruction's start; the next instruction (= SP after this one) finalizes lo = min(SP_before, SP_after).
        # A pre-index push writes at the new SP and a post-index store at the old SP (lo == addr, safe); an ordinary negative-offset red-zone write has lo == SP > addr (a violation).
        if TARGET == 'android' and not (TSD <= addr < TSD + 0x10000): st['pend'].append((addr, st['spb']))
    def bad(uc, acc, addr, size, val, d): st['fault'] = (acc, addr); return False
    # Keep one code hook for the emulator's lifetime. Repeatedly adding/removing
    # hooks lets cached Unicorn translation blocks run past a new stop boundary
    # (1.4.3 main+0x1590c); a fresh emulator does not reproduce that discrepancy.
    def code(uc, address, size, data):
        callback = st.get('code_hook')
        if callback is not None: callback(uc, address, size, data)
    u.hook_add(UC_HOOK_MEM_WRITE, wr)
    u.hook_add(UC_HOOK_MEM_INVALID, bad)
    u.hook_add(UC_HOOK_CODE, code)
    return u, st

def restore(u, st):
    for a, s, v in st['w']:
        if ARENA <= a < ARENA + ARENA_SZ: u.mem_write(a, st['arena'][a:a + s])
    st['w'].clear(); st['fault'] = None; st['spb'] = SP0; st['pend'] = []; st['maxlo'] = {}

def finalize_pend(st, sp_now):
    """Finalize pending writes: lo = min(SP_before, SP_after), keep the max lo per address (addr < it ⇒ written below the live SP at least once).
    Computed on both the patch and the original side; trial flags a violation as "written below live SP by the patch but not by the original" (so low-address data writes are not misjudged)."""
    m = st['maxlo']
    for addr, spb in st['pend']:
        lo = spb if spb < sp_now else sp_now
        if addr not in m or lo > m[addr]: m[addr] = lo
    st['pend'] = []

def state(u, st, patched):
    g = [u.reg_read(r) for r in GP]
    tp = u.reg_read(UC_ARM64_REG_TPIDR_EL0)
    if patched: g[18] = struct.unpack('<Q', u.mem_read(TSD + 8 * SLOT18, 8))[0]; tp = struct.unpack('<Q', u.mem_read(TSD + 8 * SLOTTLS, 8))[0]
    g = g + [tp]
    # Only the runtime's actual TLS slots are private. A guest register-offset
    # store may legitimately land elsewhere in the mapped TSD page (v1.1.2
    # main+0x24dd68 does so in the fixed random corpus). Do not hide that write.
    private_slots = {TSD + 8 * slot for slot in KEY_SLOT.values()}
    w = [x for x in st['w'] if not (patched and x[0] in private_slots and x[1] == 8)]
    return g, u.reg_read(UC_ARM64_REG_SP), u.reg_read(UC_ARM64_REG_NZCV), [u.reg_read(r) for r in VR], w, st['fault']

def run(u, st, start, patched, init):
    restore(u, st)
    for r, v in zip(GP, init['g']): u.reg_write(r, v)
    for r, v in zip(VR, init['v']): u.reg_write(r, v)
    u.reg_write(UC_ARM64_REG_SP, SP0); u.reg_write(UC_ARM64_REG_NZCV, init['nzcv'])
    u.reg_write(UC_ARM64_REG_TPIDR_EL0, init['tls'])
    if not patched: u.reg_write(UC_ARM64_REG_TPIDRRO_EL0, init['tlr'])
    if patched:
        for key, idx in KEY_IDX.items():
            u.mem_write(IB + KEY_PAGE_OFF + 8 * idx, struct.pack('<Q', KEY_SLOT[key] * 8))
        if TARGET == 'android':
            # key page index 8 = host context entry stub
            u.mem_write(IB + KEY_PAGE_OFF + 8 * 8, struct.pack('<Q', ENTRY))
            # Poison the patched side's real host registers; the context arrives only via the key-page[8] entry stub (the code hook sets x16=TSD); any patch-area read/write of the real tpidr(ro) is exposed as HOSTLEAK / a fault.
            u.reg_write(UC_ARM64_REG_TPIDRRO_EL0, POISON_RO)
            u.reg_write(UC_ARM64_REG_TPIDR_EL0, POISON_TP)
        u.mem_write(TSD + 8 * SLOT18, struct.pack('<Q', init['g'][18]))
        u.mem_write(TSD + 8 * SLOTTLS, struct.pack('<Q', init['tls']))
        u.mem_write(TSD + 8 * SLOTTLR, struct.pack('<Q', init['tlr']))
        u.reg_write(GP[18], 0xDEADBEEFDEADBEEF)
    end = {'pc': None}
    if patched:
        seen = {'patch': False}; android = TARGET == 'android'
        def hk(uc, addr, size, d):
            if android:
                # host context entry: changes only x16 = context base (= TSD)
                if addr == ENTRY:
                    uc.reg_write(UC_ARM64_REG_X16, TSD); return
                # = SP after the previous instruction; finalize its pending writes
                sp = uc.reg_read(UC_ARM64_REG_SP)
                if st['pend']: finalize_pend(st, sp)
                st['spb'] = sp
            if addr >= IB + PATCH_OFF: seen['patch'] = True
            elif seen['patch']:
                if start - IB in callinfo and start <= addr < IB + winend[start - IB]:
                    return
                if any(IB + lo <= addr < IB + hi for lo, hi in atomic_groups.get(start - IB, [])):
                    return
                end['pc'] = addr; uc.emu_stop()
        st['code_hook'] = hk
        try: u.emu_start(start, IB + 0x7FFFFFFF, count=400)
        except UcError: pass
        st['code_hook'] = None
        # writes of the last instruction
        if android and st['pend']: finalize_pend(st, u.reg_read(UC_ARM64_REG_SP))
        if end['pc'] is None and st['fault'] is None: end['pc'] = 'NOEXIT'
        # The patch area must not read/write the real host tpidr(ro). If the poison changed (msr tpidr / writing back an mrs result, etc.), mark HOSTLEAK — it differs from the original side's end PC and surfaces as a divergence.
        if android and (u.reg_read(UC_ARM64_REG_TPIDRRO_EL0) != POISON_RO or u.reg_read(UC_ARM64_REG_TPIDR_EL0) != POISON_TP):
            end['pc'] = 'HOSTLEAK'
    elif start - IB in winend:
        lo, hi = start, IB + winend[start - IB]
        android = TARGET == 'android'
        group = atomic_groups.get(start - IB, [(lo - IB, hi - IB)])
        def hk2(uc, addr, size, d):
            # the original side also tracks each write's live SP (both sides are compared)
            if android:
                sp = uc.reg_read(UC_ARM64_REG_SP)
                if st['pend']: finalize_pend(st, sp)
                st['spb'] = sp
            if not any(IB + a <= addr < IB + b for a, b in group): end['pc'] = addr; uc.emu_stop()
        st['code_hook'] = hk2
        try: u.emu_start(start, IB + 0x7FFFFFFF, count=400)
        except UcError: pass
        st['code_hook'] = None
        if android and st['pend']: finalize_pend(st, u.reg_read(UC_ARM64_REG_SP))
        if end['pc'] is None and st['fault'] is None: end['pc'] = 'NOEXIT'
    else:
        st['code_hook'] = None
        try: u.emu_start(start, IB + 0x7FFFFFFF, count=1)
        except UcError: pass
        end['pc'] = u.reg_read(UC_ARM64_REG_PC) if st['fault'] is None else None
        # single original instruction: lo = min(SP0, final SP)
        if TARGET == 'android' and st['pend']: finalize_pend(st, u.reg_read(UC_ARM64_REG_SP))
    s = state(u, st, patched)
    return s + (end['pc'],)

def init_state(rng, kind, site_br=None):
    g = []
    for i in range(31):
        if kind == 0: g.append(rng.randrange(0, 0x10000) & ~7)
        elif kind == 1: g.append(rng.randrange(0, ARENA_SZ - 0x10000))
        else: g.append(rng.getrandbits(64))
    if site_br: g[18] = IB + site_br[0] + 4 * rng.randrange(site_br[1])
    return {'g': g, 'v': [rng.getrandbits(128) for _ in range(32)], 'nzcv': rng.randrange(16) << 28, 'tls': rng.randrange(0, 0x100000) & ~15, 'tlr': rng.randrange(0x100000, 0x200000) & ~15}

# Per-write classification (pure function, so the counterexample can assert it directly without Unicorn): a patch write below its live SP where the original did not → spviol
# (the same-address-different-SP boundary violation included); otherwise original-also-wrote / unexpected-high writes are kept, framed scratch is dropped. plo/olo = max live SP per addr (addr < it ⇒ below SP then).
def classify_writes(writes, ow, plo, olo, sp0=SP0):
    keep = []; spviol = False
    for x in writes:
        a_ = x[0]
        pl = plo.get(a_); below_patch = pl is not None and a_ < pl
        ol = olo.get(a_); below_orig = ol is not None and a_ < ol
        if below_patch and not below_orig:
            spviol = True; keep.append(x); continue
        if a_ in ow: keep.append(x); continue
        if a_ >= sp0: keep.append(x)
        # else framed scratch above live SP: drop
    return keep, spviol

def trial(UO, SO, UP, SP, a, init, noexit_inconclusive=False):
    so = run(UO, SO, IB + a, False, init)
    sp_ = run(UP, SP, IB + a, True, init)
    # A forced window branch can make the (de-exclusived) loop endless on both sides; the states at the instruction
    # budget are then cut at different points (a grouped copy has one extra b per fragment), so the trial says nothing.
    if noexit_inconclusive and so[6] == 'NOEXIT' and sp_[6] == 'NOEXIT': return True, True, []
    ow = {x[0] for x in so[4]}
    spviol = False
    if TARGET == 'android':
        keep, spviol = classify_writes(sp_[4], ow, SP['maxlo'], SO['maxlo'])
        sp_ = sp_[:4] + (keep,) + sp_[5:]
    else:
        sp_ = sp_[:4] + ([x for x in sp_[4] if not (SP0 - 128 <= x[0] < SP0 and x[0] not in ow)],) + sp_[5:]
    both = bool(so[5] and sp_[5])
    same = (so[5] == sp_[5]) if (so[5] or sp_[5]) else (so == sp_)
    flds = [k for k, (x, y) in enumerate(zip(so, sp_)) if x != y]
    if spviol: flds = flds + ['spviol']
    return (same and not spviol), both, flds

def work(job):
    idx, chunk, mutated = job
    rng = random.Random(A.seed * 1000003 + idx * 2 + mutated)
    arena0 = rng.randbytes(ARENA_SZ)
    UO, SO = mk(orig, arena0); UP, SP = mk(pat_mut if mutated else pat, arena0)
    out = []
    for a in chunk:
        n = eff = 0; diffs = []
        kinds = [0, 1] + ([] if a in brinfo else [2])
        extra = 0
        while kinds or (eff < A.min_eff and extra < A.max_extra):
            if kinds: kind = kinds.pop(0)
            else: kind = (1, 0)[extra % 2]; extra += 1
            initial = init_state(rng, kind, brinfo.get(a))
            if a in callinfo:
                initial['g'][18] = IB + 0x1000 + 4 * rng.randrange(1024)
            same, both, fl = trial(UO, SO, UP, SP, a, initial)
            n += 1; eff += not both
            if not same: diffs.append((kind, fl))
            if mutated and diffs: break
        if not mutated:
            for c, wo, to, cp, wp, tp_ in win_br.get(a, ()):
                for mode in ('不跳', '必跳'):
                    UO.mem_write(IB + c, struct.pack('<I', 0xD503201F if mode == '不跳' else enc_b(c, to)))
                    UP.mem_write(IB + cp, struct.pack('<I', 0xD503201F if mode == '不跳' else enc_b(cp, tp_)))
                    UO.ctl_remove_cache(IB + c, IB + c + 4); UP.ctl_remove_cache(IB + cp, IB + cp + 4)
                    for kind in (0, 1, 0, 1):
                        same, both, fl = trial(UO, SO, UP, SP, a, init_state(rng, kind), noexit_inconclusive=True)
                        n += 1; eff += not both
                        if not same: diffs.append((f'窗口分支 {c:#x} 强制{mode}', fl))
                    UO.mem_write(IB + c, struct.pack('<I', wo)); UP.mem_write(IB + cp, struct.pack('<I', wp))
                    UO.ctl_remove_cache(IB + c, IB + c + 4); UP.ctl_remove_cache(IB + cp, IB + cp + 4)
        out.append((a, n, eff, diffs))
    return out

def runall(lst, mutated):
    # chunk count is fixed, so the result varies only with seed, not with jobs
    k = 64
    jobs = [(i, lst[i::k], mutated) for i in range(k) if lst[i::k]]
    with mp.get_context('fork').Pool(A.jobs) as p:
        return [r for part in p.imap_unordered(work, jobs) for r in part]

def ins_of(a): return next(md.disasm(orig[a:a + 4], a))
if __name__ == '__main__':
    res = sorted(runall(sites, 0))
    T = sum(r[1] for r in res); E = sum(r[2] for r in res)
    zero = [r for r in res if r[2] == 0]; low = [r for r in res if r[2] < A.min_eff]
    bad = [r for r in res if r[3]]
    for a, n, eff, diffs in bad:
        i = ins_of(a)
        for kind, fl in diffs: print(f'DIFF {a:#x} {i.mnemonic} {i.op_str} kind={kind} fields={fl}', flush=True)
    for a, n, eff, _ in zero:
        i = ins_of(a); print(f'NOEFF {a:#x} {i.mnemonic} {i.op_str} trials={n}')
    caught = miss = 0; mut_by = '-'
    sp_contaminated = 0
    if mut_sites:
        mres = runall(mut_sites, 1)
        caught = sum(1 for r in mres if r[3])
        for a, n, eff, d in mres:
            if not d: i = ins_of(a); print(f'MUTMISS {a:#x} {mut_kind[a]} {i.mnemonic} {i.op_str} trials={n} eff={eff}')
        # spdown must be caught ONLY via spviol: every divergence includes spviol and no field other than the write list (field 4, a consequence of keeping
        # the spviol write for comparison) differs (registers / SP / NZCV / vectors / fault / end PC identical). Otherwise it is not a spviol control.
        for a, n, eff, d in mres:
            if mut_kind[a] != 'spdown' or not d: continue
            if not all('spviol' in fl and set(fl) <= {4, 'spviol'} for _, fl in d):
                sp_contaminated += 1
                i = ins_of(a); print(f'SPDOWN_NOT_SPVIOL {a:#x} {i.mnemonic} {i.op_str} fields={[fl for _, fl in d]}')
        miss = len(mres) - caught
        mk_ = {}
        for a, n, eff, d in mres: c = mk_.setdefault(mut_kind[a], [0, 0]); c[0] += 1; c[1] += bool(d)
        mut_by = ' '.join(f'{k}:{v[1]}/{v[0]}' for k, v in sorted(mk_.items()))
    if A.tsv:
        with open(A.tsv, 'w') as f:
            f.write('site\ttrials\teff\tdiffs\n')
            for a, n, eff, d in res: f.write(f'{a:#x}\t{n}\t{eff}\t{len(d)}\n')
    # a missed mutation / an spdown not caught via spviol = the criterion is unreliable, fail
    ok = not bad and not zero and not miss and not sp_contaminated
    print(f'EQUIV {MOD} {"OK" if ok else "FAIL"} 站点={len(res)} 试验={T} 有效试验={E} 双异常={T - E} '
          f'有效≥1站点={len(res) - len(zero)} 有效≥{A.min_eff}站点={len(res) - len(low)} 零有效={len(zero)} 分歧站点={len(bad)} '
          f'窗口={len(winend)} 其中含条件分支={len(win_br)}（{sum(len(v) for v in win_br.values())} 条×2 向强制） 门站点(结构核对)={ngate} 已登记剪口(结构核对)={ncut} 观测门(结构核对)={nobs} 变异={len(mut_sites)}（{mut_by}） 抓到={caught} 漏抓={miss} seed={A.seed}', flush=True)
    sys.exit(0 if ok else 1)
