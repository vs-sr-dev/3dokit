"""Recompile 3DO programs (AIF images) to C++, one module per program.

    python -m 3dokit.recomp --out build/recomp NAME=FILE[+SEED,...] ... [--optest]
                            [--per-file 6000] [--no-comments]

Each program is discovered (`recomp.discover`) and emitted (`recomp.emit`)
into its own namespace, p_<name>. Every 3DO program is linked at 0, so one
module is active at a time; the runtime recognises a program in memory by
the crc32 of its read-only area. Writes, into --out:

    p_<name>_funcs.h        prototypes and the module descriptor
    p_<name>_NNN.cpp        the functions, split by instruction count
    p_<name>_table.cpp      the module: its read-only area's size and crc32, its entries
    modules.cpp             every module of the build (g_arm_modules)
    pf_names.cpp            the SDK's names for SWIs and folio slots (3dokit.sdk)
    CMakeLists.txt          the library `recomp`, the runtime, the self-test
    report.txt              per module: functions, instructions, transfers by
                            kind, static targets that are no entry

--optest adds 3dokit's own instruction test (`recomp.selftest`): a
synthetic program with ARMv3's instruction forms, as module OPTEST, and
its vectors in --out/selftest/optest.txt.
"""
import argparse
import os
import time
import zlib

from ..aif import AIF
from . import discover
from . import emit as E

RUNTIME = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', 'runtime'))


class Out:
    """A generated file, written only if its content changed: a rebuild after
    a small change recompiles only what it touched."""

    def __init__(self, path):
        self.path, self.parts = path, []

    def write(self, s):
        self.parts.append(s)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        text = ''.join(self.parts)
        try:
            with open(self.path, encoding='utf-8', newline='') as f:
                if f.read() == text:
                    return
        except OSError:
            pass
        with open(self.path, 'w', encoding='utf-8', newline='') as f:
            f.write(text)


def parse_spec(spec):
    """NAME=FILE[+SEED,SEED...] -> (name, path, seeds)."""
    name, _, rest = spec.partition('=')
    path, plus, seeds = rest.rpartition('+')
    if not plus or os.path.exists(rest):
        path, seeds = rest, ''
    return name, path, [int(s, 16) for s in seeds.split(',') if s]


class Module:
    def __init__(self, name, path, seeds=()):
        self.name, self.path = name, path
        self.ns = 'p_' + name.lower()
        self.prog = discover.Program(path, seeds)
        self.aif = self.prog.aif
        self.size = self.aif.ro
        self.crc = zlib.crc32(self.aif.d[:self.size])
        self.entries = sorted(self.prog.funcs)
        self.files, self.sites, self.unknown = [], {}, []
        self.n_ins = 0

    def generate(self, out, per_file, comments):
        prog, entries = self.prog, set(self.entries)
        buf, count, idx = [], 0, 0

        def flush():
            nonlocal buf, count, idx
            if not buf:
                return
            name = '%s_%03d.cpp' % (self.ns, idx)
            with Out(os.path.join(out, name)) as f:
                f.write('#include "%s_funcs.h"\n\nnamespace %s {\n\n' % (self.ns, self.ns))
                f.write('\n\n'.join(buf))
                f.write('\n\n}  // namespace %s\n' % self.ns)
            self.files.append(name)
            buf, count, idx = [], 0, idx + 1

        for e in self.entries:
            fn = prog.funcs[e]
            body = E.Body(prog, fn, entries, comments)
            lines = body.emit()
            if fn.name:
                lines.insert(0, '// %s' % fn.name)
            buf.append('\n'.join(lines))
            for k, v in body.sites.items():
                self.sites[k] = self.sites.get(k, 0) + v
            self.unknown += [(t, e) for t in body.unknown]
            count += len(fn.code)
            self.n_ins += len(fn.code)
            if count >= per_file:
                flush()
        flush()

        with Out(os.path.join(out, self.ns + '_funcs.h')) as f:
            f.write('#pragma once\n#include "arm60.h"\n\nnamespace %s {\n' % self.ns)
            for e in self.entries:
                f.write('void %s(ArmCpu& c);\n' % E.fname(e))
            f.write('extern const ArmModule module;\n}  // namespace %s\n' % self.ns)
        with Out(os.path.join(out, self.ns + '_table.cpp')) as f:
            f.write('#include "%s_funcs.h"\n\nnamespace %s {\n\n' % (self.ns, self.ns))
            f.write('static const ArmFuncEntry funcs[] = {\n')
            for e in self.entries:
                f.write('    {0x%08Xu, %s},\n' % (e, E.fname(e)))
            f.write('};\n\n')
            f.write('extern const ArmModule module = {"%s", 0u, %du, 0x%08Xu, funcs, %d};\n'
                    % (self.name, self.size, self.crc, len(self.entries)))
            f.write('\n}  // namespace %s\n' % self.ns)
        self.files.append(self.ns + '_table.cpp')


