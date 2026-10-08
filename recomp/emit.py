"""ARM60 instruction -> C++, against 3dokit/runtime/arm60.h.

`stmt(i, a)` gives the C++ for one instruction that does not change the
flow of control, with the semantics of `armemu`, instruction for
instruction: the self-test (`recomp.selftest`) holds the two together.
`Body` turns a discovered function into a C++ function:

* every instruction in address order, a label wherever something branches;
  a `goto` where the code goes on past a literal pool;
* a conditional instruction as an `if` on the flags; the flags as four
  words of `ArmCpu`, computed where an instruction sets them;
* `bl` as a C++ call with lr set first and the return checked (ARM_RET:
  the callee must come back to the word after the call); a `b` to another
  function's entry as a tail call; a return (`mov pc, lr`, `ldm ..., pc`,
  `ldr pc, [sp], #4`) records where it went in `c.pc` and returns -- or,
  in a function with local subroutines (discovery's `local_returns`), goes
  to the one of them it names;
* the compiler's switch (`addls pc, pc, rI, lsl #2`) as a C++ `switch` over
  its branch table; any other write to pc after `mov lr, pc` as a call
  through `arm_call`, and otherwise a jump through it (a tail);
* `swi` as `arm_swi`, the OS's door; folio vectors are calls through
  `arm_call` to what the folio's table holds in guest memory;
* safe points (ARM_POLL) at backward branches and before calls;
* the clocks the ARM60 would take (ARM_TICK): a block (from a label, or
  from the word after one that can leave) pays at its start for every
  instruction in it, a conditional one 1 clock there (a failed condition)
  and the rest inside its `if` (`clocks`).

What `armemu` refuses as unpredictable in user mode the emitter refuses
too (`Unsupported`), so the C++ never quietly does something else.
"""
from .. import arm60

PC, LR, SP = 15, 14, 13
TEST_OPS, MOVE_OPS = arm60.TEST_OPS, arm60.MOVE_OPS


class Unsupported(Exception):
    pass


def fname(addr):
    return 'f_%08X' % addr


def _h(v):
    v &= 0xFFFFFFFF
    return '0x%08Xu' % v if v > 0xFFFF else '0x%Xu' % v


def R(n):
    return 'c.r[%d]' % n


def _abs(v):
    """An address of the program's own, as it is where the program is
    loaded: the module's base (`mb`, set by the runtime) plus the address
    linked at 0."""
    return '(mb + %s)' % _h(v)


COND = {0: 'c.z', 1: '!c.z', 2: 'c.c', 3: '!c.c', 4: 'c.n', 5: '!c.n',
        6: 'c.v', 7: '!c.v', 8: 'c.c && !c.z', 9: '!c.c || c.z',
        10: 'c.n == c.v', 11: 'c.n != c.v', 12: '!c.z && c.n == c.v',
        13: 'c.z || c.n != c.v'}


def _reg(n, a, plus=8):
    """A register as an operand: pc is the instruction's address + 8 (+ 12
    under a register shift)."""
    return _abs(a + plus) if n == PC else R(n)


def _shift_imm(m, kind, amount, carry):
    """(expression, carry expression or None for 'C unchanged') of an
    immediate shift of the expression `m` (a local)."""
    if kind == 'rrx':
        return '(c.c << 31 | %s >> 1)' % m, '(%s & 1u)' % m
    if kind == 'lsl':
        if amount == 0:
            return m, None
        return '(%s << %d)' % (m, amount), '(%s >> %d & 1u)' % (m, 32 - amount)
    if kind == 'lsr':
        if amount == 32:
            return '0u', '(%s >> 31)' % m
        return '(%s >> %d)' % (m, amount), '(%s >> %d & 1u)' % (m, amount - 1)
    if kind == 'asr':
        if amount == 32:
            return '(uint32_t)((int32_t)%s >> 31)' % m, '(%s >> 31)' % m
        return '(uint32_t)((int32_t)%s >> %d)' % (m, amount), '(%s >> %d & 1u)' % (m, amount - 1)
    return 'std::rotr(%s, %d)' % (m, amount), '(%s >> %d & 1u)' % (m, amount - 1)


