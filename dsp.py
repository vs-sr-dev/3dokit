"""3DO DSP instruments: the `.dsp` files in `System/Audio/dsp`.

The stock Portfolio instrument library every title ships -- 64 files with
OS 23.10 (Immercenary), 77 with 24.225 (OMF2097), the latter a superset in
which 60 of the 64 shared instruments carry the same code. A port has to
reproduce whatever a game asks the audio folio to build, and this is where
"what does `halfmono8` actually do" is written down.

Each file is IFF:

    FORM 3INS
      NAME                  the file's own name
      FORM DSPP
        DHDR                4 words: a catalogue number, a format version
                            (2; 3 on `splitexec` in 24.225; 1 on
                            `dcsqxdstereo` and `timesplus` in the 1993
                            set), then two zeros
        DCOD                3 words -- 0, 12, and a code word count -- then
                            that many 16-bit DSP instructions
        DRSC                16 bytes a resource: type, count-or-offset, 0, 0
        DRLC                16 bytes a relocation: mask, 0, resource index,
                            code word to patch
        DNMS                the resource names, NUL-separated, in the same
                            order as DRSC
        DKNB                a linked list of knob records (below)
      FORM ATNV / ENVL      attenuation and envelope tables, on three files

The resource types are not documented here from an SDK header; they are what
the sixty-four files themselves show, by correlating each type number with
the names that carry it:

    0   Entry, once per instrument -- the code block itself; field 2 is its
        length in words, and it equals DCOD's word count on all sixty-four
    1   a knob: a host-writable parameter, described further by DKNB
    2   a variable: field 2 is how many words of DSP data memory it needs
    3   a variable the host reads back (Monitor, EO_LeftCount, EO_RightCount)
    5   a ring-buffer base (MYRB, LeftRBASE, RightRBASE)
    6   an input FIFO
    7   an output FIFO
    8   Ticks, once per instrument -- field 2 is what the instrument costs
    9   the left ADC          10  the right ADC
    0x4000  a subroutine this file exports
    0x8000  a subroutine this file imports

Checked on both libraries: every file walks to its last byte and every
structural claim below holds (`--verify`).

`StepSizes` in `decodeadpcm.dsp` asks for 89 words and `IndexDeltas` for 8,
which are exactly the two IMA ADPCM tables -- the cheapest confirmation that
field 2 of a type-2 resource is a word count.

A relocation names a code word that the loader patches with a resource's
address once it has been placed. Every word a relocation points at has its
top bit set, and the low fifteen bits are an addend, so the code as shipped
carries `0x8000 | offset` wherever an address belongs.

A knob record is what the 1993 audio folio (`System/Folios/AUDIOFOLIO`,
V20.19) reads in place: the offset of the next record from the start of
DKNB (0 for the last), the minimum, the maximum and the default, a count
of targets, a 32-byte name, then per target 16 bytes -- the resource it
writes, a calculation type and two operands `a` and `b`. Every knob in the
three libraries has one target, its own resource, so a record is 68 bytes.
`TweakKnob` passes the value through each target's calculation (0: as it
is; 1: `v * a + b`; 2: `v * a / b`; 3: `v / 44100`, the folio's sample
rate -- a frequency in 16.16 Hz to a phase step), clamps the first
target's result to [min, max], and writes it; `TweakRawKnob` skips the
calculation, and a new instrument's knobs start at their defaults that
way. Type 3 is the oscillators' `Frequency`; the 24.225 library's type 4
(`square_lfo`, `triangle_lfo`, `pulse_lfo`) is one the 1993 folio refuses.

The relocations come in two kinds, told apart by the mask word: with bit
17 (0x20a00, a resource's address) the code word's low ten bits are the
next word of a chain to patch, 0 ending it, so one record serves every use
of the resource; with bit 16 (0x10a00, the code's own addresses -- branch
targets) the word's low ten bits are an offset into the instrument, to
which its place in the DSP's memory is added. `0x600` patches an RBASE
instruction with a ring's base (sampler.dsp's MYRB).

The code
--------

The DSP's instruction set is not in the SDK; it is read here as the FreeDO
emulator reads it (the Opera emulator carries that code; read, not
copied), and the instruments on the disc bear it out (`--dis`). A word
with bit 15 clear is an arithmetic instruction:

    bits 13-14  operand words that follow     bit 12  the multiplier's second
    bits 10-11  the ALU's A input             operand an operand (else the
    bits 8-9    its B input                   accumulator's top 17 bits)
    bits 4-7    the operation                 bits 0-3  the barrel shifter
    inputs: 0 the accumulator, 1 and 2 the two ALU operands, 3 the multiplier
    operations: TRA NEG ADD ADDC SUB SUBB INC DEC, then (logical) TRL NOT
                AND NAND OR NOR XOR XNOR
    shifter: none, <<1 <<2 <<3 <<4 <<5 <<8, CLIP, by an operand, >>16 >>8
             >>5 >>4 >>3 >>2 >>1 (arithmetic, or logical for TRL and after)

The operands fill, in order, the multiplier's (one or two), then the ALU's,
then the shifter's; one more than those is where the result is written
(its top 16 bits), as is an address operand marked write-back. An operand
word is an address (0x8000 | addr, bit 10 indirect, bit 11 write-back), an
immediate (0xc000 | 13 bits, signed; bit 13 shifts it up 3), or one, two
or three registers (relative to RBASE). A word with bit 15 set is a
control instruction: NOP 0x8000, BAC 0x8080, RBASE 0x8100, RMAP 0x8180,
RTS 0x8200, OP_MASK 0x8280, SLEEP 0x8380 (the frame's end), JUMP 0x8400,
JSR 0x8800, BFM 0x8c00, MOVEREG 0x9000 (a register from an operand), MOVE
0x9800 (an address from an operand, bit 10 indirect), and from 0xa000 a
conditional branch on the flags (bits 10-14: two masks, a select, a mode).
The memory the instruments use: their variables and knobs at the
addresses relocated in, I memory 0x100-0x2ff (0x106 and 0x107 the left
and right sums the mixers add to; head.dsp moves them to the DAC at 0x3fe
and 0x3ff and clears them), the FIFOs read at 0x0f0 + n.

Usage
-----

    python -m 3dokit.dsp System/Audio/dsp               # the catalogue
    python -m 3dokit.dsp System/Audio/dsp/sampler.dsp -v
    python -m 3dokit.dsp System/Audio/dsp/sampler.dsp --dis   # its code
    python -m 3dokit.dsp System/Audio/dsp --verify
    python -m 3dokit.dsp System/Audio/dsp --used GAME   # which ones a program names
"""
import struct, os, re, glob, argparse, collections

