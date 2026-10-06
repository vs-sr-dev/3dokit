"""The ARM60's instruction set, decoded: ARMv3, 32-bit, big-endian.

The 3DO's CPU is an ARM60 -- the ARM6 core, architecture version 3. What a
recompiler, an interpreter and a code/data classifier all need is the same
decoding, with the architecture's edges exact, so it lives here, in pure
Python, and `capstone` is only a cross-check (`--check`).

What ARMv3 has, by bits 27..20 and 7..4 of the word:

    data processing   cond 00 I opcode S Rn Rd operand2
    multiply          cond 000000 A S Rd Rn Rs 1001 Rm
    swap              cond 00010 B 00 Rn Rd 0000 1001 Rm
    MRS / MSR         the TST/TEQ/CMP/CMN encodings with S clear
    single transfer   cond 01 I P U B W L Rn Rd offset
    block transfer    cond 100 P U S W L Rn register-list
    branch            cond 101 L offset24
    software intr.    cond 1111 comment24
    coprocessor       cond 110x / 1110 -- no coprocessor on the 3DO

and what it does **not** have, decoded here as `undefined` so that a word
that needs them is data, not code: the halfword and signed-byte transfers
(`ldrh`, `ldrsb`... ARMv4), the long multiplies (`umull`... ARMv3M), `bx`
(ARMv4T), a register-shifted single transfer (bit 4 set), every
coprocessor instruction, and the NV condition (never emitted by a compiler;
deprecated by ARM), and a word whose should-be-zero field (Rn of MOV/MVN
and of MUL, Rd of TST/TEQ/CMP/CMN) is not.

An operand2 is `('imm', value, carry)` -- the 8-bit immediate rotated, and
the shifter's carry out, None when the rotation is 0 and the carry is the
C flag -- or `('reg', rm, shift, amount)` with the immediate amount's
special cases already applied (`lsr #0` is `lsr #32`, `asr #0` is
`asr #32`, `ror #0` is `rrx`, amount 1 with shift `rrx`), or
`('regreg', rm, shift, rs)`, shifted by the bottom byte of a register.

    python -m 3dokit.arm60 IMAGE --dis ADDR [-n N]     # disassembly
    python -m 3dokit.arm60 IMAGE --check               # against capstone
"""
import argparse
import struct
import sys

CONDS = ('eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc',
         'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv')
DP_OPS = ('and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc',
          'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn')
TEST_OPS = {8, 9, 10, 11}           # write flags only
MOVE_OPS = {13, 15}                 # no Rn
SHIFTS = ('lsl', 'lsr', 'asr', 'ror', 'rrx')
REGS = ('r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7',
        'r8', 'sb', 'sl', 'fp', 'ip', 'sp', 'lr', 'pc')
PC, LR, SP = 15, 14, 13


class Insn:
    """One decoded word. `kind` is one of dp, mul, swp, mrs, msr, sdt, bdt,
    b, swi, undefined; the other attributes depend on it."""
    __slots__ = ('addr', 'word', 'cond', 'kind', 'op', 's', 'rd', 'rn', 'rs',
                 'rm', 'op2', 'a', 'b', 'p', 'u', 'w', 'l', 'regs', 'psr',
                 'mask', 'target', 'link', 'imm', 'offset')

    def __init__(self, addr, word):
        self.addr, self.word = addr, word
        for k in self.__slots__[2:]:
            setattr(self, k, None)

    # -- what control flow needs ------------------------------------------
    @property
    def conditional(self):
        return self.cond != 14

    def writes_pc(self):
        """Does this instruction (when it executes) set pc?"""
        k = self.kind
        if k == 'b':
            return True
        if k == 'dp':
            return self.rd == PC and self.op not in TEST_OPS
        if k == 'sdt':
            return self.l and self.rd == PC
        if k == 'bdt':
            return self.l and bool(self.regs & (1 << PC))
        if k in ('mul', 'swp', 'mrs'):
            return self.rd == PC
        return False

    def reads(self):
        """The registers it reads (pc included where it is an operand)."""
        k, r = self.kind, set()
        if k == 'dp':
            if self.op not in MOVE_OPS:
                r.add(self.rn)
            r |= _op2_regs(self.op2)
        elif k == 'mul':
            r |= {self.rm, self.rs} | ({self.rn} if self.a else set())
        elif k == 'swp':
            r |= {self.rn, self.rm}
        elif k == 'msr':
            r |= _op2_regs(self.op2)
        elif k == 'sdt':
            r.add(self.rn)
            if self.offset[0] == 'reg':
                r.add(self.offset[1])
            if not self.l:
                r.add(self.rd)
        elif k == 'bdt':
            r.add(self.rn)
            if not self.l:
                r |= {i for i in range(16) if self.regs >> i & 1}
        return r

    def writes(self):
        k, w = self.kind, set()
        if k == 'dp' and self.op not in TEST_OPS:
            w.add(self.rd)
        elif k in ('mul', 'swp', 'mrs'):
            w.add(self.rd)
        elif k == 'sdt':
            if self.l:
                w.add(self.rd)
            if self.w or not self.p:
                w.add(self.rn)
        elif k == 'bdt':
            if self.l:
                w |= {i for i in range(16) if self.regs >> i & 1}
            if self.w:
                w.add(self.rn)
        elif k == 'b' and self.link:
            w.add(LR)
        return w

    def sets_flags(self):
        k = self.kind
        if k == 'dp':
            return bool(self.s) or self.op in TEST_OPS
        if k == 'mul':
            return bool(self.s)
        if k == 'msr':
            return True
        return False

    def __repr__(self):
        return '%08x  %08x  %s' % (self.addr, self.word, text(self))


