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
  - Music and sound effects play through SDL. The audio device pulls from a ring buffer kept about
    100 ms ahead; clock drift between the game and the sound card is absorbed by nudging the read rate
    (±0.5%, interpolated) instead of dropping chunks, and a stall that empties it fades out and refills
    before resuming. (Before, a queue that started empty and dropped whole chunks past 150 ms crackled.)
  - Flight stages run at 60 fps and hold it through comm scenes (`SFA_FPS=1`: 59.8–60 frames/s); ground
    stages run at a steady 30, their own rate. GX commands run on their own thread (see *GX thread*);
    before that, GX work on the game's thread pushed some frames past 16.7 ms when comm portraits were on
    screen, and the frame rate dipped to about 40–50.
  - Models are lit with the real GX lighting model (per-vertex lights, attenuation and diffuse functions).
  - The 3D comm portraits (Falco, Slippy, …) and the Arwing's shadow render through EFB-to-texture copies.
- **System font:** the SDK reads the console's boot-ROM font (`OSInitFont`/`OSLoadFont`). `__OSReadROM` is HLE and
  serves it from `user/GF7E01/ipl.bin` (your own IPL dump), else from font files in `user/fonts/`, else from the
  replacement fonts compiled into the program: `recomp/fonts/font_western.bin` and `font_japanese.bin`, which the
  Dolphin project generated from Droid Sans (Apache License 2.0, text in `recomp/fonts/font-licenses.txt`, shipped
  with every release package). `tools/embed_fonts.cmake` turns them into C arrays at build time. So players need
  only a disc image. The comm subtitles and menu text are drawn with it.
- All text renders: menus, file and level select, pop-up messages, the mission briefing and the comm subtitles.

### Other missions

`--stage=N` starts stage N of the game's stage table instead of Mission 1, and
`python recomp/tools/mission_test.py` runs each mission that way with `--auto-press` and frame dumps,
then writes a log and a contact sheet per mission to `build/GF7E01/missions/`.
- **How it works:** the stage table at `0x80223560` has 20-byte records (byte 0 = mission − 1, byte 5 =
  play type); the first stages of missions 1–10 are 0, 5, 9, 11, 15, 19, 20, 24, 27 and 29. The game's
  set-current-stage function `fn_8001D5B4` copies a record into the game info at `0x802A2B60` (`+0x13C`
  stage, `+0x140` mission, `+0x13F` play type, which picks the gameplay module: `m8` or `m9`). An entry
  hook there turns the first request for stage 0 into stage N, and the game does the rest itself: the
  mission's own briefing, its gameplay module and the stage start.
