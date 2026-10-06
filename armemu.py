"""An ARM60 interpreter, for running isolated guest functions.

    python -m 3dokit.armemu IMAGE --call ADDR [--regs r0=7,r1=100]
    python -m 3dokit.armemu --test                  # the ARMv3 edges, known answers
    python -m 3dokit.armemu --check [-n 20000]      # random words against unicorn

It is the reference the recompiler is checked against (`recomp.selftest`),
and a way to ask the game's own code what a helper computes without an
emulator. It models the CPU in user mode only: r0-r15 and the flags N Z C
V, over `arm60`'s decoding. Memory is a set of flat big-endian regions; an
access outside them raises, so a function that touches hardware says so.

What the ARM60 does that a later core does not, and which this follows:

* **An unaligned `ldr`** reads the word at the aligned address and rotates
  it right by 8 x address[1:0]: big-endian, offsets 0 and 2 leave the
  addressed byte in bits 31-24, offsets 1 and 3 in bits 15-8 (the ARM6
  and ARM7 datasheets' big-endian configuration). `str`, `ldm` and `stm`
  ignore address[1:0]; `ldrb`/`strb` are exact.
* **`pc` as an operand** reads the instruction's address + 8, or + 12 when
  the instruction shifts by a register; **`str pc` and `stm {..., pc}`**
  store the address + 12. The APCS prologue's `stmfd sp!, {..., pc}`
  stores it, and only a backtrace reads it, but it is stored exactly.
* **A write to `pc`** ignores bits 1-0 (32-bit mode).
* **`ldm` with the base in the list and writeback** leaves the loaded
  value; **`stm`** stores the base's original value if it is the lowest
  register of the list, the written-back one otherwise.
* **`mul`/`mla` with S** set N and Z; the datasheet leaves C "meaningless"
  and V unchanged: here both are left unchanged, and the recompiler does
  the same.
* **`msr cpsr`** in user mode writes the flags field (mask bit 3) only;
  **`mrs cpsr`** reads the flags over user mode's 0x10.

What user mode cannot do raises `Unpredictable`: `movs pc` and the like
(an S on a write to pc: SPSR in user mode), `ldm/stm ^`, `msr/mrs spsr`, a
writeback to pc or a load into the base it writes back. Hand-written code
takes liberties compiled code never does (Crash 'n Burn's routines at
0x39148 use `sp` as a plain register and load flags with `msr`); those it
runs, these it refuses.

`CPU.call(entry, r0=.., ...)` sets the arguments, points lr at a return
sentinel and runs until the function returns there. A `swi` calls
`cpu.on_swi(cpu, number)`, and a jump to an address in `cpu.traps` calls
`cpu.traps[addr](cpu)` instead of executing it (an OS routine, a stub):
the handler sets pc, as a return to lr would.
"""
import argparse
import random
import struct
import sys

from . import arm60

M32 = 0xFFFFFFFF
RETURN_SENTINEL = 0xFFFFFFF0
PC, LR, SP = 15, 14, 13


class MemoryError_(Exception):
    pass


class Unpredictable(Exception):
    """What ARMv3 leaves undefined in user mode, or what is not an
    instruction: refused rather than guessed."""


class Memory:
    """Flat big-endian regions (lo, hi, bytearray)."""

    def __init__(self):
        self.regions = []
        self.log = None              # set to a list to record (kind, addr, size, value)
        self.watch = None            # called with (addr, size) on every write

    def add(self, lo, size, data=None):
        buf = bytearray(size)
        if data:
            buf[:len(data)] = data
        self.regions.append((lo, lo + size, buf))
        return buf

    def _find(self, addr, size):
        for lo, hi, buf in self.regions:
            if lo <= addr and addr + size <= hi:
                return buf, addr - lo
        raise MemoryError_('unmapped %d-byte access at %08X' % (size, addr))

    def read(self, addr, size):
        addr &= M32
        buf, o = self._find(addr, size)
        v = int.from_bytes(buf[o:o + size], 'big')
        if self.log is not None:
            self.log.append(('r', addr, size, v))
        return v

    def write(self, addr, size, value):
        addr &= M32
        buf, o = self._find(addr, size)
        value &= (1 << (8 * size)) - 1
        buf[o:o + size] = value.to_bytes(size, 'big')
        if self.log is not None:
            self.log.append(('w', addr, size, value))
        if self.watch is not None:
            self.watch(addr, size)


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & M32 if n else v


