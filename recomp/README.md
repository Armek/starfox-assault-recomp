# Static recompiler

Translates the Gekko (PowerPC 750CL) machine code in `main.dol` into portable C, then links it with a
host runtime into a native executable. This is the route toward a PC / Linux / Steam Deck build.

## Status: milestone 4 done (Mission 1 is playable, with sound)

- **Milestone 1:** all of `main.dol` translates to C with 0 untranslated instructions. It compiles with
  clang 19 with no errors or warnings, and `sfa --selftest` passes 5/5.
- **Milestone 2:** `__start` → `OSInit` → `main` runs from recompiled code, and the Dolphin OS banner prints.
- **Milestone 2b:** the game runs its threaded main loop at 60 Hz, starts CRI's ADX threads and loads files
  from the disc ([docs/boot_trace.md](../docs/boot_trace.md)).
- **Milestone 3:** `sfa` opens a window and renders the game's GX output with OpenGL, including code from
  the REL modules. With a button press (keyboard, a game controller, or `--auto-press`) it gets through:
  - the health-and-safety warning, the Sofdec logo and the "NOW LOADING…" screen
  - the **3D title screen** (Arwings in the hangar) and the **STAR FOX ASSAULT logo**
  - save-file creation on the emulated memory card, the **NAME ENTRY** and **CONTROLLER SETTING** screens
  - the **Mission 1 briefing** ("Mission 1: Take Out Oikonny"), loading `m9.rel`, `fpc/s_01_01.fpc` and the map data.
- **Milestone 4:** audio, and the game reaches **Mission 1 gameplay** ("01 — A New Enemy — Fortuna").
  - It shows the Arwing in combat with the fleet, the full HUD, wingman tags and the pause menu.
  - Music and sound effects play through SDL.
  - The frame rate is 28–30 fps in the mission, which looks like a 30 fps cap: the game still idles
    20–28% of the time waiting for the retrace.
  - Models are lit with the real GX lighting model (per-vertex lights, attenuation and diffuse functions).
  - The 3D comm portraits (Falco, Slippy, …) and the Arwing's shadow render through EFB-to-texture copies.