- **All ten missions run:** each ran 3 minutes from the first stage without a crash, with correct
  rendering, HUD, radar, pause map, briefings and wingman comms: the flight missions (1 Fortuna, 5, 8
  Orbital Gate, 10 Aparoid tunnel) and the on-foot/ground ones (2 Katina, 3, 4, 6, 7 Corneria, 9).
  (Swapping the stage later, when the stage starts, isn't enough: the gameplay module is already chosen,
  so ground missions ran Mission 1's flight module and crashed.)
- **Only first stages start cold:** a mission's later parts and boss stages take their resource list
  from RAM that the earlier parts fill in (the stage table points outside the DOL's read-only data), so
  `--stage` on them crashes at once and the runtime warns. Reaching them needs real progress through the
  mission.
- **Auto-play:** while a stage part is being played (state 16 of the state machine), `--auto-press` stops
  tapping START (which paused the game half the time) and plays instead: A every half second, stick
  forward with a slow left-right sweep. Results screens and story scenes between parts get the menu taps
  again (before, play mode stayed on and runs sat on the first results screen). Mission 1 then flies on
  to Oikonny's flagship.
- **Whole-game check (v0.2.1):** every mission run from its first stage with `--auto-press`,
  `SFA_AUTO_CHARGE=1` and `SFA_CLEAR_PART=30`, 8 minutes each, on Windows (hidden desktop, real
  rendering) and Linux (`--no-video`). No crashes or fatal errors in either. Between them the runs played
  every stage of the story's next-part chain (table at `0x8021CF38`): 0, 2–15 and 18–22, and 24–31,
  including each mission's last part. They also carried on into later missions on their own (2→3→4,
  6→7, 8→9→10). Stages 1, 16, 17 and 23 are in the stage table but not in that chain. Not covered: the
  scenes after a boss when it is "cleared" this way (the boss never dies, so the cutscene waits), the
  ending, VS mode and the bonus games.
- **Also seen working:** the full opening cinematic (fleet battle, subtitles), Mission 1's story intro
  (General Pepper's briefing and the in-engine cutscene), and the attract demo's 4-player split-screen
  VS matches.
- **Threads:** the game starts a streaming worker thread over and over (more than 120 in 4 minutes).
  Exited threads used to keep their host thread and table slot until the 64-slot table ran out ("too
  many guest threads"); now `OSExitThread` marks the thread and it ends once it switches away.

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
  Batching fell to about a sixth of its cost.
- **Vertex fast paths:** attributes are read through host pointers resolved once per span (not byte by
  byte through guest-memory checks), fixed-point scales are precomputed multipliers, the per-draw XF setup
  is reused until an XF light or register write bumps a serial, and a channel's color and alpha halves share
  their per-light terms when their light setup matches. Vertex cost fell from about 120 to 72 ns per vertex.
- **Render state:** a BP write invalidates the render state only when the value changes (or the register
  acts as a command: TLUT loads, the EFB copy, TEV color writes), and decoded vertex formats are cached per
  VAT until a CP register changes. Only about 2% of draws rebuild the state.
- **Where the time goes now** (Mission 1, per 10 s of wall time): about 3.5 million draws; vertices
  2.1–2.3 s, waiting on EFB-to-texture copies about 0.8 s, batching about 0.3 s, and the guest idles about
  32%. The "state" figure (about 0.6 s) is mostly the profiler's own clock reads. (Earlier figures in this
  history were per 10,000 profiler samples, about 19 s of wall time, not per 10 s.)
- **GX thread:** like the real GPU, GX commands run on their own thread. The game's thread only appends
  pipe bytes to an 8 MB ring (and records display lists itself, which must happen in program order); the
  GX thread parses and executes them: vertex transform, batching, EFB copies and frame submission. The game sees GX progress only through the PE token register,
  which it polls, and `GXDrawDone`, which now waits until everything queued has run. `SFA_GX_SYNC=1`
  runs the commands on the game's thread as before.
- **EFB copies on the GPU:** an EFB-to-texture copy used to stop the pipeline: the GX thread waited while
  the renderer drew everything so far and read the rectangle back, then encoded it into RAM, where it was
  decoded and uploaded again. Mission 1 makes two every frame (the comm portrait and the Arwing's shadow),
  so in busy scenes the game spent up to 43% of its time waiting and dropped to 45-52 fps on an i5-1135G7
  laptop (Iris Xe). Now a copy is a marker in the frame: the renderer converts the rectangle into a texture
  of its own with a small shader (same format conversion and rounding as before), and texture lookups at
  the destination use it. Guest RAM there isn't written; if the CPU later stores something else at that
  address (its hash changes), memory wins again. The same stretch now holds 58-60 fps. `SFA_EFB_READBACK=1`
  brings back the read-back path, for a game that reads copies with the CPU.
- **Mission 10, part 1 (the heaviest flight stage):** the first ~40 s after the stage starts run at 30 fps.
  That stretch is the in-engine story scene (Star Wolf joins, no HUD), and the game draws its cutscenes at
  30: during it the guest idles about half the time and the GX thread is a third busy. Once "Mission
  Start" shows, it holds 60 fps on an i5-1135G7 with about 9,000 draws and 43,000 vertices a frame. Per
  10 s there: GX thread busy about 4.8 s (vertices 2.9 s, state 0.7 s, batching 0.35 s), renderer about
  1 s, guest about 50% busy, and GXDrawDone waits under 0.2 s. A CPU about 1.7 times slower per core (the
  Steam Deck is roughly that) would push the GX thread past 16.7 ms a frame here, so that is where the
  Deck dips.
- **Guest code overhead, from an instruction-level profile (`SFA_PROFILE_NATIVE=1`):**
  - **`ldexp`:** 6-7% of the game thread's time went to the C library `ldexp`, called on every quantized
    paired-single load and store for a power-of-two scale. `pow2i()` builds that value from its exponent
    bits instead (exact for these exponents).
  - **Indirect calls (`bctrl`):** each binary-searched all 7,694 functions (`find_func`, 1.7%); a 4,096-entry
    direct-mapped cache now answers repeats, and is cleared whenever a REL module is linked or unlinked.
  - **SDK call tracing:** it stops once its log is full, instead of costing a call on every SDK function.
  - **Measured:** together, in Mission 2 at 30 fps, alternating with v0.2.7: the game thread was busy 18.9-20.0%
    against 21.0-22.0%, about 9% less.
  - **Fixed-address guest RAM:** guest RAM is mapped at host address `0x200000000000` (`PPC_MEM_HOST`), so its
    base is a constant: with a global pointer, every guest access reloaded it, since a byte store may alias
    anything. That gave about 3-7%, within the noise of the measurement.
  - **Where the time goes now:** about 70% of the generated code's time is in guest memory accesses (loads 40%,
    stores 20%, paired-single loads/stores 10%), then compares, float and integer ops (5-8% each).
  - **The biggest lever was the compiler:** see *Linux and Steam Deck* below.
- **Vertex work on worker threads:** reading vertices (indexed lookups into the game's arrays, mostly
  memory latency) and transforming them was most of that GX thread time. Now `draw()` only copies a
  draw's vertex bytes, snapshots what reading and transforming them needs (read context: vertex format,
  array bases and strides, default matrix indices; the XF setup; the matrix memory, about 1.5 KB, when it
  changed), reserves the output vertices in the frame and publishes a job. Up to 4 worker threads (CPU
  count minus 3) take jobs while the GX thread goes on with the commands.
  - **When the workers must be done:** the workers read the game's arrays a little later than the GX thread
    would have. The game may only reuse an array once the GPU is past its draws, which it learns through
    GXDrawDone and PE tokens, so the GX thread finishes all jobs before reporting either. It also finishes
    them before handing over a frame or growing `f->verts`.
  - **Same output:** the arithmetic per vertex is unchanged. `SFA_GX_VERIFY=1` redoes each frame on one
    thread and compares: 250 million vertices in Mission 10 matched exactly.
  - **Measured in Mission 10, same build:** `SFA_GX_DEFER=0` (the old inline path) against the default,
    alternating runs. GX thread time per frame fell by 40-60% (6.5 to 3.9 ms, 11.8 to 7.0 ms, 24.4 to
    10.4 ms at matching points), and its vertex time from 3.4 s to 0.55 s per 10 s. Batching went up
    from about 0.4 to 0.9 s, which covers the byte copy and publishing jobs.
  - **Settings:** `SFA_GX_WORKERS=n` sets the number of workers (0 = the GX thread does the jobs itself).
  - **A race, fixed after v0.2.7:** the job counters used to reset to 0 for each batch. A worker that had read
    them just before a reset could win a claim on job numbers that weren't published yet in the new batch,
    and crash on a job that wasn't filled in: 3-6 of 40 boots on Linux, in the intro. Job numbers now only
    grow (compared by difference, so they may wrap), and each job carries its frame. After the fix, 0 of
    40 boots crashed.
- **60 fps option (launcher "Frame rate", or `SFA_60FPS=1`; `SFA_60FPS=0` turns it off):**
  - **How the game sets its rate:** one target frame rate at `0x80385AF4` controls it: 60 in flight, 30
    set while an on-foot stage loads and for in-engine cutscenes; menus run at 60. Once a frame,
    `fn_8003AEB0` derives the frame period from it (`0x80385B04`, in VI units of 0x444: the VI callback
    `fn_8000DE20` pays down a budget at `0x80385B40` each retrace, and the main loop waits while it is
    positive) and the time-step constants the game code reads. Each frame then advances game time by
    one step.
  - **What the hook does:** an entry hook there sets the rate to 60. Measured in Mission 2: the game clock
    (`0x802A2D3C`, a float in seconds) advanced exactly frames / 60, so at a full 60 fps the game runs at
    normal speed. Cutscenes, subtitles and the HUD looked right.
  - **When 60 can't be held:** since game time is per frame, a PC that can't hold 60 would run the game
    in slow motion. So while a part is played (state 16, after its first 5 s, which have loading
    hitches and shader compiles) the hook counts frames per second. Below 54 for 2 s, it gives the
    game its own rate back and retries after 20 s, doubling the wait after each failure up to 160 s; a
    new stage starts over at 20 s.
  - **On-foot missions hold 60 on an i5-1135G7:** Mission 2 ran at 59-60 fps for most of a 2-minute run
    (52-57 in its busiest stretch), with the guest idle about half the time. At 30 fps the guest is
    idle 68-75% (8-10 ms of work a frame). Earlier figures of 17-23 ms came from runs while the laptop
    was busy with other work. `SFA_FPS_LOG=1` logs rate changes and fallbacks.
  - **Tried without gain** (Mission 2 at 30 fps, alternating runs; the difference was smaller than the
    spread between runs):
    - `restrict` on the generated functions' context pointer, so guest stores through `uint8_t*` no
      longer force register reloads.
    - Batching the write-gather pipe's ring updates (2,500-8,000 writes a frame, each with locked atomics).
- **Shader cache:** every TEV setup the game uses is appended to `user/shaders.bin` (a header, then
  `GxTevConfig` records) the first time it's built. At start, the renderer builds the saved ones in the
  background, up to 6 ms a frame, during the logos and menus. A file whose header doesn't match is
  started over, and `SFA_SHADER_CACHE=0` turns the cache off.
  - **Measured in Mission 1:** the first run saved 44 setups (9 KB), including the 28 that used to
    compile at once when Mission 1's effects first show (a 77 ms frame). On the second run 43 were
    built during the logos, and none were compiled during play.
- **Curve lengths (`fn_800FA400`), Mission 1's biggest hitch:** when a part starts, the game measures the
  lengths of its fleet's flight paths, cubic Bézier curves summed over 1,000 steps each: 542 curves at
  the start of part 1 and 679 at part 3.
  - **Before:** recompiled, each step called a vector subtract and a vector length through Ctx and guest
    memory, which took 25-42 ms in one frame (85-114 ms frames, the same moments every run).
  - **The replacement:** an HLE in `hle.c` does the same float operations in the same order (every
    rounding to single, every fused multiply-add, the rsqrt and three Newton steps of `fn_8015ECE4`). It
    is built twice: plain, and with the FMA instruction, chosen with CPUID (the library `fma()` call keeps
    the CPU from overlapping steps).
  - **Same output:** `SFA_CURVE_VERIFY=1` also runs the game's own helpers for every call and compares bits;
    no differences in 2,000 curves checked.
  - **After:** the bursts take 11.7 ms and 15.0 ms, and the longest frames there are 34-40 ms.
  - **Measuring it:** `SFA_PROFILE` reports each burst (`[curve]`). `SFA_STALL_MS=n` lowers the `[stall]`
    threshold, and stall lines now give each sample's caller (the guest's lr) and the frame's texture uploads.
    `SFA_PROBE=<addr>` with `SFA_COUNT_CALLS` and `SFA_PROBE_ARGS=1` counts a function's calls per second
    and the range of its `f1` argument.