def _op2(i, a, want_carry):
    """(lines setting the locals `b` and, if want_carry, `sc`) for a
    data-processing operand2."""
    op2 = i.op2
    if op2[0] == 'imm':
        _, v, carry = op2
        out = ['const uint32_t b = %s;' % _h(v)]
        if want_carry:
            out.append('const uint32_t sc = %s;' % ('c.c' if carry is None else '%du' % carry))
        return out
    if op2[0] == 'reg':
        _, rm, kind, amount = op2
        out = ['const uint32_t m = %s;' % _reg(rm, a)]
        e, ce = _shift_imm('m', kind, amount, None)
        out.append('const uint32_t b = %s;' % e)
        if want_carry:
            out.append('const uint32_t sc = %s;' % (ce or 'c.c'))
        return out
    _, rm, kind, rs = op2
    if rs == PC:
        raise Unsupported('a shift by pc')
    return ['uint32_t sc;',
            'const uint32_t b = arm_%s(%s, %s & 0xFFu, c.c, sc);' % (kind, _reg(rm, a, 12), R(rs))]


_LOGIC = {0: 'a & b', 1: 'a ^ b', 8: 'a & b', 9: 'a ^ b', 12: 'a | b',
          13: 'b', 14: 'a & ~b', 15: '~b'}
_ARITH = {2: ('a', '~b', '1u'), 10: ('a', '~b', '1u'), 3: ('b', '~a', '1u'),
          4: ('a', 'b', '0u'), 11: ('a', 'b', '0u'), 5: ('a', 'b', 'c.c'),
          6: ('a', '~b', 'c.c'), 7: ('b', '~a', 'c.c')}
_PLAIN = {2: 'a - b', 3: 'b - a', 4: 'a + b', 5: 'a + b + c.c',
          6: 'a - b - (c.c ^ 1u)', 7: 'b - a - (c.c ^ 1u)'}


def dp_value(i, a):
    """Lines that leave the data-processing result in the local `r` and
    set the flags if it does (rd not written)."""
    op = i.op
    s = i.s or op in TEST_OPS
    logical = op in _LOGIC
    out = _op2(i, a, s and logical)
    if op not in MOVE_OPS:
        out.append('const uint32_t a = %s;' % _reg(i.rn, a, 12 if i.op2[0] == 'regreg' else 8))
    if logical:
        out.append('const uint32_t r = %s;' % _LOGIC[op])
        if s:
            out.append('c.n = r >> 31; c.z = r == 0; c.c = sc;')
    elif s:
        x, y, cin = _ARITH[op]
        out.append('const uint32_t r = arm_adc(c, %s, %s, %s);' % (x, y, cin))
    else:
        out.append('const uint32_t r = %s;' % _PLAIN[op])
    return out


def sdt_address(i, a):
    """Lines that set the locals `ad` (the address transferred) and `wb`
    (the base written back), for a single transfer."""
    if i.offset[0] == 'imm':
        off = _h(i.offset[1])
        pre = []
    else:
        _, rm, kind, amount = i.offset
        if rm == PC:
            raise Unsupported('pc as a transfer offset')
        pre = ['const uint32_t m = %s;' % R(rm)]
        off, _ = _shift_imm('m', kind, amount, None)
    if i.rn == PC and i.offset[0] == 'imm':
        moved = (a + 8 + i.offset[1]) if i.u else (a + 8 - i.offset[1])
        return [], _abs(moved if i.p else a + 8), None
    base = _reg(i.rn, a)
    moved = '%s %s %s' % (base, '+' if i.u else '-', off)
    if i.p:
        if i.w:
            return pre + ['const uint32_t ad = %s;' % moved], 'ad', 'ad'
        return pre, '(%s)' % moved, None
    return pre + ['const uint32_t ad = %s;' % base], 'ad', moved


def bdt_plan(i):
    """(registers, lines setting the local `ad0` (the lowest address) and
    the written-back value `fin`)."""
    regs = [k for k in range(16) if i.regs >> k & 1]
    n = 4 * len(regs)
    base = R(i.rn)
    if i.u:
        start = '%s + 4u' % base if i.p else base
        fin = '%s + %du' % (base, n)
    else:
        start = '%s - %du' % (base, n) if i.p else '%s - %du' % (base, n - 4)
        fin = '%s - %du' % (base, n)
    return regs, ['const uint32_t ad0 = (%s) & ~3u;' % start,
                  'const uint32_t fin = %s;' % fin]


