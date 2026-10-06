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

    python -m 3dokit.pfcheck OS_CODE DIR --graphix GRAPHIX

DIR is what `pfboot PROGRAM --snap N DIR` wrote: the memory before and after
one call of the Graphics folio (the N-th OS call; 0 is the folio's start of
its system VDLs) and the call (call.txt). Here the call is made by the
folio's own code -- `System/Folios/GRAPHIX`, unpacked and relocated to
0x700000 by its own relocations -- over the memory before, and its result
and the memory after must be the runtime's. What the folio asks of the
kernel runs as the kernel's own code (`AllocMemFromMemLists`, `InitList`,
`CheckItem`, the 1993 slot -168 that checks a task may write some memory),
except what the runtime does its own way, which a stand-in here does the
runtime's way: items (created as `pf_item_new` makes them, numbered on from
the runtime's, their nodes and `InitList`'s names taken from where the
runtime's next own allocation would be; looked up in the runtime's table),
and `IsItemOpened` (yes). An error path that deletes an item stops the
check. Only the GRAPHIX of Crash 'n Burn's disc (16 August 1993) is known.
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
    'InitList': 0x11cd4, 'LookupItem': 0x12b40, 'CheckItem': 0x12bf8,
    'ValidateMem': 0x1617c,              # (Task*, p, size): slot -168 in 1993
    # the vector table ends at 0x1beac (slot -4); the SWI table is reversed from 0x1bddc
    'vectors': {-28: 0x15688, -32: 0x15370, -36: 0x11cd4, -44: 0x157c4, -48: 0x12b40,
                -64: 0x12bf8, -100: 0x15e4c, -168: 0x1617c},
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


# ---- one call of the Graphics folio ------------------------------------------------------

GRAPHIX_AT = 0x700000                    # where the folio is loaded here: no guest memory
GRAF_BASE = OS_BASE + 0x2000             # the runtime's GrafBase (pf_folio_base)

# GRAPHIX of 16 August 1993: the words its code reads KernelBase and GrafBase from, its
# tables, and the glue by which its functions reach the kernel's (the folio calls them
# through KernelBase's tables; here the glue runs the kernel's function directly, or a
# stand-in). Each is checked against the folio's own words before anything runs.
G1993 = {
    'kernelbase': 0x5180, 'grafbase': 0x50ac,
    'vectors_end': 0x5430,               # slot -4 at 0x542c
    'swis': 0x52a0, 'nswis': 51,         # SWI n at swis + 4 * (50 - n)
    'nodedb': 0x5430,                    # NodeData, 4 bytes a node type
    'stack_extend': 0x13c,
    # glue address: (what, its distinctive word and where)
    'glue': {0x168: ('CreateSizedItem', 0x168, 0xe3a0c000),
             0x178: ('DeleteItem', 0x178, 0xe3a0c003),
             0x240: ('ValidateMem', 0x240, 0xe3e0c00a),
             0x2bc: ('AllocMemFromMemLists', 0x2cc, 0xe519f01c),
             0x2ec: ('CheckItem', 0x2fc, 0xe519f040),
             0x304: ('LookupItem', 0x314, 0xe519f030),
             0x40c: ('IsItemOpened', 0x41c, 0xe519f080),
             0x4dfc: ('InitList', 0x4e04, 0xe512f024)},
    'check': {0x5400: 0x3e44, 0x52a0: 0x27a0,    # vector -48 and SWI 50, unrelocated
              0x41c8: 0xe3a094e1},               # in the system's VDLs' builder
    'inits': {'vdls': 0x41b4},           # steps of the folio's start (pfboot --snap 0)
}
CLIO = 0x3300000                         # the hardware the folio's start writes, not compared


def r32(mem, a):
    return mem.read(a, 4)


def r8(mem, a):
    return mem.read(a, 1)


def w32(mem, a, v):
    mem.write(a, 4, v)


def w8(mem, a, v):
    mem.write(a, 1, v)


def load_graphix(path):
    with open(path, 'rb') as f:
        data = f.read()
    img = aif.relocated(data, GRAPHIX_AT)
    raw = aif.relocated(data, 0)
    word = lambda a: struct.unpack_from('>I', raw, a)[0]
    g = G1993
    for a, v in list(g['check'].items()) + [(at, w) for _, at, w in g['glue'].values()]:
        if word(a) != v:
            raise KernelError('%s: the word at %#x is not %#x: not the 1993 GRAPHIX' % (path, a, v))
    return img