- **Remaining hitches:** loading screens, and new textures: about 48 at once near Mission 1's first
  effects still coincided with an 80 ms frame. Texture decoding itself is minor (about 2 ms a second).
  Matrices change about every 18 draws (about 6,000 matrix loads a second), so it needs per-draw matrix
  snapshots in a buffer the shader indexes, not one uniform set per batch.

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
- **Calls into unlinked modules:** leaving a stage, `fn_8001B098` unlinks the gameplay module (`m9` in
  Mission 1) and frees it, and its caller `fn_8003CE84` then calls a function pointer into it anyway. The
  console runs the old code from the freed memory. So an unlinked module keeps its section addresses, and
  an address no linked module covers maps to the module linked there most recently (unlinked ranges
  overlap, since modules share the heap). Before this, finishing Mission 1 or a game over stopped with
  "indirect call to unknown address".
- **The FPU's non-IEEE mode:** `OSInit` calls `PPCSetFpNonIEEEMode`, so the game always runs with FPSCR
  NI set: a float result too small for a normal number (a denormal) becomes 0, and denormal operands count
  as 0. Game code relies on it. Mission 1's final boss, after losing its wings, steers toward a point by
  normalizing (target − position). As it arrives, the squared distance (~1e-39) must become 0, so the
  distance is 0 and the step is skipped. Kept as a denormal, `PSVECNormalize`'s Newton step overflowed
  to infinity: the boss's position went to −inf, then NaN, so it was never drawn, and the debris it sheds
  inherited NaN positions, never landed, never expired, and piled up by the thousand.
  `PPCSetFpNonIEEEMode` is now HLE: it sets NI and turns on the host's equivalent on every host thread
  that runs guest code (x86: MXCSR flush-to-zero and denormals-are-zero, which also cover the
  double-to-single conversions the generated code rounds through; AArch64: FPCR FZ). Besides fixing the
  boss, it removes slow denormal arithmetic: Mission 1's first stage ran about 50% faster in a like-for-like
  test.