def s32(x):
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


def cond_passed(cond, n, z, c, v):
    if cond == 14:
        return True
    if cond == 0: return z
    if cond == 1: return not z
    if cond == 2: return c
    if cond == 3: return not c
    if cond == 4: return n
    if cond == 5: return not n
    if cond == 6: return v
    if cond == 7: return not v
    if cond == 8: return c and not z
    if cond == 9: return not c or z
    if cond == 10: return n == v
    if cond == 11: return n != v
    if cond == 12: return not z and n == v
    if cond == 13: return z or n != v
    raise Unpredictable('condition NV')


def shift(value, kind, amount, carry, by_register):
    """The barrel shifter: (result, carry out). `amount` is the immediate's
    (with arm60's special cases applied: lsr/asr #32, rrx) or a register's
    bottom byte (`by_register`, where 0 leaves the operand and the carry
    alone and amounts of 32 and more have their own rules)."""
    if kind == 'rrx':
        return (carry << 31) | (value >> 1), value & 1
    if by_register:
        if amount == 0:
            return value, carry
        if kind == 'lsl':
            if amount < 32:
                return (value << amount) & M32, (value >> (32 - amount)) & 1
            return 0, (value & 1) if amount == 32 else 0
        if kind == 'lsr':
            if amount < 32:
                return value >> amount, (value >> (amount - 1)) & 1
            return 0, (value >> 31) if amount == 32 else 0
        if kind == 'asr':
            if amount < 32:
                return (s32(value) >> amount) & M32, (value >> (amount - 1)) & 1
            return (M32 if value >> 31 else 0), value >> 31
        # ror
        if amount & 31 == 0:
            return value, value >> 31
        amount &= 31
        return ror(value, amount), (value >> (amount - 1)) & 1
    # an immediate amount, 0..32
    if kind == 'lsl':
        if amount == 0:
            return value, carry
        return (value << amount) & M32, (value >> (32 - amount)) & 1
    if kind == 'lsr':
        if amount == 32:
            return 0, value >> 31
        return value >> amount, (value >> (amount - 1)) & 1
    if kind == 'asr':
        if amount == 32:
            return (M32 if value >> 31 else 0), value >> 31
        return (s32(value) >> amount) & M32, (value >> (amount - 1)) & 1
    return ror(value, amount), (value >> (amount - 1)) & 1