RTYPE = {0: 'code', 1: 'knob', 2: 'variable', 3: 'readback', 5: 'ringbase',
         6: 'in-fifo', 7: 'out-fifo', 8: 'ticks', 9: 'left-adc',
         10: 'right-adc', 0x4000: 'exports', 0x8000: 'imports'}


def chunks(d, off, end):
    """Every IFF chunk in [off, end), descending into FORMs."""
    while off + 8 <= end:
        tag = d[off:off + 4]
        n = struct.unpack_from('>I', d, off + 4)[0]
        if tag == b'FORM':
            yield d[off + 8:off + 12], off + 12, min(end, off + 8 + n)
            yield from chunks(d, off + 12, min(end, off + 8 + n))
        else:
            yield tag, off + 8, off + 8 + n
        off += 8 + n + (n & 1)


class Resource:
    __slots__ = ('name', 'type', 'value')

    def __init__(self, name, type_, value):
        self.name, self.type, self.value = name, type_, value

    def __str__(self):
        return '%-18s %-9s %d' % (self.name, RTYPE.get(self.type, self.type),
                                  self.value)


class Knob:
    """A DKNB record: its range, its default, and its targets, each
    (resource, calculation type, a, b)."""
    __slots__ = ('name', 'lo', 'hi', 'default', 'targets', 'next')

    @property
    def resource(self):
        return self.targets[0][0] if self.targets else None

    def __str__(self):
        calc = ''.join('  calc %d (%d, %d)' % t[1:] for t in self.targets if any(t[1:]))
        return '%-18s %6d .. %-6d default %-6d -> resource %s%s' % (
            self.name, self.lo, self.hi, self.default,
            ', '.join(str(t[0]) for t in self.targets), calc)


