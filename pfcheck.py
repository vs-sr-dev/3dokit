"""The runtime's Portfolio against the 1993 OS's own code.

    python -m 3dokit.pfcheck OS_CODE DIR

DIR is what `pfboot PROGRAM --memtest DIR` wrote: the guest memory before
and after a random run of memory calls (before.bin, after.bin: DRAM and VRAM
from 0, then the OS's memory from 0x400000) and the run itself (ops.txt).
Here the same run is made by the kernel's own functions -- the System's
`os_code`, decompressed by its own code, run in `armemu` over the memory
before -- and every result and the memory after must be the runtime's, byte
for byte.

The kernel's functions run as they would on the console, with these
stand-ins, none of which touches the memory compared:

* the test of the processor's mode (user or supervisor) answers what the
  run's line says, and a SWI's handler runs in supervisor mode;
* `AllocMemBlocks`, `ControlMem` and `SystemScavengeMem`, called by SWI, are
  the kernel's own functions for them, called directly;
* the memory protection's updates are skipped (no MMU here), and a kernel
  error (its `Fail`) stops the check.

Only the kernel of Crash 'n Burn's disc (v0.16, 1993) is known: its
functions' addresses are below, checked against its own vector and SWI
tables before anything runs.
"""
import argparse
import os
import struct
import sys

from . import aif, armemu

DRAM_VRAM = 0x300000
OS_BASE, OS_SIZE = 0x400000, 0x100000
STACK_BASE, STACK_SIZE = 0x600000, 0x10000
KERNEL_BASE = 0x401000                   # the runtime's KernelBase (pf_folio_base)

# os_code v0.16: where its functions are, and where its tables say they are
V016 = {
    'base': 0x10000,
    'AllocMemFromMemLists': 0x15688, 'FreeMemToMemLists': 0x15370,
    'ScavengeMem': 0x157c4, 'SystemScavengeMem': 0x157c0,
    'AllocMemBlocks': 0x15d5c, 'ControlMem': 0x16538,
    'is_user': 0x15a40, 'mmu_task': 0x1a974, 'mmu_page': 0x145fc,
    'Fail': 0x19518, 'stack_extend': 0x10758,
    # the vector table ends at 0x1beac (slot -4); the SWI table is reversed from 0x1bddc
    'vectors': {-28: 0x15688, -32: 0x15370, -44: 0x157c4, -100: 0x15e4c},
    'swis': {13: 0x15d5c, 20: 0x16538, 33: 0x157c0},
}


class KernelError(Exception):
    pass


def load_kernel(path):
    with open(path, 'rb') as f:
        base, img = aif.decompress(f.read())
    k = V016
    if base != k['base']:
        raise KernelError('%s: linked at %#x, not a known kernel' % (path, base))
    word = lambda a: struct.unpack_from('>I', img, a - base)[0]
    for slot, fn in k['vectors'].items():
        if word(0x1beac + 4 + slot) != fn:
            raise KernelError('%s: vector %d is not at %#x: not os_code v0.16'
                              % (path, slot, fn))
    for n, fn in k['swis'].items():
        if word(0x1bddc - 4 * n) != fn:
            raise KernelError('%s: SWI %d is not at %#x: not os_code v0.16'
                              % (path, n, fn))
    return base, img