def _op2_regs(op2):
    if op2 is None or op2[0] == 'imm':
        return set()
    if op2[0] == 'reg':
        return {op2[1]}
    return {op2[1], op2[3]}


def _ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xffffffff if n else v


def _shift_imm(w):
    """Operand2 / offset with an immediate shift amount, ARM's special
    cases applied."""
    rm, typ, amt = w & 15, (w >> 5) & 3, (w >> 7) & 31
    if amt == 0:
        if typ == 0:
            return ('reg', rm, 'lsl', 0)
        if typ == 3:
            return ('reg', rm, 'rrx', 1)
        return ('reg', rm, SHIFTS[typ], 32)
    return ('reg', rm, SHIFTS[typ], amt)


def decode(word, addr=0):
    """Decode one ARMv3 word at `addr`."""
    i = Insn(addr, word)
    cond = word >> 28
    i.cond = cond
    if cond == 15:
        i.kind = 'undefined'
        return i
    b27_25 = (word >> 25) & 7
    if b27_25 in (0, 1):
        if b27_25 == 0 and (word & 0x90) == 0x90:
            # multiply, swap, or ARMv4's halfword transfers / ARMv3M's long mul
            if (word >> 22) & 0x3f == 0 and (word >> 4) & 15 == 9:
                i.kind = 'mul'
                i.a, i.s = (word >> 21) & 1, (word >> 20) & 1
                i.rd, i.rn = (word >> 16) & 15, (word >> 12) & 15
                i.rs, i.rm = (word >> 8) & 15, word & 15
                if i.rd == PC or i.rd == i.rm or (not i.a and i.rn):
                    # unpredictable, or MUL's should-be-zero Rn: never compiled
                    i.kind = 'undefined'
                return i
            if (word >> 23) & 0x1f == 2 and (word >> 20) & 3 == 0 and \
                    (word >> 4) & 0xff == 9:
                i.kind = 'swp'
                i.b = (word >> 22) & 1
                i.rn, i.rd, i.rm = (word >> 16) & 15, (word >> 12) & 15, word & 15
                return i
            i.kind = 'undefined'
            return i
        op, s = (word >> 21) & 15, (word >> 20) & 1
        if op in TEST_OPS and not s:
            # MRS / MSR
            if (word >> 23) & 0x1f == 2 and (word >> 16) & 0x3f == 0x0f and \
                    word & 0xfff == 0 and b27_25 == 0:
                i.kind, i.psr, i.rd = 'mrs', (word >> 22) & 1, (word >> 12) & 15
                return i
            if (word >> 23) & 3 == 2 and (word >> 20) & 3 == 2 and \
                    (word >> 12) & 15 == 15:
                i.kind, i.psr, i.mask = 'msr', (word >> 22) & 1, (word >> 16) & 15
                if b27_25 == 1:
                    rot = ((word >> 8) & 15) * 2
                    i.op2 = ('imm', _ror(word & 0xff, rot), None)
                elif (word >> 4) & 0xff == 0:
                    i.op2 = ('reg', word & 15, 'lsl', 0)
                else:
                    i.kind = 'undefined'
                return i
            i.kind = 'undefined'
            return i
        i.kind, i.op, i.s = 'dp', op, s
        i.rn, i.rd = (word >> 16) & 15, (word >> 12) & 15
        if (op in MOVE_OPS and i.rn) or (op in TEST_OPS and i.rd):
            i.kind = 'undefined'        # a should-be-zero field that is not
            return i
        if b27_25 == 1:
            rot = ((word >> 8) & 15) * 2
            v = _ror(word & 0xff, rot)
            i.op2 = ('imm', v, (v >> 31) if rot else None)
        elif (word >> 4) & 1:
            if (word >> 7) & 1:
                i.kind = 'undefined'
                return i
            i.op2 = ('regreg', word & 15, SHIFTS[(word >> 5) & 3], (word >> 8) & 15)
        else:
            i.op2 = _shift_imm(word)
        return i
    if b27_25 in (2, 3):
        if b27_25 == 3 and (word >> 4) & 1:
            i.kind = 'undefined'
            return i
        i.kind = 'sdt'
        i.p, i.u, i.b = (word >> 24) & 1, (word >> 23) & 1, (word >> 22) & 1
        i.w, i.l = (word >> 21) & 1, (word >> 20) & 1
        i.rn, i.rd = (word >> 16) & 15, (word >> 12) & 15
        i.offset = ('imm', word & 0xfff) if b27_25 == 2 else _shift_imm(word)
        return i
    if b27_25 == 4:
        i.kind = 'bdt'
        i.p, i.u, i.psr = (word >> 24) & 1, (word >> 23) & 1, (word >> 22) & 1
        i.w, i.l = (word >> 21) & 1, (word >> 20) & 1
        i.rn, i.regs = (word >> 16) & 15, word & 0xffff
        if i.regs == 0:
            i.kind = 'undefined'
        return i
    if b27_25 == 5:
        i.kind, i.link = 'b', (word >> 24) & 1
        off = word & 0xffffff
        if off & 0x800000:
            off -= 0x1000000
        i.target = (addr + 8 + off * 4) & 0xffffffff
        return i
    if (word >> 24) & 15 == 15:
        i.kind, i.imm = 'swi', word & 0xffffff
        return i
    i.kind = 'undefined'
    return i


