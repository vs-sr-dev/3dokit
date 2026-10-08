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
   any `add`/`sub lr, pc, #k` that gives lr the same word); a folio vector is
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
6. **Data** is every word of the image that no function reaches. The image
   is searched whole, not to `aif.code_end`: hand-written routines can be
   linked past the compiler's read-only area. A descent that meets an `undefined` word is
   reported: code does not run into data.

`Program` keeps funcs {entry: Function}, code (word addresses), switches
{the addls: [targets]}, and the indirect transfers by kind.
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
                 'problems', 'switches', 'indirect', 'local_returns')

    def __init__(self, entry, origin):
        self.entry, self.origin = entry, origin
        self.code = set()
        self.calls, self.tails = set(), set()
        self.name, self.problems = None, []
        self.switches, self.indirect = {}, []
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

        def seed(a, origin):
            if self.start <= a < self.end and a not in self.funcs:
                self.funcs[a] = Function(a, origin)
                self.funcs[a].name = self.names.get(a)
                todo.append(a)

        seed(self.aif.entry, 'entry')
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
        self.code = set()
        for f in self.funcs.values():
            self.code |= f.code

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
        f.problems, f.switches, f.indirect = [], {}, []
        f.local_returns = set()
        todo = [f.entry]
        while todo:
            a = todo.pop()
            taken = set()       # conditions branched away on since the flags last changed
            while True:
                if a in f.code:
                    break
                if not (self.start <= a < self.end):
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
                        if self.start <= i.target < self.end:
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
        if prev is not None and prev.kind == 'dp' and prev.op == 13 and \
                prev.rd == LR and prev.op2 == ('reg', PC, 'lsl', 0):
            return 'call'                                    # mov lr, pc first
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
        out.append('code %d words, data %d words, between %#x and %#x' % (
            len(self.code), span - len(self.code), self.start, self.end))
        named = set(self.names)
        out.append('embedded names: %d, all found as functions: %s' % (
            len(named), named <= set(self.funcs)))
        out.append('functions reached by pointer (relocated words): %d' % len(self.pointed))
        called = set()
        for f in fs:
            called |= f.calls | f.tails
        uncalled = [f for f in fs if f.entry not in called and f.origin != 'entry'
                    and f.entry not in self.pointed]
        out.append('functions nothing calls, tail-calls or points at: %d' % len(uncalled))
        sw = sum(len(f.switches) for f in fs)
        out.append('switches: %d' % sw)
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
