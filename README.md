# Star Fox: Assault decompilation

A decompilation project for **Star Fox: Assault** (GameCube, USA, `GF7E01`), built on
[decomp-toolkit](https://github.com/encounter/decomp-toolkit) and its
[dtk-template](https://github.com/encounter/dtk-template). The long-term goal is C source
that rebuilds the original game exactly, which a native PC/Linux/Steam Deck port can then build on.

## Status

- `main.dol` and 22 of 23 REL modules are split into objects, and **all 23 rebuild byte-for-byte**
  (`config/GF7E01/build.sha1`).
- 7,654 functions found (5,375 in the DOL, 2,279 in RELs). 0% decompiled so far.
- Library functions identified (see [docs/sdk_boundary.md](docs/sdk_boundary.md) and `tools/symbol_id/`):
  Dolphin SDK 777/811, MetroTRK 161/161, MSL/Runtime 96/198, CRI ADX/Sofdec 120/1088.

### main.dol layout

| Range | Contents |
|---|---|
| `0x8000DDA0`–`0x8012D438` | Game code (`main` at `0x8000DE88`) |
| `0x8012D438`–`0x8015CA04` | Dolphin SDK (OS, EXI, SI, DB, DVD, VI, PAD, AI, AR, ARQ, AX, AXFX, MIX, DSP, CARD, GX) |
| `0x8015CA04`–`0x80198B1C` | Namco engine libraries ("NU Sound" driver, BGM control, `dvdfs.c`, heaps) |
| `0x80198B1C`–`0x801E8D48` | CRI middleware (ADX, LSC, SVM, SJ, Sofdec/MWSFD, SFX) |
| `0x801E8D48`–`0x801EEFA0` | MetroTRK debugger |
| `0x801EEFA0`–`0x80200B20` | MSL C library and CodeWarrior runtime |
- Toolchain: Metrowerks CodeWarrior (MSL runtime). The linker is set to `GC/2.6`, which is assumed
  from the 2004 build dates and still needs confirming against compiled code.
- Middleware: Dolphin SDK (OS, GX, AX, DSP, ARQ, CARD, DVD, PAD, VI), CRI ADX/Sofdec (Sep 2004).

### Notes on the modules

| Module | Notes |
|---|---|
| `m009.rel` | **Excluded.** The game never loads it, and 72 of its DOL relocations point past the end of this `main.dol`. It was built against an older DOL. |
| `m4.rel` | Shared code module imported by `m2`, `m3` and `m5`. |
| `m009`/`m7`/`m8` | Share module ID 11, so every module sets `links:` explicitly. `mb02`/`mb10a` are assumed to link against `m8`. |
| `mbg0`–`mbg2` | Contain a hand-written interpreter loop (jump tables, likely the core for the bundled arcade games). Need 4 `add_relocations` each, plus decomp-toolkit **v1.8.4 or newer** (older versions drop same-section REL14 relocations). |

## Setup

Requirements: Python 3, [ninja](https://ninja-build.org/). On Windows, nothing else is needed.
On Linux, `wine` or `wibo` is downloaded automatically for the compilers.

1. Put your own legally dumped disc in `orig/GF7E01/`, either as an extracted `sys/` + `files/` tree
   or as a disc image (`.rvz`/`.iso`). Nothing from the disc is committed (see `.gitignore`).
2. Run:

   ```sh
   python configure.py
   ninja
   ```

   The first run downloads decomp-toolkit, objdiff and the CodeWarrior compilers into `build/`, splits the
   binaries, relinks them, and checks the SHA-1s.

## Working on it

- Assembly for every split object: `build/GF7E01/asm/` (DOL) and `build/GF7E01/<module>/asm/` (RELs).
- Rename symbols in `config/GF7E01/symbols.txt` and define translation units in `splits.txt`.
  See [docs/](docs/) for the full workflow, including `objdiff` and decomp.me.
- Add C/C++ units to `config.libs` in `configure.py` as they are decompiled.

## Static recompilation (PC port)

[recomp/](recomp/README.md) translates `main.dol` to C and links it into a native executable.
Milestones 1-3 are done. The recompiled game, REL level modules included, runs in a window with
an OpenGL GX renderer (full TEV), keyboard and controller input, and an emulated memory card. It
gets through the title screen, save creation, name entry, controller settings and the Mission 1
briefing. Audio (AX/DSP HLE) is next; the mission load waits on streamed music.

## See also

- `recomp-probe/REPORT.md`: why the GCRecomp static recompiler can't handle this game yet.
