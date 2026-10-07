# 3dokit

A game-agnostic toolkit for Panasonic 3DO reverse engineering and native PC
ports: disc images and the Opera filesystem, the executables and the ARM60
code in them, the Portfolio OS surface a program touches, the CEL engine's
pixel formats, the DataStream with its Cinepak films and SDX2 sound, AIFF
samples, the DSP instrument library, and a C99 runtime that reads all of it
for an engine.

The same idea as [wiikit](https://github.com/vs-sr-dev/wiikit),
[saturnkit](https://github.com/vs-sr-dev/saturnkit),
[ps2kit](https://github.com/vs-sr-dev/pc-extermination/tree/main/ps2kit)
and jaguarkit, for the 3DO. Each 3DO game has its own engine and formats,
but a large part of every port is the *same* work: the same Opera disc, the
same AIF executables loaded by the same Portfolio, the same CEL engine
drawing the same cels, and very often the same SDK libraries -- the
DataStream, Cinepak, the sound spooler. 3dokit collects that shared part. It
grows inside the ports: each piece is written because a game needed it,
then kept free of that game's knowledge. Game formats and game fixes live
in the ports.

## Ports built on it

| Port | Game | What it asked of 3dokit |
|---|---|---|
| [pc-immercenary](https://github.com/vs-sr-dev/pc-immercenary) | Immercenary (1995, 3DO) | everything so far: the disc, the AIF and binary headers, the ARM cross-referencer, the OS surface, library proofs and the pairing of its two executables, the cels, the DataStream and its Cinepak and SDX2, the AIFF samples, the DSP instruments, and the C runtime |

Written from Immercenary's tools and checked on a second disc, the
OMF2097 port's own ISO: a different mastering tool (3doiso), a later OS
(Portfolio 24.225 against 23.10), a modern toolchain (the 3do-devkit), and
cels written by a different converter (3it). Where the two disagreed, the
kit took the general form -- see *Checks* below. Where the discs could not
settle a hardware question, the Opera emulator's MADAM was the reference.

## Using it

3dokit is its own repository. A port takes it as a git submodule at
`3dokit/`:

```sh
git submodule add <3dokit's URL> 3dokit
git clone --recursive <a port's URL>       # or: git submodule update --init
```

It sits at the port's root, so that `python -m 3dokit.…` works
from there and an engine can compile `3dokit/runtime/*.c`. The package name
starts with a digit, so Python code imports it with
`importlib.import_module('3dokit.cel')`.

```sh
python -m 3dokit.disc GAME.cue                        # volume, ROM tags, size
python -m 3dokit.disc GAME.img --list                 # every file and its copies
python -m 3dokit.disc GAME.img --extract build/disc   # every file
python -m 3dokit.disc GAME.img --verify               # every copy, compared
python -m 3dokit.rom BIOS.bin [--unpack DIR]          # a console ROM: its volume, its programs
python -m 3dokit.aif --scan build/disc                # every executable
python -m 3dokit.aif build/disc/LaunchMe              # headers, relocations
python -m 3dokit.arm GAME -s 'regex'                  # who references a string
python -m 3dokit.arm GAME -c 3b118                    # who calls what
python -m 3dokit.arm GAME -S game.sym -d fe30 -n 60   # disassembly, with names
python -m 3dokit.arm GAME --stats                     # the call graph's reach
python -m 3dokit.arm GAME --names > game.sym          # the compiler's embedded names
python -m 3dokit.portfolio GAME --sites               # SWIs and folio vectors, named
python -m 3dokit.sdk                                  # the SDK's SWI and slot names
python -m 3dokit.aof SDK/lib/3do/graphics.lib         # an SDK library's members
python -m 3dokit.aof SDK/lib/3do/*.lib --glue         # folio slots its glue names
python -m 3dokit.arm60 GAME --dis 100 -n 40         # ARMv3, decoded exactly
python -m 3dokit.arm60 GAME --check                 # ...against capstone
python -m 3dokit.recomp.discover GAME --report      # functions, code and data
python -m 3dokit.armemu --test                      # the ARM60 interpreter's known answers
python -m 3dokit.armemu --check                     # ...and random words against unicorn
python -m 3dokit.armemu GAME --call 17418 --regs r0=100,r1=100   # run one function
python -m 3dokit.recomp --out build/recomp GAME=GAME --optest   # C++, one module per program
python -m 3dokit.recomp.selftest --image GAME=GAME --auto --out build/recomp/selftest/game.txt
cmake -S build/recomp -B build/recomp-build -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build/recomp-build && build/recomp-build/selftest build/recomp/selftest/*.txt
build/recomp-build/pfboot GAME [--trace 2] [--lenient]   # on the Portfolio runtime, OS calls traced
build/recomp-build/pfboot GAME --disc DIR                # the disc's files from DIR (default: GAME's directory)
build/recomp-build/pfboot GAME --memtest DIR --ops 4000  # the allocator, a random run written down
python -m 3dokit.pfcheck DISC/System/Kernel/os_code DIR  # ...replayed on the 1993 kernel's own code
build/recomp-build/pfboot GAME --snap N DIR              # the memory before and after the N-th OS call
build/recomp-build/pfboot GAME --frames DIR              # what the display shows, a PPM per change
build/recomp-build/pfboot GAME --frames DIR --frames-at 39000-41000/10   # ...of those fields only
build/recomp-build/pfboot GAME --pad a@1300x1            # the first pad's A at field 1300 (BUTTONS@FIELD[xN][/E][+H])
build/recomp-build/pfboot GAME --pad a@7600+600          # ...or held, from field 7600 for 600 fields
build/recomp-build/pfboot GAME --trace 0 --window --record pad.txt   # in a window (SDL3), real time, keyboard/gamepad as the pad
python -m 3dokit.pfcheck DISC/System/Kernel/os_code DIR --graphix DISC/System/Folios/GRAPHIX   # ...on the folio's
python -m 3dokit.aif --decompress DISC/System/Folios/GRAPHIX graphix.bin   # by its own decompressor
python -m 3dokit.shapes library GAME --corpus 'build/disc/System/Programs/*'
python -m 3dokit.shapes pair A B --names a.sym --out b.sym
python -m 3dokit.cel FILE --png out/                  # frames as RGBA PNG
python -m 3dokit.cel --survey build/disc              # which encodings a disc uses
python -m 3dokit.cel --check build/disc               # decode every cel
python -m 3dokit.stream --scan build/disc             # every DataStream
python -m 3dokit.stream FILM --frames out/ --wav out.wav
python -m 3dokit.audio --scan build/disc              # every AIFF, loops
python -m 3dokit.dsp build/disc/System/Audio/dsp --verify
python -m 3dokit.dsp build/disc/System/Audio/dsp --used GAME
python -m 3dokit.check GAME.img --frames 8            # the runtime's check, in Python
cc -O2 -std=c99 -o tdkcheck 3dokit/runtime/*.c
tdkcheck GAME.img --frames 8                          # ...and in C: diff the two
```

## What a 3DO disc is

Worth writing down in one place, because every port starts here. Checked on
both discs:

* **One data track, Mode 1**, and no ISO 9660: block 0 is an **Opera volume
  header** (`01 ZZZZZ`, a label, the block count, the root directory and
  every copy of it). A directory is a run of consecutive blocks whose
  internal `next`/`prev` links are indices *inside the run*; following them
  as addresses loses subtrees. Names match without case.
* **Everything is stored more than once**, for seek time. The copies of a
  directory need not spell its names the same way (Immercenary's
  `System/Drivers` says `CPORT1.ROM` in one and `cport1.rom` in the others).
* **`rom_tags`, in block 1**, is what the boot ROM reads before any
  filesystem: 32-byte records pointing at `System/Kernel/boot_code`,
  `os_code` (whose version is the OS release: 23.10, 24.225), `misc_code`,
  the `BannerScreen` and `LaunchMe`, each offset counted **from the block of
  the copy that holds it** -- the second copy stores -224 where the first
  stores 1. `signatures` (335,872 bytes on both discs) is the disc's
  signature data.
* **`AppStartup`** is a shell script of aliases (`alias Perfect
  $boot/Perfect`); the shell then runs **`LaunchMe`**.
* **Every executable is a big-endian AIF image** linked at 0, with a
  128-byte **3DO binary header** at 0x80 (node type, OS version, stack,
  name, build time, signature) and a relocation list after the data. The
  System tree's folios, tasks and drivers are mostly *compressed* AIF; a
  game's programs are not. Privileged images carry a 64-byte signature after
  the relocations.
* **The OS is reached by `swi` and by folio vectors**: a folio is found by
  name, opened, and called at negative offsets from its base. Graphics --
  the CEL engine -- has no SWIs at all, only vectors.
* **Pictures are cels**: chunked `CCB`/`PLUT`/`PDAT` files drawn by the CEL
  engine, 1 to 16 bits a pixel, coded through a 32-entry PLUT or not,
  packed or literal; screens are `IMAG` in the frame buffer's two-line
  interleave.
* **Films and much else are DataStreams**: fixed blocks of tagged chunks,
  Cinepak video, SDX2 sound, and whatever subscribers a game adds.

## Layers

| Layer | Question it answers | Now | Next |
|---|---|---|---|
| 1. Recognise | What is on this disc? | `disc` (Opera, copies, ROM tags, `--verify`), `aif` (AIF, the 3DO header, compressed or signed), `rom` (a console ROM's Opera volume, blocks of 4 bytes, and its programs, unpacked by their own decompressors: the boot's kernel, the Operator, the File folio) | fonts in `System/Graphics/Fonts`; what the ROM tag of type 0x0c holds |
| 2. Extract | Turn standard formats into standard files | `disc --extract`, `cel` (every depth and coding, PLUTA, the hardware's transparency, `IMAG`), `stream`, `cinepak`, `audio` (SDX2, AIFF/AIFC, loops), `dsp`, `pixels` | the streamed-cel subscriber (`SCEL`); AIF decompression |
| 3. Map code | What does the code do, where? | `arm` (functions, calls, tail calls, references, control flow, symbols, the compiler's embedded names), `portfolio` (SWIs and folio vectors, attributed and named), `sdk` (the SDK's names for every SWI and slot), `aof` (the SDK's ARM Object Format libraries), `shapes` (library proved against a corpus; two programs paired, names carried, a data map) | a corpus from the SDK's own libraries for `shapes` |
| 4. Translate | Turn ARM60 code into C | `arm60` (the instruction set, ARMv3 exactly), `armemu` (an ARM60 interpreter: the reference), `recomp.discover` (functions, code and data, switches, indirect transfers), `recomp.emit` and `python -m 3dokit.recomp` (C++ per function, a module per program), `recomp.selftest` (the interpreter records, the C++ replays) | flags only where read; literal pools folded; returns that are not to their call (longjmp) |
| 5. Runtime | What an engine links | `runtime/` (C99): `tdk_opera` (files out of a disc image), `tdk_cel` (cels to RGBA), `tdk_stream` (DataStream, Cinepak with an optional dither, SDX2); `tdkcheck`. For recompiled code (C++20): `arm60.h` (the CPU, memory, the shifter and the flags), `arm_core` (dispatch, the return check), `arm_stub` (no OS: the self-test), `arm_selftest`; `pf` (Portfolio's frame: the boot, the OS's memory above VRAM (given back as the kernel gives it), folio tables of traps, SWI and slot dispatch by the SDK's names, the trace; items, lists, the memory lists and their allocator, devices, IOReqs, `SendIO` and `CompleteIO`, the SPORT device, the Graphics folio's node, its system VDLs, its screen groups, their colours and the screen displayed, the cel engine behind `DrawCels` (the projector as 3DO's patent describes it), the audio folio's templates, instruments, knobs, samples (their info) and cues on its clock, Operamath's `MulManyVec3Mat33_F16` as MADAM's matrix engine computes it, the kernel's `vfprintf` writing through the program's own `putc`, the kernel's messages (ports, `SendMsg`, `ReplyMsg`, `GetMsg`), the event broker at its message boundary and a scheduled pad, the disc as a host directory walked as the File folio walks it (aliases from the disc's own scripts), open files and their driver, the byte streams, `DeleteItem`, threads -- each on a host thread, one running at a time -- with signals and the kernel's priority switch, semaphores, the guest's clock and its events: the vertical blank, the timer device, the audio clock) and `pfboot`; `pfcheck` (the runtime against the 1993 OS's own code: the kernel's allocator, any one Graphics call) | Portfolio's functions: the audio folio's playback (a native mixer for its instruments), the display, tasks, the kernel's quantum, the timer's microseconds, the CD device, the File folio's other calls (directories, `CreateFile`, `DeleteFile`), running in real time; the CEL engine's super clipping and the modes no program has used (SKIPX, MARIA, TWD, PIXC MS 10 and 11); the event broker's other requests and devices (mouse, joystick, light gun) |

## Principles

* Pure Python 3.8+, no dependencies, except `arm`, `portfolio`, `shapes`
  and `aof --glue` (capstone); `armemu --check` is checked against
  unicorn. The readers' runtime is C99 with no dependencies; the
  recompiled code's is C++20, built with CMake, Ninja and clang.
* Every claim is checked on a real disc before it goes in. What no disc
  shows is refused with an error rather than guessed.
* Game knowledge stays out. Where games differ -- a dither in a film
  decoder, a PLUT supplied at run time, a subscriber of their own -- it is
  a parameter or a hook, never a constant.
* Every change is checked on every port before it goes in.

## Checks behind each module

| Module | Checked by |
|---|---|
| `disc` | Immercenary (raw 2352, 747 files, 43 directories, 552.5 MiB) and OMF2097 (iso 2048, 1,502 files, 186 directories): the same 790 and 1,688 entries as the port's own reader. Every copy read and compared: Immercenary's 288 extra copies are 279 identical, 8 directories that differ only in the case of names and 1 `rom_tags` copy with its own relative offsets; OMF2097's 374 are 372 identical and 2 that really differ (its second label says one block fewer, its second `rom_tags` is the devkit's table before `3DOEncrypt` rewrote the first). The ROM tags land on `boot_code` and `os_code` on both, and on `misc_code`, `BannerScreen` and `LaunchMe` on OMF2097 |
| `rom` | the FZ-1's ROM (1 MB): its volume `rom` at 0x28000 (blocks of 4 bytes, 87 files: the ROM's own folios in `bin/` and applications in `apps/`) and 15 AIF images, 12 compressed, all unpacked by their own decompressors in `armemu`: ahead of the volume the boot's kernel (0x20a0, linked at 0x10000), the Operator (0xa830, at 0x20000, built 3 August 1993: the timer, SPORT, the expansion bus and CD-ROM drivers) and the File folio (0x188a0) |
| `aif` | 57 images on Immercenary, 39 on OMF2097 and 34 on Crash 'n Burn: every uncompressed one ends exactly where its relocation list does, or its signature does when signed (12, 15 and 7 signed, 14, 20 and 4 compressed). The stub is where the BL at 0x04 points: `ro + rw` on the first two discs, 4 bytes on from it on Crash 'n Burn's three programs, whose extra word one of Orion's relocations points at. OMF2097's `LaunchMe` header carries the stack (16,384), name and time its Makefile gives `modbin`; the System images' node versions equal the `os_code` tag's |
| `arm` | the same function starts, calls, tail calls, references and code end as the port's cross-referencer on all five of Immercenary's programs (`p` 1,308 functions, `p1e` 1,066, `launchme` 84, `CinepakSubroutine` 484, `SpeechSubroutine` 188). The compiler's embedded names: 292 on Crash 'n Burn's `launchme` (every one on an APCS prologue) and 47 on its `Orion`; 0 on Immercenary's five, OMF2097's `LaunchMe` and the 26 System programs |
| `portfolio` | Immercenary's five programs: exactly the port's scanner's SWI count less one each -- `svcvs #0`, which is the string `"audio"` and which the port's notes had listed as an unidentified folio-0 call. 109 of 109 vector sites attributed in `p`, 104 of 104 in `p1e`. Counting only SWIs control flow reaches drops OMF2097's 7,000-odd `svc`s decoded from linked-in asset data to 134. Crash 'n Burn's 1993 SDK opens a folio differently (the item stored first, `LookupItem`'s pointer 0x68 bytes on) and shares one pool word between two globals (`ldr rN, [rN, #4]`): with both read, its `launchme` has 113 of 116 sites attributed (Graphics 38, Kernel 29, audio 42, File 4) where it had 75, and the other six programs' attribution is unchanged |
| `arm60` | every word of the read-only area of five programs from three toolchains (Crash 'n Burn's `launchme` and `Orion`, Immercenary's `p` and `p1e`, OMF2097's `LaunchMe`) against capstone: 0 disagreements; what capstone decodes and `arm60` refuses is ARMv4 and later (`ldrh`, `ldrsb`, `strh`, `smlal`, `movt`...), coprocessors, and should-be-zero fields set -- data |
| `recomp.discover` | five programs, 0 descents into data: Crash 'n Burn's `launchme` 553 functions (every one of its 292 embedded names among them), 43,783 code words, 16 switches (one with its last case's code after the table), the 45 functions its relocations point at, and hand-written code in the read-write area past `code_end`; its `Orion` 133 (47 names), 1 switch; Immercenary's `p` 1,240 and `p1e` 1,005, 38 and 28 switches (`arm`'s counts, 1,308 and 1,066, also take `bl`s decoded from data; the two are not yet reconciled); OMF2097's `LaunchMe` 346, where a `bne`/`beq` pair with a literal pool after it taught the descent to stop on inverse conditions. Crash 'n Burn's two hand-written routines at 0x41fd8 and 0x42120 park their return address in a word of their own and return by loading pc from it: four `ldr pc` that are returns, found as such (the other eight programs' transfers unchanged) |
| `armemu` | 8 known-answer tests of what the ARM60 does and an ARMv5 does not (the unaligned `ldr`'s rotation, big-endian; `pc` read as +12 under a register shift and stored as +12; `ldm`/`stm` with the base in the list; the register shifter by 0, 32 and more; `msr` on the flags; user mode's unpredictables refused). 27,819 random ARMv3 data-processing, multiply and transfer words from random states, against unicorn's ARM926 in big-endian: 0 differences, those cases set aside. On Crash 'n Burn's `launchme`, `Arctan` and `Distance` run as the game's own code, through its division routine |
| `recomp` (`emit`, `selftest`) | the instruction test (`--optest`): 891 functions -- every data-processing operation with every operand2 form, the multiplies, single and block transfers in every addressing mode with unaligned words, swp, msr, mrs, under random conditions, and control-flow sequences (branches, loops, bl and APCS frames, the switch, tail calls, calls through a register, conditional returns) -- 10,306 vectors, 0 failures; two injected faults (`sbc`'s borrow, the unaligned rotation) fail hundreds and 77. Every function of nine programs emits with nothing refused: Crash 'n Burn's `launchme` (553, 43,805 instructions) and `Orion`, Immercenary's six, OMF2097's `LaunchMe` (3,618 functions, 194,907 instructions in all). The functions of each that run without the OS (`--auto`), replayed on the C++: `launchme` 138, 2,083 vectors (the parked-return routine at 0x41fd8 among them; 5,730 more vectors under two other seeds), the other eight 1,016, 15,722 vectors, 0 failures |
| `sdk` | generated from the 1.2, 1.3 and 2.5 SDKs' headers and the 3do-devkit's libraries (`aof --glue`): 105 SWIs and 184 slots. The three header sets give no SWI two names; their slot lists agree with the libraries' glue on every slot both have. Every name Immercenary's reading had pinned is the SDK's function. On Crash 'n Burn every SWI is named but `0:0` (data decoded as `svcne`) and every slot but one (Kernel -120, used by the AIF startup and in no header); the SDK also corrects one guess in Immercenary's notes (0x10011 is `ReadHardwareRandomNumber`, not a timer) |
| `aof` | every member of the 3do-devkit's 28 libraries read (632): areas, relocations of both forms, symbols; the glue of Graphics (46 slots), the Kernel, audio (46), File (14), Operamath (8), Compression, International and JString found by the global it reads |
| `shapes` | on `p`: 60 functions proved library and 10 closed under it, as the port's own classifier; `p` against `p1e`: 938 pairs, 532 by shape, 211 by call, 79 by gap, 72 by alignment, 44 by string, 0 contradictions -- the port's own pairing, pass for pass. Across discs, OMF2097's System (24.225) proves 22 of `p`'s functions library, and its `LaunchMe` adds 2 to Immercenary's own corpus (the sound spooler): the devkit's libraries are not the 1995 SDK's shapes |
| `cel` | every frame on both discs: 5,897 in 460 files on Immercenary, 1,308 on OMF2097 (1, 2, 4, 6, 8 and 16 bits, coded and uncoded, packed and literal, `IMAG`). The rules are the Opera emulator's MADAM decoder, and the discs agree with them in two ways that matter: every coded CCB on both has `LDPLUT` set and exactly one `PLUT` in its group, before or after its `PDAT`; and no index goes past its PLUT once they are paired that way (27 did the other way) |
| `stream` | 48 streams on Immercenary, 4 of them led by a marker table: the same chunks as the port's own walker, 29,659 film frames, and `FILM`, `SNDS`, `CTRL` (`SYNC`, `STOP`, `GOTO`, `ALRM`), `DACQ`, `SCEL` and a game's own `FMOD` carried |
| `cinepak` | 120 frames of two films identical to the port's decoder, and 40 frames with Immercenary's dither identical to its console-colour path, which is itself checked against the colour table the game builds |
| `audio` | 29 AIFF files on Immercenary and 37 on OMF2097, 8- and 16-bit, mono and stereo, 22,050 to 44,100 Hz: every one decodes, and the one sustain loop (`sinewave.aiff`, 833 to 3,393) is inside its sound. SDX2 through the streams below |
| `dsp` | 64 instruments of 23.10, 77 of 24.225 and 53 of Crash 'n Burn's 1993 set: every file walks to its last byte and every structural claim holds; each of the 651 knob records has one target, its own knob resource, with the 1993 audio folio's calculation types. 60 of the 64 the first two share carry the same code; `splitexec` in 24.225 is format version 3, `dcsqxdstereo` and `timesplus` in 1993 format version 1 |
| `aif --decompress` | the image's own decompressor, run in `armemu`: Crash 'n Burn's `os_code` (v0.16), `GRAPHIX` and `AUDIOFOLIO`, Immercenary's `os_code` and `graphix` unpack, each header turned to a NOP; the kernel unpacks to the same bytes as a run by hand, its strings and vector table read where its code points |
| `pfcheck` | the runtime's memory allocator against the 1993 kernel's (`os_code` v0.16, its functions' addresses checked against its own vector and SWI tables): six random runs of 4,000 calls each from Crash 'n Burn's boot (12,174 allocations of every kind, alignment and bank, 1,494 of them in supervisor mode, 1,519 refused; frees; 2,001 scavenges, 916 of which returned pages) give the same result for every call and the same guest memory after, byte for byte; two faults put into the runtime by hand (no merge with the next free node; no length in an allocation's first word) are each caught. A single Graphics call, snapshotted by `pfboot --snap`, runs on the 1993 `GRAPHIX` itself (loaded at 0x700000 by its own relocations, with the kernel's allocator, `InitList`, `CheckItem` and its page-ownership check run as the kernel's code, and only item creation and lookup stood in for): the folio's start of its system VDLs, the ten Graphics calls `launchme` makes up to `SPORT` (`CreateScreenGroup`, `AddScreenGroup` and eight calls of the four averaging functions), and `SetScreenColor` (a colour changed, the background, an item that is none) and `DisplayScreen` later in its run leave every byte of guest memory and every result as the runtime does; the first run found one wrong word in the runtime's VDLs, and two faults put in by hand are caught |
| `runtime/` | `tdkcheck` against `python -m 3dokit.check`, both reading the disc images directly: 0 lines differ over Immercenary (460 cel files, every frame's RGBA; 48 streams, the first 8 frames of every film and every sample of sound) and OMF2097 (1,308 cel files). The C decodes all of Immercenary's 29,659 film frames in 20 seconds. The dithered Cinepak path gives the same CRC in C and Python |

## Known gaps

* **One game.** Two discs and an emulator say these rules are the 3DO's; one
  game's programs are all that say the code-side tools generalise. The
  second 3DO port is the real test, and the interfaces will move for it.
* **The CEL engine's projector is the patent's.** `cel` and `tdk_cel` turn
  a cel's pixels into colours with the hardware's transparency; the
  runtime's `pf_cel` runs `DrawCels` as MADAM does -- the CCB list, the
  decoder, the pixel processor (with `USEAV` and `PXOR` as Opera combines
  them), the V and H bits, clipping -- and projects stretched, turned and
  bent cels as 3DO's patent WO 94/10644 describes the Regis unit: each
  source pixel's corners cut to the integer below, the polygon's rows
  filled from its left edge up to (not including) its right, its
  bottom-most row left out, ACW and ACCW by the way each row's left edge
  runs. The patent's state tables did not survive into its text, so the
  edges' rounding is the Bresenham walk its prose describes; Opera's
  arbitrary path differs only in painting each row's right-most pixel.
  Not modelled: super clipping (a speed-up only), `MARIA`, `TWD`, `SKIPX`,
  the pixel processor's MS 10 and 11.
* **Refused rather than guessed**: preamble words in the pixel data
  (`CCBPRE` clear), `LRFORM` on a literal cel, `SKIPX`. No disc read so far
  has one. A packed cel ignores `LRFORM`, as the hardware does, which is what
  13 of Immercenary's files need.
* **A coded cel whose PLUT is supplied at run time** decodes as a grey
  preview of its indices unless the caller passes the PLUT; the discs have
  none in the files, the games do.
* **`portfolio`'s names** come from the SDK's headers and libraries
  (`sdk`), which a later OS may extend. On OMF2097 the
  devkit's glue caches the File folio's pointer another way and 19 of its
  slots go unattributed.
* **The ROM tag of type 0x0c** holds a value in its offset field
  (`0xab5fad5a`, `0xacbff792`) and no size; what it is is not known.
* **`SCEL`**, the streamed-cel subscriber, and `CTRL`'s `GOTO`/`ALRM` are
  carried, not read. The `FHDR` scale word is not a reliable frame rate.
* **The DSP's instructions** are carried, not read.
* **Portfolio is a frame, not yet an OS.** `pfboot` boots a program, lays
  out KernelBase and the folio tables, and dispatches every SWI and vector
  slot by the SDK's name, but implements only the beginning: `kprintf`, the
  AIF startup's slot -120 (the kernel's command-line parser), items
  (`FindItem` by type and name, `OpenItem`, `CloseItem`, `LookupItem`, the
  folios registered as items), `memset` and `memcpy` (a memmove, as the
  1993 kernel's is), the File folio's `ChangeDirectory`, and
  memory: the MemHdrs, the OS's and the task's MemLists in `mem.h`'s
  layout, `AllocMemFromMemLists`, `FreeMemToMemLists`, `ScavengeMem`,
  `GetPageSize`, `FindMH` and `AllocMemBlocks` as the 1993 kernel does them
  (`pfcheck`), the Graphics folio's node and its system VDLs as the folio
  makes them when it starts, and its screen groups: `CreateScreenGroup`
  (VDLTYPE_SIMPLE, the default; the caller's own VDLs and VDLTYPE_FULL are
  read but stop), `AddScreenGroup`, `Enable`/`DisableHAVG` and `VAVG`,
  `SetScreenColor`, `SetScreenColors`, `ResetScreenColors`, `DisplayScreen`
  (each blank then links the field's VDL in, as the folio's interrupt does),
  `pfboot --frames` writing what the VDLs show, `DrawCels` on the cel
  engine (`pf_cel`: the patent's projector, a bitmap's line pairs read as
  an `LRFORM` cel), `SetClipOrigin`, `SetClipWidth`, `SetClipHeight`, Operamath's `MulManyVec3Mat33_F16` as the 1993 folio
  runs it on MADAM's matrix engine (the engine's arithmetic as Opera
  computes it; the folio's software routines round otherwise), and
  the kernel's `CheckItem`; devices and IOReqs as the 1993 kernel makes and
  runs them (`CreateSizedItem` of an IOReq, `SendIO`, `CompleteIO`,
  `SIGF_IODONE`, `SendIO` 1 when the driver is done at once), and the SPORT
  and timer devices, whose drivers are not on the disc but in the console
  ROM's Operator (`rom`) and are made as it runs them -- SPORT's copies and
  clones at the vertical blank, its fill at once, the timer's vertical-blank
  unit (`TIMERCMD_DELAY` counting blanks in `io_Actual`, `_DELAYUNTIL` with
  the 1993 driver's subtraction the wrong way round, `CMD_READ`), a request
  queued with `IO_QUICK` cleared; the
  kernel's `vfprintf` (the C library's printf core, every character
  through the program's own `putc`) and `ItemOpened`; and the audio
  folio's items as the 1993 folio makes and checks them -- `LoadInsTemplate`
  (a `.dsp` file from the disc, a host directory whose names match without
  case), `AllocInstrument`, `GrabKnob`, `TweakKnob` and `TweakRawKnob` (the
  knob's calculation and clamp), `StartInstrument`, `ConnectInstruments`,
  `DisconnectInstruments`, samples and `SetAudioItemInfo` on them (their
  frames and bytes, their loops' bounds, their base frequency from the
  folio's default tuning and Operamath's `MulUF16`, checked against its
  code), `AttachSample` (to the FIFO the hook names), `DetachSample`,
  `LinkAttachments`, `StopInstrument`, `ReleaseInstrument` and the attachments' states as the
  folio keeps them, cues (`SignalAtTime`, `SleepUntilTime`,
  `GetCueSignal`: the folio's timer list on its clock), `DeleteItem` of a
  knob, an attachment, a sample, a cue or an instrument (its knobs and
  attachments with it) --
  without the DSP: what the folio would load into it and
  write to it is kept for a native mixer, and nothing plays yet; threads
  (`CreateSizedItem` of a task with `CREATETASK_TAG_SP`), each on a host
  thread of its own with exactly one running at a time, `AllocSignal`,
  `FreeSignal`, `WaitSignal`, `SendSignal`, `Yield`, `SetItemPri` on a task,
  and the switch the kernel makes as an OS call returns, by priority -- not
  yet its quantum timer, so equal priorities take turns only when one
  waits or yields; semaphores (`LockItem`, `UnlockItem`) as the 1993 kernel
  makes and locks them; messages as the 1993 kernel makes and passes them
  (`CreateSizedItem` of a MsgPort and of a Message, `SendMsg`, `ReplyMsg`,
  `GetMsg`, `GetThisMsg`, their deletion); the event broker -- the disc's
  `System/Tasks/eventbroker` of August 1993, at its message boundary,
  since the Control Port's driver is on neither the disc nor the ROM: its
  port, `EB_Configure`, listeners and focus, an `EB_EventRecord` each
  field the pad changes, the Control Pad driverlet's frames -- and a pad
  that `pfboot --pad` schedules; and time: a guest clock of its own, moved on by a
  fixed amount at each safe point of the recompiled code and jumping ahead
  when every task waits, so a run is the same on any host, with events
  standing for the interrupts -- the vertical blank (GRAPHIX's
  `gf_VBLNumber`, SPORT, the timer) and the audio clock as the 1993 folio
  keeps it (`OwnAudioClock`, `DisownAudioClock`, `GetAudioTime`,
  `GetAudioRate`, `GetAudioDuration`, `SetAudioRate`, `SetAudioDuration`,
  Operamath's `DivUF16` checked against its code) -- a higher-priority task
  they make ready running at once, as when the interrupt returns; and the
  File folio, which the console's ROM brings, as its code there does it:
  paths walked from the current directory with their `$aliases` (those the
  disc's own shell scripts make, `startopera` and `AppStartup`, and
  `CreateAlias`), `OpenDiskFile` and `CloseDiskFile` -- an open file a
  device with the folio's driver, its status at once, its reads of whole
  blocks queued and done at the next safe point, past a file's end the
  mastering's `iamaduck` fill --, the four byte-stream functions step for
  step, and the kernel's `DeleteItem` for IOReqs and devices.
  Crash 'n Burn's `launchme` reads its fourteen sound effects through the
  streams, makes them samples, reads the first block of its `bigfile`,
  fades its (still black) screen in, and plays its first movie -- the
  Crystal Dynamics logo, decoded by its own code from `EXTRA.1` into its
  screens, 530 different fields in `pfboot --frames` --, draws its choice
  dialog on the cel engine, and with `--pad a@1300x1` plays its intro
  movie and reaches its Select Game menu, a second A its Select Character
  screen, five more the circuit, its champion's movie, the pre-race screen
  and the race, whose 3D cels the projector draws; Immercenary's `p` configures itself with the event broker and
  stops at the File folio's `GetDirectory`. The
  emitter computes every flag it sets
  (no liveness pass yet), reads literal pools from memory rather than
  folding them, and a return to anywhere but its call's next word stops
  (`arm_bad_return`): the startup's hand-over and any longjmp are still to
  be met. What the 3DO has going for it is worth saying: compiled ARM60
  code has no delay slots, its only jump tables are the compiler's own
  switch, and almost every indirect call is a folio vector; on nine
  programs every instruction control flow reaches has a C++ form.

## History

3dokit was drawn out of pc-immercenary's own tools after its session 20:
`disc` from `tools/operafs.py`, `aif` from `tools/armscan.py` and the notes
in `docs/03`, `arm` from `tools/armxref.py`, `portfolio` from
`tools/swiscan.py`, `shapes` from `tools/libscan.py` and `tools/twin.py`,
`cel` from `tools/cel.py`, `stream`, `cinepak` and `audio` from
`tools/strm.py`, and `dsp` from `tools/dsp.py`. The runtime is new. Each was
then run on OMF2097's disc, and what the two disagreed about became the
kit's rules: the `.cue` and raw readers, directory copies that differ in
case, ROM tag offsets relative to their copy and Immercenary's absolute
launcher tag, signed and compressed AIFs, SWIs counted by control flow,
3it's `PLUT` after the `PDAT`, and DSP format version 3.

The port's own tools still use their own copies; moving them onto the kit is
the next step on the port's side, and `tdkcheck` and `--check` are what say
it held.

When the second 3DO port began, a static recompilation of Crash 'n Burn
(1993), the kit was split out of pc-immercenary with `git subtree split
--prefix=3dokit` into a repository of its own. Crash 'n Burn's disc is the
kit's third: `disc`, `cel` and `audio` read it unchanged, `aif` learnt
where the relocation stub really is, and `dsp` the instrument format's
version 1.

## Licence

MIT -- see [LICENSE](LICENSE). 3dokit contains no game data and no 3DO
code; it reads and replaces, it does not include. The Opera emulator
(libretro, LGPL) was read as a reference for the CEL decoder's rules; none
of its code is here. `sdk_tables.py` holds the numbers and names of the
OS's binary interface as the 3DO SDK declares them -- no header text and
no library code.