def clocks(i):
    """The clocks the ARM60 takes over an instruction that executes, as
    its datasheet counts the cycles: S (sequential), N (non-sequential),
    I (internal); a failed condition is 1S. The 3DO's ARM60 has no cache
    and an N cycle takes two clocks (as Opera counts it), S and I one. A
    multiply's internal cycles depend on rs: arm_mul_m adds them where it
    runs, so 1S here."""
    k = i.kind
    if k == 'dp':
        n = 1 + (i.op2[0] == 'regreg')
        if i.rd == PC and i.op not in TEST_OPS:
            n += 3                                      # + S + N
        return n
    if k == 'sdt':
        if i.l:
            return 7 if i.rd == PC else 4               # S + N + I (+ S + N)
        return 4                                        # 2N
    if k == 'bdt':
        x = bin(i.regs).count('1')
        if i.l:
            return x + (6 if i.regs >> PC & 1 else 3)   # xS + N + I (+ S + N)
        return x + 3                                    # (x - 1)S + 2N
    if k == 'swp':
        return 6                                        # S + 2N + I
    if k in ('b', 'swi'):
        return 4                                        # 2S + N
    return 1                                            # mul, mrs, msr: S


def stmt(i, a):
    """C++ for one instruction that does not write pc (and is not a branch
    or a swi): a block of statements, or a single one."""
    k = i.kind
    if k == 'dp':
        if i.rd == PC and i.op not in TEST_OPS:
            raise Unsupported('a write to pc in stmt()')
        lines = dp_value(i, a)
        if i.op not in TEST_OPS:
            lines.append('%s = r;' % R(i.rd))
        return '{ %s }' % ' '.join(lines)
    if k == 'mul':
        if PC in (i.rd, i.rm, i.rs) or (i.a and i.rn == PC):
            raise Unsupported('a multiply with pc')
        e = '%s * %s' % (R(i.rm), R(i.rs))
        if i.a:
            e += ' + %s' % R(i.rn)
        if i.s:
            return '{ const uint32_t r = %s; %s = r; c.n = r >> 31; c.z = r == 0; }' % (e, R(i.rd))
        return '%s = %s;' % (R(i.rd), e)
    if k == 'sdt':
        writeback = i.w or not i.p
        if writeback and i.rn == PC:
            raise Unsupported('writeback to pc')
        pre, ad, wb = sdt_address(i, a)
        if i.l:
            if i.rd == PC:
                raise Unsupported('a load into pc in stmt()')
            if wb is not None and i.rd == i.rn:
                raise Unsupported('a load into the base it writes back')
            load = 'ld8(%s)' % ad if i.b else 'ldw(%s)' % ad
            body = pre + ['const uint32_t v = %s;' % load]
            if wb is not None:
                body.append('%s = %s;' % (R(i.rn), wb))
            body.append('%s = v;' % R(i.rd))
        else:
            v = _abs(a + 12) if i.rd == PC else R(i.rd)
            body = pre + ['const uint32_t v = %s;' % v,
                          ('st8(%s, v);' if i.b else 'st32(%s, v);') % ad]
            if wb is not None:
                body.append('%s = %s;' % (R(i.rn), wb))
        return '{ %s }' % ' '.join(body)
    if k == 'bdt':
        if i.psr:
            raise Unsupported('ldm/stm ^ in user mode')
        if i.rn == PC:
            raise Unsupported('ldm/stm on pc')
        regs, body = bdt_plan(i)
        if i.l:
            if PC in regs:
                raise Unsupported('a load into pc in stmt()')
            body += ['const uint32_t v%d = ld32(ad0 + %du);' % (j, 4 * j) for j in range(len(regs))]
            if i.w:
                body.append('%s = fin;' % R(i.rn))
            body += ['%s = v%d;' % (R(r), j) for j, r in enumerate(regs)]
        else:
            for j, r in enumerate(regs):
                if r == PC:
                    v = _abs(a + 12)
                elif r == i.rn and i.w and j > 0:
                    v = 'fin'
                else:
                    v = R(r)
                body.append('st32(ad0 + %du, %s);' % (4 * j, v))
            if i.w:
                body.append('%s = fin;' % R(i.rn))
        return '{ %s }' % ' '.join(body)
    if k == 'swp':
        if PC in (i.rd, i.rn, i.rm):
            raise Unsupported('swp with pc')
        if i.b:
            return '{ const uint32_t ad = %s; const uint32_t v = ld8(ad); st8(ad, %s); %s = v; }' % (
                R(i.rn), R(i.rm), R(i.rd))
        return '{ const uint32_t ad = %s; const uint32_t v = ldw(ad); st32(ad, %s); %s = v; }' % (
            R(i.rn), R(i.rm), R(i.rd))
    if k == 'mrs':
        if i.psr:
            raise Unsupported('mrs spsr in user mode')
        return '%s = arm_cpsr(c);' % R(i.rd)
    if k == 'msr':
        if i.psr:
            raise Unsupported('msr spsr in user mode')
        if not i.mask & 8:
            return ';'
        v = _h(i.op2[1]) if i.op2[0] == 'imm' else _reg(i.op2[1], a)
        return 'arm_set_flags(c, %s);' % v
    raise Unsupported('%08X: %s' % (a, arm60.text(i)))


