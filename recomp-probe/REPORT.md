# Star Fox Assault (GF7E01) — PC recompilation feasibility probe

Date: 2026-11-12. Input: `sys/main.dol` extracted from `Star Fox - Assault (USA).rvz` plus the 23 `files/*.rel` modules.

## Verdict

**GCRecomp 0.0.1-alpha cannot recompile this game.** Its output isn't valid Rust, and it gets core
instructions wrong. Getting a native PC/Linux/Steam Deck build means either building a proper
decomp project (recommended first step) or writing a much more complete recompiler. In both cases the
GameCube SDK (graphics, audio, file I/O, input) and the CRI middleware have to be reimplemented for PC.

## What the game's code contains

| Item | Measurement |
|---|---|
| `main.dol` code | 2.05 MB, 513,168 instructions |
| `.rel` modules | 23 files, 1.29 MB of code (`m4.rel` is a shared library that `m2`, `m3` and `m5` import) |
| Total instructions | 835,329 |
| Functions found (heuristic: `bl` targets) | 4,706 |
| Floating-point instructions | ~107k, 27k of them single-precision (opcode 59) |
| Gekko paired-single instructions | ~7,000 (opcodes 4, 56, 57, 60, 61) |
| SDK libraries | Dolphin SDK: OS, GX, AX, AI, AR, ARQ, DSP, DVD, CARD, PAD, SI, VI, plus the MetroTRK debugger stub |
| Middleware | CRI ADX (audio) and Sofdec/SFD/MPV (video), 2004 builds |
| Source names left in the binary | `GCN_Mem_Alloc.c`, `dvdfs.c`, `librel.c`, `nk_memcard.c`, `ns_file.c` |

## GCRecomp results (see `probe_report.txt`)

GCRecomp's analysis step needs Ghidra and silently `pip install`s an extra tool (`reoxide`). I bypassed both
with `harness/`, which feeds the game code straight into GCRecomp's own decoder and code generator.

1. **The decoder doesn't recognise 5.7% of instructions (47,354).** The gaps are basic, not exotic:
   - Opcode 59 (`fadds`, `fmuls`, `fmadds` and the rest of single-precision float math): 27,493. That's most of the game's 3D math.
   - Opcode 7 (`mulli`) and 8 (`subfic`): integer basics, 4,329.
   - Paired-single and `psq_l`/`psq_st` (Gekko SIMD): ~7,000.
   - Some opcode 31 and 63 forms: 8,500.
   - **46% of functions (2,181 of 4,706) contain at least one instruction it can't translate.**
2. **The code generator reports "success" on every function, but the output is broken** (see `sample_generated.rs`):
   - Half of each instruction's generated statement ends up inside a `//` comment, so loads and stores use `addr` before it's defined.
   - `bl` (function call) fails ("First operand must be condition") and becomes a TODO. No calls are emitted.
   - `blr` (return) is treated as a conditional branch on a CR bit. Branches jump to `pc + 0` and use `return;` in a function that returns `Result`, which won't compile.
   - `mflr`/`mtlr` are treated as "cache control". `mr r31,r3` is generated with source and destination swapped.
3. Graphics, audio and input runtimes are listed as "in progress" and don't exist yet.

## Realistic path to a native port

1. **Set up a proper decomp project** with [decomp-toolkit](https://github.com/encounter/decomp-toolkit)
   and the dtk-template. This is the standard GameCube setup (it's what the Twilight Princess, Melee and
   Star Fox Adventures decomps use). It splits the DOL and RELs into objects, finds every function and
   its data references, and identifies the Dolphin SDK and MSL/Runtime code automatically. That alone
   covers a big chunk of the 4,700 functions without hand-decompiling them.
2. **From there, either:**
   - **Decomp route:** decompile the game code to C, one function at a time (checked with objdiff), then
     replace the SDK layer with a PC backend. This is the Ship of Harkinian / Twilight Princess approach. It takes years of community work.
   - **Recomp route:** use decomp-toolkit's function and relocation data to drive a correct static recompiler
     (N64Recomp-style, generating C), and replace the SDK functions at the function level with PC code
     (GX to Vulkan/OpenGL, AX/DSP and ADX to SDL audio, DVD to file reads, PAD to SDL gamepad).
     Faster to first boot, but still many months of work.
3. **Until then:** Dolphin on Steam Deck runs the game well.

## Files

- `probe_report.txt`: full decoder and codegen statistics, per section
- `sample_generated.rs`: a sample of GCRecomp's output for functions it fully decoded
- `harness/`: the Rust probe (`cargo run --release -- <game folder> <out folder>`)
- `analyze.py`: DOL/REL structure and string analysis