### Graphics and input (milestone 3)

| Piece | How | Where |
|---|---|---|
| **Command processor** | Bytes written to the write-gather pipe (`0xCC008000`) are parsed as soon as a command is complete. It handles CP/XF/BP register loads (with the BP mask register), indexed XF loads, `CALL_DL` display lists from guest memory, and all 8 primitive types. | `gx.c` |
| **Vertices** | Every attribute (matrix indices, position, normal/NBT, two colors, eight texcoords) in direct, 8-bit and 16-bit indexed modes, all component formats and color formats. They're transformed on the CPU: position matrix, perspective/orthographic projection and viewport, written as clip-space vertices so GL still clips and interpolates with perspective correction. Texcoords go through the texgen source row and texture matrix (ST/STQ). | `gx.c` |
| **Textures** | I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2 (TLUT loads into a TMEM model) and CMPR. Decoded to RGBA8 and cached by address/format/size/palette, then re-decoded when the source bytes' hash changes (checked once per frame). | `gx.c` |
| **Texcoord scale** | The setup unit's per-texcoord sizes (SU_SSIZE/SU_TSIZE, BP `0x30`–`0x3F`) are applied: a texcoord is rescaled by its SU size over the bound texture's size. This matters for `GXSetTexCoordScaleManually`, which the system-font text uses to send texel coordinates (the font sheet is drawn as CI4 with an RGB5A3 alpha-ramp palette). | `gx.c` |
| **TEV** | Full: up to 16 stages with every color/alpha combiner input, bias/scale/clamp/subtract and compare modes, the TEV color registers and konst colors (BP `0xE0`–`0xE7`, split by bit 23), konst selectors, swap tables, both raster color channels and 8 texture maps. The alpha compare uses both references and its logic op. Each distinct configuration becomes a generated GLSL fragment shader, compiled once and cached. | `gx.c`, `video.c` |
| **Render state** | Blend factors/subtract, color/alpha write masks, depth test/function/write, scissor. | `gx.c`, `video.c` |
| **Render-to-texture** | An EFB copy to texture is recorded as a marker in the frame (ordered with the clears) and an entry mapping the destination address to a renderer texture. At the marker, the render thread runs a small shader pass that box-filters the rectangle if half-scale is set and converts it into what the requested copy format (R4/I4, R8/I8, RA4/IA4, RA8/IA8, RGB565, RGB5A3, RGBA8, A8, R8, G8, B8, RG8, GB8; YUV intensity when set) decodes to, so nothing waits. Texture lookups at the destination use that texture while guest RAM there is unchanged. With `SFA_EFB_READBACK=1` the copy instead ends a frame *segment* that the renderer draws and reads back, and the guest encodes it into RAM, tiled like textures. Mission 1 uses copies for the 3D comm portraits (69×65 RGBA8) and the Arwing shadow silhouette (160×160 RG8). | `gx.c`, `video.c` |
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
| **Disc drive (DI)** | The disc is read straight from a disc image (`.iso`/`.gcm`), or rebuilt virtually from an extracted disc like `orig/GF7E01` (`boot.bin`, `bi2.bin`, apploader, DOL, FST and all 960+ files at their FST offsets). The boot loader reads `boot.bin`, the DOL and the FST through the same layer, checks the disc ID is GF7E01 and that the DOL's size and CRC32 match the `main.dol` the code was translated from (the generator records them in `funcs.h`), and refuses another disc with a clear message. Read, disc-ID read, inquiry and status commands complete immediately with the DI interrupt, and the SDK's DVD driver runs unchanged. | `dvd.c`, `runtime.c` |
| **HLE overrides** | Console output, DSP boot, MMU setup, context switching, `GXDrawDone` (no GPU), `PADRead` (host input), `__OSReadROM` (system font), `OSNotifyLink`/`OSNotifyUnlink` and the `CARD` API. | `hle.c` |
| **Entry hooks** | An `entry NAME` line in `hle_overrides.txt` keeps the recompiled function but makes it call `rt_hook_XXXXXXXX(c)` first, so the runtime can watch or adjust its arguments. Used for `OSExitThread` (ending host threads), set-current-stage (`--stage`) and logging stage starts. | `hle.c`, `threads.c` |

