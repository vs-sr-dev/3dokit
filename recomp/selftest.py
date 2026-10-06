"""The recompiler's self-test: the interpreter records, the recompiled code replays.

    python -m 3dokit.recomp.selftest --image NAME=FILE --out VECTORS.txt
                                     [--auto] [--funcs A,B,...] [--vectors 16] [--seed 1]
    python -m 3dokit.recomp.selftest --optest DIR

A vectors file (the format is in runtime/arm_selftest.cpp) says which
images to load and which module to activate, then, function by function,
r0-r14 and the flags before and after each call as `armemu` computed them,
and the crc32 of guest memory after the function's vectors. The `selftest`
executable of a generated build replays them on the recompiled code.

Game functions: `--funcs` names them; `--auto` takes every function that,
on a few random register states, returns without an OS call, without a
fault, and writing to none of its code: the arithmetic, the fixed-point and table helpers, and
with r0-r3 pointing into random data, the routines on vectors and
structures. The stack is at the top of DRAM with sl below it, as the APCS
stack check wants. A vector that faults in the interpreter is left out, and
its writes undone.

The instruction test (`--optest`, or `python -m 3dokit.recomp --optest`) is
3dokit's own: a synthetic AIF image with ARMv3's instruction forms under
random conditions -- every data-processing operation with every operand2
form, the multiplies, the single and block transfers in every addressing
mode (unaligned words included), swp, msr and mrs -- each in a function of
its own (`insn; mov pc, lr`), and sequences for the control flow: branches
both ways, loops, bl and APCS frames, the compiler's switch, tail calls,
calls through a register, conditional returns, `ldr pc, [sp], #4`.
"""
import argparse
import os
import random
import struct
import zlib

from .. import arm60
from .. import armemu
from ..aif import AIF, STUB
from . import discover

SENTINEL = armemu.RETURN_SENTINEL
M32 = 0xFFFFFFFF
MEM = 0x300000
EDGE = [0, 1, 2, 31, 32, 33, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
        0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF]
STACK_TOP = 0x1FFF00              # the tests' stack: the top of DRAM
STACK_LIMIT = 0x1F0000            # sl, below it
SCRATCH = 0x240000                # random data pointer arguments point into (in VRAM)
SCRATCH_SIZE = 0x40000
PC, LR, SP = 15, 14, 13


class JournalMemory(armemu.Memory):
    """armemu's memory with an undo journal, so a vector that faults leaves
    no trace (the recompiled side never runs it). A write to a word of
    `protect` (the program's code) faults too: the interpreter would run the
    changed code, the recompiled C++ cannot. Data words among the code may
    be written (hand-written routines park their return address there)."""

    def __init__(self, protect=frozenset()):
        super().__init__()
        self.journal = []
        self.protect = protect

    def write(self, addr, size, value):
        if (addr & ~3 & M32) in self.protect:
            raise armemu.MemoryError_('a write into the code at %08X' % (addr & M32))
        buf, o = self._find(addr & M32, size)
        self.journal.append((buf, o, bytes(buf[o:o + size])))
        super().write(addr, size, value)

    def undo(self):
        for buf, o, old in reversed(self.journal):
            buf[o:o + len(old)] = old
        self.journal = []


def new_memory(images, protect=frozenset()):
    """Guest memory, zero, with the images (data, base) copied in: what
    arm_selftest.cpp starts from."""
    mem = JournalMemory(protect)
    buf = mem.add(0, MEM)
    for data, base in images:
        buf[base:base + len(data)] = data
    return mem


def crc(mem):
    return zlib.crc32(mem.regions[0][2])


def run(mem, entry, st, max_steps):
    cpu = armemu.CPU(mem)
    cpu.r = list(st[:15]) + [0]
    cpu.cpsr = st[15]
    cpu.call(entry, max_steps=max_steps)
    return cpu.r[:15] + [cpu.cpsr]