class GraphicsCall:
    """One Graphics call of a snapshot (pfboot --snap) on the folio's own code."""

    def __init__(self, kern, graphix, d):
        self.kern, self.d = kern, d
        self.items, self.call = {}, None
        with open(os.path.join(d, 'call.txt')) as f:
            for line in f:
                w = line.split()
                if w[0] == 'call':
                    self.call = w[1:]
                elif w[0] == 'regs':
                    self.regs = [int(x, 16) for x in w[1:]]
                elif w[0] == 'task':
                    self.task_item = int(w[2])
                elif w[0] == 'osnext':
                    self.os_next = int(w[1], 16)
                elif w[0] == 'item':
                    self.items[int(w[1])] = int(w[2], 16)
                elif w[0] == 'result':
                    self.want = int(w[1], 16)
        mem, cpu, g = kern.mem, kern.cpu, G1993
        mem.add(GRAPHIX_AT, (len(graphix) + 0xfff) & ~0xfff, graphix)
        mem.add(CLIO, 0x1000)
        w32(mem, GRAPHIX_AT + g['kernelbase'], KERNEL_BASE)
        w32(mem, GRAPHIX_AT + g['grafbase'], GRAF_BASE)
        self.next_item = max(self.items) + 1
        k = V016

        def ret(c, value):
            c.r[0] = value & 0xFFFFFFFF
            c.r[armemu.PC] = c.r[armemu.LR] & ~3

        def kernel(fn, n):
            return lambda c: ret(c, kern.call(fn, c.r[0:n], kern.user[-1]))

        def refuse(why):
            def trap(c):
                raise KernelError(why)
            return trap

        G = GRAPHIX_AT
        cpu.traps[k['LookupItem']] = lambda c: ret(c, self.items.get(c.r[0], 0))
        cpu.traps[G + 0x168] = lambda c: ret(c, self.create_item(c.r[0]))
        cpu.traps[G + 0x178] = refuse('DeleteItem: an error path, not checked here')
        cpu.traps[G + 0x240] = kernel(k['ValidateMem'], 3)
        cpu.traps[G + 0x2bc] = kernel(k['AllocMemFromMemLists'], 3)
        cpu.traps[G + 0x2ec] = kernel(k['CheckItem'], 3)
        cpu.traps[G + 0x304] = kernel(k['LookupItem'], 1)
        cpu.traps[G + 0x40c] = lambda c: ret(c, 0)
        cpu.traps[G + 0x4dfc] = lambda c: ret(c, kern.call(
            k['InitList'], [c.r[0], self.os_string(c.r[1])], kern.user[-1]))
        cpu.traps[G + g['stack_extend']] = refuse('the folio ran out of stack')
        kernel_swi = cpu.on_swi

        def on_swi(c, number):
            n = number - 0x20000
            if 0 <= n < g['nswis']:
                fn = r32(mem, G + g['swis'] + 4 * (g['nswis'] - 1 - n))
                c.r[0] = kern.call(fn, c.r[0:4], user=False)
            else:
                kernel_swi(c, number)

        cpu.on_swi = on_swi

    # The stand-ins for what the runtime does its own way, the same way it does: the OS's own
    # allocations upward from where the runtime's next one would be, and items numbered on.
    def os_alloc(self, size):
        a = self.os_next
        self.os_next = (a + size + 3) & ~3
        return a

    def os_string(self, p):
        s = bytearray()
        while True:
            b = r8(self.kern.mem, p + len(s))
            s.append(b)
            if not b:
                break
        a = self.os_alloc(len(s))
        for i, b in enumerate(s):
            w8(self.kern.mem, a + i, b)
        return a

    def create_item(self, ctype):
        """CreateSizedItem of a node of the folio's: the node database's size, the runtime's
        ItemNode (pf_item_new), the current task its owner."""
        mem = self.kern.mem
        size = r8(mem, GRAPHIX_AT + G1993['nodedb'] + 4 * (ctype & 0xff))
        n = self.os_alloc(size)
        w32(mem, n + 12, size)
        w8(mem, n + 8, ctype >> 8 & 0xff)
        w8(mem, n + 9, ctype & 0xff)
        w8(mem, n + 11, 0x10)
        w32(mem, n + 16, 0)
        item = self.next_item
        self.next_item += 1
        self.items[item] = n
        w32(mem, n + 24, item)
        w32(mem, n + 28, self.task_item)
        return item

    def run(self):
        """(the folio's result, the runtime's)"""
        what = self.call
        if what[:2] == ['init', 'Graphics'] and what[2] in G1993['inits']:
            fn, user = GRAPHIX_AT + G1993['inits'][what[2]], False
        elif what[:2] == ['slot', 'Graphics']:
            fn, user = r32(self.kern.mem, GRAPHIX_AT + G1993['vectors_end'] + int(what[2])), True
        else:
            raise KernelError('call.txt: %s is not a Graphics call' % ' '.join(what))
        return self.kern.call(fn, self.regs[0:4], user=user), self.want


def run_call(os_code, graphix_path, d, out=sys.stdout):
    base, img = load_kernel(os_code)
    graphix = load_graphix(graphix_path)
    with open(os.path.join(d, 'before.bin'), 'rb') as f:
        before = f.read()
    with open(os.path.join(d, 'after.bin'), 'rb') as f:
        after = f.read()
    kern = Kernel(base, img, before)
    call = GraphicsCall(kern, graphix, d)
    try:
        got, want = call.run()
    except (KernelError, armemu.MemoryError_, armemu.Unpredictable) as e:
        print('%s: the folio broke: %s' % (' '.join(call.call), e), file=out)
        return False
    mine = kern.image()
    lo, hi = kern.kernel_range
    diffs = [a for a in range(len(after))
             if after[a] != mine[a] and not lo <= a < hi]
    print('%s: the folio returns %08X, the runtime %08X; memory after: %d bytes differ'
          % (' '.join(call.call), got, want, len(diffs)), file=out)
    for a in diffs[:16]:
        addr = a if a < DRAM_VRAM else OS_BASE + a - DRAM_VRAM
        print('  %08X  runtime %02X  folio %02X' % (addr, after[a], mine[a]), file=out)
    return got == want and not diffs


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='python -m 3dokit.pfcheck', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('os_code', help='System/Kernel/os_code of the disc')
    ap.add_argument('dirs', nargs='+', help="what pfboot --memtest or --snap wrote")
    ap.add_argument('--graphix', help='System/Folios/GRAPHIX of the disc, for a snapshot')
    a = ap.parse_args(argv)
    ok = True
    for d in a.dirs:
        print('%s:' % d)
        if os.path.exists(os.path.join(d, 'call.txt')):
            if not a.graphix:
                ap.error('%s is a snapshot of a call: --graphix is needed' % d)
            ok &= run_call(a.os_code, a.graphix, d)
        else:
            ok &= run(a.os_code, d)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