class Instrument:
    def __init__(self, path):
        self.path = path
        self.file = os.path.basename(path)
        self.name = self.file[:-4] if self.file.endswith('.dsp') else self.file
        d = open(path, 'rb').read()
        self.size = len(d)
        self.forms, g = [], {}
        for tag, a, b in chunks(d, 0, len(d)):
            g.setdefault(tag, d[a:b])
            if tag in (b'3INS', b'DSPP', b'ATNV', b'ENVL'):
                self.forms.append(tag.decode())
        self.chunks = {k.decode(): v for k, v in g.items()}

        self.id, self.version = struct.unpack_from('>2I', g[b'DHDR'])
        self.code = g[b'DCOD'][12:]
        self.words = len(self.code) // 2

        names = [x.decode() for x in g[b'DNMS'].rstrip(b'\0').split(b'\0')]
        self.resources = [
            Resource(nm, *struct.unpack_from('>2I', g[b'DRSC'], i * 16)[:2])
            for i, nm in enumerate(names)]

        self.knobs = []
        k = g.get(b'DKNB')
        o = 0
        while k:
            nxt, lo, hi, dflt, n = struct.unpack_from('>IiiiI', k, o)
            kn = Knob()
            kn.name = k[o + 20:o + 52].split(b'\0')[0].decode()
            kn.lo, kn.hi, kn.default = lo, hi, dflt
            kn.targets = [struct.unpack_from('>Iiii', k, o + 52 + 16 * t)
                          for t in range(n) if o + 68 + 16 * t <= len(k)]
            kn.next = (o + 52 + 16 * n, nxt)    # where its targets end, its link
            self.knobs.append(kn)
            if not nxt:
                break
            o = nxt

        self.relocs = [struct.unpack_from('>4I', g[b'DRLC'], o)
                       for o in range(0, len(g[b'DRLC']) - 15, 16)]

    def _find(self, type_):
        return next((r for r in self.resources if r.type == type_), None)

    @property
    def code_size(self):
        """What the code resource asks for, in words.  Always DCOD's own
        word count, which is the check that reading it as a size rather than
        as a start offset is right."""
        r = self._find(0)
        return r.value if r else None

    @property
    def ticks(self):
        r = self._find(8)
        return r.value if r else None

    def ports(self, *types):
        return [r for r in self.resources if r.type in types]

    def summary(self):
        return ('%-20s id %3d  %3d code words  %4d ticks  '
                '%2d knobs  %2d vars  %d in / %d out fifo'
                % (self.name, self.id, self.words, self.ticks,
                   len(self.knobs), len(self.ports(2, 3)),
                   len(self.ports(6)), len(self.ports(7))))

    def detail(self):
        out = ['%s  (%d bytes)  %s' % (self.file, self.size,
                                       ' '.join(self.forms))]
        out.append('  catalogue id %d, format version %d, %d code words, '
                   '%d ticks' % (self.id, self.version, self.words, self.ticks))
        out.append('  resources:')
        out += ['    ' + str(r) for r in self.resources]
        if self.knobs:
            out.append('  knobs:')
            out += ['    ' + str(k) for k in self.knobs]
        out.append('  relocations: %d' % len(self.relocs))
        for mask, _, idx, off in self.relocs[:8]:
            out.append('    code word %-3d <- %-18s (mask %#x, word is %#06x)'
                       % (off, self.resources[idx].name, mask,
                          struct.unpack_from('>H', self.code, off * 2)[0]))
        if len(self.relocs) > 8:
            out.append('    ... and %d more' % (len(self.relocs) - 8))
        return '\n'.join(out)


ALU_OPS = ['TRA', 'NEG', 'ADD', 'ADDC', 'SUB', 'SUBB', 'INC', 'DEC',
           'TRL', 'NOT', 'AND', 'NAND', 'OR', 'NOR', 'XOR', 'XNOR']
ALU_IN = ['ACC', 'OP1', 'OP2', 'MUL']
SHIFTS = ['', '<<1', '<<2', '<<3', '<<4', '<<5', '<<8', 'CLIP', '<<op',
          '>>16', '>>8', '>>5', '>>4', '>>3', '>>2', '>>1']


def resolved(ins):
    """The code's words with every relocation's chain cleared, and per word
    the resource it names: a resource's address (a chain through the low
    ten bits), or the instrument's own offset."""
    words = list(struct.unpack('>%dH' % ins.words, ins.code))
    names = {}
    for mask, _, idx, off in ins.relocs:
        if mask & 0x20000:
            w, seen = off, set()
            while w < len(words) and w not in seen:
                seen.add(w)
                names[w] = ins.resources[idx].name
                nxt = words[w] & 0x3ff
                words[w] &= ~0x3ff
                if not nxt:
                    break
                w = nxt
        else:
            names.setdefault(off, ins.resources[idx].name if mask != 0x10a00 else None)
    return words, names