def rand_word(rng):
    k = rng.random()
    if k < 0.3:
        return rng.getrandbits(32)
    if k < 0.5:
        return rng.randint(-16, 16) & M32
    if k < 0.7:
        return rng.choice(EDGE)
    return rng.getrandbits(16)


def _flags(rng):
    return rng.getrandbits(4) << 28 | 0x10


def rand_state(rng, mode='any'):
    """16 words: r0-r14, cpsr. Modes: any (random), ptr (every register a
    pointer into the optest blob, at any alignment, or a small number), game
    (sp on the tests' stack, sl below it), args (and r0-r3 pointers into the
    scratch data), loop (and r1 a small count), switch (and r0 a small
    index)."""
    w = [rand_word(rng) for _ in range(15)]
    if mode == 'ptr':
        w = [_ptr(rng) if rng.random() < 0.6 else rng.randrange(0, 0x100) for _ in range(15)]
    elif mode in ('game', 'args'):
        w[13], w[10] = STACK_TOP, STACK_LIMIT
        w[11] = rng.choice([0, STACK_TOP + 0x40])
        if mode == 'args':
            for i in range(4):
                w[i] = SCRATCH + rng.randrange(0x100, SCRATCH_SIZE - 0x1000, 4)
    elif mode in ('stack', 'loop', 'switch'):
        w[13] = _ptr(rng) & ~3
        if mode == 'loop':
            w[1] = rng.randint(1, 40)
        if mode == 'switch':
            w[0] = rng.randint(0, 6)
    w[14] = SENTINEL
    return w + [_flags(rng)]


def record(mem, entry, label, states, max_steps=200_000):
    """The vector lines for one function (faulting states left out)."""
    lines = ['func %08X %s' % (entry, label)]
    for st in states:
        mem.journal = []
        try:
            out = run(mem, entry, st, max_steps)
        except (armemu.MemoryError_, armemu.Unpredictable, RuntimeError):
            mem.undo()
            continue
        lines.append('v ' + ' '.join('%08X' % x for x in st + out))
    mem.journal = []
    lines.append('mem %08X' % crc(mem))
    return lines, len(lines) - 2


def image_bytes(aif):
    """What the loader leaves at 0: the image up to its relocation stub
    (the zero-initialised data after it is zero in a new memory)."""
    return aif.d[:aif.stub]


def pure_functions(images, prog, rng, trials=5, max_steps=20_000):
    """{entry: mode} of the functions that return on random states without
    a fault and without writing to the program's code."""
    out = {}
    for e in sorted(prog.funcs):
        for mode in ('game', 'args'):
            mem = new_memory(images, prog.code)
            ok = True
            for _ in range(trials):
                try:
                    run(mem, e, rand_state(rng, mode), max_steps)
                except (armemu.MemoryError_, armemu.Unpredictable, RuntimeError):
                    ok = False
                    break
            if ok:
                out[e] = mode
                break
    return out


def scratch_data(seed=1):
    rng = random.Random(seed * 104729)
    return bytes(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))


# ---- the instruction test --------------------------------------------------------------
BLOB = 0x100000                   # random data the tests read and write
BLOB_SIZE = 0x40000


def _ptr(rng):
    p = BLOB + rng.randrange(0x400, BLOB_SIZE - 0x1000)
    return p & ~3 if rng.random() < 0.7 else p


def _op2_variants(rng):
    """(bits 25 and 11..0 of a data-processing word) for every operand2
    form: immediates rotated and not, every immediate shift at its edges,
    every register shift."""
    out = []
    for rot in (0, 1, 4, 15):
        out.append((1, rot << 8 | rng.getrandbits(8)))
    for typ in range(4):
        for amt in (0, 1, rng.randrange(2, 31), 31):
            out.append((0, amt << 7 | typ << 5 | rng.randrange(16)))
        out.append((0, rng.randrange(16) << 8 | typ << 5 | 1 << 4 | rng.randrange(16)))
    return out


