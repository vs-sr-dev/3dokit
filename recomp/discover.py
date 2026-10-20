"""Function discovery for 3DO executables: which words are code, where each
function starts and what it reaches.

    python -m 3dokit.recomp.discover IMAGE [--report] [--out FUNCS.tsv] [--against GHIDRA.tsv]

A 3DO program is an AIF image linked at 0 (`aif`), built by the ARM C
compiler, with its literal pools, strings and `const` tables inside the
code and the linked libraries' read-only data after it. So this is a
recursive descent over `arm60`'s exact ARMv3 decoding that classifies each
word as code or data while it finds the functions:

1. **Seeds**: the AIF entry, the compiler's embedded names
   (`arm.Image.embedded_names`), every APCS prologue (`mov ip, sp` then an
   unconditional `stmfd sp!, {..., lr, ...}`), and every relocated word
   (`aif` relocations: the image's own record of which words are
   addresses) that points at a prologue or a name -- a function reached by
   pointer, the tables of a game's behaviours.
2. **A function** is what control flow reaches from its entry: both ways
   at a conditional branch, on past a `bl`, a `swi` and an indirect call,
   stopping at a return (`mov pc, lr`, `ldm ..., pc`, `ldr pc, [sp], #4`),
   an unconditional `b` (followed, unless it lands on another function's
   entry: a tail call), an indirect jump, `swi 0x11` (exit), and a pair of
   conditional exits under inverse conditions with the flags unchanged
   between (`bne x; beq y`).
3. **Calls** are `bl`s *in reached code*: a `bl` decoded from data does not
   make a function (the read-only data decodes as plenty of them).
4. **Indirect transfers**: `mov lr, pc` followed by a write to `pc` is a
   call and returns to the word after it -- and so is `add lr, pc, #0` (or
   any `add`/`sub lr, pc, #k` that gives lr the same word), and `mov rN,
   pc` with another register, the callee returning through it (The Need
   for Speed's entry of an overlay at its AIF header, whose fourth word it
   has made `mov pc, r7`); a folio vector is
   `ldr pc, [rB, #-slot]` (`portfolio`); a load of `pc` from the word the
   function stored `lr` in, both addresses computed from `pc`, is a return
   (hand-written code that parks its return address in a word of its own);
   a word that `add`/`sub lr, pc, #k` puts in lr and the descent reaches
   as the function's code is a local subroutine's return (`local_returns`:
   a subroutine reached by a plain `b` comes back there; the word is not
   followed from lr alone, as hand-written code points lr at tables too);
   anything else that writes `pc` is a jump through a pointer, listed.
5. **The compiler's switch**: `cmp rI, #n` ... `addls pc, pc, rI, lsl #2`,
   then `b default`, then n+1 cases, each a `b case` word except perhaps the
   last, whose code can follow the table: every case is a code address.
   And hand-written code's jump into the code after it, `add pc, pc, rX`
   (shifted or not), where the straight line before it makes rX from a
   `mov rX, #k` or an `and rX, rY, #mask`, then constants or'd, added,
   subtracted, and'd or eor'd in, under any condition (The Need for Speed's
   arctangent picks its octant so, its decompressor its unrolled copy's
   length): every value rX can take is a case (`computed`).
6. **Data** is every word of the image that no function reaches. The image
   is searched whole, not to `aif.code_end`: hand-written routines can be
   linked past the compiler's read-only area. A descent that meets an `undefined` word is
   reported: code does not run into data.

`Program` keeps funcs {entry: Function}, code (word addresses), switches
{the addls: [targets]}, computed jumps {the add: (rX, {value: target})},
and the indirect transfers by kind.
"""
import argparse
import bisect
import collections
import struct
import sys

from .. import arm60
from ..aif import AIF
from ..arm import Image

PC, LR, SP = 15, 14, 13
EXIT_SWI = 0x11


class Function:
    __slots__ = ('entry', 'code', 'calls', 'tails', 'origin', 'name',
                 'problems', 'switches', 'computed', 'indirect', 'local_returns')

    def __init__(self, entry, origin):
        self.entry, self.origin = entry, origin
        self.code = set()
        self.calls, self.tails = set(), set()
        self.name, self.problems = None, []
        self.switches, self.computed, self.indirect = {}, {}, []
        self.local_returns = set()

    @property
    def end(self):
        return max(self.code) + 4 if self.code else self.entry