class Body:
    """One discovered function as C++.

    `prog` is the `discover.Program`, `f` its Function, `entries` every
    function entry of the program. After emit(): `sites` counts the
    transfers by kind, `unknown` lists static targets that are not
    entries."""

    def __init__(self, prog, f, entries, comments=True):
        self.p, self.f, self.entries, self.comments = prog, f, entries, comments
        self.ops = {a: prog.at(a) for a in f.code}
        self.sites = {}
        self.unknown = []

    def _count(self, kind):
        self.sites[kind] = self.sites.get(kind, 0) + 1

    def _goto(self, t, src):
        if t not in self.f.code or (t != self.f.entry and t in self.entries):
            return self._tail(t)
        if t <= src:
            return 'ARM_POLL(c); goto L_%08X;' % t
        return 'goto L_%08X;' % t

    def _tail(self, t):
        if t in self.entries:
            return '%s(c); return;' % fname(t)
        self.unknown.append(t)
        return 'arm_call(c, %s); return;' % _abs(t)

    def _call(self, t, ret):
        if t in self.entries:
            call = '%s(c);' % fname(t)
        else:
            self.unknown.append(t)
            call = 'arm_call(c, %s);' % _abs(t)
        return '%s = %s; ARM_POLL(c); %s ARM_RET(c, %s);' % (R(LR), _abs(ret), call, _abs(ret))

    def _pc_write(self, i, a, value):
        """Leave through pc = `value` (an expression of locals), as
        discovery classified the write."""
        kind = self.p._pc_write(i, a, self.f)
        if kind == 'return':
            self._count('return')
            local = sorted(t for t in getattr(self.f, 'local_returns', ()) if t in self.f.code)
            if local:                                   # back from a local subroutine, or out
                cases = ' '.join('case 0x%08Xu: goto L_%08X;' % (t, t) for t in local)
                return '{ const uint32_t t = %s & ~3u; switch (t - mb) { %s } c.pc = t; return; }' % (
                    value, cases)
            return 'c.pc = %s & ~3u; return;' % value
        if kind == 'call':
            self._count('indirect call')
            return 'ARM_POLL(c); arm_call(c, %s & ~3u); ARM_RET(c, %s);' % (value, _abs(a + 4))
        self._count('indirect jump')
        return 'arm_call(c, %s & ~3u); return;' % value

    def labels(self):
        out = set()
        code = self.f.code
        for a, i in self.ops.items():
            if i.kind == 'b' and not i.link and i.target in code:
                out.add(i.target)
            if a in self.f.switches:
                out |= {t for t in self.f.switches[a] if t in code}
        out |= {t for t in getattr(self.f, 'local_returns', ()) if t in code}
        return out

    def _insn(self, a):
        """(C++ for the instruction at `a`, whether control can go on to
        the next word, the clocks its block pays for it, whether it can
        leave the block)."""
        i = self.ops[a]
        k = i.kind
        cond = COND.get(i.cond)
        on = True
        leaves = k in ('b', 'swi') or i.writes_pc() or a in self.f.switches
        if k == 'b':
            if i.link:
                text = self._call(i.target, a + 4)
                self._count('call')
            else:
                text = self._goto(i.target, a)
                on = cond is not None
        elif k == 'swi':
            text = 'arm_swi(c, %s, %s);' % (_h(i.imm), _abs(a))
            self._count('swi')
            if i.imm == 0x11 and cond is None:
                on = False
        elif a in self.f.switches:
            targets = self.f.switches[a]
            if i.cond != 9:
                raise Unsupported('%08X: a switch under a condition other than ls' % a)
            cases = ' '.join('case %d: goto L_%08X;' % (n, t) for n, t in enumerate(targets[1:]))
            text = 'switch (%s) { %s default: arm_fault(c, %s, "switch index"); }' % (
                R(i.op2[1]), cases, _abs(a))
            self._count('switch')
        elif i.writes_pc():
            on = cond is not None
            if k == 'dp':
                if i.s:
                    raise Unsupported('%08X: an S on a write to pc' % a)
                text = '{ %s %s }' % (' '.join(dp_value(i, a)), self._pc_write(i, a, 'r'))
            elif k == 'sdt':
                if (i.w or not i.p) and i.rn == PC:
                    raise Unsupported('writeback to pc')
                pre, ad, wb = sdt_address(i, a)
                load = 'ld8(%s)' % ad if i.b else 'ldw(%s)' % ad
                body = pre + ['const uint32_t x = %s;' % load]
                if wb is not None:
                    body.append('%s = %s;' % (R(i.rn), wb))
                text = '{ %s %s }' % (' '.join(body), self._pc_write(i, a, 'x'))
            elif k == 'bdt':
                if i.psr:
                    raise Unsupported('%08X: ldm ^ in user mode' % a)
                regs, body = bdt_plan(i)
                body += ['const uint32_t v%d = ld32(ad0 + %du);' % (j, 4 * j) for j in range(len(regs))]
                if i.w:
                    body.append('%s = fin;' % R(i.rn))
                body += ['%s = v%d;' % (R(r), j) for j, r in enumerate(regs) if r != PC]
                text = '{ %s %s }' % (' '.join(body), self._pc_write(i, a, 'v%d' % (len(regs) - 1)))
            else:
                raise Unsupported('%08X: %s writes pc' % (a, arm60.text(i)))
        else:
            text = stmt(i, a)
        if k == 'mul':
            text = 'ARM_TICK(c, arm_mul_m(%s)); %s' % (R(i.rs), text)
        full = clocks(i)
        if cond is None:
            return text, on, full, leaves
        if full > 1:
            text = 'ARM_TICK(c, %d); %s' % (full - 1, text)
        return 'if (%s) { %s }' % (cond, text), on, 1, leaves

    def emit(self):
        """The C++ function, as a list of lines."""
        labels = self.labels()
        code = self.f.code
        order = sorted(code)
        body, charge, leaves = [], {}, set()
        for n, a in enumerate(order):
            text, on, charge[a], out = self._insn(a)
            if out:
                leaves.add(a)
            if self.comments:
                text += '  // %08X %s' % (a, arm60.text(self.ops[a]))
            nxt = order[n + 1] if n + 1 < len(order) else None
            after = None
            if on and a + 4 != nxt:
                if a + 4 in code:                       # on past a literal pool
                    after = self._goto(a + 4, a)
                    labels.add(a + 4)
                else:
                    # discovery stopped here: an exit under a condition and
                    # its inverse (`bne x; beq y`) leaves nothing to run on
                    after = 'arm_fault(c, %s, "past a two-way exit");' % _abs(a + 4)
                    self._count('dead end')
            body.append((a, text, after))
        lines = ['void %s(ArmCpu& c) {' % fname(self.f.entry)]
        if order and order[0] != self.f.entry:          # code shared from below the entry
            labels.add(self.f.entry)
            lines.append('    goto L_%08X;' % self.f.entry)
        # each block's clocks, paid at its first word
        tick, start = {}, None
        for n, a in enumerate(order):
            if start is None or a in labels or order[n - 1] in leaves or order[n - 1] + 4 != a:
                start = a
                tick[a] = 0
            tick[start] += charge[a]
        for a, text, after in body:
            if a in labels:
                lines.append('L_%08X:' % a)
            if a in tick:
                text = 'ARM_TICK(c, %d); %s' % (tick[a], text)
            lines.append('    ' + text)
            if after:
                lines.append('    ' + after)
        lines.append('}')
        return lines