The generator also emits **extra entry points** for calls into the middle of a function (8, all in
`__save_fpr`/`__restore_fpr`), and the timebase uses a high-resolution clock (`__AI_SRC_INIT` measures 1/48000 s).

## Next

1. **Performance:** a disk cache for compiled shaders (removes the first-time hitches), and GPU-side
   vertex transform if the Steam Deck needs more headroom.
2. **Performance:** the GX thread is now the limit in Mission 1's busiest scenes (about 11,800 small draws
   and 50,000 vertices a frame); a smaller vertex layout or GPU-side transform would lower it.
3. **Indirect textures** (the TEV indirect stages, BP `0x10`–`0x27`). Unused through the front end and Mission 1
   (0 of 27 million draws), so this waits until a later stage needs it.
4. **Steam Deck:** release packages exist (see *Release packages*); still to do: the windowed renderer
   on Linux and a test on the Deck itself (controls, fullscreen under gamescope, speed).
5. **Longer play:** later parts and bosses of each mission, VS mode menus and the bonus games are
   untested (they need real play, or menu scripts that wait for each menu to accept input).

### Debugging aids

- **Reaching later parts:** `SFA_CLEAR_PART=<seconds>[:<stop stage>]` marks each stage part cleared once it
  has been played that long (until the stop stage), so the game moves on through its own logic (state 16
  of the state machine `fn_8000ECC8`, outcome byte game info +0x142 = 7) with the state earlier parts
  build up. With `--stage=0 --auto-press` and `SFA_CLEAR_PART=20:4`, Mission 1's final boss is reached in
  about two minutes. `SFA_AUTO_CHARGE=1` makes `--auto-press` fire charged lock-on shots, which damage
  bosses. `SFA_GAME_STATES=1` logs the state machine's states.