class CPU:
    def __init__(self, mem):
        self.mem = mem
        self.r = [0] * 16
        self.n = self.z = self.c = self.v = 0
        self.steps = 0
        self.on_swi = None           # fn(cpu, number)
        self.traps = {}              # address: fn(cpu), which sets pc
        self._decoded = {}

    # -- the flags as a word
    @property
    def cpsr(self):
        return self.n << 31 | self.z << 30 | self.c << 29 | self.v << 28 | 0x10

    @cpsr.setter
    def cpsr(self, w):
        self.n, self.z = (w >> 31) & 1, (w >> 30) & 1
        self.c, self.v = (w >> 29) & 1, (w >> 28) & 1

    def fetch(self, pc):
        ins = self._decoded.get(pc)
        if ins is None:
            ins = self._decoded[pc] = arm60.decode(self.mem.read(pc, 4), pc)
        return ins

    def call(self, entry, max_steps=10_000_000, **regs):
        for k, v in regs.items():
            self.r[{'sb': 9, 'sl': 10, 'fp': 11, 'ip': 12, 'sp': 13, 'lr': 14}.get(k) or int(k[1:])] = v & M32
        self.r[LR] = RETURN_SENTINEL
        self.r[PC] = entry
        limit = self.steps + max_steps
        while self.r[PC] != RETURN_SENTINEL:
            self.step()
            if self.steps > limit:
                raise RuntimeError('no return after %d steps (pc %08X)' % (max_steps, self.r[PC]))
        return self.r[0]

    def step(self):
        pc = self.r[PC]
        trap = self.traps.get(pc)
        if trap is not None:
            self.steps += 1
            trap(self)
            return
        if pc & 3:
            raise Unpredictable('pc %08X is not word-aligned' % pc)
        ins = self.fetch(pc)
        self.steps += 1
        if not cond_passed(ins.cond, self.n, self.z, self.c, self.v):
            self.r[PC] = (pc + 4) & M32
            return
        target = self.execute(ins, pc)
        self.r[PC] = (pc + 4) & M32 if target is None else target & ~3 & M32

    # -- operands
    def _reg(self, n, pc, plus=8):
        return (pc + plus) & M32 if n == PC else self.r[n]

    def _op2(self, op2, pc):
        """(value, carry out) of a data-processing operand2."""
        if op2[0] == 'imm':
            _, v, carry = op2
            return v, self.c if carry is None else carry
        if op2[0] == 'reg':
            _, rm, kind, amount = op2
            return shift(self._reg(rm, pc), kind, amount, self.c, False)
        _, rm, kind, rs = op2
        if rs == PC:
            raise Unpredictable('a shift by pc')
        return shift(self._reg(rm, pc, 12), kind, self.r[rs] & 0xFF, self.c, True)

    # -- execution: returns the new pc, or None to fall through
    def execute(self, ins, pc):
        k = ins.kind
        r = self.r
        if k == 'dp':
            return self._dp(ins, pc)
        if k == 'sdt':
            return self._sdt(ins, pc)
        if k == 'bdt':
            return self._bdt(ins, pc)
        if k == 'b':
            if ins.link:
                r[LR] = (pc + 4) & M32
            return ins.target
        if k == 'mul':
            if PC in (ins.rm, ins.rs) or (ins.a and ins.rn == PC):
                raise Unpredictable('a multiply by pc')
            v = r[ins.rm] * r[ins.rs]
            if ins.a:
                v += r[ins.rn]
            v &= M32
            r[ins.rd] = v
            if ins.s:
                self.n, self.z = v >> 31, int(v == 0)
            return None
        if k == 'swi':
            if self.on_swi is None:
                raise MemoryError_('swi %#x at %08X with no handler' % (ins.imm, pc))
            r[PC] = (pc + 4) & M32
            self.on_swi(self, ins.imm)
            return r[PC]
        if k == 'swp':
            if PC in (ins.rd, ins.rn, ins.rm):
                raise Unpredictable('swp with pc')
            a = r[ins.rn]
            if ins.b:
                old = self.mem.read(a, 1)
                self.mem.write(a, 1, r[ins.rm])
            else:
                old = ror(self.mem.read(a & ~3, 4), 8 * (a & 3))
                self.mem.write(a & ~3, 4, r[ins.rm])
            r[ins.rd] = old
            return None
        if k == 'mrs':
            if ins.psr:
                raise Unpredictable('mrs spsr in user mode')
            r[ins.rd] = self.cpsr
            return None
        if k == 'msr':
            if ins.psr:
                raise Unpredictable('msr spsr in user mode')
            v = ins.op2[1] if ins.op2[0] == 'imm' else self._reg(ins.op2[1], pc)
            if ins.mask & 8:
                self.cpsr = v
            return None
        raise Unpredictable('not an ARMv3 instruction: %08X at %08X' % (ins.word, pc))

    def _dp(self, ins, pc):
        r, op = self.r, ins.op
        b, carry = self._op2(ins.op2, pc)
        a = 0 if op in arm60.MOVE_OPS else \
            self._reg(ins.rn, pc, 12 if ins.op2[0] == 'regreg' else 8)
        v = self.v
        if op in (0, 8):                       # and, tst
            res = a & b
        elif op in (1, 9):                     # eor, teq
            res = a ^ b
        elif op == 12:
            res = a | b
        elif op == 13:
            res = b
        elif op == 14:
            res = a & ~b & M32
        elif op == 15:
            res = ~b & M32
        else:
            if op in (2, 10):                  # sub, cmp: a - b
                x, y, cin = a, b ^ M32, 1
            elif op == 3:                      # rsb
                x, y, cin = b, a ^ M32, 1
            elif op in (4, 11):                # add, cmn
                x, y, cin = a, b, 0
            elif op == 5:                      # adc
                x, y, cin = a, b, self.c
            elif op == 6:                      # sbc
                x, y, cin = a, b ^ M32, self.c
            else:                              # rsc
                x, y, cin = b, a ^ M32, self.c
            full = x + y + cin
            res = full & M32
            carry = full >> 32
            v = ((x ^ res) & (y ^ res)) >> 31
        if ins.s:
            if ins.rd == PC and op not in arm60.TEST_OPS:
                raise Unpredictable('an S on a write to pc (SPSR in user mode)')
            self.n, self.z, self.c, self.v = res >> 31, int(res == 0), carry, v
        if op in arm60.TEST_OPS:
            return None
        if ins.rd == PC:
            return res
        r[ins.rd] = res
        return None

    def _sdt(self, ins, pc):
        r = self.r
        base = self._reg(ins.rn, pc)
        if ins.offset[0] == 'imm':
            off = ins.offset[1]
        else:
            _, rm, kind, amount = ins.offset
            if rm == PC:
                raise Unpredictable('pc as a transfer offset')
            off = shift(r[rm], kind, amount, self.c, False)[0]
        moved = (base + off if ins.u else base - off) & M32
        addr = moved if ins.p else base
        writeback = ins.w or not ins.p
        if writeback and ins.rn == PC:
            raise Unpredictable('writeback to pc')
        if ins.l:
            if writeback and ins.rd == ins.rn:
                raise Unpredictable('a load into the base it writes back')
            if ins.b:
                val = self.mem.read(addr, 1)
            else:
                val = ror(self.mem.read(addr & ~3 & M32, 4), 8 * (addr & 3))
            if writeback:
                r[ins.rn] = moved
            if ins.rd == PC:
                return val
            r[ins.rd] = val
            return None
        val = (pc + 12) & M32 if ins.rd == PC else r[ins.rd]
        if ins.b:
            self.mem.write(addr, 1, val)
        else:
            self.mem.write(addr & ~3 & M32, 4, val)
        if writeback:
            r[ins.rn] = moved
        return None

    def _bdt(self, ins, pc):
        r = self.r
        if ins.psr:
            raise Unpredictable('ldm/stm ^ in user mode')
        if ins.rn == PC:
            raise Unpredictable('ldm/stm on pc')
        regs = [i for i in range(16) if ins.regs >> i & 1]
        n = len(regs)
        base = r[ins.rn]
        if ins.u:
            start = base + 4 if ins.p else base
            final = base + 4 * n
        else:
            start = base - 4 * n if ins.p else base - 4 * n + 4
            final = base - 4 * n
        start &= ~3 & M32
        final &= M32
        if ins.l:
            vals = [self.mem.read((start + 4 * k) & M32, 4) for k in range(n)]
            if ins.w:
                r[ins.rn] = final
            target = None
            for i, val in zip(regs, vals):
                if i == PC:
                    target = val
                else:
                    r[i] = val
            return target
        for k, i in enumerate(regs):
            if i == PC:
                val = (pc + 12) & M32
            elif i == ins.rn and ins.w and k > 0:
                val = final
            else:
                val = r[i]
            self.mem.write((start + 4 * k) & M32, 4, val)
        if ins.w:
            r[ins.rn] = final
        return None