def _condition(b):
    """A conditional branch's bits 10-14 (two masks, a select, a mode) as a
    condition: modes 1 and 2 ask that the masked flags (N and V, or with the
    select C and Z) all be set, or all clear; mode 3 compares (LT LE GE GT
    on N, V and Z; HI LS on C and Z; then the exact tests)."""
    m0, m1, sel, mode = b & 1, (b >> 1) & 1, (b >> 2) & 1, b >> 3
    flags = [f for f, m in ((('C' if sel else 'N'), m1), (('Z' if sel else 'V'), m0)) if m]
    if mode in (1, 2) and flags:
        return ('' if mode == 1 else 'N') + ''.join(flags)
    if mode == 3:
        if not sel:
            return ['LT', 'LE', 'GE', 'GT'][m0 + 2 * m1]
        return ['HI', 'LS', 'XE', 'XNE'][m0 + 2 * m1]
    return '?%02x' % b


def _operand(w, name):
    """One operand word: its text and how many operands it gives."""
    if not w & 0x8000:
        regs = []
        for sh in (10, 5, 0):
            r = 'R%d' % ((w >> sh) & 0xf)
            regs.append('[%s]' % r if (w >> (sh + 4)) & 1 else r)
        return ','.join(regs), 3
    kind = w >> 13
    if kind == 4:
        s = name or '0x%03x' % (w & 0x3ff)
        if name and w & 0x3ff:
            s += '+%d' % (w & 0x3ff)
        if w & 0x400:
            s = '[%s]' % s
        return s + ('!' if w & 0x800 else ''), 1
    if kind == 5:
        r1 = ('[R%d]' if w & 0x10 else 'R%d') % (w & 0xf) + ('!' if w & 0x800 else '')
        if w & 0x400:
            r2 = ('[R%d]' if w & 0x200 else 'R%d') % ((w >> 5) & 0xf) + ('!' if w & 0x1000 else '')
            return r2 + ',' + r1, 2
        return r1, 1
    v = w & 0x1fff
    if v & 0x1000:
        v -= 0x2000
    if w & 0x2000:
        v <<= 3
    s = '#0x%04x' % (v & 0xffff)
    return (s + ' <%s>' % name) if name else s, 1


def disassemble(ins):
    """The instrument's code, a line an instruction: an operand that names
    a resource shows its name ('!' marks a write-back)."""
    words, names = resolved(ins)
    out, pc = [], 0
    while pc < len(words):
        w, start = words[pc], pc
        pc += 1
        if w & 0x8000:
            op, a = (w >> 7) & 0xff, w & 0x3ff
            tgt = '%d' % a
            if op == 0: s = 'NOP'
            elif op == 1: s = 'BAC'
            elif op == 2: s = 'RBASE %s' % (names.get(start) or (a & 0x3f) << 2)
            elif op == 3: s = 'RMAP %d' % (a & 7)
            elif op == 4: s = 'RTS'
            elif op == 5: s = 'OP_MASK 0x%x' % (a & 0x1f)
            elif op == 7: s = 'SLEEP'
            elif op < 16: s = 'JUMP %s' % tgt
            elif op < 24: s = 'JSR %s' % (names.get(start) or tgt)
            elif op < 32: s = 'BFM %s' % tgt
            elif op < 64:
                o, _ = _operand(words[pc], names.get(pc))
                pc += 1
                if op < 48:
                    r = 'R%d' % (w & 0xf)
                    s = 'MOVEREG %s, %s' % ('[%s]' % r if w & 0x10 else r, o)
                else:
                    d = names.get(start) or '0x%03x' % a
                    s = 'MOVE %s, %s' % ('[%s]' % d if w & 0x400 else d, o)
            else:
                s = 'B%s %s' % (_condition((w >> 10) & 0x1f), tgt)
        else:
            n = (w >> 13) & 3
            s = '%-4s %s, %s%s' % (ALU_OPS[(w >> 4) & 0xf], ALU_IN[(w >> 10) & 3],
                                   ALU_IN[(w >> 8) & 3], ' ' + SHIFTS[w & 0xf] if w & 0xf else '')
            if (w >> 12) & 1:
                s += ' (MUL op*op)'
            ops, got = [], 0
            while got < n and pc < len(words):
                o, k = _operand(words[pc], names.get(pc))
                pc += 1
                ops.append(o)
                got += k
            if ops:
                s += '  ' + ', '.join(ops)
        out.append('%4d  %-19s %s' % (start, ' '.join('%04x' % x for x in words[start:pc]), s))
    return out


def load_all(where):
    if os.path.isdir(where):
        paths = sorted(glob.glob(os.path.join(where, '*.dsp')))
    else:
        paths = [where]
    return [Instrument(p) for p in paths]


