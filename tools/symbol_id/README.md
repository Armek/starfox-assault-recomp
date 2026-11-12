# Library symbol identification

Scripts used to name the Dolphin SDK, CRI, MetroTRK and MSL functions in `config/GF7E01/symbols.txt`.
Run them from the project root after `ninja`. The reference repositories go in a scratch folder, not in this repo.

| Script | Method | Used with |
|---|---|---|
| `build_sdk.py <dolsdk2004> <mwcceppc.exe> <outdir>` | Compiles every SDK source file in release mode (`-O4,p`, GC/1.2.5n), as the dolsdk2004 Makefile does. | [doldecomp/dolsdk2004](https://github.com/doldecomp/dolsdk2004) (4-20-2004 SDK) |
| `objmatch.py <main.dol> <symbols.txt> <objdir> <out.json>` | Byte-matches each compiled function against our functions of the same size, ignoring relocated words. Only unique matches are kept. | Compiled SDK objects; CRI objects compiled from [Jovinull/sonicheroes](https://github.com/Jovinull/sonicheroes) `src/game/cri` with GC/1.3.2 and that project's per-file flags |
| `align_symbols.py <ours> <ref symbols.txt>...` | Between functions named in both our map and a reference map, aligns the runs of function sizes and adopts names where sizes match. | symbols.txt from ttyd, pikmin2, tp, mkdd, marioparty4 and sonicheroes |
| `cri_strings.py <asm dir> <out.json>` | Names CRI functions after the identifier in the error strings they reference. | `build/GF7E01/asm` |

Safeguards applied when merging results:
- Byte matches win over alignment when the two disagree (1 case in 162).
- Matches of 16 bytes or less are kept only if a neighbouring function matched from the same library. Two
  two-instruction coincidences (`GXGetTexObjTlut`, `DEMOWinListSetCursor` inside CRI code) were rejected this way.
- CRI matches must fall inside the CRI address range.
- After every merge, `ninja` must still report `23 files OK`.

`tools/sdk_boundary.py` then writes [docs/sdk_boundary.md](../../docs/sdk_boundary.md).