# -- known answers ----------------------------------------------------------

def _run_words(words, regs=(), flags=0, mem=None, at=0x1000):
    m = Memory()
    m.add(0, 0x10000)
    for k, w in enumerate(words):
        m.write(at + 4 * k, 4, w)
    for a, data in (mem or {}).items():
        m.regions[0][2][a:a + len(data)] = data
    cpu = CPU(m)
    for i, v in dict(regs).items():
        cpu.r[i] = v
    cpu.cpsr = flags
    cpu.r[PC] = at
    for _ in words:
        cpu.step()
    return cpu


TESTS = []


def _test(f):
    TESTS.append(f)
    return f


@_test
def t_unaligned_ldr():
    """ldr r0, [r1] at offsets 0..3 of 11 22 33 44: the word rotated right."""
    want = [0x11223344, 0x44112233, 0x33441122, 0x22334411]
    for off in range(4):
        c = _run_words([0xe5910000], {1: 0x2000 + off}, mem={0x2000: bytes([0x11, 0x22, 0x33, 0x44])})
        assert c.r[0] == want[off], (off, '%08x' % c.r[0])
    # the halfword idiom: offsets 0 and 2 leave the halfword in bits 31-16
    c = _run_words([0xe5910000, 0xe1a00820], {1: 0x2002}, mem={0x2000: bytes([0x11, 0x22, 0x33, 0x44])})
    assert c.r[0] == 0x3344, '%08x' % c.r[0]       # ldr r0, [r1]; mov r0, r0, lsr #16


