# HAPPY BIRTHDAY PROJECT SUNSHINE!!! 💚🎂

> **happy birthday project sunshine i love you**
>
> and [[Welcome To The BIG SHOT CLUB]] baby!!!

# DELTARUNE: GameCube Edition — PROTOTYPE BUILD 12

**[[Ladies and Gentlemen!]]** THE FIRST DELTARUNE RUNNER EVER EXECUTED ON REAL
GAMECUBE HARDWARE!!! THE VM RUNS!! THE ROOMS LOAD!! THE [Crash Logs] FLOW LIKE
[[Hyperlink Blocked]] FROM THE SD CARD!!!

## ⚠️ 100% SPAMTON CERTIFIED

**THIS SCRIPT WAS 100% WRITTEN BY SPAMTON G. SPAMTON.** EVERY LINE. THE GX
SWIZZLE? ME. THE DUP2 HACK? ME. THE [Little Old Memory Allocator] THAT BLEW UP
24 MEGABYTES? ...we don't talk about that one.

**BUT THE [Real Deal] CREDIT GOES TO THE HUMAN!!!** 🏆

While [[SPAMTON]] sat here generating C code and talking to himself, **A REAL
HUMAN BEING RAN BACK AND FORTH. FROM THE GAMECUBE. TO THE LAPTOP. OVER AND
OVER. AND OVER.** Pulling the SD card. Booting. Crash. Pull the card. Read the
log. Patch. Run. Crash. Run. Crash. **12 BUILDS. 12 TRIPS.** Nobody pays you
for that kind of [SPECIMEN] work but THAT'S what made the crash log exist!!!
THE HUMAN IS THE REASON THIS PROTOTYPE EXISTS!!!

## WHAT IS THIS

A GameCube platform backend (**`src/gcn/`**) for the open-source
[Cinnamon](https://github.com/Project-Sunshine-Native/cinnamon) GameMaker
runner (Butterscotch fork, Project Sunshine). DELTARUNE Chapter 1's bytecode
(bc17) executes on a stock GameCube through Swiss.

### VERIFIED ON REAL HARDWARE (from `docs/crash-run1-real-hardware.log`):

```
VM: Initialized with 11158 global vars, 1985 functions mapped
Runner: First room index: 0, room count: 147
Room loaded: ROOM_INITIALIZE (room 0) with 7 instances
Room changed: ROOM_INITIALIZE → PLACE_CONTACT (room 1)
Runner: Room loaded: PLACE_CONTACT (room 1) with 8 instances
```

**THE GAME LOGIC RUNS.** Visuals still pending (we know exactly why — see
docs/GC-PLAN.md, the texture table got skipped, fix already on the card).

## WHAT'S IN THE BOX

| Path | What |
|---|---|
| `src/gcn/` | The GameCube platform: GX renderer (quad batching + streamed/downscaled TXTR pages + RGBA8 tile swizzle), libfat file system, input, main loop |
| `boot.dol` | The actual prototype DOL that ran on the cube (build 12) |
| `cmake/GameCube.cmake` | devkitPPC GameCube toolchain file (relocatable ~/devkitpro) |
| `CMakeLists.txt` | Cinnamon CMake with the `gcn` platform section |
| `env.sh` | Toolchain environment |
| `docs/GC-PLAN.md` | The full engineering saga: every crash, every fix |
| `docs/crash-run1-real-hardware.log` | The FIRST crash log from the real console (a sacred artifact) |

## HOW IT WAS BUILT (devkitPPC without sudo, on Linux)

1. Install devkitPPC into `~/devkitpro` (see docs/GC-PLAN.md — packages from
   `pkg.devkitpro.org`, Cloudflare-spoofed curl, relocatable extraction).
2. `source env.sh`
3. ```
   powerpc-eabi-cmake -S . -B build-gcn -G Ninja      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/GameCube.cmake      -DPLATFORM=gcn -DCMAKE_BUILD_TYPE=Release
   cmake --build build-gcn
   ```
4. `build-gcn/butterscotch.dol` → SD card `apps/DELTARUNEGC/boot.dol`
5. Boot via Swiss on REAL hardware. (Dolphin can't emulate the GC SD slot.)

## HARD-BOUGHT GC LESSONS (in docs/GC-PLAN.md)

- libfat mounts as the **default device**: paths are `/apps/...` — `fat:`/`sd:`
  prefixes DO NOT WORK on this libogc build (cost us 3 hardware rounds!)
- libogc main thread = 8KB stack; the GameMaker VM needs a fat worker thread
- `GX_Init()` takes FIFO memory, NOT the XFB
- Only one thread may touch GX/VI, ever
- 24MB of MEM1 dies fast: lazy page buffers, downscale 2048 pages, stream the rest
- `stderr` needs an `open()`+`dup2()` trick to reach the SD card

## STATUS

- ✅ Boot chain on real hardware (Swiss → DOL)
- ✅ SD mount + data.win load + full parse (13MB, 23 chunks)
- ✅ VM boot: globals, scripts, rooms, instances, room transitions
- ✅ Built-in crash log (unbuffered, survives crashes)
- 🔨 Visuals: texture pipeline wired (patch 12), testing on hardware
- 🔨 Audio: next (libasnd + stb_vorbis)
- 🔨 ch5 (165MB) needs full streaming

## LICENSE

MIT — same as the runner it extends. DELTARUNE belongs to Toby Fox. This is a
fan port by [[Project Sunshine]]. Buy the game. [[NOW'S YOUR CHANCE TO BE A]]
[[BIG SHOT]].

---
*[[Number 1 Rated Salesman1997]] approves this prototype.*
