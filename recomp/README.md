# Static recompiler

Translates the Gekko (PowerPC 750CL) machine code in `main.dol` into portable C, then links it with a
host runtime into a native executable. This is the route toward a PC / Linux / Steam Deck build.

## Status: milestone 2 done (boots into `main`)

- **Milestone 1:** all of `main.dol` translates to C with 0 untranslated instructions. It compiles with
  clang 19 with no errors or warnings, and `sfa --selftest` passes 5/5 (recompiled `memset`, `strlen`,
  `sprintf` checked against the host C library).
- **Milestone 2:** `sfa` boots the game.
  - It runs the recompiled `__start` → `OSInit` → `main`. The Dolphin OS banner prints from the game's own code
    ("Kernel built : May 21 2004", "Console Type : Retail 3", "Memory 24 MB", arena range).
  - It then runs the game's video and engine initialization.
  - It stops after **949 SDK calls to 246 distinct functions**, when `GXDrawDone` sleeps waiting for a
    GPU interrupt. See [docs/boot_trace.md](../docs/boot_trace.md).
- **The SDK is recompiled too**, except for the 5 functions in `runtime/hle_overrides.txt`, which are
  reimplemented in `runtime/hle.c`:
  - console output (`__write_console`)
  - audio DSP init
  - memory protection
  - `VIWaitForRetrace`, paced at 60 Hz.

  Each SDK call is logged to `build/GF7E01/sdk_trace.txt`. The MetroTRK debugger is stubbed.
- **Jump tables:** the generator also finds them by pattern (`lis`/`addi`/`lwzx`/`mtctr`/`bctr`), not just
  from dtk's `jumptable_` symbols. That resolves 162 more `bctr` sites.
- **Watchdog:** a guest polling one hardware register 2,000,000 times stops with a report of the last 24
  functions entered.

## Next: milestone 2b (threads and interrupts)

The boot stops at the first place the game sleeps a thread to wait for hardware. Getting further needs:
1. **Thread switching.** `OSSaveContext`/`OSLoadContext` switch stacks with `rfi`, which compiled code can't
   do. Run each guest thread on its own host thread or coroutine, and let only one run at a time.
2. **Interrupt delivery** from the runtime: VI retrace (60 Hz), PE finish/token (from the GX layer),
   DVD transfer complete, AI/DSP. These should call the SDK's own recompiled handlers (`__VIRetraceHandler`, …).
3. **DVD reads** served from `orig/GF7E01/files` through the FST (`DVDReadAbsAsyncPrio` / the low-level layer).
4. **A GX command stream sink** at `0xCC008000`. For now it only needs to raise the draw-done/token interrupts.
   Real rendering comes later.

## Build

```sh
python configure.py && ninja             # once: split the game (needs your disc in orig/GF7E01)
python recomp/tools/ppcrecomp.py         # writes build/GF7E01/recomp/*.c
cmake -S recomp -B build/GF7E01/cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/GF7E01/cmake
build/GF7E01/cmake/sfa --selftest        # run from the project root
```

On Windows with Visual Studio's bundled clang, also pass
`-DCMAKE_C_COMPILER=".../VC/Tools/Llvm/x64/bin/clang.exe" -DCMAKE_RC_COMPILER=".../llvm-rc.exe"`.
On Linux and Steam Deck, any gcc or clang works.

The generated code is derived from the game's code, so it stays under `build/` and is never committed.
Regenerate it from your own dump.

## How the translation works

- **One C function per guest function:** `void f_XXXXXXXX(Ctx *c)`. Function boundaries come from
  decomp-toolkit's analysis (`config/GF7E01/symbols.txt`).
- **CPU state** (`runtime/ppc.h`): GPRs, FPRs as `{ps0, ps1}` paired-single pairs, CR, LR, CTR, XER.CA/SO,
  FPSCR, GQRs and other SPRs.
- **Memory:** guest RAM is a 24 MB buffer mirrored at `0x80000000`/`0xC0000000`, accessed big-endian
  through `rd*`/`wr*`. Hardware registers (`0xCC000000`, including the GX FIFO at `0xCC008000`) and the
  locked cache (`0xE0000000`) go to runtime slow paths.
- **Control flow:**
  - Branches inside a function become `goto`.
  - `bl` becomes a direct C call.
  - `blr` becomes `return`.
  - A `b` to another function becomes a tail call.
  - `bctr` becomes a `switch` over that function's jump-table targets (read from the 164 jump tables dtk found), falling back to `rt_jump`.
  - `bctrl` and `blrl` go through `rt_call`, which binary-searches the generated function table.
  - Conditional branches implement all BO/BI forms, including CTR decrement.
- **Floating point:**
  - Single-precision ops round through `float`.
  - `fmadd`-family ops use C `fma` (Gekko fuses them).
  - Single loads and ops write both paired-single slots.
  - All `ps_*` ops and quantized `psq_l`/`psq_st` (u8, s8, u16, s16, float with GQR scale) are supported.

## Known gaps (for later milestones)

- **Not booted yet (milestone 2):** needs the HLE SDK (OS init, heaps, threads, interrupts) and running `__start`/`main`.
- **REL modules (milestone 3):** the 22 `.rel` files are not translated yet. They also need an `OSLink` replacement.
- **Code that rewrites LR or the stack to change control flow** won't work as compiled code.
  This includes setjmp/longjmp, C++ exception unwinding (`ExPPC_*`) and thread context switches.
  These need runtime replacements once the game is observed to use them.
- **Approximations:** XER.OV/SO are not tracked, and FPSCR rounding modes are ignored. FP record forms
  (`Rc=1`) are rare and not modelled. `fres`/`frsqrte` return exact results, not hardware estimates.
- **Performance is untuned:** registers live in `Ctx`, so the compiler must reload them around memory accesses.