- **System font:** the SDK reads the console's boot-ROM font (`OSInitFont`/`OSLoadFont`). `__OSReadROM` is HLE and
  serves it from `user/GF7E01/ipl.bin` (your own IPL dump) or from free replacement fonts in `user/fonts/`
  (`font_western.bin`, `font_japanese.bin`, Apache 2.0, from Dolphin's `Sys/GC`). The comm subtitles are drawn with it.
- All text renders: menus, file and level select, pop-up messages, the mission briefing and the comm subtitles.

### Memory card

`card.c` reimplements the 20 `CARD*` functions the game uses (everything except `CARDInit`).
- **Slot A** is a 59-block card, and slot B reports no card.
- **Saves** are standard `.gci` files (64-byte directory entry + data, compatible with Dolphin and save
  tools) in `user/GF7E01/card_a/`, which git ignores. The game's own save is
  `GF7E01-starfox.dat.gci` (5 blocks).
- **`CARDGetStatus`** computes the banner and icon offsets the way the SDK's `__CARDUpdateIconOffsets` does.
- **Async calls** complete immediately and call their callback.

### Audio (milestone 4)

`ax.c` is an HLE of the DSP's AX microcode. The SDK's AX library stays recompiled on the CPU side.
- **AI DMA** (`runtime.c`): the audio interface plays each 0x280-byte buffer (160 stereo frames at 32 kHz,
  5 ms) in real time, sends it to an SDL audio device, then raises AIDINT (interrupt 5). That runs
  `__AXOutAiCallback`, the game's sound engine (NU Sound) and CRI ADX streaming.
- **Mailbox:** `__AXOutNewFrame` mails `0xBABE0180` plus a command-list address. The fake DSP executes
  the list and answers `0xDCD10001` with a DSP interrupt (`__AXDSPResumeCallback`).
- **Command list:** studio setup, surround buffer, voice list (`2`), process voices (`3`), aux A/B
  send/return (`4`/`5`), compressor, final output (`0xE`), end (`0xF`).
- **Voices** (`AXPB`, 0xF4 bytes):
  - per-millisecond parameter updates
  - DSP-ADPCM, PCM16 and PCM8 from ARAM, with loop and end handling (restoring the loop ADPCM context
    for non-stream voices)
  - pitch resampling, the volume envelope and the one-pole low-pass filter
  - mixing into main, aux A and aux B with volume ramps.

  Positions, ADPCM history, envelopes and `state` are written back, so the game sees playback progress.
  That's what lets the streamed mission music keep loading, which was what blocked the Mission 1 load.
- **Idle sleep** also wakes for the next AI buffer, so audio interrupts keep their 200 Hz pace.

### Performance

`SFA_PROFILE=1` samples which guest function runs every millisecond and times the GX stages. A Mission 1
frame has about 20,000 small draws (fleet, lasers, particles, HUD).
- **The fixes that got it to 25–29 fps:**
  - compile the runtime optimized (`CMakeLists.txt` now defaults to RelWithDebInfo *before* `project()`;
    clang on Windows otherwise picks Debug and `-O0`)
  - read debug environment variables once
  - hash texture contents at most once per frame
  - decode XF transform/channel/texgen setup once per draw
  - batch by render-state serial number instead of comparing whole states.
- **Frame buffer pool:** frames go back to a small pool with their vertex and batch arrays, so a new frame
  reuses memory that is already allocated and paged in instead of growing ~17 MB of vertices from zero.
  Batching fell from about 2.6 s to 0.45 s per 10 s, and the game builds about 50% more frames in the same
  time (82 vs 53 GX frames per 10 s in Mission 1, as counted by the profiler).
- **Where the time goes now** (Mission 1, per 10 s): vertex transform about 5 s, state rebuilds about 1 s,
  waiting on EFB-to-texture copies about 1.5 s (3–4 copies a frame), batching about 0.45 s.
- **Further headroom:** move vertex transform to a GPU vertex shader, and cut per-draw state rebuilds
  (the game rewrites BP registers between almost every draw).

### Display lists built at run time

`GXBeginDisplayList` points the CPU FIFO (PI `0xCC00300C`–`0x3014`) at a RAM buffer while the GPU FIFO
(CP `0x20`/`0x22`) stays put. While those differ, pipe writes are **recorded** into RAM at the PI write
pointer instead of executed. Reading `0x3014` (as `GXEndDisplayList` does) pads the list to 32 bytes with
NOPs, like the write-gather pipe draining. Before this, recorded lists held stale memory. That's what left
the dynamic menu text, the title logo and other generated UI pieces blank.

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
| **Texcoord scale** | The setup unit's per-texcoord sizes (SU_SSIZE/SU_TSIZE, BP `0x30`–`0x3F`) are applied: a texcoord is rescaled by its SU size over the bound texture's size. This matters for `GXSetTexCoordScaleManually`, which the system-font text uses to send texel coordinates (the font sheet is drawn as CI4 with an RGB5A3 alpha-ramp palette). | `gx.c` |
| **TEV** | Full: up to 16 stages with every color/alpha combiner input, bias/scale/clamp/subtract and compare modes, the TEV color registers and konst colors (BP `0xE0`–`0xE7`, split by bit 23), konst selectors, swap tables, both raster color channels and 8 texture maps. The alpha compare uses both references and its logic op. Each distinct configuration becomes a generated GLSL fragment shader, compiled once and cached. | `gx.c`, `video.c` |
| **Render state** | Blend factors/subtract, color/alpha write masks, depth test/function/write, scissor. | `gx.c`, `video.c` |
| **Render-to-texture** | An EFB copy to texture ends the current *segment*. The render thread draws it (never dropped), reads the rectangle back and signals the guest. The guest box-filters it if half-scale is set, encodes it into the requested copy format (R4/I4, R8/I8, RA4/IA4, RA8/IA8, RGB565, RGB5A3, RGBA8, A8, R8, G8, B8, RG8, GB8; YUV intensity when set), tiled like textures, and writes it to the destination in RAM. Mission 1 uses this for the 3D comm portraits (69×65 RGBA8) and the Arwing shadow silhouette (160×160 RG8). | `gx.c`, `video.c` |
| **Lighting** | GX per-vertex lighting for both color channels (color and alpha separately). Normals are decoded (s8/s16/float, including the NBT index form) and transformed by the normal matrix (XF `0x400`). Ambient plus up to 8 lights from XF `0x600`: color, position, direction, spot (angle/distance) or specular attenuation, and the none/sign/clamp diffuse functions, multiplied by the material color from a register or the vertex. | `gx.c` |
| **Frames** | Each frame's batches are built on the guest threads and handed to a **render thread** that owns the SDL window and an OpenGL 3.3 core context. It draws into a 640×528 EFB framebuffer and scales the XFB-copy rectangle into the window. EFB clears requested by copies are applied in order. | `video.c` |
| **Input** | Port 1 merges the keyboard and the first SDL game controller (Steam Deck included) into `PADRead`. `--auto-press` taps START and A every 4 seconds for unattended testing. | `video.c`, `hle.c` |
| **Diagnostics** | `--dump-frames` saves every 15th presented frame to `build/GF7E01/frames/*.bmp`. Files are named by frame number and older runs' files aren't removed, so empty the folder before a run you want to compare. `--no-video` runs headless. With `SFA_GX_DEBUG=1`, each new draw configuration, texture upload (with how many source bytes are non-zero), EFB-to-texture copy and ARAM DMA is logged, and decoded textures are saved to `build/GF7E01/textures/`. | `video.c`, `gx.c` |

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
| **HLE overrides** | Console output, DSP boot, MMU setup, context switching, `GXDrawDone` (no GPU), `PADRead` (host input), `__OSReadROM` (system font), `OSNotifyLink`/`OSNotifyUnlink` and the `CARD` API. | `hle.c` |

The generator also emits **extra entry points** for calls into the middle of a function (8, all in
`__save_fpr`/`__restore_fpr`), and the timebase uses a high-resolution clock (`__AI_SRC_INIT` measures 1/48000 s).

## Next

1. **Performance:** GPU-side vertex transform (now the largest cost, about half of the guest's time).
2. **Performance:** keep EFB-to-texture copies on the GPU instead of reading them back synchronously
   (about 15% of the guest's time).
3. **Indirect textures** (the TEV indirect stages, BP `0x10`–`0x27`). Unused through the front end and Mission 1
   (0 of 27 million draws), so this waits until a later stage needs it.
4. **Packaging:** a release build layout, and a Linux/Steam Deck build and test.

### Debugging aids

- **`--dump-frames`, `--no-video`, `--auto-press`.**
- **`SFA_GX_DEBUG=1`** logs draw configurations, textures (with how many source bytes are non-zero),
  TMEM preloads, EFB-to-texture copies and ARAM DMA, and saves decoded textures as images.
- **`SFA_GX_FRAMEDUMP=<n>`** lists every batch of frame *n*.
- **`SFA_PROFILE=1`** prints the hottest guest functions, the GX stage times, GX frames built and the
  time spent waiting on EFB-to-texture copies, every 10 s.
- **`SFA_AX_DEBUG=1`** logs AX frame and AI buffer counts. **`SFA_DUMP_AUDIO=1`** writes everything
  played to `build/GF7E01/audio.wav`.
- **`SFA_GX_TEXADDR=<hex>`** shows which code binds that texture address and which display list sets it.
- **Write watchpoint:** a build with `-DRT_WATCH=ON` (in its own build folder) and
  `SFA_WATCH=<hex addr>:<hex len>` logs every non-zero write to that range with the functions that made it.

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