def _bad(i):
    """Words the instruction test leaves out: what writes pc or lr (the
    test's own return), and what armemu refuses."""
    if i.kind == 'undefined' or PC in i.writes() or LR in i.writes():
        return True
    if i.kind == 'bdt' and (i.regs >> PC & 1 or i.rn in (PC, LR) or i.psr):
        return True
    if i.kind == 'sdt' and i.rn == PC and (i.w or not i.p):
        return True
    if i.kind == 'sdt' and i.offset[0] == 'reg' and i.offset[1] == PC:
        return True
    if i.kind == 'sdt' and i.l and (i.w or not i.p) and i.rd == i.rn:
        return True
    if i.kind == 'dp' and i.op2[0] == 'regreg' and i.op2[3] == PC:
        return True
    if i.kind == 'mul' and PC in (i.rm, i.rs, i.rn):
        return True
    return False


def _cond(rng):
    return 14 if rng.random() < 0.5 else rng.randrange(14)


def single_forms(rng):
    """(word, label, mode) for each instruction tested alone."""
    out = []

    def add(w, mode):
        i = arm60.decode(w, 0)
        if not _bad(i):
            out.append((w, mode))

    for op in range(16):
        for s in (0, 1):
            if op in arm60.TEST_OPS and not s:
                continue
            for imm, low in _op2_variants(rng):
                rd = rng.choice([0, rng.randrange(13)])
                rn = rng.choice([rd, rng.randrange(13), PC]) if op not in arm60.MOVE_OPS else 0
                if op in arm60.TEST_OPS:
                    rd = 0
                if low & 0x10 and not imm and rn == PC:
                    rn = 3                              # pc + 12: tested once below
                w = _cond(rng) << 28 | imm << 25 | op << 21 | s << 20 | rn << 16 | rd << 12 | low
                add(w, 'any')
    for typ in range(4):                                # pc read under a register shift: + 12
        add(0xE08F0010 | 1 << 8 | typ << 5 | 2, 'any')  # add r0, pc, r2, <typ> r1
        add(0xE1A0001F | 1 << 8 | typ << 5, 'any')      # mov r0, pc, <typ> r1
    for a in (0, 1):
        for s in (0, 1):
            for _ in range(3):
                rd, rm, rs, rn = rng.sample(range(13), 4)
                if rng.random() < 0.3:
                    rs = rm
                add(_cond(rng) << 28 | a << 21 | s << 20 | rd << 16 | (rn if a else 0) << 12
                    | rs << 8 | 0x90 | rm, 'any')
    for l in (0, 1):
        for b in (0, 1):
            for p in (0, 1):
                for u in (0, 1):
                    for w in (0, 1):
                        for reg in (0, 1):
                            for _ in range(2):
                                rn, rd, rm = rng.randrange(13), rng.randrange(13), rng.randrange(13)
                                if reg:
                                    off = rng.choice([0, 1, 2, 3]) << 7 | rng.randrange(3) << 5 | rm
                                else:
                                    off = rng.getrandbits(8)
                                word = _cond(rng) << 28 | 1 << 26 | reg << 25 | p << 24 | u << 23 | \
                                    b << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | off
                                add(word, 'ptr')
    for l in (0, 1):                                    # literal loads, pc-relative stores of pc
        for u in (0, 1):
            add(0xE51F0000 | u << 23 | l << 20 | rng.randrange(13) << 12 | rng.randrange(0, 64, 4), 'any' if l else 'ptr')
    add(0xE581F000, 'ptr')                              # str pc, [r1]: + 12
    for l in (0, 1):
        for p in (0, 1):
            for u in (0, 1):
                for w in (0, 1):
                    for _ in range(3):
                        rn = rng.randrange(13)
                        regs = rng.getrandbits(13) | 1 << rng.randrange(13)
                        if rng.random() < 0.3:
                            regs |= 1 << rn
                        word = _cond(rng) << 28 | 4 << 25 | p << 24 | u << 23 | w << 21 | l << 20 | rn << 16 | regs
                        add(word, 'ptr')
    add(0xE92D8000 | 0x0003, 'ptr')                     # stmfd sp!, {r0, r1, pc}: + 12
    for b in (0, 1):
        for _ in range(3):
            rd, rn, rm = rng.randrange(13), rng.randrange(13), rng.randrange(13)
            add(_cond(rng) << 28 | 0x01000090 | b << 22 | rn << 16 | rd << 12 | rm, 'ptr')
    for _ in range(3):
        add(_cond(rng) << 28 | 0x010F0000 | rng.randrange(13) << 12, 'any')          # mrs
        add(_cond(rng) << 28 | 0x0128F000 | rng.randrange(13), 'any')                # msr cpsr_f, rm
        add(_cond(rng) << 28 | 0x0328F000 | rng.randrange(16) << 8 | rng.getrandbits(8), 'any')
    return [(w, '%s' % arm60.text(arm60.decode(w, 0)), mode) for w, mode in out]