def decode_at(data, addr, base=0):
    return decode(struct.unpack_from('>I', data, addr - base)[0], addr)


# -- text ------------------------------------------------------------------

def _op2_text(op2):
    if op2[0] == 'imm':
        return '#%#x' % op2[1] if op2[1] > 9 else '#%d' % op2[1]
    if op2[0] == 'reg':
        rm, sh, n = op2[1], op2[2], op2[3]
        if sh == 'lsl' and n == 0:
            return REGS[rm]
        if sh == 'rrx':
            return '%s, rrx' % REGS[rm]
        return '%s, %s #%d' % (REGS[rm], sh, n)
    return '%s, %s %s' % (REGS[op2[1]], op2[2], REGS[op2[3]])


def text(i):
    c = CONDS[i.cond]
    k = i.kind
    if k == 'dp':
        m = DP_OPS[i.op] + c + ('s' if i.s and i.op not in TEST_OPS else '')
        if i.op in TEST_OPS:
            return '%s %s, %s' % (m, REGS[i.rn], _op2_text(i.op2))
        if i.op in MOVE_OPS:
            return '%s %s, %s' % (m, REGS[i.rd], _op2_text(i.op2))
        return '%s %s, %s, %s' % (m, REGS[i.rd], REGS[i.rn], _op2_text(i.op2))
    if k == 'mul':
        if i.a:
            return 'mla%s%s %s, %s, %s, %s' % (c, 's' if i.s else '', REGS[i.rd],
                                              REGS[i.rm], REGS[i.rs], REGS[i.rn])
        return 'mul%s%s %s, %s, %s' % (c, 's' if i.s else '', REGS[i.rd],
                                      REGS[i.rm], REGS[i.rs])
    if k == 'swp':
        return 'swp%s%s %s, %s, [%s]' % (c, 'b' if i.b else '', REGS[i.rd],
                                         REGS[i.rm], REGS[i.rn])
    if k == 'mrs':
        return 'mrs%s %s, %s' % (c, REGS[i.rd], 'spsr' if i.psr else 'cpsr')
    if k == 'msr':
        return 'msr%s %s_%x, %s' % (c, 'spsr' if i.psr else 'cpsr', i.mask,
                                    _op2_text(i.op2))
    if k == 'sdt':
        m = ('ldr' if i.l else 'str') + c + ('b' if i.b else '') + \
            ('t' if not i.p and i.w else '')
        sign = '' if i.u else '-'
        if i.offset[0] == 'imm':
            off = '#%s%#x' % (sign, i.offset[1]) if i.offset[1] else ''
        else:
            off = sign + _op2_text(i.offset)
        base = REGS[i.rn]
        if i.p:
            body = '[%s%s]%s' % (base, ', ' + off if off else '', '!' if i.w else '')
        else:
            body = '[%s]%s' % (base, ', ' + off if off else '')
        note = ''
        if i.rn == PC and i.offset[0] == 'imm' and i.p:
            note = '   ; [%#x]' % (i.addr + 8 + (i.offset[1] if i.u else -i.offset[1]))
        return '%s %s, %s%s' % (m, REGS[i.rd], body, note)
    if k == 'bdt':
        mode = ('d', 'i')[i.u] + ('a', 'b')[i.p]
        regs = ', '.join(REGS[r] for r in range(16) if i.regs >> r & 1)
        return '%s%s%s %s%s, {%s}%s' % ('ldm' if i.l else 'stm', c, mode,
                                         REGS[i.rn], '!' if i.w else '', regs,
                                         '^' if i.psr else '')
    if k == 'b':
        return '%s%s %#x' % ('bl' if i.link else 'b', c, i.target)
    if k == 'swi':
        return 'swi%s %#x' % (c, i.imm)
    return '.word %#010x' % i.word