- **Finding what a module does:** `SFA_COUNT_CALLS=<name prefix>` (e.g. `mb01b:`) counts calls to those
  functions and reports them every 10 s from the stop stage on. `SFA_PROBE=<hex address>` prints the
  object (`r3`) and vector arguments of every 997th call (`SFA_PROBE_EVERY=n`), and with `SFA_PROBE_NAN=1`
  (field `SFA_PROBE_FIELD`, default `0x80`) the first calls where that float has turned NaN, with what
  changed and the `+0x10` parent chain. `SFA_RAM_SNAPSHOTS=<seconds>` saves main RAM to
  `build/GF7E01/ram_NN.bin`. In a `-DRT_WATCH` build, `SFA_WATCH=<addr>:<len>` with `SFA_WATCH_NAN=1`
  reports writes of infinite floats with the guest call stack and FPRs. `SFA_VEC_DEBUG=1` logs
  `PSVECNormalize` inputs too small to normalize.
- **`SFA_LAUNCHER_SCRIPT=down,enter,…`** drives the start screen for testing: every 0.6 s it saves
  `build/GF7E01/launcher_NN.bmp` and presses the next key (`up down left right pgdn enter esc start`;
  anything else waits a step); it quits after the last one. Scripts use the built-in browser unless
  `SFA_LAUNCHER_DIALOG=1` is set too. With `SDL_VIDEODRIVER=offscreen` (Linux) it runs without a window;
  a stand-in `zenity` on `PATH` then tests the system-dialog path.
- **`--help`** lists the options. **`--fullscreen`** and **`--window=WxH`** set up the window; F11 or
  Alt+Enter toggles fullscreen.
- **`--dump-frames`, `--no-video`, `--auto-press`.**
- **`--auto-keys=start,w,down,a,…`** presses one button per 1.5 s step from launch (`w` waits), then
  hands over to `--auto-press`. Directions also move the stick. Menus ignore input for a moment after
  they appear, so fixed timing is fragile.