class Asm:
    """ARM words with labels: word(value | fn(labels, pc)), label(name)."""

    def __init__(self, base):
        self.base, self.items, self.labels = base, [], {}

    def here(self):
        return self.base + 4 * len(self.items)

    def label(self, name):
        self.labels[name] = self.here()

    def w(self, v):
        self.items.append(v)

    def b(self, cond, link, name):
        self.items.append(lambda L, pc, c=cond, l=link, n=name:
                          c << 28 | 0x0A000000 | l << 24 | ((L[n] - pc - 8) >> 2) & 0xFFFFFF)

    def assemble(self):
        out = bytearray()
        for k, v in enumerate(self.items):
            pc = self.base + 4 * k
            out += struct.pack('>I', (v(self.labels, pc) if callable(v) else v) & M32)
        return bytes(out)


RET = 0xE1A0F00E                  # mov pc, lr


def sequences(a):
    """Control flow, hand-assembled: (entry, label, mode) for each."""
    seqs = []

    def seq(label, mode):
        seqs.append((a.here(), label, mode))

    # a subroutine others call: r3 += r1, with an APCS frame and the stack check
    seq('APCS frame', 'stack')
    a.label('SUB')
    a.w(0xE1A0C00D)                     # mov ip, sp
    a.w(0xE92DD800)                     # stmfd sp!, {fp, ip, lr, pc}
    a.w(0xE24CB004)                     # sub fp, ip, #4
    a.w(0xE0833001)                     # add r3, r3, r1
    a.w(0xE91BA800)                     # ldmdb fp, {fp, sp, pc}
    for cond, name in ((0, 'beq'), (1, 'bne'), (10, 'bge'), (8, 'bhi')):
        seq('%s over an add' % name, 'any')
        a.b(cond, 0, 'T%d' % len(seqs))
        a.w(0xE2811001)                 # add r1, r1, #1
        a.label('T%d' % len(seqs))
        a.w(0xE2822002)                 # add r2, r2, #2
        a.w(RET)
    seq('subs/bne loop', 'loop')
    a.label('L%d' % len(seqs))
    a.w(0xE0833002)                     # add r3, r3, r2
    a.w(0xE2511001)                     # subs r1, r1, #1
    a.b(1, 0, 'L%d' % len(seqs))
    a.w(RET)
    seq('bl with lr saved', 'stack')
    a.w(0xE52DE004)                     # str lr, [sp, #-4]!
    a.b(14, 1, 'SUB')
    a.w(0xE2833007)                     # add r3, r3, #7
    a.w(0xE49DF004)                     # ldr pc, [sp], #4
    seq('conditional bl, conditional return', 'stack')
    a.w(0xE92D4000)                     # stmfd sp!, {lr}
    a.w(0xE3510010)                     # cmp r1, #16
    a.b(3, 1, 'SUB')                    # blcc SUB
    a.w(0xE3530000)                     # cmp r3, #0
    a.w(0x08BD8000)                     # ldmeqfd sp!, {pc}
    a.w(0xE2833001)                     # add r3, r3, #1
    a.w(0xE8BD8000)                     # ldmfd sp!, {pc}
    seq('tail call', 'stack')
    a.w(0xE2811003)                     # add r1, r1, #3
    a.b(14, 0, 'SUB')
    seq('call through a register', 'stack')
    a.w(0xE92D4000)                     # stmfd sp!, {lr}
    a.w(lambda L, pc: 0xE59F4000 | 0x00C)            # ldr r4, [pc, #12] -> the literal
    a.w(0xE1A0E00F)                     # mov lr, pc
    a.w(0xE1A0F004)                     # mov pc, r4
    a.w(0xE8BD8000)                     # ldmfd sp!, {pc}
    a.w(0xE1A00000)                     # (pad)
    a.w(lambda L, pc: L['SUB'])         # the literal
    seq('conditional return by mov pc, lr', 'any')
    a.w(0xE3510000)                     # cmp r1, #0
    a.w(0x11A0F00E)                     # movne pc, lr
    a.w(0xE3A02005)                     # mov r2, #5
    a.w(RET)
    seq('switch', 'switch')
    k = len(seqs)
    a.w(0xE3500003)                     # cmp r0, #3
    a.w(0x908FF100)                     # addls pc, pc, r0, lsl #2
    a.b(14, 0, 'DEF%d' % k)
    for n in range(4):
        a.b(14, 0, 'C%d_%d' % (k, n))
    for n in range(4):
        a.label('C%d_%d' % (k, n))
        a.w(0xE3A02000 | 10 + n)        # mov r2, #10+n
        a.w(RET)
    a.label('DEF%d' % k)
    a.w(0xE3A0200D)                     # mov r2, #13
    a.w(RET)
    seq('flags loaded by msr, then conditional', 'any')
    a.w(0xE128F001)                     # msr cpsr_f, r1
    a.w(0x22833001)                     # addcs r3, r3, #1
    a.w(0x02833002)                     # addeq r3, r3, #2
    a.w(0x42833004)                     # addmi r3, r3, #4
    a.w(0x62833008)                     # addvs r3, r3, #8
    a.w(RET)
    seq('adds/adc 64-bit add, rsbs/rsc negate', 'any')
    a.w(0xE0900002)                     # adds r0, r0, r2
    a.w(0xE0A11003)                     # adc r1, r1, r3
    a.w(0xE2744000)                     # rsbs r4, r4, #0
    a.w(0xE2E55000)                     # rsc r5, r5, #0
    a.w(RET)
    return seqs