@_test
def t_unaligned_str_ldm():
    """str and stm ignore address[1:0]."""
    c = _run_words([0xe5810000], {0: 0xAABBCCDD, 1: 0x2003})       # str r0, [r1]
    assert c.mem.read(0x2000, 4) == 0xAABBCCDD
    c = _run_words([0xe8810003], {0: 1, 1: 0x2002})                 # stmia r1, {r0, r1}
    assert c.mem.read(0x2000, 4) == 1 and c.mem.read(0x2004, 4) == 0x2002


@_test
def t_pc_reads():
    """pc reads +8; +12 with a register shift; str/stm of pc store +12."""
    c = _run_words([0xe1a0000f])                         # mov r0, pc
    assert c.r[0] == 0x1008
    c = _run_words([0xe1a0011f], {1: 0})                 # mov r0, pc, lsl r1
    assert c.r[0] == 0x100c
    c = _run_words([0xe08f0011], {1: 0})                 # add r0, pc, r1, lsl r0
    assert c.r[0] == 0x100c
    c = _run_words([0xe581f000], {1: 0x2000})            # str pc, [r1]
    assert c.mem.read(0x2000, 4) == 0x100c
    c = _run_words([0xe92dd800], {13: 0x3000})           # stmfd sp!, {fp, ip, lr, pc}
    assert c.mem.read(0x3000 - 4, 4) == 0x100c and c.r[13] == 0x3000 - 16


@_test
def t_shifter_edges():
    """Register shifts by 0, 32, 33 and more; immediate lsr/asr #32; rrx."""
    cases = [  # (kind, value, amount, carry in, by register, result, carry out)
        ('lsl', 0x80000001, 0, 1, True, 0x80000001, 1),
        ('lsl', 0x80000001, 32, 0, True, 0, 1),
        ('lsl', 0x80000001, 33, 1, True, 0, 0),
        ('lsr', 0x80000001, 32, 0, True, 0, 1),
        ('lsr', 0x80000001, 200, 1, True, 0, 0),
        ('asr', 0x80000001, 40, 0, True, M32, 1),
        ('asr', 0x40000001, 40, 1, True, 0, 0),
        ('ror', 0x80000001, 32, 0, True, 0x80000001, 1),
        ('ror', 0x80000001, 36, 0, True, 0x18000000, 0),
        ('lsr', 0x80000000, 32, 0, False, 0, 1),
        ('asr', 0x80000000, 32, 0, False, M32, 1),
        ('rrx', 0x00000003, 1, 1, False, 0x80000001, 1),
        ('ror', 0x000000F0, 4, 0, False, 0x0000000F, 0),
    ]
    for kind, val, amt, cin, byreg, res, cout in cases:
        got = shift(val, kind, amt, cin, byreg)
        assert got == (res, cout), (kind, hex(val), amt, byreg, got)