- **`--stage=N`** starts stage N instead of Mission 1 (the first stage of any of the ten missions), and
  **`recomp/tools/mission_test.py`** runs a list of missions and saves logs and contact sheets.
- **`SFA_GX_DEBUG=1`** logs draw configurations, textures (with how many source bytes are non-zero),
  TMEM preloads, EFB-to-texture copies and ARAM DMA, and saves decoded textures as images.
- **`SFA_GX_FRAMEDUMP=<n>`** lists every batch of frame *n*.
- **`SFA_PROFILE_NATIVE=1`** (Windows, with `SFA_PROFILE=1`) samples the instruction the game thread is
  running every millisecond. Every 10 s it prints the hottest source lines through the build's debug info,
  and a breakdown of the generated code by kind of guest operation.
- **Crash reports** include the host call stack: function and line on Windows, and offsets on Linux to
  resolve with `addr2line -f -e sfa <offset>`.
- **`SFA_PROFILE=1`** prints the hottest guest functions, the GX stage times, GX frames built and the
  EFB-to-texture copies (with the time spent waiting on them under `SFA_EFB_READBACK`), every 10 s of wall
  time. With `SFA_FPS=1` too, each frame that comes more than 80 ms late prints a `[stall]` line: how long
  the renderer spent on that frame, and which guest functions (and threads) were sampled during the gap,
  or how much of it the guest was idle. A `[prof] split` line gives, per 10 s, how long the GX thread
  worked, how long the renderer spent in its frames, and how long the guest waited in `GXDrawDone` or on
  a full GX ring. This shows which thread limits the frame rate.
- **`SFA_EFB_READBACK=1`** reads EFB-to-texture copies back into guest RAM (slow; the old path).
- **`SFA_GX_WORKERS=n`, `SFA_GX_DEFER=0`, `SFA_GX_VERIFY=1`:** the number of vertex worker threads, the old
  inline vertex path (for comparison), and checking the workers' output against one thread.
- **Crash reports:** a fatal error (`rt_fatal`) or a host crash (a signal on Linux, an unhandled
  exception on Windows) appends a report to `user/crash.log`: the message, build date, guest registers,
  current stage and the last 64 guest functions entered. Outside test runs it also shows an error box.
  When stdout isn't a terminal and it isn't a test run, all output goes to `user/sfa.log` (the last
  run), so runs started from Steam or a file manager leave a log too.
- **`SFA_FPS=1`** prints, every 5 s, the presented frame rate, the longest gap between frames and how
  many exceeded 50 ms, shader compiles, texture uploads, and audio underruns and dropped samples.
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

### Linux and Steam Deck

Build with clang, which the release packages use. gcc works too, but its code for the generated sources is
much slower: in Mission 2 (headless, alternating runs on an i5-1135G7) the game's code was busy 27.7-29.3%
of the time built with gcc 14 against 16.5-17.4% with clang 19. On the Steam Deck that is the difference
between holding 60 fps on foot or not. On Debian or Ubuntu:

```sh
sudo apt-get install build-essential clang cmake ninja-build pkg-config libsdl2-dev
CC=clang cmake -S recomp -B build/linux-clang -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/linux-clang
build/linux-clang/sfa --selftest         # run from the project root
```

The generated sources from `ppcrecomp.py` are the same on every platform, so a Windows checkout can be
built from WSL without regenerating. Tested in WSL Debian 13 (gcc 14, SDL2 2.32.4): the build has no
warnings, the self-test passes 5/5, and a 90 s headless run (`--no-video --auto-press`) boots into
Mission 1 with streamed music. The windowed renderer hasn't been tried on Linux yet.

For the Steam Deck, the binary needs glibc 2.34 or newer (its newest symbol is `GLIBC_2.34`) and
`libSDL2-2.0.so.0`, which SteamOS has. Copy `build/linux/sfa` (or a release package) to the Deck with your disc
image next to it. If it won't start there, build
inside Valve's Steam Runtime SDK or on the Deck itself.

### Updates

`updater.c`: `recomp/VERSION` holds the release version, compiled in as `SFA_VERSION`, shown on the start
screen and logged at boot. Bump it before each release, and tag the release `v` plus that version.
- **The check:** when the start screen opens (unless `check_updates=0` in `user/launcher.cfg`, or
  `SFA_NO_UPDATE_CHECK` is set), a background thread fetches the repository's releases from GitHub's API
  and picks the newest one above this version that has this platform's package. Pre-releases count. The
  download URL must be a GitHub release asset of this repository.