def verify(where):
    """Every structural claim in this file's docstring, checked."""
    bad = collections.Counter()
    ins = load_all(where)
    knobs = relocs = 0
    for i in ins:
        d = open(i.path, 'rb').read()
        if struct.unpack_from('>I', d, 4)[0] + 8 != len(d):
            bad['the outer FORM does not cover the file'] += 1
        if d[:4] != b'FORM' or d[8:12] != b'3INS':
            bad['not a FORM 3INS'] += 1
        if i.version not in (1, 2, 3):
            bad['DHDR version is not 1, 2 or 3'] += 1
        if struct.unpack_from('>2I', i.chunks['DHDR'], 8) != (0, 0):
            bad['DHDR does not end in two zeros'] += 1
        h = struct.unpack_from('>3I', i.chunks['DCOD'])
        if h[0] or h[1] != 12 or h[2] != i.words:
            bad['DCOD header is not (0, 12, word count)'] += 1
        if len(i.chunks['DRSC']) != 16 * len(i.resources):
            bad['DRSC is not 16 bytes a name'] += 1
        if sum(1 for r in i.resources if r.type == 0) != 1:
            bad['not exactly one entry point'] += 1
        if sum(1 for r in i.resources if r.type == 8) != 1:
            bad['not exactly one Ticks'] += 1
        if i.code_size != i.words:
            bad["the code resource does not ask for DCOD's word count"] += 1
        if len(i.knobs) != sum(1 for r in i.resources if r.type == 1):
            bad['knob count does not match the type-1 resources'] += 1
        for k in i.knobs:
            knobs += 1
            end, nxt = k.next
            if end > len(i.chunks['DKNB']) or nxt not in (0, end):
                bad['a knob record is not its targets long'] += 1
            if not k.targets or i.resources[k.resource].name != k.name:
                bad['a knob does not name its own resource'] += 1
            if any(i.resources[t[0]].type != 1 for t in k.targets):
                bad['a knob writes a resource that is not a knob'] += 1
            if not k.lo <= k.default <= k.hi:
                bad['a knob default is outside its range'] += 1
        for mask, z, idx, off in i.relocs:
            relocs += 1
            if z:
                bad['a relocation has a non-zero second word'] += 1
            if idx >= len(i.resources):
                bad['a relocation names no resource'] += 1
            if off >= i.words:
                bad['a relocation points past the code'] += 1
            elif not struct.unpack_from('>H', i.code, off * 2)[0] & 0x8000:
                bad['a relocated word has no top bit set'] += 1
    print('%d instruments, %d knobs, %d relocations' % (len(ins), knobs, relocs))
    if bad:
        for k, v in bad.most_common():
            print('  FAIL  %-52s %d' % (k, v))
        return 1
    print('  every file walks to its last byte and every structural check '
          'passes')
    return 0


def used(where, image_path):
    """Which instruments does an ARM image name?"""
    d = open(image_path, 'rb').read()
    ins = load_all(where)
    hits = [i for i in ins if i.file.encode() in d]
    print('%s names %d of the %d instruments:' % (image_path, len(hits), len(ins)))
    for i in hits:
        print('  ' + i.summary())
    # A name in the image is usually packed against the byte before it, so
    # match on the suffix rather than on the whole run of printable bytes.
    have = {i.file for i in ins}
    missing = set()
    for m in re.findall(rb'[\w.]+\.dsp', d):
        t = m.decode()
        if not any(t.endswith(h) for h in have):
            missing.add(t.lstrip('!"#$%&()*+,-./0123456789:;<=>?@'))
    if missing:
        print('  ...and names %d the disc does not carry: %s'
              % (len(missing), ' '.join(sorted(missing))))
    return 0


def main():
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.dsp', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path', help='a .dsp file or the directory of them')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='full detail rather than one line each')
    ap.add_argument('--verify', action='store_true',
                    help='check every structural claim about the format')
    ap.add_argument('--used', metavar='IMAGE',
                    help='which instruments this ARM image names')
    ap.add_argument('--dis', action='store_true',
                    help='the code, disassembled')
    a = ap.parse_args()
    if a.verify:
        raise SystemExit(verify(a.path))
    if a.used:
        raise SystemExit(used(a.path, a.used))
    ins = load_all(a.path)
    if a.dis:
        for i in ins:
            print('%s  (%d words)' % (i.file, i.words))
            print('\n'.join(disassemble(i)))
        return
    if a.verbose or len(ins) == 1:
        print('\n\n'.join(i.detail() for i in ins))
    else:
        for i in ins:
            print(i.summary())
        print('\n%d instruments, %d code words, %d knobs in all'
              % (len(ins), sum(i.words for i in ins),
                 sum(len(i.knobs) for i in ins)))


if __name__ == '__main__':
    main()
