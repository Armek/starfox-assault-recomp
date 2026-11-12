# Static recompiler

Translates the Gekko (PowerPC 750CL) machine code in `main.dol` into portable C, then links it with a
host runtime into a native executable. This is the route toward a PC / Linux / Steam Deck build.

## Status: milestone 3 in progress (the game draws its first screens)

- **Milestone 1:** all of `main.dol` translates to C with 0 untranslated instructions. It compiles with
  clang 19 with no errors or warnings, and `sfa --selftest` passes 5/5.
- **Milestone 2:** `__start` → `OSInit` → `main` runs from recompiled code, and the Dolphin OS banner prints.
- **Milestone 2b:** the game runs its threaded main loop at 60 Hz, starts CRI's ADX threads and loads files
  from the disc ([docs/boot_trace.md](../docs/boot_trace.md)).
- **Milestone 3 (so far):** `sfa` opens a window and renders the game's GX output with OpenGL, including
  code from the REL modules.
  - The **"WARNING – HEALTH AND SAFETY"** screen draws correctly. With a button press (keyboard, a game
    controller, or `--auto-press`) the game moves on.
  - The **Sofdec logo** and the full **"NOW LOADING…" screen** draw correctly. The loading screen has the
    Star Fox emblem, curved text, the progress bar and "SEARCHING FOR TARGETS… TARGET ACQUIRED".
  - The game loads and links `m4.rel` and `m2.rel` and runs their recompiled code, then loads `m3.rel`,
    `m5.rel` and `fpc/select.fpc`.
  - **Current stop:** a dialog box (most likely the memory-card check) whose text texture
    (`0x814761A0`, CMPR 528×24) is still all zero when drawn. It isn't written by DVD, ARAM DMA or an EFB
    copy, so it's probably generated at run time from a memory-card result or by a loader that is waiting.
    Next step: emulate a memory card in slot A.

### REL modules

The 22 `.rel` level modules (every module except `m009`, which the game never loads) are translated
together with `main.dol`. That's 2,282 functions, 52 of them code that dtk left as gaps, with 0
untranslated instructions.
- **Addresses:** each module's code gets a virtual address range (`0x90000000 + n × 4 MB`), so its functions
  have unique names (`f_91400104` is `m4:_prolog`) and its internal branches resolve when the C is generated.
- **Relocations:** the 59,585 code relocations become runtime expressions. `ADDR16_HA/HI/LO` immediates read
  `g_rel[module].sec[section] + addend`, and DOL targets fold to constants. `REL24`/`REL14` branches become
  direct C calls into the DOL, the module itself, or the module it imports (m2/m3/m5 → m4,
  mb02/mb10a → m8). Note that `ADDR16` relocations point at the 16-bit field, i.e. instruction + 2.
- **Jump tables:** read through the 4,966 data relocations. The `switch` maps the runtime `CTR` back to
  generated addresses.
- **Linking:** the SDK's own `OSLink`/`Link`/`Relocate` run unchanged. `OSNotifyLink`/`OSNotifyUnlink` are
  HLE hooks that recognise the module (by ID and section sizes) and record its real section addresses. After
  that, `rt_call`/`rt_jump` map pointers into the module (vtables, `_prolog`, callbacks) to its generated
  functions.

### Graphics and input (milestone 3)

| Piece | How | Where |
|---|---|---|
| **Command processor** | Bytes written to the write-gather pipe (`0xCC008000`) are parsed as soon as a command is complete. It handles CP/XF/BP register loads (with the BP mask register), indexed XF loads, `CALL_DL` display lists from guest memory, and all 8 primitive types. | `gx.c` |
| **Vertices** | Every attribute (matrix indices, position, normal/NBT, two colors, eight texcoords) in direct, 8-bit and 16-bit indexed modes, all component formats and color formats. They're transformed on the CPU: position matrix, perspective/orthographic projection and viewport, written as clip-space vertices so GL still clips and interpolates with perspective correction. Texcoords go through the texgen source row and texture matrix (ST/STQ). | `gx.c` |
| **Textures** | I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2 (TLUT loads into a TMEM model) and CMPR. Decoded to RGBA8 and cached by address/format/size/palette, then re-decoded when the source bytes' hash changes (checked once per frame). | `gx.c` |
| **TEV** | Full: up to 16 stages with every color/alpha combiner input, bias/scale/clamp/subtract and compare modes, the TEV color registers and konst colors (BP `0xE0`–`0xE7`, split by bit 23), konst selectors, swap tables, both raster color channels and 8 texture maps. The alpha compare uses both references and its logic op. Each distinct configuration becomes a generated GLSL fragment shader, compiled once and cached. | `gx.c`, `video.c` |
| **Render state** | Blend factors/subtract, color/alpha write masks, depth test/function/write, scissor. | `gx.c`, `video.c` |
| **Color channels** | Material/vertex color and ambient per channel. Lighting is still an estimate (ambient + 0.6), since normals aren't used yet. | `gx.c` |
| **Frames** | Each frame's batches are built on the guest threads and handed to a **render thread** that owns the SDL window and an OpenGL 3.3 core context. It draws into a 640×528 EFB framebuffer and scales the XFB-copy rectangle into the window. EFB clears requested by copies are applied in order. | `video.c` |
| **Input** | Port 1 merges the keyboard and the first SDL game controller (Steam Deck included) into `PADRead`. `--auto-press` taps START and A every 4 seconds for unattended testing. | `video.c`, `hle.c` |
| **Diagnostics** | `--dump-frames` saves every 15th presented frame to `build/GF7E01/frames/*.bmp`. `--no-video` runs headless. With `SFA_GX_DEBUG=1`, each new draw configuration, texture upload (with how many source bytes are non-zero), EFB-to-texture copy and ARAM DMA is logged, and decoded textures are saved to `build/GF7E01/textures/`. | `video.c`, `gx.c` |

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

1. **Memory card (the current blocker).** Emulate a card in slot A backed by a file, either as an EXI
   device or by HLE of the `CARD*` API. Then find out what fills the dialog's text texture.
2. **Indirect textures** (the TEV indirect stages, BP `0x10`–`0x27`), for warps and effects.
3. **Lighting:** XF light channels (diffuse/specular/attenuation) using the decoded normals.
4. **EFB copies to textures:** render-to-texture, used for effects and the pause screen.
5. **Audio:** AX/DSP HLE, then CRI ADX (already recompiled) and AI out through SDL audio.

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