- **The offer:** a banner reads "Update available". Y, U or a click installs it.
- **Installing:**
  - It downloads into `user/update/` and checks the file against the size GitHub reports.
  - It unpacks with `tar`, which also reads `.zip` on Windows 10/11.
  - It moves the files over the program's. On Windows the running `sfa.exe` and `SDL2.dll` are renamed to
    `*.old` first, and removed at the next start; on Linux the old binary is unlinked.
  - It restarts with the same arguments (`execv`, or a new process on Windows).
- **Tools:** downloads use `curl`, or `wget` where there's no curl. Both run without the Steam runtime's
  `LD_PRELOAD`/`LD_LIBRARY_PATH`, so no libraries are added.
- **Tested end to end:** with `SFA_VERSION_OVERRIDE=0.2.0` and `SFA_UPDATE_TEST=1`, which allows the check in
  scripted runs, from a copy of each package. On Windows (curl) and Linux (wget) it found v0.2.8, installed
  it byte-identical to the release's program, and restarted into it.

### Release packages

`python recomp/tools/package.py` (on Windows) or `python3 recomp/tools/package.py` (on Linux, after the
Linux build) makes `dist/sfa-recomp-<platform>/` and a `.zip`/`.tar.gz` of it (about 11 MB). It holds the
executable (stripped on Linux; `SDL2.dll` on Windows), `sfa.sh` on Linux (starts fullscreen),
`font-licenses.txt` (for the built-in fonts) and `PLAY.txt` with the setup steps, controls and options. It
never contains game data: players put their own disc image next to the executable, and that's all.

**Start screen** (`launcher.c`): started without a disc on the command line, the program first shows a
launcher: a dark gradient with a slow starfield and a planet's glow, the title, a game disc card (the
disc, its status and Browse…), a Window/Fullscreen switch, and Play and Quit. It uses SDL's 2D renderer
and needs no files: text is drawn with the system font compiled into the program (Droid Sans: the IPL
font is Yay0-decompressed and its 2-bit sheet copied into an atlas at 1x, and at 3x upscaled with sharpened
edges for large text), and rounded shapes come from a generated disc texture. It works with the mouse
(hover highlights), the keyboard and a controller (a focus ring marks the selected control; button badges
in the footer). Browse… opens the system's file dialog on its own thread, so the screen keeps drawing:
Windows' Open dialog (`GetOpenFileName`), or on Linux `kdialog` (KDE, the Steam Deck's desktop) or
`zenity`, started without `LD_PRELOAD`/`LD_LIBRARY_PATH` so the Steam overlay and runtime stay out of it.
With a controller, in the Deck's Game Mode (gamescope), or when no dialog program exists, a built-in
browser opens instead (places: program folder, home, Desktop, Downloads, drives on Windows,
`/run/media/$USER` and `/media` on Linux), showing folders and `.iso`/`.gcm` files, an extracted disc
folder as one entry, and compressed images marked for conversion. A picked disc is checked at once (disc
ID GF7E01, and the DOL's size and CRC32), so a wrong disc is refused there with the reason. The choices are kept in `user/launcher.cfg`. `--play`, a disc on the command line, any testing
option or `SFA_NO_LAUNCHER=1` skips it; if no window can be opened, the program looks for the game as below.
Tested on Windows: picking the test ISO in the browser and pressing START boots from it, and a disc with one
changed DOL byte shows "different version of the game".

**Finding the game:** a disc image or extracted-disc folder named on the command line, or else, in the
current folder and then next to the executable: `orig/GF7E01/`, or any `.iso`/`.gcm` file whose disc ID
is GF7E01 (any file name). When it's found next to the executable, the program runs from there, so saves
go to that folder's `user/` too; a package runs from a shortcut, a file manager or Steam. Compressed
images (`.rvz`, `.wia`, `.gcz`, `.ciso`, `.wbfs`, NKit) can't be read; the program says to convert them
to ISO with Dolphin's Convert File. Tested on Windows and in WSL Debian with an image rebuilt from the
extracted disc: booted from the folder, from an image argument, from an image found in the current
folder and next to the packaged executable (into Mission 2), and a disc with one changed DOL byte was
refused.

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