OPTEST_CODE = 0x100


def optest(seed=1):
    """(AIF image, [(entry, label, mode)]) of the instruction test."""
    rng = random.Random(seed)
    a = Asm(OPTEST_CODE)
    funcs = []
    for w, label, mode in single_forms(rng):
        funcs.append((a.here(), label, mode))
        a.w(w)
        a.w(RET)
    funcs += sequences(a)
    code = a.assemble()
    end = OPTEST_CODE + len(code)
    ro = end
    stub = ro                                           # no rw, the stub right after
    head = bytearray(0x100)
    struct.pack_into('>I', head, 0x00, 0xE1A00000)
    struct.pack_into('>I', head, 0x04, 0xEB000000 | ((stub - 0x04 - 8) >> 2))
    struct.pack_into('>I', head, 0x08, 0xE1A00000)
    struct.pack_into('>I', head, 0x0C, 0xEB000000 | ((funcs[0][0] - 0x0C - 8) >> 2))
    struct.pack_into('>I', head, 0x10, 0xEF000011)
    struct.pack_into('>5I', head, 0x14, ro, 0, 0, 0, 0)
    struct.pack_into('>I', head, 0x30, 0x20)
    img = bytes(head) + code + bytes(STUB) + struct.pack('>I', M32)
    return img, funcs