class Program:
    def __init__(self, path, seeds=()):
        self.path = path
        self.aif = AIF(path)
        self.d = self.aif.d
        self.start = 0x100
        # the whole image: hand-written code can sit past the compiler's
        # read-only area (Crash 'n Burn has some at 0x445d8, in RW)
        self.end = self.aif.stub
        im = Image(path)
        self.names = im.embedded_names()
        self.prologues = {f for f in im.funcs
                          if self._word(f) == 0xe1a0c00d}          # mov ip, sp
        self.relocs = set(self.aif.relocs)
        self.insn = {}
        self.funcs = {}
        todo = []

        self.names = im.embedded_names()
        # The AIF header's own routines: what its BLs at 0x00 (decompression), 0x04
        # (self-relocation) and 0x08 (zero-init) call -- code below 0x100, or past the
        # image (the relocation stub follows RW, its list follows it), each searched in
        # its own extent. A program needs them when it is loaded elsewhere than 0: code
        # loaded with LoadCode, a program a task is made from.
        self.extents, self.aif_routines = [], []
        for off in (0x00, 0x04, 0x08):
            w = self._word(off)
            if w >> 24 != 0xEB:
                continue
            disp = w & 0xFFFFFF
            t = off + 8 + 4 * (disp - 0x1000000 if disp & 0x800000 else disp)
            if 0 <= t < self.start:
                self.extents.append((t, self.start))
            elif self.end <= t < len(self.d):
                self.extents.append((t, len(self.d) & ~3))
            else:
                continue
            self.aif_routines.append(t)

        def seed(a, origin):
            if self._inside(a) and a not in self.funcs:
                self.funcs[a] = Function(a, origin)
                self.funcs[a].name = self.names.get(a)
                todo.append(a)

        seed(self.aif.entry, 'entry')
        for a in self.aif_routines:
            seed(a, 'aif')
        for a in sorted(self.names):
            seed(a, 'name')
        for a in sorted(self.prologues):
            seed(a, 'prologue')
        self.pointed = set()
        for r in sorted(self.relocs):
            if r + 4 <= len(self.d):
                v = self._word(r)
                if v in self.names or v in self.prologues:
                    self.pointed.add(v)
        for a in seeds:
            seed(a, 'seed')
        for f in self.funcs.values():
            f.name = self.names.get(f.entry)

        while todo:
            e = todo.pop()
            f = self.funcs[e]
            self._descend(f)
            for t in sorted(f.calls):
                if t not in self.funcs:
                    seed(t, 'call')
        # a tail branch can only be told from a local one once every entry
        # is known: redo the functions whose branches land on a later one
        changed = True
        while changed:
            changed = False
            for f in list(self.funcs.values()):
                if any(t in self.funcs and t != f.entry for t in self._branch_targets(f)):
                    before = len(f.code)
                    self._descend(f)
                    changed |= len(f.code) != before
                    for t in sorted(f.calls):
                        if t not in self.funcs:
                            seed(t, 'call')
                            changed = True
                    while todo:
                        e = todo.pop()
                        self._descend(self.funcs[e])
        self._pointed_leaves(todo, seed)
        self.code = set()
        for f in self.funcs.values():
            self.code |= f.code

    def _pointed_leaves(self, todo, seed):
        """A function with neither a prologue nor a name, reached only through a pointer: a
        frameless routine the compiler laid right after another function's last word (lib3DO's
        DataStream hands one over from `OpenStreamFile`: Escape from Monster Manor's 0x1c4c8,
        Doctor Hauzer's 0x2ced4). A relocated word in a literal pool that reached code loads
        (`ldr rN, [pc, #k]`), naming the word just past a function's code, a word no function
        reaches, is such a function when its own descent is clean -- no data met, no word of
        another function's -- and so is what it calls. So is one named by a relocated word that
        such a pool word itself names: a pointer variable whose address the code loads, to call
        through it (`ldr r2, =var; ldr pc, [r2]`: the C library's `return 0` stub as The Need
        for Speed's hook at 0x4245c holds it), and that one may follow a routine nothing
        reaches, when the word before it is an unconditional return. So, too, is a word of a
        table of functions: a relocated word of the read-write data with a relocated word
        beside it naming a function's entry (in the read-only area, a literal pool's words sit
        side by side so) (the services The Need for Speed's `LaunchMe` hands its overlay,
        0x3ebf4 on), wherever it lies -- after a literal pool too -- and so is code a pointer
        names that opens as a function does: a push of lr (`LaunchMe`'s 0x2a4d8, after a
        string, a callback its objects hold), or a wrapper, a few data-processing words and a `b`
        to a function's entry (`frontovl`'s 0x5c14, after a string: the allocator it hands
        DefineSampleHere). The pool and the table matter: a debugger's tables linked into the
        read-write area (Escape from Monster Manor's Cinepak library, built with them) name the
        words after functions' last returns too, and no code loads them nor any function sits
        beside them."""
        while True:
            code = set()
            for f in self.funcs.values():
                code |= f.code
            pools = set()
            for a in code:
                i = self.at(a)
                if i.kind == 'sdt' and i.l and i.rn == PC and i.offset[0] == 'imm':
                    pools.add(a + 8 + (i.offset[1] if i.u else -i.offset[1]))
            pooled = self.relocs & pools
            named = {self._word(p) for p in pooled if p + 4 <= len(self.d)}

            def beside_a_function(r):
                return any(q in self.relocs and q + 4 <= len(self.d) and self._word(q) in self.funcs
                           for q in (r - 4, r + 4))
            tabled = {r for r in self.relocs if r >= self.aif.ro and r + 4 <= len(self.d) and
                      beside_a_function(r)}
            found, regrow = [], set()
            for r in sorted(pooled | (self.relocs & named) | tabled):
                if r + 4 > len(self.d):
                    continue
                v = self._word(r)
                if v & 3 or not self.start <= v < self.end or v in self.funcs:
                    continue
                if v in code:
                    # code of another function that it reaches only by a branch -- a tail branch
                    # to a routine a table or a pool names (Demolition Man's bytecode handlers:
                    # 0xd514's `b 0x8f78`, the 0x2931c table's second entry). Its own function
                    # when no word of that function runs into it and its descent is that
                    # function's code from it; that function's branch is then a tail call.
                    if (r in pooled or r in tabled) and self._splits(v):
                        found.append(v)
                        regrow.update(f.entry for f in self.funcs.values() if v in f.code)
                    continue
                if v - 4 not in code and r not in tabled and not self._thunk(v) and                         (r in pooled or not self._return_before(v)):
                    continue
                g = Function(v, 'pointer')
                self._descend(g)
                if g.problems:
                    continue
                if not g.code & code:
                    found.append(v)
                    continue
                # A tail it shares with a function found before it: the code both branch to,
                # which no word of that function runs into (Demolition Man's handlers 0x74ac and
                # 0x74c8, each ending in `b 0x742c`). The tail is a function of its own, and the
                # branches to it tail calls, when that leaves this one's descent clear.
                tails = {t for t in self._branch_targets(g) if t in code and self._splits(t)}
                if not tails:
                    continue
                shared = set()
                for t in tails:
                    h = Function(t, 'tail')
                    self._descend(h)
                    shared |= h.code
                    self.funcs[t] = h
                self._descend(g)
                for t in tails:
                    del self.funcs[t]
                if not g.problems and not g.code & (code - shared):
                    found.append(v)
                    for t in tails:
                        if t not in found:
                            found.append(t)
                        regrow.update(f.entry for f in self.funcs.values() if t in f.code)
            if not found:
                return
            for v in found:
                seed(v, 'pointer')
                self.pointed.add(v)
            for e in regrow:
                self._descend(self.funcs[e])
            while todo:
                f = self.funcs[todo.pop()]
                self._descend(f)
                for t in sorted(f.calls):
                    if t not in self.funcs:
                        seed(t, 'call')

    def _splits(self, t):
        """Is `t`, code of other functions and no function's entry, reached by them only by a
        branch -- no word of any of them runs into it --, its own descent clean and inside each
        of them? Then it can be a function of its own, their branches to it tail calls."""
        owners = [f for f in self.funcs.values() if t in f.code]
        if not owners or t in self.funcs:
            return False
        h = Function(t, 'tail')
        self._descend(h)
        return not h.problems and all(t - 4 not in f.code and h.code <= f.code for f in owners)

    def _thunk(self, v):
        """Does the code at `v` look like a function's entry: an unconditional push of lr
        (`stmfd sp!, {..., lr}`, a prologue without `mov ip, sp`), or a wrapper -- at most eight
        unconditional data-processing words that leave pc alone, then an unconditional `b` to a
        function's entry (a tail call)?"""
        i = self.at(v)
        if i.kind == 'bdt' and not i.l and i.w and i.p and not i.u and i.rn == SP and i.cond == 14 \
                and i.regs & (1 << LR) and not i.regs & (1 << PC):
            return True
        for a in range(v, v + 36, 4):
            if not self._inside(a):
                return False
            i = self.at(a)
            if i.kind == 'b':
                return not i.link and i.cond == 14 and i.target in self.funcs
            if i.kind != 'dp' or i.cond != 14 or i.writes_pc():
                return False
        return False

    def _return_before(self, v):
        """Is the word before `v` an unconditional return (`mov pc, lr`, `ldm ..., pc`,
        `ldr pc, [sp], #4`)?"""
        if v - 4 < self.start:
            return False
        i = self.at(v - 4)
        return i.cond == 14 and i.writes_pc() and self._pc_write(i, v - 4) == 'return'

    def _inside(self, a):
        """In the code searched: the image's, or an AIF routine's extent."""
        return self.start <= a < self.end or any(lo <= a < hi for lo, hi in self.extents)

    # -- decoding ---------------------------------------------------------
    def _word(self, a):
        return struct.unpack_from('>I', self.d, a)[0]

    def at(self, a):
        i = self.insn.get(a)
        if i is None:
            i = self.insn[a] = arm60.decode(self._word(a), a)
        return i

    def _branch_targets(self, f):
        return {self.at(a).target for a in f.code
                if self.at(a).kind == 'b' and not self.at(a).link}

    # -- the descent ------------------------------------------------------
    def _descend(self, f):
        f.code, f.calls, f.tails = set(), set(), set()
        f.problems, f.switches, f.computed, f.indirect = [], {}, {}, []
        f.local_returns = set()
        todo = [f.entry]
        while todo or self._unreached_returns(f, todo):
            a = todo.pop()
            taken = set()       # conditions branched away on since the flags last changed
            while True:
                if a in f.code:
                    break
                if not self._inside(a):
                    f.problems.append((a, 'runs out of the code'))
                    break
                i = self.at(a)
                if i.kind == 'undefined':
                    f.problems.append((a, 'runs into data (%08x)' % i.word))
                    break
                f.code.add(a)
                always = i.cond == 14
                if i.sets_flags():
                    taken.clear()
                if i.kind == 'dp' and i.op in (2, 4) and i.rd == LR and i.rn == PC and \
                        i.op2[0] == 'imm' and not i.s:
                    # lr set to a word that may be this function's code: a local
                    # subroutine's return, if the descent reaches it by other ways (not
                    # followed from here: hand-written code also points lr at tables)
                    v = (a + 8 + (i.op2[1] if i.op == 4 else -i.op2[1])) & 0xFFFFFFFF
                    if self.start <= v < self.end and (v == f.entry or v not in self.funcs):
                        f.local_returns.add(v)
                if i.kind == 'b':
                    if i.link:
                        taken.clear()
                        if self._inside(i.target):
                            f.calls.add(i.target)
                        else:
                            f.problems.append((a, 'calls %#x, outside the code' % i.target))
                        a += 4
                        continue
                    t = i.target
                    if t != f.entry and t in self.funcs:
                        f.tails.add(t)
                    else:
                        todo.append(t)
                    if always or self._both_ways(taken, i.cond):
                        break
                    a += 4
                    continue
                if i.kind == 'swi':
                    taken.clear()
                    if i.imm == EXIT_SWI and always:
                        break
                    a += 4
                    continue
                if i.writes_pc():
                    kind = self._pc_write(i, a, f)
                    if kind == 'switch':
                        targets = self._switch(f, a)
                        if targets is None:
                            f.problems.append((a, 'a switch with no bound'))
                            break
                        f.switches[a] = targets
                        todo.extend(targets)
                        todo.append(a + 4)
                        break
                    if kind == 'jump' and always:
                        cases = self._computed(f, a)
                        if cases is not None:
                            f.computed[a] = (i.op2[1], cases)
                            todo.extend(sorted(cases.values()))
                            break
                    if kind == 'call':
                        f.indirect.append((a, self._indirect_kind(i)))
                        taken.clear()
                        a += 4
                        continue
                    if kind == 'return':
                        if always or self._both_ways(taken, i.cond):
                            break
                        a += 4
                        continue
                    f.indirect.append((a, self._indirect_kind(i) + ' jump'))
                    if always or self._both_ways(taken, i.cond):
                        break
                    a += 4
                    continue
                a += 4

    def _unreached_returns(self, f, todo):
        """When the descent has run out: a local return it has not reached that lies between
        words of the function's own code is code too -- hand-written code can reach a word only
        by returning to it through lr (Immercenary's CinepakSubroutine: `add lr, pc, #0x18` at
        0xa954, its routines' `mov pc, lr`, the word after a `b`). A word lr points at outside
        the function's span is left alone: hand-written code points lr at tables too."""
        if not f.code:
            return False
        lo, hi = min(f.code), max(f.code)
        more = sorted(v for v in f.local_returns if v not in f.code and lo < v < hi and self._inside(v)
                      and self.at(v).kind != 'undefined')
        todo.extend(more)
        return bool(more)

    @staticmethod
    def _both_ways(taken, cond):
        """Has the path now left under a condition and its inverse (eq/ne,
        cs/cc, ... differ in the low bit) with the flags unchanged between?
        Then nothing runs on: `bne x; beq y` and a literal pool after it."""
        taken.add(cond)
        return cond ^ 1 in taken

    def _pc_write(self, i, a, f=None):
        if i.kind == 'dp' and i.op == 4 and i.rn == PC and i.op2[0] == 'reg' \
                and i.op2[2] == 'lsl' and i.op2[3] == 2 and i.cond == 9:
            return 'switch'                                  # addls pc, pc, rI, lsl #2
        if i.kind == 'dp' and i.op == 13 and i.op2 == ('reg', LR, 'lsl', 0):
            return 'return'                                  # mov pc, lr
        if i.kind == 'bdt' and i.l:
            return 'return'                                  # ldm ..., pc
        if i.kind == 'sdt' and i.l and i.rn == SP and not i.p and i.u:
            return 'return'                                  # ldr pc, [sp], #4
        if f is not None and self._parked_lr(f, i, a):
            return 'return'                                  # ldr pc, [the word lr was stored in]
        prev = self.at(a - 4) if a - 4 >= self.start else None
        if prev is not None and prev.kind == 'dp' and prev.op == 13 and not prev.s and \
                prev.rd != PC and prev.op2 == ('reg', PC, 'lsl', 0):
            return 'call'                                    # mov lr (or rN), pc first
        if prev is not None and prev.kind == 'dp' and prev.op in (2, 4) and prev.rd == LR and \
                prev.rn == PC and prev.op2[0] == 'imm' and not prev.s and \
                (a - 4 + 8 + (prev.op2[1] if prev.op == 4 else -prev.op2[1])) & 0xFFFFFFFF == a + 4:
            return 'call'                                    # add lr, pc, #0 first: the same lr
        return 'jump'

    def _pc_relative(self, f, a, reg, cond):
        """The value of `reg` before the instruction at `a`, if the straight
        line just before it computes it from pc: `add/sub reg, pc, #k`, then
        any number of `add/sub reg, reg, #k`, each unconditional or under
        `cond`, all in f's code. None otherwise."""
        more, b = 0, a - 4
        while b in f.code and a - b <= 32:
            j = self.at(b)
            if reg in j.writes():
                if j.kind != 'dp' or j.op not in (2, 4) or j.op2[0] != 'imm' or \
                        j.cond not in (14, cond):
                    return None
                k = j.op2[1] if j.op == 4 else -j.op2[1]
                if j.rn == PC:
                    return (b + 8 + k + more) & 0xFFFFFFFF
                if j.rn != reg:
                    return None
                more += k
            b -= 4
        return None

    def _transfer_address(self, f, i, a):
        """The constant address a single transfer at `a` uses, or None."""
        if i.offset[0] != 'imm' or i.rn == PC:
            return None
        base = self._pc_relative(f, a, i.rn, i.cond)
        if base is None:
            return None
        off = i.offset[1] if i.u else -i.offset[1]
        return (base + off) & 0xFFFFFFFF if i.p else base

    def _parked_lr(self, f, i, a):
        """A load of pc from a word the function stored lr in, both
        addresses computed from pc: hand-written code that parks its return
        address in a word of its own (Crash 'n Burn's 0x41fd8 and 0x42120)
        and returns through it."""
        if i.kind != 'sdt' or not i.l or i.rd != PC or i.b:
            return False
        at = self._transfer_address(f, i, a)
        if at is None:
            return False
        for b in f.code:
            j = self.at(b)
            if j.kind == 'sdt' and not j.l and not j.b and j.rd == LR and \
                    self._transfer_address(f, j, b) == at:
                return True
        return False

    def _indirect_kind(self, i):
        if i.kind == 'sdt' and i.l and i.offset[0] == 'imm' and not i.u and i.p:
            return 'folio vector'
        if i.kind == 'sdt':
            return 'pointer'
        return 'register'

    def _switch(self, f, a):
        """The targets of the compiler's switch at `a`: the default branch
        at a+4 and n+1 entries from a+8, n from the `cmp rI, #n` before."""
        i = self.at(a)
        idx = i.op2[1]
        for b in range(a - 4, max(self.start, a - 48), -4):
            j = self.at(b)
            if j.kind == 'dp' and j.op == 10 and j.rn == idx and j.op2[0] == 'imm':
                # n+1 cases from a+8, each a `b` -- except the last, whose
                # code may follow the table directly
                n = j.op2[1]
                return [a + 4] + [a + 8 + 4 * k for k in range(n + 1)]
            if idx in j.writes():
                return None
        return None

    def _computed(self, f, a):
        """The cases of a computed jump at `a`, `add pc, pc, rX[, lsl #s]`: {value of rX:
        target}, when the straight line before it, in f's code, makes rX from an
        unconditional `mov rX, #k` or `and rX, rY, #mask` (any of the mask's bits), then
        any number of `orr`, `add`, `sub`, `rsb`, `and` or `eor rX, rX, #k` -- under a
        condition, either value --, nothing else writing rX; each target a + 8 + (value <<
        s) word-aligned and inside the code. None otherwise."""
        i = self.at(a)
        if not (i.kind == 'dp' and i.op == 4 and i.rn == PC and not i.s and
                i.op2[0] == 'reg' and i.op2[2] == 'lsl' and isinstance(i.op2[3], int)):
            return None
        x, shift = i.op2[1], i.op2[3]
        if x in (PC, SP, LR):
            return None
        ops = {12: lambda v, k: v | k, 4: lambda v, k: v + k, 2: lambda v, k: v - k,
               3: lambda v, k: k - v, 0: lambda v, k: v & k, 1: lambda v, k: v ^ k}
        steps = []
        for b in range(a - 4, max(self.start, a - 256) - 4, -4):
            if b not in f.code:
                return None
            j = self.at(b)
            if x not in j.writes():
                continue
            if j.kind != 'dp' or j.op2[0] != 'imm' or j.s:
                return None
            k = j.op2[1]
            if j.cond == 14 and j.op == 13:                 # mov rX, #k
                values = {k}
            elif j.cond == 14 and j.op == 0 and j.rn != x and bin(k).count('1') <= 6:
                bits = [1 << n for n in range(32) if k >> n & 1]
                values = {sum(bt for n, bt in enumerate(bits) if m >> n & 1)
                          for m in range(1 << len(bits))}   # and rX, rY, #mask
            elif j.op in ops and j.rn == x:
                steps.append((j.op, k, j.cond != 14))
                continue
            else:
                return None
            for op, k, cond in reversed(steps):
                after = {ops[op](v, k) & 0xFFFFFFFF for v in values}
                values = values | after if cond else after
            if len(values) > 64:
                return None
            cases = {v: (a + 8 + (v << shift)) & 0xFFFFFFFF for v in sorted(values)}
            if any(t & 3 or not self._inside(t) or t - a > 0x400 for t in cases.values()):
                return None
            return cases
        return None

    # -- reading out -------------------------------------------------------
    def func_of(self, a):
        es = sorted(self.funcs)
        k = bisect.bisect_right(es, a) - 1
        return es[k] if k >= 0 else None

    def report(self):
        out = []
        fs = self.funcs.values()
        by = collections.Counter(f.origin for f in fs)
        out.append('%s: %d functions (%s)' % (
            self.path, len(self.funcs),
            ', '.join('%s %d' % kv for kv in sorted(by.items()))))
        span = (self.end - self.start) // 4
        inside = sum(1 for a in self.code if self.start <= a < self.end)
        out.append('code %d words, data %d words, between %#x and %#x' % (
            inside, span - inside, self.start, self.end))
        if self.extents:
            out.append('AIF routines: %s, %d words of code' % (
                ', '.join('%#x' % a for a in self.aif_routines), len(self.code) - inside))
        named = set(self.names)
        out.append('embedded names: %d, all found as functions: %s' % (
            len(named), named <= set(self.funcs)))
        out.append('functions reached by pointer (relocated words): %d' % len(self.pointed))
        called = set()
        for f in fs:
            called |= f.calls | f.tails
        uncalled = [f for f in fs if f.entry not in called and f.origin not in ('entry', 'aif')
                    and f.entry not in self.pointed]
        out.append('functions nothing calls, tail-calls or points at: %d' % len(uncalled))
        sw = sum(len(f.switches) for f in fs)
        out.append('switches: %d' % sw)
        cj = sum(len(f.computed) for f in fs)
        if cj:
            out.append('computed jumps: %d' % cj)
        ind = collections.Counter(k for f in fs for _, k in f.indirect)
        out.append('indirect transfers: %s' % ', '.join(
            '%s %d' % kv for kv in sorted(ind.items())))
        probs = [(f, p) for f in fs for p in f.problems]
        out.append('problems: %d' % len(probs))
        for f, (a, why) in probs[:40]:
            out.append('  %#07x in %s: %s' % (
                a, f.name or '%#x' % f.entry, why))
        overlap = collections.Counter()
        for f in fs:
            for a in f.code:
                overlap[a] += 1
        shared = sum(1 for v in overlap.values() if v > 1)
        out.append('words in more than one function: %d' % shared)
        return '\n'.join(out)

    def write_tsv(self, path):
        with open(path, 'w') as f:
            for e in sorted(self.funcs):
                fn = self.funcs[e]
                f.write('%08x\t%d\t%s\t%s\n' % (e, fn.end - e, fn.name or '', fn.origin))


