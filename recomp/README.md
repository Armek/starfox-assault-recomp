# Static recompiler

Translates the Gekko (PowerPC 750CL) machine code in `main.dol` into portable C, then links it with a
host runtime into a native executable. This is the route toward a PC / Linux / Steam Deck build.

## Status: milestone 3 in progress (the game draws its first screens)

- **Milestone 1:** all of `main.dol` translates to C with 0 untranslated instructions. It compiles with
  clang 19 with no errors or warnings, and `sfa --selftest` passes 5/5.
- **Milestone 2:** `__start` → `OSInit` → `main` runs from recompiled code, and the Dolphin OS banner prints.
- **Milestone 2b:** the game runs its threaded main loop at 60 Hz, starts CRI's ADX threads and loads files
  from the disc ([docs/boot_trace.md](../docs/boot_trace.md)).
- **Milestone 3 (so far):** `sfa` opens a window and renders the game's GX output with OpenGL.
  - The **"WARNING – HEALTH AND SAFETY"** screen draws correctly. With a button press (keyboard, a game
    controller, or `--auto-press`) the game moves on.
  - The **Sofdec logo** draws correctly. The game streams `movie/loop_demo.sfd` through the recompiled CRI
    Sofdec decoder, but movie frames come out black: the YUV→RGB conversion uses multi-stage TEV, which isn't
    implemented yet.
  - It then loads `m4.rel`, links it with the SDK's own `OSLink`, and stops on the first call into the module.
    REL code isn't translated yet.

### Graphics and input (milestone 3)

| Piece | How | Where |
|---|---|---|
| **Command processor** | Bytes written to the write-gather pipe (`0xCC008000`) are parsed as soon as a command is complete. It handles CP/XF/BP register loads (with the BP mask register), indexed XF loads, `CALL_DL` display lists from guest memory, and all 8 primitive types. | `gx.c` |
| **Vertices** | Every attribute (matrix indices, position, normal/NBT, two colors, eight texcoords) in direct, 8-bit and 16-bit indexed modes, all component formats and color formats. They're transformed on the CPU: position matrix, perspective/orthographic projection and viewport, written as clip-space vertices so GL still clips and interpolates with perspective correction. Texcoords go through the texgen source row and texture matrix (ST/STQ). | `gx.c` |
| **Textures** | I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2 (TLUT loads into a TMEM model) and CMPR. Decoded to RGBA8 and cached by address/format/size/palette, then re-decoded when the source bytes' hash changes (checked once per frame). | `gx.c` |
| **Render state** | Blend factors/subtract, color/alpha write masks, depth test/function/write, scissor, alpha test. TEV stage 0 is approximated as texture × rasterized color, texture only, or color only. | `gx.c`, `video.c` |
| **Frames** | Each frame's batches are built on the guest threads and handed to a **render thread** that owns the SDL window and an OpenGL 3.3 core context. It draws into a 640×528 EFB framebuffer and scales the XFB-copy rectangle into the window. EFB clears requested by copies are applied in order. | `video.c` |
| **Input** | Port 1 merges the keyboard and the first SDL game controller (Steam Deck included) into `PADRead`. `--auto-press` taps START and A every 4 seconds for unattended testing. | `video.c`, `hle.c` |
| **Diagnostics** | `--dump-frames` saves every 15th presented frame to `build/GF7E01/frames/*.bmp`. `--no-video` runs headless. | `video.c` |

Keyboard: arrows/WASD = control stick, Enter = START, Space/J = A, K/Backspace = B, L = X, I = Y,
Q/E = L/R, Z = Z, numpad 8/4/6/2 = C-stick.

### How the runtime provides the hardware

| Piece | How | Where |
|---|---|---|
| **Threads** | Each guest thread runs on its own host thread with its own `Ctx`, and a baton lets one run at a time. `OSSaveContext`/`OSLoadContext` are HLE: saving records the context a host thread owns, and loading hands the baton to its owner, starting a host thread the first time an `OSCreateThread` context is loaded. The SDK's own `SelectThread` does all scheduling. | `threads.c` |
| **Exceptions/interrupts** | The generator puts `RT_SAFEPOINT` on every loop back-edge (3,700+). When an interrupt or the decrementer is due and MSR[EE] is set, the runtime saves registers into the current `OSContext` (marked EXC) and calls the SDK's registered handler with the `__OSDispatchInterrupt` sequence. `OSLoadContext` of that context `longjmp`s back, like `rfi`, which also works when another thread later switches back to it. | `threads.c` |
| **Decrementer** | `mtdec`/`mfdec` become a host timer, so `OSAlarm` (timeouts, `OSSleepTicks`, DVD retries) works through the recompiled `DecrementerExceptionCallback`. | `threads.c` |
| **VI** | A 59.94 Hz retrace sets the VI interrupt bit and runs `__VIRetraceHandler`, which wakes `VIWaitForRetrace` sleepers and runs the game's retrace callbacks. | `runtime.c` |
| **Hardware registers** | `0xCC000000` is a register file, so reads return what was written. EXI/SI transfers complete instantly. The AI sample counter runs at 48 kHz. | `runtime.c` |
| **ARAM** | 16 MB with instant DMA and the ARAM interrupt. | `runtime.c` |
| **DSP** | A fake mailbox completes the SDK task loader's handshake (`0x8071FEED`, then "task init" `0xDCD10000` via the DSP interrupt). No microcode runs, so there's no audio yet. | `runtime.c` |
| **Disc drive (DI)** | The disc is rebuilt virtually from `orig/GF7E01` (`boot.bin`, `bi2.bin`, apploader, DOL, FST and all 960+ files at their FST offsets). Read, disc-ID read, inquiry and status commands complete immediately with the DI interrupt, and the SDK's DVD driver runs unchanged. | `dvd.c` |
| **HLE overrides** | Console output, DSP boot, MMU setup, context switching, `GXDrawDone` (no GPU) and `PADRead` (port 1 connected and neutral). | `hle.c` |

The generator also emits **extra entry points** for calls into the middle of a function (8, all in
`__save_fpr`/`__restore_fpr`), and the timebase uses a high-resolution clock (`__AI_SRC_INIT` measures 1/48000 s).

## Next

1. **REL modules (the current blocker).** Translate the 22 `.rel` files. Their load address is only known
   at run time, so the generator has to emit relocation-aware code: module-relative addresses become
   `module_base + offset`, and DOL references resolve statically. Hook `OSLink`/`OSUnlink` to register
   each module's base addresses and its functions with the dispatcher.
2. **Full TEV:** all 16 stages, the color/alpha combiners, konst colors, TEV registers, swap tables and
   indirect textures, generated as GLSL per TEV configuration. This is needed for movies (YUV→RGB) and
   almost all 3D effects.
3. **Lighting:** XF light channels (diffuse/specular/attenuation) using the decoded normals.
4. **EFB copies to textures:** render-to-texture, used for effects and the pause screen.
5. **Memory card:** emulate a card in slot A (EXI) backed by a file, or HLE the `CARD*` API.
6. **Audio:** AX/DSP HLE, then CRI ADX (already recompiled) and AI out through SDL audio.

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
