"""ARM Object Format: the SDK's libraries and objects, read.

The 3DO SDK ships its libraries (`clib.lib`, `graphics.lib`, `audio.lib`,
`operamath.lib`, ...) as **ALF** archives of **AOF** objects, the ARM
toolchain's formats, both built on the same chunk file:

    +0   0xc3cbc6c5      the chunk file id; its byte order is the file's
    +4   max chunks, used chunks
    +12  entries of 16 bytes: an 8-byte id, an offset, a size

An ALF library has `LIB_DIRY` (one entry per member: the chunk index of
its `LIB_DATA`, then the member's name) and a `LIB_DATA` chunk per member,
each a chunk file of its own holding one AOF object:

    OBJ_HEAD   type 0xc5e2d080, version, areas, symbols, entry area and
               offset, then 5 words an area: name, attributes and
               alignment, size, relocation count, base
    OBJ_AREA   each area's bytes (none for a zero-init area), then its
               relocations, 8 bytes each: an offset and a flags word
    OBJ_SYMT   16 bytes a symbol: name, attributes, value, area name
    OBJ_STRT   the strings every name above is an offset into

A relocation's flags word comes in two forms. Type 2 (bit 31 set): bits
0-23 a symbol or area index, 24-25 the field (byte, half, word,
instruction), 26 PC-relative, 27 symbol (set) or area (clear). Type 1
(bit 31 clear): bits 0-15 the index, 16-17 the field, 18 PC-relative, 19
symbol.

What a 3DO port wants from the libraries is names. A library's **folio
glue** is a function that loads a folio's base pointer from a global and
jumps through one of its negative slots, `ldr pc, [rN, #-slot]`: the global
says which folio, the slot which function, the symbol the function's name.
`glue()` reads that out of every member. The same shapes are what
`shapes` can prove a game's unnamed library code against.

    python -m 3dokit.aof LIB                   # members and their symbols
    python -m 3dokit.aof LIB... --glue         # folio slots named by the glue
"""
import argparse
import collections
import os
import struct
import sys

CHUNK_ID = 0xc3cbc6c5
AOF_TYPE = 0xc5e2d080
AREA_CODE = 0x200
AREA_ZEROINIT = 0x1000
SYM_DEFINED = 1
SYM_GLOBAL = 2


class Chunks:
    """A chunk file: {id: [(offset, size), ...]} over its bytes."""

    def __init__(self, data):
        self.d = data
        if struct.unpack_from('>I', data, 0)[0] == CHUNK_ID:
            self.e = '>'
        elif struct.unpack_from('<I', data, 0)[0] == CHUNK_ID:
            self.e = '<'
        else:
            raise ValueError('not a chunk file')
        mx = struct.unpack_from(self.e + 'I', data, 4)[0]
        self.entries = []
        for k in range(mx):
            cid, off, size = struct.unpack_from(self.e + '8sII', data, 12 + 16 * k)
            self.entries.append((cid.rstrip(b'\0').decode('latin1'), off, size))

    def get(self, cid):
        for c, off, size in self.entries:
            if c == cid and size:
                return self.d[off:off + size]
        return None

    def word(self, b, at):
        return struct.unpack_from(self.e + 'I', b, at)[0]


class Area:
    def __init__(self, name, attrs, data, relocs):
        self.name, self.attrs, self.data, self.relocs = name, attrs, data, relocs

    @property
    def code(self):
        return bool(self.attrs & AREA_CODE)


class Object:
    """One AOF object: its areas (with bytes and relocations) and symbols."""

    def __init__(self, data, name=''):
        c = self.chunks = Chunks(data)
        self.name, self.e = name, c.e
        head, strt = c.get('OBJ_HEAD'), c.get('OBJ_STRT') or b''
        if head is None or c.word(head, 0) != AOF_TYPE:
            raise ValueError('%s: not a relocatable AOF object' % name)
        nareas, nsyms = c.word(head, 8), c.word(head, 12)

        def string(off):
            e = strt.find(b'\0', off)
            return strt[off:e].decode('latin1')

        area_bytes = c.get('OBJ_AREA') or b''
        self.areas, pos = [], 0
        for k in range(nareas):
            an, attrs, size, nrel, _ = struct.unpack_from(c.e + '5I', head, 24 + 20 * k)
            data = b''
            if not attrs & AREA_ZEROINIT:
                data, pos = area_bytes[pos:pos + size], pos + size
            relocs = []
            for r in range(nrel):
                off, flags = struct.unpack_from(c.e + '2I', area_bytes, pos + 8 * r)
                relocs.append(_reloc(off, flags))
            pos += 8 * nrel
            self.areas.append(Area(string(an), attrs, data, relocs))

        symt = c.get('OBJ_SYMT') or b''
        self.symbols = []
        for k in range(nsyms):
            n, attrs, value, an = struct.unpack_from(c.e + '4I', symt, 16 * k)
            self.symbols.append((string(n), attrs, value,
                                 string(an) if attrs & SYM_DEFINED else None))

    def defined(self):
        """[(name, area index, offset)] of the symbols defined in code."""
        by = {a.name: k for k, a in enumerate(self.areas)}
        return [(n, by[an], v) for n, attrs, v, an in self.symbols
                if attrs & SYM_DEFINED and an in by and self.areas[by[an]].code]