@_test
def t_flags():
    """cmp/adds/subs/adc/sbc/rsc carry and overflow."""
    c = _run_words([0xe1500001], {0: 5, 1: 5})           # cmp r0, r1
    assert (c.n, c.z, c.c, c.v) == (0, 1, 1, 0)
    c = _run_words([0xe1500001], {0: 0, 1: 1})
    assert (c.n, c.z, c.c, c.v) == (1, 0, 0, 0)
    c = _run_words([0xe1500001], {0: 0x80000000, 1: 1})
    assert (c.n, c.z, c.c, c.v) == (0, 0, 1, 1)
    c = _run_words([0xe0900001], {0: 0xFFFFFFFF, 1: 1})  # adds r0, r0, r1
    assert c.r[0] == 0 and (c.z, c.c, c.v) == (1, 1, 0)
    c = _run_words([0xe0a00001], {0: 1, 1: 1}, flags=1 << 29)   # adc r0, r0, r1
    assert c.r[0] == 3
    c = _run_words([0xe0c00001], {0: 5, 1: 3}, flags=0)  # sbc r0, r0, r1: 5 - 3 - 1
    assert c.r[0] == 1
    c = _run_words([0xe0f00001], {0: 3, 1: 5}, flags=0)  # rscs r0, r0, r1: 5 - 3 - 1
    assert c.r[0] == 1 and c.c == 1
    c = _run_words([0xe1b00000], {0: 0}, flags=0x3 << 28)   # movs r0, r0: C, V kept
    assert (c.z, c.c, c.v) == (1, 1, 1)
    c = _run_words([0xe0100091], {1: 0xFFFFFFFF, 0: 0}, flags=0x3 << 28)  # muls r0, r1, r0
    assert (c.z, c.c, c.v) == (1, 1, 1)


@_test
def t_ldm_stm_base():
    """The base in the list: ldm's loaded value wins over the writeback;
    stm stores the original if it is the lowest, else the new base."""
    c = _run_words([0xe8b00003], {0: 0x2000}, mem={0x2000: bytes(range(8))})  # ldmia r0!, {r0, r1}
    assert c.r[0] == 0x00010203
    c = _run_words([0xe8a10003], {1: 0x2000, 0: 7})      # stmia r1!, {r0, r1}
    assert c.mem.read(0x2004, 4) == 0x2008 and c.r[1] == 0x2008
    c = _run_words([0xe8a00003], {0: 0x2000, 1: 9})      # stmia r0!, {r0, r1}
    assert c.mem.read(0x2000, 4) == 0x2000
    c = _run_words([0xe9300006], {0: 0x2010}, mem={0x2008: bytes(range(8))})  # ldmdb r0!, {r1, r2}
    assert c.r[1] == 0x00010203 and c.r[2] == 0x04050607 and c.r[0] == 0x2008


@_test
def t_msr():
    """msr cpsr_f loads the flags; the control field is ignored."""
    c = _run_words([0xe128f002], {2: 0xA00000DF})        # msr cpsr_f, r2
    assert (c.n, c.z, c.c, c.v) == (1, 0, 1, 0)
    c = _run_words([0xe10f0000], flags=0x60000000)       # mrs r0, cpsr
    assert c.r[0] == 0x60000010


@_test
def t_unpredictable():
    for w in (0xe1b0f00e,          # movs pc, lr
              0xe8fd8000,          # ldmia sp!, {pc}^
              0xe5b11004):         # ldr r1, [r1, #4]!
        try:
            _run_words([w], {1: 0x2000, 13: 0x2000, 14: 0x1000})
        except Unpredictable:
            continue
        raise AssertionError('%08x ran' % w)


def selftest():
    bad = 0
    for t in TESTS:
        try:
            t()
            print('ok    %s' % t.__name__)
        except AssertionError as e:
            bad += 1
            print('FAIL  %s: %s' % (t.__name__, e))
    print('%d of %d known-answer tests pass' % (len(TESTS) - bad, len(TESTS)))
    return bad


# -- against unicorn ---------------------------------------------------------