def against(prog, path):
    """Compare with a function list (hex entry first on each line, as
    Ghidra's ExportFuncs writes it)."""
    theirs = set()
    for line in open(path):
        p = line.split()
        if p:
            try:
                theirs.add(int(p[0], 16))
            except ValueError:
                pass
    mine = set(prog.funcs)
    lines = ['against %s: %d functions there, %d here, %d in both' % (
        path, len(theirs), len(mine), len(theirs & mine))]
    for a in sorted(theirs - mine)[:40]:
        lines.append('  only there: %#07x' % a)
    for a in sorted(mine - theirs)[:40]:
        lines.append('  only here : %#07x %s' % (a, prog.funcs[a].name or prog.funcs[a].origin))
    return '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.recomp.discover', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('--seeds', default='', help='extra entry points, hex, comma-separated')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--out', help='write entry, size, name, origin as TSV')
    ap.add_argument('--against', help="a function list to compare with (Ghidra's)")
    a = ap.parse_args(argv)
    seeds = [int(s, 16) for s in a.seeds.split(',') if s]
    p = Program(a.image, seeds)
    print(p.report() if a.report else '%d functions' % len(p.funcs))
    if a.out:
        p.write_tsv(a.out)
    if a.against:
        print(against(p, a.against))
    return 0


if __name__ == '__main__':
    sys.exit(main())