def _reloc(off, flags):
    """(offset, field, pc-relative, symbol?, index)"""
    if flags >> 31:
        return (off, (flags >> 24) & 3, bool(flags >> 26 & 1),
                bool(flags >> 27 & 1), flags & 0xffffff)
    return (off, (flags >> 16) & 3, bool(flags >> 18 & 1),
            bool(flags >> 19 & 1), flags & 0xffff)


class Library:
    """An ALF library: [(member name, Object)]."""

    def __init__(self, path):
        self.path = path
        data = open(path, 'rb').read()
        c = Chunks(data)
        diry = c.get('LIB_DIRY') or b''
        self.members, at = [], 0
        while at + 12 <= len(diry):
            idx, elen, dlen = struct.unpack_from(c.e + '3I', diry, at)
            if elen == 0:
                break
            name = diry[at + 12:at + 12 + dlen].split(b'\0')[0].decode('latin1')
            cid, off, size = c.entries[idx]
            if cid == 'LIB_DATA' and size:
                self.members.append((name, Object(data[off:off + size], name)))
            at += elen


def load(path):
    """A library's members, or a lone object as one member."""
    head = open(path, 'rb').read(4096)
    c = Chunks(head + b'\0' * 64)
    if any(e[0] == 'LIB_DIRY' for e in c.entries):
        return Library(path).members
    return [(path, Object(open(path, 'rb').read(), path))]


def _insns(area):
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_BIG_ENDIAN, \
        CS_MODE_LITTLE_ENDIAN
    return Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_BIG_ENDIAN)


def glue(members):
    """{(base symbol, slot in bytes): [function names]} from every member's
    folio glue: a code symbol whose function, before the next symbol, jumps
    through `ldr pc, [rN, #-slot]` (or loads `ip` that way and moves it to
    `pc`) after loading a word that a relocation ties to a global."""
    import re
    out = collections.defaultdict(set)
    vec = re.compile(r'^(pc|ip|r\d+), \[(\w+), #-(0x[0-9a-f]+|\d+)\]$')
    for mname, ob in members:
        syms = sorted(ob.defined(), key=lambda s: (s[1], s[2]))
        for k, (name, ai, off) in enumerate(syms):
            area = ob.areas[ai]
            end = len(area.data)
            for n2, a2, o2 in syms[k + 1:]:
                if a2 == ai and o2 > off:
                    end = o2
                    break
            md = _insns(area)
            md.skipdata = True
            relsyms = {r[0]: ob.symbols[r[4]][0] for r in area.relocs
                       if r[3] and r[4] < len(ob.symbols) and
                       not ob.symbols[r[4]][0].startswith('__rt_')}
            base = [relsyms[o] for o in sorted(relsyms) if off <= o < end + 64]
            for i in md.disasm(area.data[off:end], off):
                if not i.mnemonic.startswith('ldr'):
                    continue
                m = vec.match(i.op_str)
                if m and (m.group(1) in ('pc', 'ip')):
                    slot = -int(m.group(3), 0)
                    out[(base[0] if base else
                         getattr(ob, 'library', '?'), slot)].add(name)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.aof', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--glue', action='store_true')
    a = ap.parse_args(argv)
    if a.glue:
        allm = []
        for f in a.files:
            for m in load(f):
                m[1].library = os.path.basename(f)
                allm.append(m)
        g = glue(allm)
        for (base, slot), names in sorted(g.items()):
            print('%-14s %5d  %s' % (base, slot, ' '.join(sorted(names))))
        return 0
    for f in a.files:
        mem = load(f)
        print('%s: %d members' % (f, len(mem)))
        for name, ob in mem:
            print('  %-28s %s' % (name, ' '.join(
                '%s(%d)' % (n, len(ob.areas[ai].data)) for n, ai, _ in ob.defined())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