def _random_word(rng):
    """A word that is an ARMv3 instruction of the kinds unicorn can judge."""
    while True:
        w = rng.getrandbits(32)
        cond = 14 if rng.random() < 0.6 else rng.randrange(15)
        w = (w & 0x0FFFFFFF) | cond << 28
        top = rng.random()
        if top < 0.55:
            w = (w & ~(3 << 26)) & M32                      # data processing / multiply
        elif top < 0.8:
            w = (w & ~(3 << 26)) | 1 << 26                  # single transfer
        else:
            w = (w & ~(7 << 25)) | 4 << 25                  # block transfer
        i = arm60.decode(w, 0x1000)
        if i.kind not in ('dp', 'mul', 'sdt', 'bdt'):
            continue
        if i.kind == 'sdt':
            if i.rd == PC or i.rn == PC or (i.offset[0] == 'reg' and i.offset[1] == PC):
                continue
        if i.kind == 'bdt' and (i.regs >> PC & 1 or i.rn == PC or i.psr):
            continue
        if i.kind == 'bdt' and not i.l and i.w and i.regs >> i.rn & 1 and \
                i.regs & ((1 << i.rn) - 1):
            continue        # the base stored after another: the ARM6 stores it written back
        if i.kind == 'dp':
            if i.rd == PC and i.op not in arm60.TEST_OPS:
                continue
            if i.op2[0] == 'regreg' and PC in (i.op2[1], i.op2[3], i.rn):
                continue
        return w


