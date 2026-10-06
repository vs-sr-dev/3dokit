"""The 3DO SDK's names for the OS's binary interface: SWI numbers and folio
vector slots.

A program reaches Portfolio by `swi #(folio << 16 | n)` and by
`ldr pc, [base, #-slot]` through a folio's table (see `portfolio`). The
numbers are the interface; the SDK names them, in two places:

* **SWIs** are declared in the SDK's C headers, `__swi(KERNELSWI+24)
  SendIO(...)`, with the folio bases (`KERNELSWI` 0x10000, `FILEFOLIOSWI`
  0x30000, `AUDIOSWI` 0x40000, `MATHSWI` 0x50000) defined beside them;
* **vector slots** are in the libraries' glue, one small function a slot
  (`aof.glue`): the global the glue reads says which folio, the offset which
  slot, the symbol the name. The headers list some of them too
  (`_DRAWCELS_ -43`, `KBV_MEMCPY -14`, `LOADINSTRUMENT (-10)`), in words,
  and name the few the libraries have no glue for (`KBV_VFPRINTF`,
  `FINDAUDIODEVICE`): those come from the headers.

`sdk_tables.py` was generated from the 1.2, 1.3 and 2.5 SDKs' headers and
the 3do-devkit's libraries, and is the interface only: numbers and
names, no code. A later OS may add entries; no number changed meaning
across the releases compared: the 1.2, 1.3 and 2.5 headers name no SWI
differently, and their slot lists (Graphics' `_X_`, the Kernel's `KBV_`,
audio's) agree with the libraries' glue on every slot both have, under
names that were sometimes changed (`MapSprite` became `MapCel`,
`LocateItem` `LookupItem`, `InsertTail` `InsertNodeFromTail`). Names a game's own code pinned
(`portfolio.SWI_NAMES`, `SLOT_NAMES`) are checked against these.

    python -m 3dokit.sdk                                  # the tables
    python -m 3dokit.sdk --headers 1p2 --headers 2p5 --libs LIBDIR > t.py
                                                          # regenerate

With several header directories the first wins and the others only add;
a number two releases name differently is reported on stderr.
"""
import argparse
import glob
import os
import re
import sys

from .sdk_tables import SWIS, SLOTS

# Which folio each library glue's base belongs to.
GLUE_FOLIO = {
    'GrafBase': 'Graphics', 'KernelBase': 'Kernel', 'clib.lib': 'Kernel',
    'audio.lib': 'audio', 'GetFileFolio': 'File', 'operamath.lib': 'Operamath',
    'CompressionBase': 'Compression', 'InternationalBase': 'International',
    'JStringBase': 'JString',
}

SWI_DECL = re.compile(r'__swi\s*\(\s*([^)]+?)\s*\)\s*\*?\s*(\w+)\s*\(')
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$', re.M)


def swi_name(n):
    return SWIS.get(n)


def slot_name(folio, slot):
    return SLOTS.get((folio, slot))


def _eval(expr, defs, depth=0):
    expr = expr.strip()
    if depth > 8:
        raise ValueError(expr)
    toks = re.findall(r'0x[0-9a-fA-F]+|\d+|\w+|<<|[()+\-|]', expr)
    out = []
    for t in toks:
        if re.match(r'^[A-Za-z_]\w*$', t):
            if t not in defs:
                raise ValueError(t)
            out.append('(%d)' % _eval(defs[t], defs, depth + 1))
        else:
            out.append(t)
    return int(eval(''.join(out), {'__builtins__': {}}))


def swis_from_headers(where):
    defs, decls = {}, []
    for p in sorted(glob.glob(os.path.join(where, '*.h'))):
        s = open(p, encoding='latin1').read()
        s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
        for m in DEFINE.finditer(s):
            defs.setdefault(m.group(1), m.group(2))
        decls += SWI_DECL.findall(s)
    out = {}
    for expr, name in decls:
        try:
            n = _eval(expr, defs)
        except (ValueError, SyntaxError):
            continue
        if 0 < n < 0x1000000:
            out.setdefault(n, set()).add(name)
    return {n: '/'.join(sorted(v)) for n, v in out.items()}


# The headers' slot lists, in words: (folio, file, define pattern).
HEADER_SLOTS = (
    ('Graphics', 'graphics.h', r'#define\s+_([A-Z0-9]+)_\s+(-\d+)'),
    ('Kernel', 'folio.h', r'#define\s+KBV_([A-Z0-9]+)\s+(-\d+)'),
    ('audio', 'audio.h', r'#define\s+([A-Z0-9]+)\s+\((-\d+)\)'),
)
PROTO = re.compile(r'\b([A-Za-z_]\w*)\s*\(')


def slots_from_headers(where):
    """{(folio, slot in bytes): name} from the headers' slot lists, each
    name spelt as the function the headers declare when one matches."""
    protos = {}
    for p in glob.glob(os.path.join(where, '*.h')):
        for n in PROTO.findall(open(p, encoding='latin1').read()):
            protos.setdefault(re.sub(r'[^a-z0-9]', '', n.lower()), n)
    out = {}
    for folio, f, rx in HEADER_SLOTS:
        p = os.path.join(where, f)
        if not os.path.exists(p):
            continue
        s = re.sub(r'/\*.*?\*/', ' ', open(p, encoding='latin1').read(), flags=re.S)
        for name, idx in re.findall(rx, s):
            if folio == 'audio' and name.startswith(('CUE_', 'AF_')):
                continue
            out[(folio, int(idx) * 4)] = protos.get(name.lower(), name)
    return out


def slots_from_libraries(libdir):
    from .aof import load, glue
    members = []
    for f in sorted(glob.glob(os.path.join(libdir, '*.lib'))):
        for m in load(f):
            m[1].library = os.path.basename(f)
            members.append(m)
    out = {}
    for (base, slot), names in glue(members).items():
        folio = GLUE_FOLIO.get(base)
        if folio is None:
            continue
        names = sorted(n for n in names if not n.startswith(('STUB', '_')))
        if names:
            out[(folio, slot)] = '/'.join(names)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.sdk', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--headers', action='append', default=[])
    ap.add_argument('--libs')
    a = ap.parse_args(argv)
    if a.headers or a.libs:
        swis = {}
        for h in a.headers:
            for n, name in swis_from_headers(h).items():
                if n in swis and not set(name.split('/')) & set(swis[n].split('/')):
                    print('swi %#x: %s, and %s in %s' % (n, swis[n], name, h),
                          file=sys.stderr)
                swis.setdefault(n, name)
        slots = slots_from_libraries(a.libs) if a.libs else {}
        for h in a.headers:
            for k, name in slots_from_headers(h).items():
                slots.setdefault(k, name)
        print('"""Generated by `python -m 3dokit.sdk --from` from the 3DO SDK\'s')
        print('headers and libraries: the binary interface\'s numbers and names."""')
        print('SWIS = {')
        for n in sorted(swis):
            print('    %#07x: %r,' % (n, swis[n]))
        print('}\n\nSLOTS = {')
        for k in sorted(slots):
            print('    (%r, %d): %r,' % (k[0], k[1], slots[k]))
        print('}')
        return 0
    for n in sorted(SWIS):
        print('swi %#07x  %s' % (n, SWIS[n]))
    for k in sorted(SLOTS):
        print('%-12s %5d  %s' % (k[0], k[1], SLOTS[k]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