def write_optest(out_dir, seed=1, vectors=12):
    """Write optest.aif, optest.img (as loaded), optest_blob.bin and
    optest.txt (the vectors) into out_dir; returns (path of the AIF, its
    entries)."""
    os.makedirs(out_dir, exist_ok=True)
    img, funcs = optest(seed)
    rng = random.Random(seed * 7919)
    blob = bytes(rng.getrandbits(8) for _ in range(BLOB_SIZE))
    paths = {}
    for name, data in (('optest.aif', img), ('optest_blob.bin', blob)):
        paths[name] = os.path.abspath(os.path.join(out_dir, name)).replace('\\', '/')
        with open(paths[name], 'wb') as f:
            f.write(data)
    aif = AIF(paths['optest.aif'])
    loaded = image_bytes(aif)
    paths['optest.img'] = os.path.abspath(os.path.join(out_dir, 'optest.img')).replace('\\', '/')
    with open(paths['optest.img'], 'wb') as f:
        f.write(loaded)
    rng = random.Random(seed + 1)
    mem = new_memory([(loaded, 0), (blob, BLOB)], frozenset(range(OPTEST_CODE, aif.ro, 4)))
    lines = ['# 3dokit instruction test: %d functions' % len(funcs),
             'image 00000000 %s' % paths['optest.img'],
             'image %08X %s' % (BLOB, paths['optest_blob.bin']),
             'module OPTEST']
    for entry, label, mode in funcs:
        ls, _ = record(mem, entry, label, [rand_state(rng, mode) for _ in range(vectors)], max_steps=10_000)
        lines += ls
    with open(os.path.join(out_dir, 'optest.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    return paths['optest.aif'], [e for e, _, _ in funcs]


def main(argv=None):
    from .__main__ import parse_spec
    ap = argparse.ArgumentParser(prog='python -m 3dokit.recomp.selftest', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--optest', metavar='DIR', help='write the instruction test into DIR')
    ap.add_argument('--image', help='NAME=FILE[+SEED,...]: the program tested')
    ap.add_argument('--funcs', default='', help='entries to test (hex, comma-separated)')
    ap.add_argument('--auto', action='store_true', help='also every function that runs without the OS')
    ap.add_argument('--vectors', type=int, default=16)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--out')
    a = ap.parse_args(argv)
    if a.optest:
        path, entries = write_optest(a.optest, a.seed)
        print('%d functions: %s' % (len(entries), path))
        return
    name, path, seeds = parse_spec(a.image)
    prog = discover.Program(path, seeds)
    aif = prog.aif
    base = os.path.splitext(os.path.abspath(a.out))[0].replace('\\', '/')
    loaded = image_bytes(aif)
    with open(base + '.img', 'wb') as f:
        f.write(loaded)
    data = scratch_data(a.seed)
    with open(base + '.scratch.bin', 'wb') as f:
        f.write(data)
    images = [(loaded, 0), (data, SCRATCH)]
    rng = random.Random(a.seed)
    funcs = {int(x, 16): 'game' for x in a.funcs.split(',') if x}
    if a.auto:
        found = pure_functions(images, prog, rng)
        n_args = sum(1 for m in found.values() if m == 'args')
        print('%s: %d of %d functions run without the OS, %d of them with pointer arguments'
              % (name, len(found), len(prog.funcs), n_args))
        for e, mode in found.items():
            funcs.setdefault(e, mode)
    lines = ['# 3dokit self-test: %s, %d functions' % (name, len(funcs)),
             'image 00000000 %s.img' % base,
             'image %08X %s.scratch.bin' % (SCRATCH, base),
             'module %s' % name]
    mem = new_memory(images, prog.code)
    total = 0
    for e, mode in sorted(funcs.items()):
        if e not in prog.funcs:
            print('  %08X is not an entry of %s: skipped' % (e, name))
            continue
        label = prog.funcs[e].name or '%s_%08X' % (name.lower(), e)
        ls, k = record(mem, e, '%s (%s)' % (label, mode), [rand_state(rng, mode) for _ in range(a.vectors)])
        lines += ls
        total += k
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d functions, %d vectors -> %s' % (name, len(funcs), total, a.out))


if __name__ == '__main__':
    main()