def check_unicorn(count=20000, seed=1):
    """Random ARMv3 words, run once each here and in unicorn (ARM926,
    big-endian), from random registers and flags over random memory: the
    registers, flags and memory compared. Excluded where the ARM60 differs
    from an ARMv5 by design: transfers involving pc (+12), `stm` with
    writeback storing its base after another register, and unaligned
    word transfers (rotated here; unicorn reads across the boundary). The
    known-answer tests (`--test`) cover those."""
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_BIG_ENDIAN
    from unicorn import arm_const as K
    rng = random.Random(seed)
    UREG = [getattr(K, 'UC_ARM_REG_R%d' % i) for i in range(13)] + \
        [K.UC_ARM_REG_SP, K.UC_ARM_REG_LR, K.UC_ARM_REG_PC]
    CODE, DATA, DSIZE = 0x1000, 0x10000, 0x10000
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(K.UC_CPU_ARM_926)
    uc.mem_map(0, 0x20000 + DSIZE)
    bad, ran, skipped = [], 0, 0
    for k in range(count):
        w = _random_word(rng)
        ins = arm60.decode(w, CODE)
        regs = [DATA + 0x4000 + rng.randrange(0, 0x8000) for _ in range(13)] + \
            [DATA + 0x8000, CODE + 0x100, CODE + 8]
        for i in range(13):
            if rng.random() < 0.35:
                regs[i] = rng.choice([0, 1, 2, 31, 32, 33, 0x7FFFFFFF, 0x80000000, M32,
                                      rng.getrandbits(32), rng.getrandbits(8)])
        if ins.kind in ('sdt', 'bdt'):
            regs[ins.rn] = (DATA + 0x4000 + rng.randrange(0, 0x8000)) & ~3
        if ins.kind == 'sdt' and ins.offset[0] == 'reg':
            regs[ins.offset[1]] = rng.randrange(0, 0x400) & ~3 if ins.offset[2] == 'lsl' and ins.offset[3] == 0 else rng.randrange(0, 0x40)
        flags = rng.getrandbits(4) << 28
        data = bytes(rng.getrandbits(8) for _ in range(DSIZE))
        mine = Memory()
        mine.add(0, 0x20000 + DSIZE)
        mine.regions[0][2][DATA:DATA + DSIZE] = data
        mine.write(CODE, 4, w)
        cpu = CPU(mine)
        cpu.r = regs[:15] + [CODE]
        cpu.cpsr = flags
        try:
            if ins.kind == 'sdt' and not ins.b:
                base = regs[ins.rn]
                if ins.offset[0] == 'imm':
                    off = ins.offset[1]
                else:
                    off = shift(regs[ins.offset[1]], ins.offset[2], ins.offset[3], (flags >> 29) & 1, False)[0]
                a = base if not ins.p else (base + off if ins.u else base - off) & M32
                if a & 3 or not (DATA <= a < DATA + DSIZE - 4):
                    skipped += 1
                    continue
            if ins.kind == 'sdt' and ins.b:
                base = regs[ins.rn]
                off = ins.offset[1] if ins.offset[0] == 'imm' else \
                    shift(regs[ins.offset[1]], ins.offset[2], ins.offset[3], (flags >> 29) & 1, False)[0]
                a = base if not ins.p else (base + off if ins.u else base - off) & M32
                if not (DATA <= a < DATA + DSIZE):
                    skipped += 1
                    continue
            cpu.step()
        except Unpredictable:
            skipped += 1
            continue
        except MemoryError_:
            skipped += 1
            continue
        uc.mem_write(DATA, data)
        uc.mem_write(CODE, struct.pack('>I', w))
        # the flags only: the mode (its banked sp, lr) and the E bit stay
        uc.reg_write(K.UC_ARM_REG_CPSR, (uc.reg_read(K.UC_ARM_REG_CPSR) & 0x0FFFFFFF) | flags)
        for i in range(15):
            uc.reg_write(UREG[i], regs[i])
        try:
            uc.emu_start(CODE, CODE + 4, count=1)
        except Exception as e:
            bad.append((w, 'unicorn: %s' % e))
            continue
        ran += 1
        theirs = [uc.reg_read(UREG[i]) for i in range(15)]
        tflags = uc.reg_read(K.UC_ARM_REG_CPSR) >> 28
        tpc = uc.reg_read(K.UC_ARM_REG_PC)
        diffs = []
        for i in range(15):
            if theirs[i] != cpu.r[i]:
                diffs.append('r%d %08x/%08x' % (i, cpu.r[i], theirs[i]))
        if tflags != cpu.cpsr >> 28:
            if not (ins.kind == 'mul' and ins.s and (tflags ^ (cpu.cpsr >> 28)) & 0b1100 == 0):
                diffs.append('nzcv %x/%x' % (cpu.cpsr >> 28, tflags))
        if tpc != cpu.r[PC]:
            diffs.append('pc %08x/%08x' % (cpu.r[PC], tpc))
        if bytes(uc.mem_read(DATA, DSIZE)) != bytes(mine.regions[0][2][DATA:DATA + DSIZE]):
            diffs.append('memory')
        if diffs:
            bad.append((w, '%s: %s' % (arm60.text(ins), ', '.join(diffs))))
    return ran, skipped, bad


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.armemu', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', nargs='?')
    ap.add_argument('--call', help='run the function at this hex address')
    ap.add_argument('--regs', default='', help='r0=7,r1=0x100,...')
    ap.add_argument('--test', action='store_true', help='the known-answer tests')
    ap.add_argument('--check', action='store_true', help='random words against unicorn')
    ap.add_argument('-n', '--count', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args(argv)
    status = 0
    if a.test:
        status |= 1 if selftest() else 0
    if a.check:
        ran, skipped, bad = check_unicorn(a.count, a.seed)
        print('%d random instructions agree with unicorn of %d run (%d skipped: '
              'unpredictable here, or outside the data)' % (ran - len(bad), ran, skipped))
        for w, why in bad[:30]:
            print('  %08x  %s' % (w, why))
        status |= 1 if bad else 0
    if a.call:
        from .aif import AIF
        img = AIF(a.image)
        mem = Memory()
        mem.add(0, 0x200000, img.d[:img.stub])       # DRAM, the image linked at 0
        mem.add(0x200000, 0x100000)                  # VRAM
        cpu = CPU(mem)
        cpu.r[SP] = 0x1FFF00
        regs = {}
        for kv in a.regs.split(','):
            if kv:
                k, v = kv.split('=')
                regs[k.strip()] = int(v, 0)
        r0 = cpu.call(int(a.call, 16), **regs)
        print('r0 = %#x after %d instructions' % (r0, cpu.steps))
        print(' '.join('%s=%08x' % (arm60.REGS[i], cpu.r[i]) for i in range(15)))
    return status


if __name__ == '__main__':
    sys.exit(main())