# -- the cross-check -------------------------------------------------------

def _capstone_kind(ci):
    m = ci.mnemonic
    w = int.from_bytes(ci.bytes, 'big')
    if m.startswith(('push', 'pop')) and (w >> 25) & 7 == 2:
        return 'sdt'                    # capstone's name for ldr/str rX, [sp], #4
    for p in ('ldm', 'stm', 'push', 'pop'):
        if m.startswith(p):
            return 'bdt'
    for p in ('ldr', 'str'):
        if m.startswith(p):
            return 'sdt'
    if m.startswith(('svc', 'swi')):
        return 'swi'
    if m.startswith(('mul', 'mla')):
        return 'mul'
    if m.startswith('swp'):
        return 'swp'
    if m.startswith('mrs'):
        return 'mrs'
    if m.startswith('msr'):
        return 'msr'
    if m.startswith('b') and not m.startswith(('bic', 'bx', 'blx', 'bkpt')):
        return 'b'
    root = m[:3]
    if root in DP_OPS or m.startswith(('lsl', 'lsr', 'asr', 'ror', 'rrx', 'nop')):
        return 'dp'
    return 'other:' + m


def check(data, start, end):
    """Every word from start to end against capstone: the kind, and for
    branches the target. Returns (agree, ARMv3-only refusals, disagreements)."""
    import collections
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_BIG_ENDIAN
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_BIG_ENDIAN)
    agree, refused, bad = 0, collections.Counter(), []
    for a in range(start, end, 4):
        w = struct.unpack_from('>I', data, a)[0]
        mine = decode(w, a)
        cs = list(md.disasm(data[a:a + 4], a))
        if not cs:
            if mine.kind != 'undefined':
                bad.append((a, w, mine.kind, '(capstone: none)'))
            else:
                agree += 1
            continue
        ck = _capstone_kind(cs[0])
        if mine.kind == 'undefined':
            refused[cs[0].mnemonic.rstrip('s')[:5]] += 1
            continue
        if ck != mine.kind:
            bad.append((a, w, text(mine), cs[0].mnemonic + ' ' + cs[0].op_str))
            continue
        if mine.kind == 'b':
            try:
                t = int(cs[0].op_str.lstrip('#'), 0)
            except ValueError:
                t = None
            if t != mine.target:
                bad.append((a, w, text(mine), cs[0].mnemonic + ' ' + cs[0].op_str))
                continue
        if mine.kind == 'swi' and int(cs[0].op_str.lstrip('#'), 0) != mine.imm:
            bad.append((a, w, text(mine), cs[0].mnemonic + ' ' + cs[0].op_str))
            continue
        agree += 1
    return agree, refused, bad


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.arm60', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('--dis', help='disassemble from this hex address')
    ap.add_argument('-n', '--count', type=int, default=40)
    ap.add_argument('--check', action='store_true',
                    help='every word of the read-only area against capstone')
    a = ap.parse_args(argv)
    data = open(a.image, 'rb').read()
    if a.dis:
        start = int(a.dis, 16)
        for k in range(a.count):
            ad = start + 4 * k
            if ad + 4 > len(data):
                break
            print(decode_at(data, ad))
    if a.check:
        ro = struct.unpack_from('>I', data, 0x14)[0]
        agree, refused, bad = check(data, 0x80, ro)
        print('%d words agree with capstone; %d that capstone decodes are not '
              'ARMv3 (or never compiled) and are refused here: %s' % (
                  agree, sum(refused.values()),
                  ', '.join('%s %d' % kv for kv in refused.most_common(12))))
        print('%d disagree' % len(bad))
        for b in bad[:30]:
            print('  %08x %08x  mine: %-36s capstone: %s' % b)
        return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