CMAKE = """cmake_minimum_required(VERSION 3.20)
project(tdk_recomp CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(TDK_RUNTIME "{rt}")
add_library(recomp STATIC
    {srcs})
target_include_directories(recomp PUBLIC ${{TDK_RUNTIME}})
target_compile_options(recomp PRIVATE -Wno-unused-label)
include(${{TDK_RUNTIME}}/runtime.cmake)
add_executable(selftest ${{TDK_RUNTIME}}/arm_selftest.cpp)
target_link_libraries(selftest tdk_arm_core tdk_arm_stub recomp)
# a program on the Portfolio runtime, its OS calls traced
add_executable(pfboot ${{TDK_RUNTIME}}/pf_main.cpp pf_names.cpp)
target_link_libraries(pfboot tdk_arm_core tdk_pf recomp)
# a port adds its own targets with -DTDK_EXTRA=file.cmake
if(DEFINED TDK_EXTRA)
  include(${{TDK_EXTRA}})
endif()
"""


def write_names(out):
    """pf_names.cpp: 3dokit.sdk's tables for the runtime's trace."""
    from ..sdk import SWIS, SLOTS
    with Out(os.path.join(out, 'pf_names.cpp')) as f:
        f.write('#include "pf.h"\n\n// generated by python -m 3dokit.recomp from 3dokit.sdk\n')
        f.write('extern const PfSwiName g_pf_swi_names[] = {\n')
        for n in sorted(SWIS):
            f.write('    {0x%05Xu, "%s"},\n' % (n, SWIS[n]))
        f.write('};\nextern const int g_pf_nswi_names = %d;\n\n' % len(SWIS))
        f.write('extern const PfSlotName g_pf_slot_names[] = {\n')
        for (folio, slot) in sorted(SLOTS):
            f.write('    {"%s", %d, "%s"},\n' % (folio, slot, SLOTS[(folio, slot)]))
        f.write('};\nextern const int g_pf_nslot_names = %d;\n' % len(SLOTS))


def generate(specs, out, per_file=6000, comments=True, optest=False, log=print):
    t0 = time.time()
    os.makedirs(out, exist_ok=True)
    if optest:
        from . import selftest
        path, entries = selftest.write_optest(os.path.join(out, 'selftest'))
        specs = list(specs) + [('OPTEST', path, entries)]
    modules = []
    for name, path, seeds in specs:
        t = time.time()
        m = Module(name, path, seeds)
        m.generate(out, per_file, comments)
        modules.append(m)
        log('%-9s %5d functions %8d instructions %3d files  %.1f s'
            % (name, len(m.entries), m.n_ins, len(m.files), time.time() - t))
    with Out(os.path.join(out, 'modules.cpp')) as f:
        for m in modules:
            f.write('#include "%s_funcs.h"\n' % m.ns)
        f.write('\nextern const ArmModule* const g_arm_modules[] = {\n')
        for m in modules:
            f.write('    &%s::module,\n' % m.ns)
        f.write('};\nextern const int g_arm_nmodules = %d;\n' % len(modules))
    srcs = [x for m in modules for x in m.files] + ['modules.cpp']
    write_names(out)
    with Out(os.path.join(out, 'CMakeLists.txt')) as f:
        f.write(CMAKE.format(rt=RUNTIME.replace('\\', '/'), srcs='\n    '.join(srcs)))
    kinds = ['call', 'return', 'swi', 'switch', 'indirect call', 'indirect jump']
    rows = ['%-9s %6s %8s  %s' % ('module', 'funcs', 'insns', ' '.join('%13s' % k for k in kinds))]
    for m in modules:
        rows.append('%-9s %6d %8d  %s' % (m.name, len(m.entries), m.n_ins,
                                          ' '.join('%13d' % m.sites.get(k, 0) for k in kinds)))
    tot = ('total: %d modules, %d functions, %d instructions, %d files, %.1f s'
           % (len(modules), sum(len(m.entries) for m in modules), sum(m.n_ins for m in modules),
              len(srcs), time.time() - t0))
    with open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8') as f:
        f.write('transfers by kind, as emitted (indirect ones go through arm_call at run time):\n')
        f.write('\n'.join(rows) + '\n' + tot + '\n\n')
        for m in modules:
            seen = set()
            for t, e in sorted(m.unknown):
                if t not in seen:
                    seen.add(t)
                    f.write('%s: target %08X (from %08X) is not an entry\n' % (m.name, t, e))
    log(tot)
    return modules


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m 3dokit.recomp', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('programs', nargs='*', help='NAME=FILE[+SEED,...]')
    ap.add_argument('--out', required=True)
    ap.add_argument('--per-file', type=int, default=6000)
    ap.add_argument('--no-comments', action='store_true')
    ap.add_argument('--optest', action='store_true', help="add 3dokit's instruction test module")
    a = ap.parse_args(argv)
    generate([parse_spec(s) for s in a.programs], a.out, a.per_file, not a.no_comments, a.optest)


if __name__ == '__main__':
    main()
