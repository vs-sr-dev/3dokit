"""3DO console ROMs: the Opera volume in them, and the programs they carry.

A 3DO's ROM (1 MB on the FZ-1 and FZ-10) is a boot program followed by
programs and an Opera volume of its own. What a disc's `System` tree does not
bring, the ROM does: the boot's kernel, the Operator (the task that runs the
devices: the timer, SPORT, the expansion bus, the CD-ROM drive) and the File
folio (filesystems, open files, the program loader) are compressed AIF images
in the ROM ahead of its volume, and the volume holds the ROM's own folios
(`bin/`) and applications (`apps/`).

The ROM's **volume** is an ordinary Opera volume header (record type 1,
"ZZZZZ") at a 2 KB boundary, with one difference from a disc's: its block size
is 4 bytes, not 2,048, and every address in it -- the root directory's, an
entry's copies -- counts in those blocks. A directory's own blocks are its
entry's block size (2,048), as on a disc. `3dokit.disc` reads discs, whose
blocks are the image's sectors; this reads the ROM's.

The ROM's **programs** are AIF images wherever the ROM has an AIF header (a
SWI 0x11 at +0x10 and a BL or NOP at +0x00 and +0x0c); a compressed one's BL
reaches the decompressor appended to it, which `3dokit.aif` runs in `armemu`
to unpack it. Each is linked at its own base (the boot's kernel at 0x10000,
the Operator at 0x20000, the rest at 0). The 3DO binary header names most of
them once unpacked; the ROM's strings name the rest.

    python -m 3dokit.rom ROM                     # the volume and the programs
    python -m 3dokit.rom ROM --extract DIR       # the volume's files
    python -m 3dokit.rom ROM --unpack DIR        # every program, unpacked: OFFSET_BASE.bin
"""
import argparse
import os
import struct

from . import aif
from .disc import DIRECTORY, LAST_IN_BLOCK, LAST_IN_DIR, Entry

SYNC = b'\x01ZZZZZ\x01'


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


class RomVolume:
    """The Opera volume in a ROM image: its header, its tree, its files."""

    def __init__(self, rom, at=None):
        self.rom = rom
        if at is None:
            at = next((a for a in range(0, len(rom), 0x800) if rom[a:a + 7] == SYNC), None)
            if at is None:
                raise ValueError('no Opera volume header in the ROM')
        self.at = at
        h = rom[at:at + 0x800]
        self.label = h[40:72].split(b'\0')[0].decode('latin1')
        (self.identifier, self.block_size, self.block_count, self.root_id,
         self.root_blocks, self.root_block_size, last) = struct.unpack_from('>7I', h, 0x48)
        self.root_copies = [be32(h, 0x64 + 4 * i) for i in range(last + 1)]
        self._entries = None

    def _addr(self, block):
        return self.at + block * self.block_size

    def directory(self, first, nblocks, dir_block_size, path='', depth=0):
        out = []
        for k in range(nblocks):
            base = self._addr(first) + k * dir_block_size
            b = self.rom[base:base + dir_block_size]
            if len(b) < 20:
                break
            end, off = be32(b, 12), be32(b, 16)
            while off + 68 <= min(end, dir_block_size):
                flags, last = be32(b, off), be32(b, off + 64)
                if flags == 0xffffffff or last > 255:
                    break
                e = Entry()
                e.flags = flags
                e.id = be32(b, off + 4)
                e.type = b[off + 8:off + 12].decode('latin1')
                (e.block_size, e.size, e.blocks, e.burst, e.gap) = struct.unpack_from('>5I', b, off + 12)
                e.name = b[off + 32:off + 64].split(b'\0')[0].decode('latin1')
                e.copies = [be32(b, off + 68 + 4 * i) for i in range(last + 1)]
                e.path = path + '/' + e.name if path else e.name
                e.depth = depth
                out.append(e)
                off += 68 + 4 * (last + 1)
                if flags & (LAST_IN_BLOCK | LAST_IN_DIR):
                    break
            if out and out[-1].flags & LAST_IN_DIR:
                break
        return out

    def walk(self, first=None, nblocks=None, dir_block_size=None, path='', depth=0):
        if first is None:
            first, nblocks, dir_block_size = self.root_copies[0], self.root_blocks, self.root_block_size
        for e in self.directory(first, nblocks, dir_block_size, path, depth):
            yield e
            if e.kind == DIRECTORY and e.block is not None:
                yield from self.walk(e.block, max(1, e.blocks), e.block_size, e.path, depth + 1)

    def entries(self):
        if self._entries is None:
            self._entries = list(self.walk())
        return self._entries

    def files(self):
        return [e for e in self.entries() if not e.is_dir]

    def where(self, entry):
        """A file's first byte in the ROM."""
        return self._addr(entry.block)

    def read(self, entry):
        a = self.where(entry)
        return self.rom[a:a + entry.size]

    def extract(self, dest):
        n = 0
        for e in self.entries():
            out = os.path.join(dest, *e.path.split('/'))
            if e.is_dir:
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
            with open(out, 'wb') as f:
                f.write(self.read(e))
            n += 1
        return n


def _is_header(rom, a):
    if a + 0x80 > len(rom) or be32(rom, a + 0x10) != 0xEF000011:
        return False
    w0, w3 = be32(rom, a), be32(rom, a + 0x0c)
    return (w0 == aif.NOP or w0 >> 24 == 0xEB) and w3 >> 24 == 0xEB


def programs(rom, volume=None):
    """Every AIF image in the ROM, in order: (offset, the volume path or '',
    its AIF header as read)."""
    names = {}
    if volume is not None:
        for e in volume.files():
            names[volume.where(e)] = e.path
    out = []
    for a in range(0, len(rom) - 0x80, 4):
        if _is_header(rom, a):
            out.append((a, names.get(a, ''), aif.AIF(rom[a:])))
    return out


def unpack(rom, offset, max_steps=50_000_000):
    """(base, bytes): the image at `offset`, unpacked by its own decompressor
    when it is compressed."""
    end = len(rom)
    return aif._unpack(rom[offset:end], max_steps)


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m 3dokit.rom', description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('--extract', metavar='DIR', help="the volume's files")
    ap.add_argument('--unpack', metavar='DIR', help='every program, unpacked, as OFFSET_BASE.bin')
    a = ap.parse_args(argv)
    rom = open(a.rom, 'rb').read()
    vol = RomVolume(rom)
    if a.extract:
        print('%d files to %s' % (vol.extract(a.extract), a.extract))
        return 0
    progs = programs(rom, vol)
    if a.unpack:
        os.makedirs(a.unpack, exist_ok=True)
    print('%s: %d bytes; volume %r at %#x, blocks of %d, %d files' % (
        a.rom, len(rom), vol.label, vol.at, vol.block_size, len(vol.files())))
    for off, path, im in progs:
        line = '  %06x  %-10s base %06x  ro %6x rw %5x bss %5x' % (
            off, 'compressed' if im.compressed else 'plain', im.base, im.ro, im.rw, im.bss)
        name = path
        if a.unpack or not path:
            try:
                base, data = unpack(rom, off)
                u = aif.AIF(data)
                name = path or u.name or '?'
                if a.unpack:
                    with open(os.path.join(a.unpack, '%06x_%06x.bin' % (off, base)), 'wb') as f:
                        f.write(data[:u.ro + u.rw])
            except (RuntimeError, ValueError) as err:
                name = path or '(%s)' % err
        print(line + '  ' + name)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