class Kernel:
    """The kernel's functions over a guest memory, called one at a time."""

    def __init__(self, base, img, before):
        mem = self.mem = armemu.Memory()
        mem.add(0, DRAM_VRAM, before[:DRAM_VRAM])
        buf = mem.regions[0][2]
        buf[base:base + len(img)] = img      # in the program's pages: never handed out
        self.kernel_range = (base, base + len(img) + 0x1000)
        mem.add(OS_BASE, OS_SIZE, before[DRAM_VRAM:DRAM_VRAM + OS_SIZE])
        mem.add(STACK_BASE, STACK_SIZE)
        cpu = self.cpu = armemu.CPU(mem)
        k = V016
        self.user = [True]                   # the mode, innermost call last

        def ret(c, value=None):
            if value is not None:
                c.r[0] = value
            c.r[armemu.PC] = c.r[armemu.LR] & ~3

        cpu.traps[k['is_user']] = lambda c: ret(c, 1 if self.user[-1] else 0)
        cpu.traps[k['mmu_task']] = lambda c: ret(c)
        cpu.traps[k['mmu_page']] = lambda c: ret(c)

        def fail(c):
            raise KernelError('the kernel failed (Fail called from %08X)' % c.r[armemu.LR])

        def stack(c):
            raise KernelError('the kernel ran out of stack')

        cpu.traps[k['Fail']] = fail
        cpu.traps[k['stack_extend']] = stack
        swis = {0x1000d: k['AllocMemBlocks'], 0x10014: k['ControlMem'],
                0x10021: k['SystemScavengeMem']}

        def on_swi(c, number):
            fn = swis.get(number)
            if fn is None:
                raise KernelError('SWI %#x at %08X' % (number, c.r[armemu.PC] - 4))
            c.r[0] = self.call(fn, c.r[0:4], user=False)

        cpu.on_swi = on_swi

    def call(self, fn, args, user):
        cpu = self.cpu
        saved = (list(cpu.r), cpu.cpsr)
        sp = saved[0][armemu.SP] if len(self.user) > 1 else STACK_BASE + STACK_SIZE
        self.user.append(user)
        try:
            for i, v in enumerate(args):
                cpu.r[i] = v & 0xFFFFFFFF
            cpu.r[9] = KERNEL_BASE           # sb: the kernel's code reads KernelBase through it
            cpu.r[10] = STACK_BASE           # sl: never under it
            cpu.r[11] = 0
            cpu.r[armemu.SP] = (sp - 64) & ~7
            cpu.r[armemu.LR] = armemu.RETURN_SENTINEL
            cpu.r[armemu.PC] = fn
            while cpu.r[armemu.PC] != armemu.RETURN_SENTINEL:
                cpu.step()
            result = cpu.r[0]
        finally:
            self.user.pop()
            regs, flags = saved
            cpu.r[:] = regs
            cpu.cpsr = flags
        return result

    def image(self):
        return bytes(self.mem.regions[0][2]) + bytes(self.mem.regions[1][2])


def run(os_code, d, out=sys.stdout):
    base, img = load_kernel(os_code)
    with open(os.path.join(d, 'before.bin'), 'rb') as f:
        before = f.read()
    with open(os.path.join(d, 'after.bin'), 'rb') as f:
        after = f.read()
    kern = Kernel(base, img, before)
    k = V016
    n = bad = 0
    with open(os.path.join(d, 'ops.txt')) as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            n += 1
            try:
                got, want = replay(kern, w, line)
            except (KernelError, armemu.MemoryError_, armemu.Unpredictable) as e:
                print('op %d: %s  -- the kernel broke: %s (the states had parted)'
                      % (n, line.strip(), e), file=out)
                return False
            if got != want:
                bad += 1
                if bad <= 10:
                    print('op %d: %s  -- the kernel: %08X' % (n, line.strip(), got), file=out)
    mine = kern.image()
    lo, hi = kern.kernel_range
    diffs = [a for a in range(len(after))
             if after[a] != mine[a] and not lo <= a < hi]
    print('%d operations, %d results differ; memory after: %d bytes differ'
          % (n, bad, len(diffs)), file=out)
    for a in diffs[:16]:
        addr = a if a < DRAM_VRAM else OS_BASE + a - DRAM_VRAM
        print('  %08X  runtime %02X  kernel %02X' % (addr, after[a], mine[a]), file=out)
    return bad == 0 and not diffs


def replay(kern, w, line):
    """One line of ops.txt on the kernel: (its result, the runtime's)."""
    k = V016
    user = w[1] == 'U'
    if w[0] == 'alloc':
        lists, size, flags, want = int(w[2], 16), int(w[3]), int(w[4], 16), int(w[6], 16)
        return kern.call(k['AllocMemFromMemLists'], [lists, size, flags], user), want
    if w[0] == 'free':
        lists, p, size = int(w[2], 16), int(w[3], 16), int(w[4])
        kern.call(k['FreeMemToMemLists'], [lists, p, size], user)
        return 0, 0
    if w[0] == 'scavenge':
        return kern.call(k['ScavengeMem'], [], user), int(w[3])
    raise ValueError('ops.txt: %r' % line)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.pfcheck', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('os_code', help='System/Kernel/os_code of the disc')
    ap.add_argument('dirs', nargs='+', help="what pfboot --memtest wrote")
    a = ap.parse_args(argv)
    ok = True
    for d in a.dirs:
        print('%s:' % d)
        ok &= run(a.os_code, d)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
