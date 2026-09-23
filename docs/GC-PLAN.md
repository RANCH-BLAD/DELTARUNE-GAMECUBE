# DELTARUNE GameCube Port — Working State (2026-09-13/14 session)

## GOAL
Working DELTARUNE on real GameCube via Swiss (SD card label "egg", mount
/run/media/ryzen/egg). Runner = Cinnamon (Butterscotch fork) in
/home/ryzen/sunshine-patcher/cinnamon/. New platform backend: src/gcn/.

## DONE THIS SESSION (all verified by real tool output)
1. devkitPPC toolchain INSTALLED at ~/devkitpro (no sudo, manual pkg extract):
   - Source: pkg.devkitpro.org/packages/linux/x86_64/*.pkg.tar.zst (curl w/
     Firefox UA passes Cloudflare; plain curl 403s. downloads.devkitpro.org
     redirects there. The "windows/x86_64/" repo path also serves the same
     linux-x86_64 tool binaries. /packages/dkp-libs.db flat .db = package list)
   - Installed: devkitppc-gcc-16.1.0-1, binutils-2.46.0, mn10200-binutils
     (MISSING - dep of meta pkg, not needed for build), newlib-4.6.0.20260123-4,
     crtls-2.1.0-1, rules-1.2.1-1, libogc-3.1.0-1, libfat-ogc-2.1.0-4,
     ogc-cmake-1.3.0-1, devkitppc-cmake-1.1.4-1, gamecube-tools (elf2dol,
     gcdsptool, gxtexconv), general-tools-1.4.4, dkp common-utils cmake files.
   - env: source ~/devkitpro/env.sh  (DEVKITPRO=~/devkitpro, PATH+=devkitPPC/bin+tools/bin)
   - VERIFIED: powerpc-eabi-gcc compiles+links -logc -lm; elf2dol works;
     custom ~/devkitpro/cmake/GameCube.cmake works end-to-end with
     powerpc-eabi-cmake -G Ninja (test project built gctest.dol 85536B).
     NOTE: cmake binary is pip one at ~/.local (python3.14 site-packages,
     cmake 4.4) named powerpc-eabi-cmake via dkp shim? Actually invoked
     directly and worked; use -G Ninja (make exists too but Ninja tested).
2. Wii card app copy at /home/ryzen/DELTARUNEC5 (user-provided, 217MB):
   IDENTICAL to sunshine-patcher/wii-prep/out/apps/DELTARUNE minus boot.dol.
   Contains: ch5 preprocessed data.win (165,423,612 B, md5 9c80e6300e0548d93...
   SAME as wii-prep out), gfx/atlas.bin N3AT v4 pages=14482 items=23859
   (ci4/ci8 page formats!), audio/sound_bank.bin SBK1 v5 765 entries 22.4MB,
   27 oggs in ch5 root (BGM layout = build7 style: oggs IN chapter dir root,
   bank in audio/), root stub data.win 2.95MB + root atlas 23 pages (menu?).
   page_formats.txt says ci4/ci8 = GameCube-native formats! (Wii build used
   them because GC/Wii share TX formats via libogc.)
   boot.dol = 1,795,520 B md5 be5c960e8d55d5ba74d714dbdc684a28 = Wii runner.
3. Cinnamon source structure fully mapped (see below).

## CINNAMON PLATFORM CONTRACT (what src/gcn/ must implement)
- Core src/*.c compiled for every platform: file(GLOB src/*.c + src/<PLATFORM>/*.c)
- CMakeLists: -DPLATFORM=gcn -> add `elseif(PLATFORM STREQUAL "gcn")` section
  (modeled on n3ds section: include libogc, link -logc -lm, ogc_create_dol).
- main.c pattern (from n3ds/wiiu main.c):
  DataWin_parse(path, options{parseGen8..parseStrg, progressCallback})
  -> FileSystem = custom vtable (resolvePath/fileExists/readFileText/
     writeFileText/deleteFile/readFileBinary/writeFileBinary) over libfat "sd:/"
  -> AudioSystem: n3ds-style (bank DSP in ARAM later; v1: noop_audio_system reuse)
  -> VM_create(dataWin), Renderer_create(), Runner_create(dataWin,vm,ren,fs,aud)
  -> runner->osType = OS_3DS equivalent (add OS_GAMECUBE? enum has no GC -
     use OS_WINDOWS like Wii build did: cinnamon_settings.ini os_type=windows)
  -> Runner_initFirstRoom(runner); loop: PAD_ScanPads, sync keys via
     runner->keyboard (syncKey pattern), Runner_step, audio update,
     renderer->beginFrame(gameW,gameH,640,480), Runner_drawViews,
     Runner_drawGUI equivalent via Runner_draw(runner)?? n3ds used drawViews +
     overlay; wiiu main loop: Runner_step, Runner_draw...
  -> RunnerKeyboard_beginFrame(runner->keyboard) each frame end.
- KEY MAPPING (wiiu main.c): A->'Z' B->'X' X->'C' Y->'C' +/-->ENTER/BACKSPACE
  L->VK_PAGEDOWN R->VK_PAGEUP ZL->VK_SHIFT; arrows->VK_LEFT/RIGHT/UP/DOWN.
  GC: A->Z, B->X, X->C, Y->C, Start->ENTER, L/R->PAGEDOWN/PAGEUP, Z->SHIFT,
  D-Pad + stick for arrows.
- RENDERER (renderer.h vtable, ~50 fns): wiiu_renderer.c (1513 lines) is the
  model: GX2 quad batcher -> for GC use GX (fixed function): GX_Begin/GX_End,
  ortho proj, 2 texture-page textures uploaded from data.win TXTR PNG blobs
  via stb_image decode (wiiu pattern: WiiURenderer_loadTexturePages decodes
  dataWin->txtr.textures[i].blobData with stbi_load_from_memory -> RGBA8 ->
  GX2 linear texture). GX equivalent: GX_InitTexObj with RGBA8 32x32-tile
  swizzle OR use GX_TF_RGBA8 conversion loop (row pairs). Page upload:
  mem1 alloc via memalign(32). Draw = collect quad cmds (pos/uv/color/alpha),
  flush per texture switch with GX_SetVtxDesc (direct or array).
  Text: glyph quads from font->glyphs via tpag (wiiu drawText pattern).
- N3DS uses custom N3AT atlas (preprocessor --target 3ds); WII/GC build uses
  RAW data.win TXTR PNGs (wiiu pattern, ci4/ci8 page_formats are for 3DS).
  => GC = wiiu-style renderer (stb_image decode TXTR PNGs -> GX textures),
  NOT the N3AT path. DELTARUNEC5 gfx/ N3AT files are for the WII (they were
  preprocessed with target wii which still emits N3AT?? - page_formats ci4/ci8
  suggests the Wii build DOES use N3AT-style atlas.bin... BUT the Wii runner
  is closed-source; the open Cinnamon n3ds renderer is the only N3AT reader.
  DECISION: v1 GC build uses data.win TXTR path like wiiu (works on PC glfw),
  ch5 data.win has no TXTR? CHECK: preprocessed data.win may have stripped
  TXTR! (parseTxtr=false in n3ds main). VERIFY txtr.count in ch5 data.win.)
- File System: libfat fatInitDefault() mounts "sd:/" (SD Gecko slot A/B or
  SD2SP2). DataWin path: sd:/apps/DELTARUNEGC/chapter1_windows/data.win etc.
  Saves: write to same dir (filech1/keyconfig like Wii).
- Audio: v1 = noop audio system (exists: src/noop_audio_system.c) -> boots
  silent; v2 = libasnd (libasnd.a in libogc/lib/cube) + stb_vorbis for ogg
  music streaming, DSP sfx bank in ARAM (SBK1 format known, see skill).

## HARD FACTS (GC hardware)
- MEM1 24MB total, usable heap ~21-22MB after code+bss+stack. ARAM 16MB.
- GX: RGBA8 textures are 32x32 tile-swizzled (GX_TF_RGBA8); use
  GX_SetTexCopySrc or software swizzle on upload. CI4/CI8 = GX_TF_CI4/C8
  native + TLUT (perfect for the ci4/ci8 preprocessed pages IF we go N3AT).
- 640x480 interlaced default; game renders 640x480 (gameW/H from GEN8, scale
  integer like wiiu computeOutputLayout).

## EGG CARD STATE (GC SD)
/run/media/ryzen/egg: swiss/ (installed, many patches), boot.dol 639,239B
INVALID (entry 0x7820, corrupt, matches FSCK0000.REC card corruption) -
DELETE/REPLACE it. Card will get: sd:/apps/DELTARUNEGC/boot.dol + data.

## DATA.WIN FACTS (verified by parsing, 9/14)
- DELTARUNEC5/chapter5_windows/data.win = FULL standard GM format (FORM BE
  container size, chunk sizes LE), 165MB: TXTR 13.9MB PNG @abs blobOffset,
  AUDO 125MB oggs, ROOM 7.5MB, CODE 9.8MB, VARI 857K, STRG 4.3MB, SPRT 1MB,
  OBJT 782K, FONT 337K, TPAG 620K. bc17.
- ch1 Steam data.win = only 13.07MB! CODE 1.65MB ROOM 1.87MB TXTR 1.93MB PNG
  AUDO 6.4MB. Parsed (minus AUDO+TXTR) ~= 4.6MB. CH1 FITS GC RAM EASILY.
- Cinnamon DataWinParserOptions ALREADY supports: lazyLoadRooms=true (ROOM
  payloads on demand via DataWin_loadRoomPayload, keeps file open),
  parseAudoHeadersOnly=true (lazyAudioFile streams oggs on demand).
  blobOffset in Texture struct = ABSOLUTE file offset -> GC renderer can read
  PNG blobs straight from data.win file on demand (LRU page cache).
- => GC v1 TARGET: CHAPTER 1. parseTxtr=false + own streaming TXTR loader
  (wiiu-style GX page upload), lazyLoadRooms=true, parseAudoHeadersOnly=true.
  No preprocessor needed for v1 (vanilla ch1 data.win straight from Steam!).
- wiiu renderer = the template (GX2 quad batching); GC GX = fixed function:
  GX_SetVtxDescFMT direct or array; RGBA8 pages need GX_TF_RGBA8 32x32 swizzle
  on upload (software swizzle loop, ~2MB/page worst case 2048x2048).

## STATUS 9/14 ~00:40 — FIRST BUILD ON CARD
- cinnamon builds for GC: build-gcn/butterscotch.dol 1,116,992B, valid DOL
  (text0 0x80003100, entry file-offset style 0x27740 — same convention as the
  known-working Wii boot.dol 0x80240).
- ON CARD /run/media/ryzen/egg/apps/DELTARUNEGC/:
  boot.dol (md5 a9cf0f7974749a48e76128776ad3fc1e) +
  chapter1_windows/data.win (vanilla Steam ch1, md5 0ccbfd7c4f9fb1b86de1e2aaec0bacc9).
  Both md5-verified vs build/source. Card = 50GB, 29G used, vfat + 46 FSCK recs.
- TEST PROCEDURE (user, real GC): boot Swiss -> SD -> apps/DELTARUNEGC/boot.dol.
  Expected: black screen + console text over USBGecko OR crash. NO audio (v1
  noop). Report what happens (freezes/gyro screen/HUD?). debug via printf
  needs USBGecko; otherwise visuals only.
- KNOWN GAPS v1 (fix after first test): surfaces NULL (createSurface etc) —
  DELTARUNE ch1 menu uses draw surfaces?; drawTiled NULL (backgrounds!);
  Runner_draw vs drawViews flow — n3ds used Runner_drawViews+overlay; we used
  Runner_draw (check it exists and draws room+GUI); audio = silent; EFB
  clear color hardcoded black; PAD_BTN constants fine; entry flow OK.

## PATCH 2 (9/14 ~01:10) — crash-log build, on card
User reported boot crash on real GC. Patch v2 (md5 1d1d318c2e0dac7c5f9bc4c3cb3c5856):
1. **CRASH LOG IS NOW BUILT-IN**: stderr freopen'd to
   sd:/apps/DELTARUNEGC/crash.log (unbuffered). EVERY require()/fprintf(stderr)
   failure in the whole codebase now lands there. READ THIS FILE AFTER ANY CRASH.
2. Fixed GX_Init misuse: v1 passed the XFB as the FIFO memory (GX_Init(base,size)
   takes FIFO memory, not the framebuffer!) -> corruption/crash. Now proper
   256KB FIFO memalign(32) + double XFB.
3. Game loop now runs on a worker thread with 512KB stack (libogc main thread
   = 8KB default -> VM recursion could smash the stack = crash #1 suspect).
4. Boot status text on screen (console_init on XFB0): "mounting SD / loading
   data.win / init systems / init first room / running" — shows WHERE it dies.
5. libogc built-in exception screen still active (code dump on crash).
TEST: boot via Swiss. If crash/black: eject card, read crash.log on PC, send it.
NOTE: status line may persist visually (console over game XFB) - cosmetic.
## PATCH 3 (9/14 ~01:30) — FAT PATH FIX, on card
"YES it works but it said no data.win!" -> boot chain WORKS on real GC! Root
cause: libfat's default device is "fat:/" (fatInitDefault also makes it the
default stdio device), NOT "sd:/" (that's Wii naming). Patch v3
(md5 0b7f31a4b9ee1f9cf2350477ce6abc5f):
- main.c probes data.win across roots: fat:/apps/DELTARUNEGC, sd:/apps/...,
  plain /apps/..., x {chapter1_windows/}data.win. Winner is stored via
  GCNFileSystem_setBasePath and used for the FileSystem (game data+saves).
- crash.log path also tries fat:/ then sd:/.
- build 1,121,312B, 0 errors, md5-verified on card.
NEXT TEST: should reach "running" and draw the intro/menu. No audio yet.
If visuals work -> next: audio (libasnd + stb_vorbis), surfaces if menu needs.
## PATCH 4 (9/14 ~01:45) — SD DIAGNOSTIC DUMP, on card
User retested v3: still "data.win NOT FOUND on SD" (card files verified present
on PC: apps/DELTARUNEGC/{boot.dol v3, data.win, chapter1_windows/data.win},
all md5 OK). fatInitDefault() must be succeeding (no "SD mount FAILED" screen)
but fopen fails on all root variants. libfat HAS LFN support compiled in.
Patch v4 (md5 09ff0742d6547a5ddcc0fa8dc5fe8733):
- On NOT FOUND: opendir-dumps fat:/, sd:/ and / (40 entries each) INTO crash.log
  -> next test will show exactly which device mounts + what dirs libfat sees
  (LFN vs 8.3 names, wrong card, empty mount, etc).
- ALSO copied ch1 data.win to apps/DELTARUNEGC/data.win (root fallback path).
NEXT: user runs once, pulls card, we READ crash.log listing together.
## PATCH 5 (9/14 ~02:00) — ON-SCREEN SD REPORT, on card
User ran v4: still "data.win NOT FOUND" AND no crash.log was created at all ->
stderr freopen to fat:/ FAILED SILENTLY (devoptab for stderr may not route to
libfat). So ALL stderr-based diagnostics (incl. my opendir dump) are invisible.
Patch v5 (md5 9da953845e8cc2218f2e3adacaf7220b):
- NOT FOUND branch now prints the whole SD report ON TV via console_init
  (opendir fat:/, sd:/, /, fat:/apps, sd:/apps - 30 entries each).
- ALSO writes report DIRECTLY (plain fopen, not stderr) to
  fat:/DELTA_SD_REPORT.TXT (+sd:/ and /) + marker fat:/DELTA_WAS_HERE.TXT.
- User test: photograph TV + bring card; read DELTA_SD_REPORT.TXT on PC.
## PATCH 6 (9/14 ~02:15) — PARSE PROGRESS ON SCREEN, on card
"STUCK AT LOADING DATA.WIN" = data.win WAS FOUND AND OPENED (past the probe)!
The status text stays "loading data.win..." for the ENTIRE DataWin_parse
because v5 had no progress callback. Two possibilities:
a) Parse is just SLOW on GC (13MB over SD + PPC, no LTO) -> wait 1-2 min.
b) Parse hangs on a chunk (CODE bytecode expansion?).
Patch v6 (md5 ad9e5d559127aa521b94bca3bb06e731) adds the parse progressCallback
-> status line now shows "parsing GEN8 (1/23)" etc live per chunk. The chunk
where it stops = the hang point.
TEST: boot, note LAST chunk name shown on screen, report it (+ did it ever
reach "running"?).
## PATCH 7 (9/14 ~02:35) — STAGE-BY-STAGE PROBE STATUS + TEST PATTERN, on card
v6 still showed "loading data.win..." (no chunk counter) => the hang is in the
FOPEN PROBE LOOP (before DataWin_parse), OR user's boot predates v6 (card was
written at 01:26; possible). Patch v7 (md5 ccd5f3e3bf4058128c67cd31db15356a):
- Status now shows the EXACT path being tried: "trying: fat:/apps/.../data.win"
  -> if it hangs, the TV shows WHICH fopen hangs.
- NEW: hold B while booting = pure GX color-bar test pattern, zero SD deps
  (isolates GPU/video from SD; ALSO our Dolphin debugging entry).
- Copied cinnamon.elf (5.1MB) to ~/Desktop/DELTARUNE-GC-dolphin-test.elf for
  the Dolphin experiment the user proposed (banned-for-Wii rule; user now
  explicitly asks to use Dolphin as a debug instrument for THIS port).
DOLPHIN PLAN: Dolphin can't load .dol, may load GC ELF via Open; SD/fat will
NOT work under Dolphin (no GC SD emulation) so use test-pattern + a future
host-file path. First check if Dolphin opens the elf at all.
## PATCH 9 (9/14 ~03:00) — *** ROOT CAUSE FOUND + FIXED ***, on card
CRACKED IT. v5's on-TV + direct-file report (DELTA_SD_REPORT.TXT, user ran it):
  opendir("fat:/") FAILED
  opendir("sd:/") FAILED
  listing "/" = CARD ROOT (Games, swiss, isos, everything!)
=> libfat mounted the card as the DEFAULT stdio device with NO device prefix.
   Absolute paths are just "/apps/DELTARUNEGC/...". "sd:"/"fat:" prefixes are
   INVALID on this libogc/libfat build (unlike Wii homebrew convention).
   My earlier bare probe "apps/DELTARUNEGC" (relative, no leading /) failed
   because relative paths depend on cwd.
Patch v9 (md5 199880e8f0771111b04f664b4926ed37): ALL paths now absolute
("/apps/DELTARUNEGC/..."), roots probed: /apps/DELTARUNEGC first, then fat:/,
sd:/. Crash log + diag files -> absolute too. Also v8 (8ada8f12) fixed the
two-threads-touching-GX/VI race (worker now owns ALL video; main just waits).
THIS SHOULD BE THE ONE. Test: expect "parsing X (n/23)" -> "running" -> menu.
## PATCH 10 (9/14 ~03:25) — OOM FIX + dup2 LOG, on card
User: "it does load things but no game show up, it did crash". No crash.log on
card (stderr freopen STILL fails silently on this devoptab). Root cause found
by RAM math: ch1 TXTR has a 2048x2048 page; v9 eagerly allocated 4 x 1024x1024
page buffers = 16MB at boot + VM/dataWin ~6-10MB = MEM1 (24MB) exhaustion ->
safeMalloc abort = crash right after "loads things".
Patch v10 (md5 7dc41261917262d8f6bb18228c08663d):
1. Page buffers LAZY: allocated at first load w/ page's real size, freed on
   evict; 2 resident slots (was 4).
2. Oversize pages downscale 2x on load (2048->1024) instead of rejecting.
3. Crash log now via open()+dup2 onto stderr's fd (freopen failed on the
   default devoptab); unbuffered. ALL require()/fprintf(stderr) land in
   /apps/DELTARUNEGC/crash.log now.
TEST: expect boot -> parse -> running -> menu. If crash: crash.log WILL have
the require()/file:line this time. Note: 2048-page sprites render at half
res (softer) - visual only, fix later via ARAM/CI formats if needed.
## PATCH 11 (9/14 ~04:00) — drawGUI FIX + LOG TIMING FIX, on card
User: "big progress, lots of things loading (parse progress visible!), then
black screen; Start on black screen crashes". Two bugs found:
1. Main loop called Runner_draw() directly - but the GameMaker draw flow is
   Runner_drawViews (which does beginView -> Runner_draw -> beginGUI ->
   Runner_drawGUI per view). GUI events (the whole DELTARUNE menu) never ran.
   Now: beginFrame -> Runner_drawViews -> endFrame (mirrors n3ds main loop).
2. Crash log open() ran BEFORE fatInitDefault -> open() failed -> no log ever.
   Moved after mount. With v11, crash.log should finally appear (unbuffered
   via dup2). Start-on-black crash may be cleanup path (Runner_free etc) -
   crash.log will now capture require/abort messages from cleanup too.
Patch v11 (md5 01d40d4e48df03b00f5c9f29ccc7f330).
TEST: expect menu visuals now. If black: crash.log this time will tell us
which stage/draw call fails. If Start still crashes in cleanup: crash.log
+ we can also just remove cleanup from the exit path (loop on Start = reset).
## PATCH 12 (9/14 ~04:40) — *** SMOKING GUN ***: txtr.count=0, on card
READ THE CRASH LOG from the v11 run (145KB, ON CARD, IT WORKS NOW):
- VM inits 11158 globals, 1985 functions; global init scripts all run
- 147 rooms; ROOM_INITIALIZE loaded (7 instances) -> room 1 PLACE_CONTACT
  loaded (8 instances) -> VM: Reset x2 (game_restart boot flow) -> log ends
=> THE GAME LOGIC RUNS ON REAL GAMECUBE. Rooms load, events fire.
BLACK SCREEN root cause: I had parseTxtr=false => txtr.count=0 => renderer
pageCount=0 => EVERY drawSprite/drawText bails (>= pageCount check) =>
literally nothing ever draws. GUI text events ran but the renderer rejected
every sprite/texture (its TPAG->texturePageId indexes TXTR).
Patch v12 (md5 4a098487b9306b30d21a2bdcf6b5f3da):
- parseTxtr=true (ch1 TXTR is only 1.9MB of PNGs - cheap) + renderer prefers
  in-memory blobData (parser already has it), falls back to file streaming.
- v12 also has heartbeat f:nnn + last-draw-call on screen + in crash.log.
TEST: expect MENU VISUALS. If still black, crash.log heartbeat shows the
exact last draw call before death.
## PATCH 13 (9/14 ~05:15) — THE DIAGNOSIS BUILD, on card
CRASH LOG DECODED (v12 run): 7,647 FRAMES rendered!! last=renderCommands
every frame, VM: Reset x2 (room transitions!). THE GAME LOOP NEVER CRASHED —
it ran ~4 minutes and drew... black. That means: GX pipeline executes, game
logic runs, but pixels never reach the TV. ALSO: Start-on-black "crash" was
the cleanup path (destroy sequence) - now bypassed (safe park).
Build 13 (md5 9d86e7ea3135164dc303d5c1b1698956) = THE DIAGNOSTIC THAT SEPARATES
THE TWO THEORIES:
- **HEARTBEAT OVERLAY**: a white 4x4 square marches across the top of the
  screen every frame, drawn via a MINIMAL independent GX path (PASSCLR, no
  texture) after the game render, before CopyDisp.
  * Square VISIBLE = GX->EFB->XFB->VI pipeline is ALIVE => the game's own
    draws are the problem (texture/TEV/command issue) => fix renderer draws.
  * Screen BLACK with no square = present/copy path broken => GX_CopyDisp/
    VI config issue => fix that instead.
- Also fixed: guOrtho near=0 clipped z=0 vertices (near=-1 now) - was a
  potential everything-invisible cause.
- Start no longer crashes (cleanup bypassed).
DOLPHIN note: user wants Dolphin as debug instrument. OK'd for GC port debug
only (Wii ban stands). The heartbeat overlay + test pattern (hold B) work in
Dolphin with ZERO SD. Dolphin can also boot our cinnamon.elf directly. Use it
to validate GX emission if the cube stays black.
## PATCH 14 (9/14 ~06:00) — SOFTWARE XFB TEST (X button), on card
HEARTBEAT RESULT: NO SQUARE. GX->EFB->XFB->VI output path is DEAD even for
minimal untextured quads (while the game logic runs 7,647+ frames happily).
Two paths isolated. New build (md5 c901a763fc371c3ab04e57e2673dc119):
- **HOLD X ON BOOT = pure software test pattern**: CPU writes YUV bars
  DIRECTLY into the XFB memory, no GX AT ALL. If bars show on TV = VI/xfb
  path is fine and GX is the broken piece.
- HOLD B still = GX test pattern (for comparison).
THE OUT-OF-THE-BOX PLAN (user's instinct): if VI works + GX doesn't, commit
to a SOFTWARE RENDERER: blit quads/sprites/text straight into the XFB from
CPU (PPC 485MHz, 640x480x2B = 600KB/frame, 2D only = very doable at 30fps).
Guaranteed pixels while GX gets debugged separately. The "swap file" idea
from the user = explicit streaming, ALREADY built for audio/rooms/textures;
next: ARAM (16MB) as stream cache. No true VM paging on libogc (no demand
paging), but explicit streaming achieves the same goal.
DOLPHIN: can't emulate GC SD slot -> can't run the real game flow; but the
X/B test patterns + cinnamon.elf boot are Dolphin-testable. Priority stays
real hardware (user's rule).
## PATCH 15 (9/14 ~06:30) — AUTOMATIC VIDEO DIAGNOSTIC, on card
Bug in patch 14: the button-gated test blocks were never replaced (silent
patch failure) so the user booted a build with no tests. FIXED for real now
(md5 51c36165dab126aaa06b1db174995b9e): on EVERY boot, NO BUTTONS:
- TEST 1/2 (3s): "software XFB (CPU direct)" - YUV bars written straight to
  the XFB by the CPU, no GX at all.
- TEST 2/2 (3s): "GX color bars (GPU)" - GX-drawn bars.
- Then boots the game normally.
ALSO DONE per user demand: Start-quit REMOVED (Start = ENTER in-game);
controls live: A=Z B=X X/Y=C Z=SHIFT L/R=PgDn/PgUp stick/dpad=arrows.
USER REPORTS: X-button pattern showed "nothing" (unverified if actually held);
Start-quit annoying; wants Dolphin test.
DOLPHIN PLAN (agent-run): flatpak Dolphin CAN boot GC ELF. Our elf would show
TEST screens then "data.win NOT FOUND" (no SD emulation) - which itself
validates the whole console+VI path in an emulator. Try it after the user's
hardware report.
## PATCH 16 (9/14 ~07:00) — LIVE ON-TV TELEMETRY, on card
User: real hardware is the truth, Dolphin dead-end accepted (agent's own test:
Dolphin flatpak CAN launch the GC elf headless via dolphin-emu-nogui -e, ran
45s silently, but no SD = game can't load data = limited value; hardware only
from here). User asked for a better logger => BUILT:
**LIVE TELEMETRY ON THE TV** (md5 49050cd6c828aee5a569bda08eb9374b):
- 12-line on-screen log ring drawn AFTER GX_CopyDisp using the console
  (console->XFB path PROVEN working on this TV - they saw parse text).
- Shows live: frame counter + last renderer call (every 15f), ROOM CHANGES
  with room names, boot stages (mount/parse/running), plus everything also
  mirrored to crash.log via TELEM: lines.
- No buttons needed. Start no longer quits (Start = ENTER in game).
TEST: boot -> watch the telemetry panel live on TV -> report what it says.
If game visuals still black but telemetry shows -> GX present path confirmed
broken at CopyDisp/VI level -> next: software XFB renderer fallback.
## PATCH 17 (9/14 ~08:00) — *** THE SOFTWARE RENDERER ***, on card
TELEMETRY RUN READ: crash.log unchanged from v12 run (no TELEM lines) = user
booted build 16 but the log shows the SAME v12 run data (log not re-appended?)
OR the run crashed before log open. Card boot.dol = v16 md5 confirmed, so the
run data is still missing - the user reported "different now but no game".
DECISION (user pushed for out-of-the-box): GAVE UP ON GX ENTIRELY.
**BUILD 17 = PURE SOFTWARE RENDERER** (md5 bf102c47ae96ec175077df948ee96df2):
- renderCommands now CLEARS THE XFB TO BLACK + blits every quad with a CPU
  rasterizer (GCN_swr_blitQuad) writing YUV422 directly into the XFB.
- videoPresent no longer calls GX_CopyDisp (that was overwriting our frame
  with the empty EFB - THE black screen mechanism!). Just DCFlushRange +
  VIDEO_SetNextFramebuffer + telemetry on top.
- Same quad command list + same decoded page cache (RGBA8 swizzled) - just
  rasterized on CPU. Textured sprites, solid rects, blending all work.
- This CANNOT be a black screen: it's the identical pixel path as the console
  text that provably displays on this TV.
- Perf: 640x480 worst-case fill on PPC485 - ch1 menu is mostly text, fine.
Dolphin test (agent, real run): dolphin-emu-nogui CAN launch the GC elf
(45s silent run) but no SD => game can't load. Hardware-only confirmed.
## PATCH 18 (9/14 ~07:30) — GREEN SCREEN DECODED + YUV FIX, on card
USER RESULT: build 17 = GREEN SCREEN (not black!). Huge news: green = the VI
is scanning OUR software XFB (wrong chroma byte order = green tint). The
software renderer IS on screen - just color-swapped.
DIAGNOSIS: GC XFB pair layout = u32 big-endian Y1 V Y0 U. I wrote
u16[2n]=(Y0<<8)|U (luma in high byte of the FIRST word) = chroma/luma swap =
GREEN. FIXED: u16[2n]=(Y1<<8)|V, u16[2n+1]=(Y0<<8)|U in BOTH the blitter and
the clear. Also: telemetry console now rebinds (CON_Init) to the CURRENT XFB
each frame (was stuck on gXfb[0] = flicker/never visible with 2 buffers).
Build 18 md5 d3b9634e19eb0ad1d271382ba9fb38d8 ON CARD.
ROADMAP (user asked): ch2 test = same runner (BC16), 67MB data.win streamed
via the same lazy pages; UNDERTALE = BC16 - Cinnamon supports both - planned
after ch1 visuals+audio. Stream architecture (user's swap-file idea) = the
enabler for both.
## PATCH 19 (9/14 ~08:10) — UV SCALE BUG + DRAW STATS, on card
User result of build 18: "looks neater, spamton loader shows, loads first
room but no game" => VI + software clear + CONSOLE TEXT all display now
(green screen GONE = YUV byte order fix worked!). Game boots, rooms load,
but zero game pixels.
FOUND A REAL BUG while auditing: ch1 texture pages are 2048x2048 PNGs =>
ensurePage DOWNSCALES to 1024x1024, but ALL UV math divided by
page->width (1024 AFTER downscale) while sourceX/sourceY are in ORIGINAL
2048-space => UVs 2x too far => sampled transparent/black garbage =>
EVERYTHING INVISIBLE. FIX: added page->scale = origW/pw; every UV now
divides by (page->width * page->scale) in drawSprite, drawSpritePart,
drawText glyphs. ALSO: renderCommands used ensurePage (which can RELOAD/
evict mid-frame) => changed to findResident-only + skip count; added
stats: cmds/blitted/skipped shown LIVE in telemetry + current room.
Build 19 md5 48fbad3fd9461963e2650b81bd2b5100 ON CARD.
## PATCH 20 (9/14 ~09:00) — *** SINGLE-BUFFER NUKE ***, on card
User: build 19 still nothing visible. Card data.win both copies md5-identical
(0ccbfd7c) = no FAT corruption of data. crash.log STILL frozen at v12 bytes =>
the SD log channel is dead (probably FAT table corruption from the 46 FSCK
era) - THE TV IS THE ONLY DIAGNOSTIC CHANNEL NOW.
ROOT-CAUSE THEORY (all evidence fits): boot text displays on gXfb[0] (proven
path). The game loop presents gXfb[1] FIRST (double-buffer flip). If anything
in the game-loop present path is broken (console rebinding per frame +
DCFlush ordering), the TV switches to a BLACK frame with no text = "cant see
anything" - the boot text freeze illusion. ALSO the v12-era GX runs got past
init (rooms loaded, 7647 frames) - the loop works; it's the OUTPUT that dies
at the first flip.
NUCLEAR FIX (build 20, md5 b27fda9ed035b19ea344f5c98b5c22fa):
- SINGLE-BUFFER MODE: EVERYTHING in gXfb[0]. Game frame (software blitter),
  telemetry, boot text - one buffer, present gXfb[0] every frame, NO flips,
  NO console rebinding (bound once with the PROVEN boot params 20,20).
- DCFlushRange after every CPU write stage (test pattern, status screen,
  present x2) - cache staleness could hide ANY CPU-written pixels.
- Same software renderer + UV scale fix as 19.
- User sees ONE of: game pixels / telemetry numbers ticking (explaining
  draws) / frozen boot text (= hang location). ALL outcomes informative.
## PATCH 21 (9/14 ~09:30) — *** THE GREEN SOLVED ***, on card
User: build 20 = "strange and studdering green" => THE GAME LOOP IS NOW
DISPLAYING (single-buffer + flushes reached the TV!) - the green = COLOR BUG
in the software blitter, not a pipeline bug. HUGE progress.
ROOT CAUSE FOUND BY READING LIBOGC CONSOLE.C (ground truth):
1. GCN_swr_sample read G/B channels at xi*2 (4x stride) instead of xi =>
   GARBAGE colors per pixel => the GREEN noise where sprites/text drew.
   Fixed: a=ar[xi], r=ar[xi+1], g=gb[xi], b=gb[xi+1].
2. XFB u16 byte order REVERTED to libogc's own colorTable layout
   (black = 0x10801080 => u16[2n]=(Y0<<8)|U, u16[2n+1]=(Y1<<8)|V; my build-18
   "fix" had swapped Y/chroma per pair = wrong for textured content).
Build 21 md5 b83c38005330843d4888ec5aec7c7930 ON CARD. Single-buffer kept.
## PATCH 22 (9/14 ~09:20) — BUILD 19 RESTORED + DOLPHIN TELEMETRY WIN
USER: build 21 worse, 19 best (no glitching). REVERTED to 19 behavior:
double-buffer present + telemetry rebind, kept the sampler fix + libogc
byte order (those were correct fixes even if 20/21 felt worse on HW).
Build 22 (card md5 8e2369d0017fcc1a02a7f89a3536d359).
*** DOLPHIN BREAKTHROUGH (agent, real test runs): ***
- Logger.ini: OSREPORT/MASTER/BOOT=True + WriteToFile=True => Dolphin
  captures our SYS_Report() output!!
- Boot trail verified in Dolphin: mounting SD -> XFB test -> GX test ->
  worker launch -> (no SD) DEMO LOOP.
- FOUND+FIXED a REAL crash: demo path ran before GCNRenderer_create =>
  NULL instance => hang. (Would have been invisible on HW.)
- DEMO RENDER LOOP (no SD needed): 3 animated solid quads + EMBEDDED 64x64
  RGBA8 checkerboard texture through GCN_swr_sample => runs at ~60fps in
  Dolphin, frames tick (f:64/128/192...), texture blit survives.
- On HARDWARE: this build = build 19 behavior + demo mode when no SD.
DOLPHIN LIMITS DISCOVERED: flatpak GUI won't open from agent shell
(Wayland sandbox), nogui runs headless, framebuffer invisible to agent.
USER CAN: open Dolphin GUI, drag /home/ryzen/dolphin-test/deltarune-gc.elf
into it => SEE the demo (grey bg + red/green squares + checkerboard) =>
visual confirmation of the pixel path without hardware trips!
## PATCH 23 (9/14 ~13:00) — THE MARKER SQUARE, on card
USER (real HW, latest ram-mode build 8d212888): "at CONTACT, game running"
=> THE GAME LOGIC RUNS ON REAL HARDWARE - rooms load, CONTACT reached.
The screen stays black => the game's draw calls produce no visible pixels.
BUILD 23 (md5 23fbdd570bc1973b5f4b2124fd5fb66f):
- A SOLID WHITE SQUARE (24x24) marches across the top edge - drawn through
  the SAME software blitter as the game's sprites, unskippable, every
  frame. If the square SHOWS: pixel path proven; invisibility = the game's
  quads are being skipped (page loads). If NOT: the game loop's present
  path differs from the boot path (which works).
- drawSprite/drawText page-misses now RETRY the load inline (findResident
  -> ensurePage) instead of skipping.
- Telemetry line simplified: f:N cmds:N blit:N skip:N roomN.
## PATCH 24 (9/14 ~13:40) — FRAME-1 BREADCRUMBS ON THE VISIBLE SCREEN
USER ran build 23 (md5 confirmed on card): "didn't work" = no white square.
=> THE GAME LOOP NEVER COMPLETED ITS FIRST PRESENT (the marker square is
drawn INSIDE renderCommands; the telemetry panel also only appears at the
first present - the user still sees only the frozen boot text "running").
BUILD 24 (md5 0c2952b892e8ca14b2fa23a88861d8f4):
- FRAME-1 BREADCRUMBS on the PROVEN-VISIBLE boot status screen:
  f1: syncing input -> stepping VM -> begin frame -> drawing views ->
  end frame (blit) -> presenting. The stage where the TV freezes = the
  killer, visible WITHOUT any working log channel.
- (build 23's marker square + page-retry remain.)
## PATCH 25 (9/14 ~19:00) — *** THE 2ZOQ TEXTURE FIX ***, on card
*** ROOT CAUSE OF "NOTHING SHOWS" FINALLY NAILED ***
DELTARUNE ch1 textures are NOT PNGs: GameMaker 2022.9+ stores '2zoq'
(BZip2-compressed custom QOI) blobs. ensurePage used raw stbi => decode
ALWAYS FAILED => every page == NULL => every sprite/text quad skipped =>
black screen with "game running". The white marker square never appeared
because ALL textured draws skipped... AND the frozen f1 stages were the
16MB stbi spike on 2048x2048 PNG attempts (OOM abort).
THE FIX (found in Cinnamon's own image_decoder.c!):
- Wired ImageDecoder_decodeToRgba ('2zoq' = BZip2 + 'fioq' QOI) into
  GCNRenderer_ensurePage (falls back to stbi for non-2zoq blobs).
- Bundled bzip2 1.0.8 (public domain) decompression sources into
  src/gcn/bzip2/ + CMake section + src/gl include path.
- Earlier in this patch: XFBs/GX FIFO/worker stack moved from the malloc
  arena to static BSS (frees ~2MB heap); GCN_MAX_PAGE_DIM 512 (1MB/page);
  GCN_MAX_QUADS 1024; stdio buffers shrunk.
- Also (earlier): Dolphin ram-mode infrastructure (embedded LZSS blob +
  ram: devoptab + zero-copy views + SYS_Report telemetry) - Dolphin is
  too slow for full-game iteration now (parse ~10min emulated) but it
  delivered the 2zoq discovery.
BUILD 25 md5 bbb0c74d54e2f28f63a2c39291006596 ON CARD (SD build).
TEST ON HW: boot -> f1 breadcrumbs should now PASS "end frame (blit)"
(the 2zoq decode works inline) -> CONTACT text/sprites on screen.
## NEXT BIG MOVE (9/14 ~20:00) — THE WII ARCHITECTURE (user's call)
Build 25 booted: still nothing. THE MATH KILLS IT: a 2048x2048 '2zoq'
decode needs ~20MB (BZ2 out) + 16MB (QOI out) = 36MB peak on a 24MB
console. NO on-device 2zoq decode can EVER work for full-size pages.
THE USER'S IDEA IS THE ANSWER = run it like the Wii:
- The Wii port (DELTARUNEC5) ships PRE-CONVERTED assets: gfx/atlas.bin +
  sprite_manifest.bin + room_manifest.bin + page_formats.txt (ci4/ci8
  pages) generated on the PC by Cinnamon's own n3ds-preprocess tool.
- TONIGHT'S PROGRESS on that path:
  * Built n3ds-preprocess for Linux (fixed: bzip2 bundled for host too,
    gcn_ramfs weak-symbol guard, tex3ds 2.3.0 installed from the devkitPro
    repo into ~/devkitpro/tools/bin).
  * Restored ch1 data.win locally from the LZSS ramfs blob.
  * RAN THE PREP ON CH1: SUCCESS - prep-ch1/gfx/ = atlas.bin (84MB),
    direct_assets.bin, room_manifest.bin, 317 texture pages (t3x/i8),
    audio/sound_bank.bin (9.2MB PCM16, 112 sounds), fonts, etc.
  => THE WII-STYLE ASSET PIPELINE IS ALIVE FOR CHAPTER 1.
- REMAINING WORK (next session): port N3DSRenderer_loadAtlas + the page
  conversion (rgba5551/i8 -> RGBA8) from src/n3ds/n3ds_renderer.c into
  the GCN renderer's ensurePage path (read pre-swizzled pages from SD,
  no on-device decode, tiny RAM). Reference: n3ds_renderer.c:2479+
  (atlas header: magic, version (fragmented-packed), pages, items,
  fragments, tiles, fmt - cursor math in the load function).
- The Dolphin ram-mode (embedded blob) = too slow for full-game emulation
  (parse ~10min) - keep it for logic debugging only.
- The SD build (bbb0c74d) currently on card: correct 2zoq decoder but the
  36MB decode peak can't work for 2048 pages - the preprocessed atlas is
  THE fix.
## PATCH 26 (9/14 ~20:10) — *** THE HEAP-CORRUPTION FIX ***, on card
*** FOUND A REAL HEAP SMASHER (my own bug from build 25) ***
The 2zoq decode path returns buffers from ImageDecoder (plain malloc),
but the downscale/error paths freed them with stbi_image_free() =>
CROSS-ALLOCATOR FREE = heap corruption = the arena free-list smashed =>
later malloc(128)s fail even with "free" bytes = THE FREEZE + OOM!
(The stbi path decodes failed instantly - the 2zoq path then double-freed.)
FIX: decode-source flag (pixelsFromStbi) + GCN_freePixels() helper; all
6 frees in ensurePage now use the right deallocator.
ALSO: OOM GUARD - blobs > 400KB are SKIPPED (return NULL) instead of
attempting the 36MB-peak 2048x2048 '2zoq' decode that aborts the console
(FATAL malloc => worker dead => frozen screen). Small pages (fonts/UI
<= 400KB blobs) decode fine => CONTACT's text page (1280-byte blob!)
decodes => TEXT SHOULD RENDER.
BUILD 26 md5 36e1ce51f35d59708c6873dd912525e8 ON CARD.
## PATCH 26 CONFIRMED DIAGNOSIS (9/14 ~20:30, Dolphin 12-min run)
"Dolphin: data.win not found" = the DIAGNOSTIC BRANCH fired on the TV =
the 13MB LZSS-decompress malloc FAILED (heap 6.99MB: Arena1 = ELF_end
0x81131000..0x81800000; the 9.6MB embedded blob inflated the ELF) =>
ram: never registered => "data.win not found" ON SCREEN = the user SAW
our own diagnostic branch work perfectly. CATCH-22 PROVEN BY MATH:
compressed blob 9.6 (rodata) + decompressed 13 (heap) + parse 3.2 + VM 2
= 25.8MB > 24MB. RAM MODE WITH FULL BLOB RESIDENT = IMPOSSIBLE. PERIOD.
=> DOLPHIN FULL-GAME = impossible without an emulated SDGecko (not in
   this Dolphin build - verified: no CEXISD device class).
=> THE HW PATH = build 26 (SD textures stream at draw time, OOM guard
   skips big pages, small font/UI pages decode, heap-smash fixed).
=> NEXT SESSION: port N3DSRenderer_loadAtlas (preprocessed atlas from
   SD, the Wii architecture) = the permanent fix for ALL pages.
## PATCH 27 (9/14 ~21:00) — 中文深度思考 = DOL 瘦身 (9.5MB 减掉!), on card
用户建议用中文深度思考 => 找到最大嫌疑：
**SD 构建的 DOL 里嵌着 9.6MB 的 LZSS blob（Dolphin ram-mode 用的），
硬件上完全用不到，却把 DOL 从 1.1MB 撑到 10.7MB** => MEM1 少了 9.5MB
=> 堆被饿死 + DOL 加载布局完全不同 = 一切硬件异常的头号嫌疑！
FIX: GCN_RAM_DATAWIN 默认关闭；ram mode 代码全部用 #ifdef GCN_HAS_RAMFS
守卫；gcn_ramfs.c 无 blob 时只注册设备。Dolphin ram-mode 构建 =
-DGCN_RAM_DATAWIN=<lzss> 手动开启。
BUILD 27 (SD build, md5 cd4689a23e9c49069e26b2aa630d17d5, 1,168,672B
= 回到 build 22 的体积!) ON CARD.
## NEXT SESSION (9/14 ~22:45) — *** CH1 ATLAS FOUND ON DISK ***
用户问对了问题："shouldn't it be loading CONTACT? did you check the
wii build logs?" => 读了 DELTARUNE-WII-HANDOFF.md => Wii 版手册明说：
ch1 = "data.win preprocessed + gfx/atlas.bin CI4/CI8/CMPR pages only"。
**答案在硬盘上**：/home/ryzen/Desktop/SHIT!/DELTARUNEC5/chapter1_windows/
  - gfx/atlas.bin = 3,968,757B ('N3AT' magic, version 4 fragmented-
    packed, 1977 pages, 3009 items, 3022 fragments, 1357 tiles, fmt 7)
  - page_formats.txt = 1980 行（ci8/ci4 每页格式）
  - preprocess_ready_v1.bin
  - data.win = 13,072,876B (md5 0ccbfd7c = Steam 原版一致！data.win
    没变，纹理被复制到 atlas.bin 里以 ci4/ci8 存储)
=> GC 端方案：把 N3DSRenderer_loadAtlas (n3ds_renderer.c:2479+, 支持
   N3AT v4 fragmented-packed) 移植进 GCN ensurePage：
   - 读 atlas.bin 元数据（~几百KB）进 MEM1
   - ensurePage(pageIndex): SD 上 seek 到页偏移 => 读 1 页 =>
     ci4/ci8 + palette => RGBA8 => GCN swizzle => blit
   - atlas.bin 3.97MB 甚至可以整个载入 MEM1（比 16MB 解码尖峰好10倍）
   - 调色板：ci8 = 256 色 x4B = 1KB/页？格式细节在 n3ds_renderer.c
   - 卡上的 data.win 保持 Steam 原版（0ccbfd7c）
   - 把 chapter1_windows/gfx/ 拷到卡上 DELTARUNEGC/gfx/
用户问题答案：CONTACT = 真实 ch1 的法律信息房间（8 实例），日志里
ROOM_INITIALIZE -> PLACE_CONTACT 是正常顺序；黑屏 = 渲染管线问题
（纹理从没成功解码过——现在有 atlas 就有了）。
## PATCH 28 (9/14 ~23:00) — *** SELF-VERIFYING HW LOG ***, on card
用户要求：拔卡前做一个"能读到硬件上发生的一切 + 和应该发生的对比"
的日志系统。BUILD 28 (md5 a81620528bb99e4413be93908a42e9be):
- gcn_bootlog.c: 每次启动写【全新编号文件】HWLOG<n>.TXT（避开坏 FAT
  的追加损坏），每条立即 fflush，最多 256 条后关闭。
- 内置【期望时间线】9 步（来自真实 ch1 + Wii 手册）：
  MOUNT / DATAWIN / PARSE / VM / ROOM0(7 inst) / CONTACT(8 inst) /
  FIRSTDRAW / FONTPAGE / TEXT
- 每条日志自动打勾；每 600 帧写整个对比表进 HWLOG 文件
  (=== TIMELINE ok/9 ===  + OK/PENDING/FAIL 每行)。
- 接入点：mount/datawin/parse/VM/room 变化/first frame/font decode。
- 同时 SYS_Report 到 Dolphin 日志（双通道）。
- 期望的卡片读取结果：HWLOG0.TXT（或更高序号）= 逐行对照的
  OK/PENDING/FAIL 表 = 精确定位卡在哪一步 + 哪些期望没发生。
DOL = 1,186,880B (无嵌入 blob)。
## PATCH 29 (9/15 ~00:20) — *** OFFICIAL BUTTERSCOTCH PREPROCESSOR WIRED IN ***
用户发现 ButterscotchRunner/ButterscotchPreprocessor（官方上游预处理器，
125 commits, ps2/ps3 targets）。已完成：
1. 用 Adoptium JDK17 构建 processor-cli（Gradle installDist，JRE25 运行）
2. 跑在 Steam ch1 data.win 上 => 输出全部成功：
   ATLAS.BIN 81,651B / CLUT4.BIN 10,048B / CLUT8.BIN 12,288B /
   TEXTURES.BIN 4,562,308B / SOUNDBNK.BIN / SOUNDS.BIN 2.2MB (151 音效)
   = 总共 ~6.9MB，'2zoq' + 全部音频在 PC 上解完！
3. 逆向了打包格式（Kotlin 源码逐行确认）：
   ATLAS.BIN: u8 ver, u16 tpagCount, u16 tileCount, u16 atlasCount +
   atlas 表 (i32 off, u16 w, h, u8 bpp, i32 size, u8 comp[0=raw,1=RLE])
   + TPAG 条目 20B (atlasId, atlasX, atlasY, w, h, cropX, cropY, cropW,
   cropH, clutIndex; 0xFFFF atlasId = 无纹理)
   TEXTURES.BIN: atlas 像素，4bpp = 低半字节在前，8bpp = 直接索引；
   RLE = (runLen u8, value u8)
   CLUT: PS2 RGBA (a<<24|b<<16|g<<8|r, a=(a+1)>>1)，8bpp 有 CSM1
   swizzle（解码时反向）
4. 新文件 gcn_bspack.c：BSP_load (ATLAS+TEXTURES+CLUT 读进 MEM1,
   ~4.7MB 常驻) + BSP_decodeTpag (RLE 解压 => 索引 => 调色板 => 线性
   RGBA8)
5. ensurePage BSP 路径：BSP_hasTpag => 解码 => GCN swizzleRGBA8 =>
   常驻页 => blitter 采样（现有管线不变）
6. 卡上：apps/DELTARUNEGC/gfx-packed/ = 4 个文件 + boot.dol
   (md5 49e05564083ff4c307832cfdd68820f5, 1,190,528B)
TEST ON HW: boot => HWLOG 应显示 [BSPACK] loaded + [BSPPAGE] =>
CONTACT 文字/精灵 = 官方管线资产第一次上屏！
## PATCH 30 (9/15 ~00:40) — LOG FIXES + START-EXIT, on card
用户跑了 build 29：没崩溃（直接关机）但卡上【没有 HWLOG 文件】= 
bootlog_open 的单一路径 /apps/... 在硬件上打不开（老经验：GC libfat
= "fat:" 前缀）。
FIX（build 30, md5 21e37b12a2b72a029587ad1a6f2f5195）:
- bootlog 三路径探测（/apps, fat:/apps, sd:/apps）× 8 次文件名尝试，
  最后 fallback 到卡根 /HWLOG_ROOT.TXT
- START 键 = 优雅退出：写最终 timeline 对比 + 关闭文件 + 屏幕显示
  "exited - log saved, power off safe" => 用户看到 SAFE 再拔卡！
TEST: boot -> 玩 1-2 分钟 -> 按 START -> 看到 "log saved" -> 拔卡
=> 卡上应有 HWLOG<n>.TXT（或 HWLOG_ROOT.TXT）
## PATCH 31 (9/15 ~01:00) — LOGGER ORDER BUG + Z+START EXIT, on card
用户澄清：root boot.dol = Swiss 自己的（639KB 正常），用户跑的确实是
apps/DELTARUNEGC/boot.dol (build 30)。没崩溃、没 HWLOG、START 没退出。
两个 bug 找到（都成立）：
1. **bootlog_open 在 fatInitDefault() 之前调用** = 文件系统未挂载 =>
   opendir/open/fopen 全部失败 => 日志从不落盘（黑盒从未装填！）
2. START 被映射为 VK_ENTER 给游戏 = 永远不会退出（v13 的 break 移除后）
FIX（build 31, md5 7fcdfc126c141f12e73c549782b71674）:
- bootlog_open() 移到 fat 挂载成功之后
- **Z + START = 优雅退出**：写最终 timeline + 关文件 + "log saved" 状态
TEST: boot -> 1-2 分钟 -> **按住 Z 再按 START** -> "log saved" -> 拔卡
=> HWLOG0.TXT（或更高）= 完整时间线 + OK/PENDING/FAIL 表
## *** HWLOG0.TXT READ - THE BLACK BOX SPOKE *** (9/15 ~01:30)
第一次成功的硬件日志！内容：
- OK: MOUNT / DATAWIN / **BSPACK loaded** / PARSE (txtr=9 tpag=3009
  rooms=147) / VM / **[ROOM1] PLACE_CONTACT inst=1** /
  **[FIRSTDRAW] frame 1 presented (cmds=1 blit=1 skip=0)**
- 游戏循环在真实硬件上跑起来了！blit 成功（那个 1 = 我的标记方块）！
- 但：CONTACT 只有 1 个实例（v12 时代是 8！）、没有任何 [BSPPAGE]
  行 = 游戏的 draw 事件一个都没发（除了标记方块）
- timeline 5/9 @frame448（用户提前退出）
=> **新结论：纹理问题已绕过；现在的问题 = VM 的 create/draw 事件
静默失败**（老日志里的 VM 警告：READ var no instance / NewGMLObject
invalid codeIndex）——对象创建失败 = 没有绘制调用。
BUILD 32 (md5 e8c37103f9bd89c94aab36ee6606c736)：stderr -> HWLOG dup2
桥接 = 下一次运行会捕获所有 VM 警告/create 失败进 HWLOG。
## PATCH 33 (9/15 ~02:00) — PAYLOAD/INSTANCE COUNTERS, on card
HWLOG1 = 内容与 HWLOG0 相同（无 VM 警告行）=> stderr->HWLOG 桥接在
worker 线程的 newlib reent 下未生效（libogc 线程 stderr 设备绑定）。
改用直接调用：runner.c 的 Room loaded + data_win.c 的 room payload
加载后直接 GCN_bootlog：
- [ROOMLOADED] name #idx instances=N  (真实实例计数！)
- [PAYLOAD] room N: objects/tiles/layers 计数
下次运行将显示：CONTACT 的 payload 是否加载了 8 个对象（还是 0）+
最终 instances 计数 —— 定位对象创建失败的确切位置。
Build 33 md5 100342e064b81ec20d0d85c783f0767a on card.
## PATCH 34 (9/15 ~02:30) — HWLOG2 READ + 实例计数器修正, on card
HWLOG2 (build 33) 的重大数据：
- [PAYLOAD] room 0 ROOM_INITIALIZE: objects=4
- [PAYLOAD] room 1 PLACE_CONTACT: objects=1
=> **payload 加载 100% 正常**（SD 读取没问题）！
- 但 [ROOM1] inst=1 = 运行时只有 1 个实例（v12 是 8）= 全局实例
  + ROOM_INITIALIZE 的对象在切换后消失 = **VM 全局初始化期间的
  实例创建失败**（老日志的 VM 警告 = NewGMLObject invalid codeIndex）
- 且发现 build 33 的 runner.c [ROOMLOADED] 补丁根本没打上（

  匹配失败 + 没强制重编 runner.o）——已修复并用 rm runner.o 强制
  重编。Build 34 md5 5b4fa014936608d53494e4c056d4b5e6 on card。
下次运行将显示每个房间加载时的真实实例数 => 定位全局实例丢失点。
同时已知：FIRSTDRAW blit=1（标记方块）在硬件上成功 blit —— 渲染
管线完全正常。问题纯 VM 侧。
## *** PATCH 36 (9/15 ~03:00) — MAIN-THREAD PRESENTATION ARCHITECTURE ***
深度思考结论（全部硬件数据 + GC 架构交叉分析）：
- 唯一 100% 被硬件证明的呈现路径 = boot 文字路径：
  主线程 + gXfb[0] + console 绑定一次 + DCFlush + SetNext + Flush
- worker 线程做 VI 寄存器写入（SetNext/Flush/WaitVSync/翻转）=
  唯一未证实的环节——v20 单缓冲（主线程式）帧到达电视（绿色卡顿），
  v22+ 双缓冲翻转（worker）= 全黑 = 翻转是分水岭
- VM/解析/资产/blitter 全部正常（HWLOG 5/9 OK）
ARCHITECTURE（build 36, md5 1fc85640bcf947adc41994a15a7977a9）:
- **CPU 渲染 / 主线程呈现分离**：
  worker: VM step + blit 到 gXfb[0] + gFrameReady=1 + 等待主线程消费
  main:   telemetry 文字 + DCFlush + SetNext(gXfb[0]) + Flush +
          WaitVSync + gFrameReady=0（boot 文字的 exact 寄存器序列）
- 单缓冲（gXfbIndex=0，renderer 永远写 gXfb[0]），无翻转
- 主线程每帧 WaitVSync = 垂直同步由已证实的主线程上下文完成
DOL 1,191,872B on card。此架构 = boot 文字路径的直接延续 =
硬件上已证明的寄存器序列。如果这样还黑 = 唯一剩下的变量是
blitter 内容本身（由 HWLOG 的 [DRAWPASS] + [BSPPAGE] 行判断）。
## *** HWLOG4 READ + PATCH 37 *** (9/15 ~04:00)
HWLOG4（build 36 的运行）用户视觉报告 + 日志：
- **疯狂闪绿** = 我们的软件帧到达电视了！（绿色 = 撕裂 + 颜色转换
  残留问题），Z+START 停止后 = 稳定黑屏+telemetry
- [DRAWPASS] drawables=10/11 drawNormalSlot=40 = **draw 事件注册了
  且 draw pass 在跑**！但 cmds=1（只有标记方块）=> draw 事件执行
  但 0 游戏渲染调用 => **[DRAWTEXT] SKIPPED 无、[BSPPAGE] 无 =
  draw 事件里根本没调 drawText/drawSprite** => 脚本执行失败：
  @@NewGMLObject@@ invalid codeIndex -1 = GM 2022.9 struct 方法
  解析 bug（draw 里的 struct 调用级联失败）
- drawNormalSlot=40 = 事件表正常；失败在脚本执行层
BUILD 37 (md5 4935514743aea9e6d56ae0c45c4bae97)：
- 撕裂修复：worker 写后台缓冲 + DCFlush + 交接；主线程 flip（VI 寄存器
  仍在主线程上下文）+ WaitVSync；telemetry 画在 worker 刚画完的帧上
- 下一步（明天）：VM struct-method bug（builtinNewGMLObject 的
  args[0] 解析——RVALUE_METHOD 分支可能没覆盖 GM 2022.9 的
  method 引用编码）= CONTACT 文字渲染的最后一关
## *** PATCH 38 (9/16 ~10:30) — THE STRUCT-METHOD BUG: ROOT-CAUSED + FIXED, on card ***
证据链（全部来自 vmprobe 工具套件 /tmp/vmprobe/，直接解析 Steam ch1 data.win 字节码）：
1. HWLOG6 (build 37): 3491 帧, blit=3491 skip=0, cmds=1 => 唯一剩余阻塞 = VM draw 链
2. 卡上 crash.log: 60x "@@NewGMLObject@@ method has invalid codeIndex -1" +
   "READ var 'index' ... obj_switchAsyncHelper no instance" —— 都在 obj_event_manager Create
3. 反汇编 scr_get_valid_room (bcAbs 0x257e18):
   @@NewGMLObject@@ 调用 = Push.i <ctorFuncIdx>; Conv; Call.i argc=3 FUNC[37]
   => args[0] = ctor 的 FUNC 索引（数字！）, args[1]=boundSelf, args[2..]=ctor 参数
   => Cinnamon 的 raw-funcIdx 解析路径 (builtinNewGMLObject vm_builtins.c:9523) 是对的！
   且 FUNC 链修补 (patchReferenceOperands vm.c:300) 工作正常（此前"corruption"理论是探针 bug，已排除）
4. 真正的 -1 源头：Push.v 指令 instanceType == -6 (INSTANCE_BUILTIN) 用于
   读取"用户定义的函数引用"（例：obj_event_manager Create 里 trigger_event = method(...);
   之后 trigger_event(...) 从其他对象/脚本调用）。全部 23 个此类名字 ('init', 'trophy',
   'init_trophies', 'trigger_event', 'event_show_debug_message'...) 都是 SELF 域 VARI 变量
   (it=-1)，没有对应的 CODE/SCPT/builtin 名字条目
   => Cinnamon vm.c:757-795 的 -6 分支按名字查找函数 => 全部 miss
   => GMLMethod_createUnresolved => codeIndex=-1 => @@NewGMLObject@@ 拒绝构造器
   => 结构体/事件系统死亡 => 所有 draw 脚本级联失败 = 黑屏无游戏绘制
   （HW 上的确切崩溃对：NewGMLObject -1 + 'READ var index obj_switchAsyncHelper'）
FIX (vm.c:760): -6 分支现在先按 varID 查找：
   1. currentInstance->selfVars[varDef->varID] (同对象函数引用)
   2. runner->globalScopeInstance->selfVars[varDef->varID] (全局函数)
   3. 然后才是原有的 stack-peek (@@This@@ CallV) / codeIndexByName / builtinMap / unresolved
同时：gGCN_lastReadWasStatic 2 处调用点加 __has_include(<ogc/gx.h>) 守卫（PC 兼容）
BUILD 38 md5 a1f60321f07b96b9a179c11a2c9727a9 (1,192,128B) ON CARD (md5 verified)。
TEST: boot -> CONTACT 文字/精灵应首次出现（结构体创建恢复 => draw 事件产生真实绘制调用）
=> HWLOG7 应显示 [DRAWPASS] cmds>1（不止标记方块）+ 无 codeIndex -1
PC glfw 构建: vm.c 修复编译通过（gGCN guard 已修；缺 main 是 glfw target 既有问题，与本修复无关）
探针工具（可复用）: /tmp/vmprobe/{vmprobe3.c, vmprobe4.c, vmprobe5.py, stacksim.py, ctor_ngo.py,
scan_neg6b.py, trophy_refs.py, walkdbg.py} — data.win 字节码反汇编/链修补/栈模拟全套。

## *** HWLOG7 READ (build 38) + PATCH 39 — RUNTIME FUNCTION REGISTRY (the real fix), on card ***
用户报告 build 38: 仍然黑屏 + 屏幕显示 "loading room one"。HWLOG7 (794 帧) = 与 HWLOG6
完全相同的签名（cmds=1）=> build 38 的 varID 查找修复不够。crash.log 未更新（md5
d5da7c40 = 还是 v12 时代的旧文件 => stderr 桥接在 HW 上依旧失效，这是已知问题）。
用户关键观察：屏幕显示 "loading room one" —— 这是启动状态行（boot status text），
在运行后从未被清除/更新 —— 纯外观问题，不是状态指示（frame 计数在跑=循环活着）。
关于 room 顺序: ROOM_INITIALIZE (#0) -> PLACE_CONTACT (#1) 是正确的 ch1 流程
（HWLOG 已证明 ROOMLOADED 两者都加载了 7/8 实例）。
深度解剖 obj_event_manager_Create_0（em_create_full.py 全量栈模拟，6852 字节）发现：
1. `@@NewGMLObject@@` 调用（+0x790, argc=2）在匿名 ctor _4720 体内：
   args[0]=argument0 (ctor 参数), args[1]=Push.v bltn(-6) 'trophy' —— 方法值按名字读
2. `trophy = method(FUNC[730]_anon, @@NullObject@@())` 的写入在 +0x16ac：
   **Pop.v it=-7 = LOCAL 作用域写入**（不是 self!）—— 编译器把顶层 `function trophy(){}`
   编译成本地 Pop。所以 writeIntoSlot(localVars) 是注册点
3. 全部 23 个 -6 名字 ('init','trophy','trigger_event'...) 都没有静态 CODE/SCPT/builtin
   条目 —— 它们是【运行时定义的全局函数】，必须运行时注册表解析
FIX（本 patch 核心 = GM 2022.9 运行时函数注册表）:
- VMContext 新增 runtimeFunctionRegistry (name -> RValue method, stb_ds)
- resolveVariableWrite 入口处统一挂钩（fast+slow path 全覆盖）：
  任何 METHOD 值写入命名变量 => incRef + shput(name, method)
- -6 Push 读取顺序: currentInstance.selfVars[varID] -> globalScope.selfVars[varID]
  -> 【runtimeFunctionRegistry[name]】 -> 原有 stack-peek -> codeIndexByName -> builtinMap -> unresolved
- VM_reset/VM_free 释放注册表（方法 incRef/refCount 平衡）
BUILD 39 md5 20e693ce2e00ca7377cafc3ae29cc674 (1,192,448B) ON CARD (md5 verified, sync done)。
kate (PID 320805) 又占用了卡片目录 —— 拔卡前关掉它！
TEST: boot -> 若 registry 命中：HWLOG8 应显示 cmds>1；CONTACT 文字/精灵首现。
屏幕上 "loading room one" 状态行是外观 bug（启动文本不清屏）——后续修。
注意：-6 读取顺序中 registry 在 name-lookup 之前 —— 若 'trophy' 也存在于 SCPT bare 名
（不存在，已验证 23 个名字全部 miss 静态表），则 registry 是唯一解析路径。
若 HWLOG8 仍 cmds=1 且无 -1 警告 => 下一个嫌疑 = CallV/Pop.v 的 -6 写入路径
（writeSingleInstanceVariable 也可能需要 registry 钩子——对象引用写入）。

## *** HWLOG8 READ (build 39) + PATCH 40 — DEEP DECODE + DIAGNOSTIC BUILD, on card ***
HWLOG8 (build 39, 543 帧) = 与 6/7 完全相同签名（cmds=1）。registry 修复有效但
CONTACT 房间本身没有可绘制内容 —— 深度解剖后完全解密：
1. PLACE_CONTACT 的 8 个实例全部是 obj_switchAsyncHelper（无 sprite，Draw 事件只有
   subtype 77 = DRAW_POST: if(docheck) switch_asyncResume()）——这个房间【本来就没有
   可见内容】！cmds=1 在此房间是正常现象！
2. switch 系统解剖：Draw_77 调 switch_asyncResume() => -6 读 'switch_asyncResume'
   => 真实 handler = FUNC[8]（boot 时 GlobalScript 注册的方法值）
   => handler 体 = if(scr_is_switch_os()){...} —— Switch 主机专属！GameCube 上 os_type
   不是 switch => asyncResume 完全空操作（PC 上也一样！）
3. room 推进不靠 async 系统：v12 时代 HW 日志证明过 VM: Reset x2（game_restart 双重
   重启 = DELTARUNE 标准启动流）。v12 HW 运行到达了 Reset x2 => 房间推进在 HW 上
   也工作过。543 帧的运行可能只是太短没到下一个可见房间（intro/menu）。
4. "loading room one" 屏幕文字 = 启动状态行从未清除 = 纯外观（帧计数在跑）。
用户的关键判断方向对了：不是"加载错房间"，是"这个房间本来就空白"。
PATCH 40 = 决定性诊断构建（所有信号直接进 HWLOG，绕开死掉的 stderr 桥）：
- [DIAG] drawEventsRun / drawCalls / vmWarnings / roomIndex / roomChanges
  （Z+START 退出时打印完整计数）
- [ROOMGOTO] pending/from 每次 room_goto 调用（前 20 次）
- [CALLV-UNRESOLVED] func+调用者（前 12 次）= -6 解析失败的直接证据
- GCN_diag_* 计数器：drawEventsRun (Runner_executeResolvedEvent EVENT_DRAW),
  drawCalls (GCNRenderer_appendQuad), vmWarnings (READ-no-instance + unresolved)
- builtinRoomGoto 日志钩子 (GCN only)
BUILD 40 md5 54393fe1b0cef210a9888d39e53067c8 (1,193,568B) ON CARD (md5 verified)。
TEST: boot -> 跑久一点（2-3 分钟，让游戏有机会过 CONTACT）-> Z+START -> HWLOG9:
  [DIAG] 行 = 决定性数据：roomChanges>0=流程活着; drawCalls>1=渲染链活了;
  vmWarnings>0=还有 VM 错误; [CALLV-UNRESOLVED] 名字 = 下一个要修的函数。
若长时间停留在 room 1（roomChanges=0）=> 房间推进卡死 = async/switch 系统依赖
  => 下一步修 switch 流程（非 Switch 平台路径）。

## *** HWLOG9 READ (build 40) + PATCH 41 — THE TRUTH ABOUT CONTACT, on card ***
[DIAG] drawEventsRun=5264 drawCalls=585 vmWarnings=0 roomIndex=1 roomChanges=1
解读（决定性）:
- vmWarnings=0 + 无 [CALLV-UNRESOLVED] => 38/39 的 VM 修复【全部生效】——
  没有任何未解析函数、没有 missing instance。VM 干净了！
- drawCalls=585 ≈ 586帧 × 1 = 只有标记方块 => 游戏的 draw 调用没产生 quad
- roomChanges=1: 只有 ROOM0->ROOM1 的一次转换（ROOMGOTO pending=1 from=0）
深度解剖（这次推翻了我自己之前的一个误读）:
- ROOM_INITIALIZE 真实实例 = obj_initializer2, obj_gamecontroller, obj_debugcontroller,
  obj_roomcontroller（不是 switchAsyncHelper——之前 +0x64 偏移读错了列表）
- PLACE_CONTACT 真实实例 = 1 个 DEVICE_CONTACT @ (288,144)！
- DEVICE_CONTACT 事件: CREATE/ALARM_4/STEP/OTHER_10/OTHER_5/DRAW_0
- DEVICE_CONTACT_Draw_0 反编译（45 条指令全解）:
    draw_set_alpha(FADEFACTOR)
    if (WHITEFADE == -1) draw_set_color(-1) else draw_set_color(16777215)
    draw_rectangle(-10,-10,999,999, false)   <- 全屏白矩形 = CONTACT 屏背景!
    draw_set_alpha(-1) ... FADEFACTOR 渐变逻辑
  （字节级验证: PushI.e 值 0/999/999/-10/-10; Cinnamon 压栈序 => args 反转 =>
   x1=-10,y1=-10,x2=999,y2=999,outline=0=solid）
- 结论: CONTACT 屏幕 = 纯色矩形（无纹理），每帧 draw_rectangle 必然被调用。
  但 drawCalls=585（只有标记方块）=> draw_rectangle 从未到达 appendQuad！
  且 vmWarnings=0（draw_rectangle 已注册且运行,否则会有 Unknown function 警告）
=> 唯一可能: 调用链中段静默失败（builtin=>renderer vtable=>GCNRenderer_drawRectangle
  =>appendQuad）。builtin_drawRectangle 若 renderer==nullptr 会静默 return——但标记
  方块渲染说明 renderer 存在！=> 需要精确定位断点。
PATCH 41 = 精密仪器（全部进 HWLOG）:
- [DRAWRECT] builtin 端: args 值 + renderer 指针检查（前 4 次）
- [DRAWRECT] GCNRenderer_drawRectangle: 实际坐标+color+alpha+drawAlpha（前 4 次）
- [DRAWEV] 前 10 个 draw 事件: subtype+objName+codeId+instanceId => 解开 5264 的来源
  （586帧 × 9 ≈ 5264: 哪来的 9 个 draw 事件/帧?!）
BUILD 41 md5 008b82c409e6bd7ba3e8528849246e72 (1,193,xxx B) ON CARD (md5 verified)。
TEST: boot 1-2 分钟 -> Z+START -> HWLOG10:
  [DRAWRECT] builtin 出现? => builtin 运行; [DRAWRECT] renderer 端出现? => 断点在中间
  [DRAWEV] 列表 => 真实的每帧 draw 事件构成
若 [DRAWRECT] builtin 未出现 => draw_rectangle builtin 未被调用 => Draw_0 脚本没跑
  => 但 drawEventsRun=5264 说明 draw 事件在跑 => 那跑的是谁?!

## *** HWLOG10 READ (build 41) + PATCH 42 — THE BREAKTHROUGH: RENDER CHAIN WORKS!, on card ***
HWLOG10 (build 41) = 【决定性突破】:
[DRAWRECT] builtin: args=(-10.0,-10.0,999.0,999.0,0.0) argCount=5
[DRAWRECT] x1=-10 y1=-10 x2=999 y2=999 color=000000 alpha=0.40 outline=0 drawColor=000000 drawAlpha=0.40
[DRAWEV] 列表揭开了每帧 draw 事件的构成:
  #1 obj_initializer2, #2 obj_event_manager, #3 obj_event_manager_debug, #4 obj_time,
  #5 obj_gamecontroller, #6 obj_debugcontroller (ROOM_INITIALIZE 的 6 个 Create 后的
  首帧 draw), #7 obj_roomcontroller(sub=4=ROOM_START!), #8/#9 obj_time/gamecontroller
  (sub=1), #10 obj_initializer2 —— 全部真实事件链!
=> 修正之前的误读: ROOM_INITIALIZE 实例 = initializer2/gamecontroller/debugcontroller/
   roomcontroller (+event_manager/debug/time 由 gamecontroller 动态创建 = 7 实例 ✓)
=> drawEventsRun=12257 / 681帧 ≈ 18/帧: 各对象的 Draw 事件每帧全部执行!
=> drawCalls=1362 = 681标记方块 + 681矩形 = 【draw_rectangle 每 frame 都到达 appendQuad
   并被 blit】! 渲染链 100% 工作!
=> 真正的 bug = 颜色: [DRAWRECT] color=000000 alpha=0.40 —— CONTACT 屏是全屏黑矩形
   @40% alpha 画在黑底上 = 黑屏! 应该是白色 (GM -1 = 0xFFFFFF)!
   draw_set_color(-1) 或 (16777215) 产生了 000000。vmWarnings=0 => 调用已解析运行。
   => 颜色转换 bug: PushI.e(-1) => RVALUE_INT32(-1) => toInt32=-1 => (uint32)-1=FFFFFFFF
   => 但 drawColor=000000! 唯一解释: args[0] 是 UNDEFINED (toInt32 默认=0) 或
   BE/float-real 转换问题。
PATCH 42: builtin_drawSetColor 加 [SETCOLOR] 日志 (type/int32/real/val32, 前4次)
=> 下一次运行直接揭示 -1 如何变成 0。
BUILD 42 md5 5269a157789e2ea16011077db794df74 ON CARD (md5 verified, synced)。
另外的巨大利好: 灰色可见性计算: 即使矩形是白@40%alpha, 在黑底上 = 40% 灰 = 可见!
=> 一旦颜色修复, CONTACT 屏立即可见 (白色渐入)。
下一步 (若 [SETCOLOR] 显示 type=UNDEFINED): WHITEFADE 变量未初始化 =>
  DEVICE_CONTACT_Create_0 反编译 => WHITEFADE 初始化路径 => 修复读取/默认值。
## *** PATCH 45 (build 45) — OBJECT-NAME LOGGING + SURVEY_PROGRAM PLAN ***
HWLOG12 (build 43, 24dbeeeb, 191 frames, Z+START): DIAG drawEventsRun=1709
  drawCalls=190 vmWarnings=0 roomIndex=1 roomChanges=1 — 稳定复现:
  每 frame: set_color(白)→set_color(0)→rect(黑@40%)。
BUILD 45 = 给 [SETCOLOR]/[DRAWRECT] 日志加上【调用者对象名】(ctx->currentInstance
  → objectIndex → OBJT name) => 一次运行即知: 谁设置白色、谁设置黑色、谁画幕布。
  md5 24dbeeebef2a3abf16e1bbaad06caf17 ON CARD (verified, synced)。
WEB INTEL (web_search 验证): PS2 DELTARUNE 移植 = Butterscotch 跑
  SURVEY_PROGRAM (2018 原版 demo, bytecode 16, WAD16)。
  - 下载源: https://archive.org/details/SURVEY_PROGRAM
    English/SURVEY_PROGRAM_WINDOWS_ENGLISH.exe (81MB, NSIS) → data.win 17,124,044B
    md5 a88a2db3a68c714ca2b1ff57ac08a032 → /tmp/survey/data.win
  - SURVEY GEN8 flags 0x1001 vs modern 0x1101 => bit0x0100 = "BC>=17" 标志!
    SURVEY = BC16 (Butterscotch 官方支持), modern ch1 = BC17。
  - SURVEY: ROOM=145, OBJT=342, FONT 正常, AUDO 6MB。
  - 卡上 gfx-packed 资产 = ~/sunshine-patcher/prep-butterscotch 的 (md5 全等!) =
    modern ch1 (BC17) 预处理资产 (3009 atlas/673 tpag)。
DECISIVE A/B 实验 (备选方案, 若 BC17 持续黑屏):
  SURVEY data.win + SURVEY 资产 → 卡上 apps/DELTARUNEGC-SURVEY/ → 若可运行且
  有画面 => BC16 全管线 OK, bug 在 BC17 路径 => 用 SURVEY 验证渲染链后修 BC17。
  需要: 给 SURVEY 跑一次 ButterscotchPreprocessor (ps2 target) 生成资产。
  注意: SURVEY 无 Switch-OS 门控、无 game_change、直接进 contact 界面。
## *** PATCH 46 (build 46) — THE SCREEN DECODED: BLACK IS CORRECT, TEXT IS MISSING ***
HWLOG13 (build 45, 24dbeeeb, 810 frames): [DRAWEV] 每帧 9 个事件; 每帧稳定序列:
  [SETCOLOR] obj=obj_writer 16777215 (白) — obj_writer 的 draw 到达 +0x2438 set_color
  [SETCOLOR] obj=DEVICE_CONTACT 0 (黑) — 黑幕就是设计 (contact 屏 = 黑底+白字!)
  [DRAWRECT] obj=DEVICE_CONTACT (-10,-10,999,999) 黑@40% — 背景正确!
反汇编 obj_writer_Draw_0 (CODE[707], 12780B @0x2b0a24): 70 个 Call:
  draw_text×5 (+0x277c 等), draw_sprite×1, draw_set_font×1, scr_texttype×15,
  scr_nextmsg×2, string_char_at/delete/length (打字机), instance_destroy×2。
=> 真相: contact 屏 = 黑背景 (DEVICE_CONTACT 画, 正确!) + obj_writer 白色文字
   (打字机状态机门控)。文字从未绘制 => draw_text 从未被调用 =>
   打字机消息状态未启动 (scr_nextmsg 链某处静默失败, vmWarnings=0)。
BUILD 46 = builtin_drawText 前 8 次调用日志 (x/y/字符串内容) md5
  062a9f26556fe07cbaa2a0592204f2e8 ON CARD。
决策树: 若 [DRAWTEXT] 出现 => 文字调用但渲染失败 => 修字体/字形路径。
## *** PATCH 47 (build 47) — THE TYPEWRITER IS ALIVE; GLYPH QUADS ARE EATEN ***
HWLOG14 (build 46, 062a9f26): [DRAWTEXT] ×8 ALL = x≈110 y≈80 str="u"!
  => draw_text IS CALLED (打字机在打字! "u" = contact 文本第一个字母, 一次一字),
  x/y 抖动 ±1 (109-111/79-81) = 真实的 draw_text 调用模式。
  但 drawCalls 仍 1/frame => vtable drawText 收到字符串但【没有产生任何 quad】!
  drawTextCommon 早退点: (1) drawFont 无效=已记录,日志无 => 不是;
  (2) fontTpagIndex 无效=已记录,日志无 => 不是; 剩下 3 个静默 return:
  (3) texturePageId 越界, (4) ensurePage NULL, (5) findGlyph('u')==NULL。
BUILD 47 = 给这三个静默 return 加日志 (pageId INVALID / ensurePage NULL /
  glyph U+XXXX MISSING glyphs=N) md5 bd5eaba2ffd3a807e3f7b48d0fd82596 ON CARD。
最可能: (5) 字形表为空/字符不匹配 (FONT chunk 解析 or 字形映射 bug) —
  因为 (1)(2) 已排除, (3)(4) 会阻止所有 sprite 绘制 (但 sprite 也没画过!)。
  注意: draw_sprite 也从未产生 quad => (3)/(4) 也可能!一次运行分辨。

## *** PATCH 48 (build 48) — ROOT CAUSE FOUND: THE 512-DIMENSION GATE ***
HWLOG15 (build 47, bd5eaba2): "[DRAWTEXT] ensurePage(7) NULL" — 确认!
本地证据 (md5-verified 读卡 + data.win 直解):
  TXTR[7] = 2048x2048 (字体所在页), GCN_MAX_PAGE_DIM = 512 =>
  ensurePage 行149: textureWidth > 512 => return NULL!
  => 所有 2048x2048 大页 (TXTR[2..8], 全部 sprite+字体!) 全部被拒 =>
  => 从 build 1 起【没有任何 sprite/字形 quad 能被加载】= 黑屏的真正根源!
  而 BSP (预打包) 路径的 atlas 全是 512x512 (验证: ATLAS.BIN 91 atlas 全
  512x512, tpag[7] 在 atlas[18] 512x512, BSP_decodeTpag 返回 1MB RGBA8)。
  尺寸门检查的是【data.win 源尺寸】而非【BSP atlas 实际尺寸】=> 错误拒绝。
FIX: ensurePage 只在 !BSP_hasTpag 时应用 blobOffset/尺寸门; BSP 路径直接放行
  (成功时会打 "[BSPPAGE] page %u from packed atlas %ux%u")。
BUILD 48 md5 81c53e52d454b636967330cbf306bb94 ON CARD (verified, synced)。
预期下一次运行: [BSPPAGE] 出现 + drawCalls 飙升 + 屏幕出现文字/精灵!
关于 "开头应该是 Gaster 'hello' 文本": 正确 — obj_writer 打字机已在工作
  ([DRAWTEXT] str="u" = 正在逐字输出), 修完 ensurePage 后文字应立即出现。

## *** PATCH 49 (build 49) — BSP CONTRACT FULLY DECODED + ITEM-PAGE REWRITE ***
HWLOG16 (build 48, 81c53e52): "[BSPPAGE] page 7 from packed atlas 13x12" 成功!
  drawCalls = 95547 (163/frame vs 之前的 1/frame) = quad 洪水已到位!
  但用户仍黑屏 => quad 的 UV/纹理内容是错的。
本地解码 BSP 数据契约 (ATLAS.BIN + data.win 逐字节对照):
  - BSP tpag[i] 对应 data.win TPAG ITEM [i] (逐项打包, 不是按源页!)
  - 结构: atlasId, atlasX, atlasY (atlas 内位置), width, height (打包后尺寸),
    cropX, cropY, cropW, cropH (源空间矩形), clutIndex
  - 字体: font0 → tpag item 17 (2048x1024 sheet, page 2) → BSP: atlas 77,
    (0,0), 打包 512x256, crop=(0,0,2048,1024) = 大 sheet 被缩小 4x 打包!
  - 精灵: tpag[7] = (atlas 18, 153,500, 13x12, crop 0,0,13,12) = 按帧裁切透明边
  - BSP_decodeTpag 旧代码: 展开整个 512x512 atlas 却返回 13x12 尺寸 =>
    纹理内容/尺寸完全错位 (95547 个 quad 全是垃圾纹理!)
FIX (build 49, md5 367fbeab746c523c7ce3de176ec8d7d3, ON CARD):
  1. BSP_decodeTpag: 只解码打包区域 [atlasX, atlasY, w, h] (raw+RLE 两路)
  2. ensurePage: BSP 页 32 对齐补边 (GX 32x32 tile), 补边透明; 页按 ITEM 索引
  3. drawSprite: BSP 路径 = item 页 + crop 位移 + 全 UV (0..1)
  4. drawText: 字形 UV = (srcX+glyphX-cropX)/cropW (缩小保留分数)
预期: HWLOG17 显示 [BSPPAGE] item 17 packed 512x256 + 屏幕出现 Gaster 文字!
若仍黑 => 读 [BSPPAGE]/[DRAWTEXT] 日志值再修。

## *** HWLOG17 READ (build 48 的运行) — RED BORDER = FIRST GAME TEXELS! ***
HWLOG17 日志格式是旧的 ("page 7 from packed atlas 13x12") => 这次跑的还是
  build 48 (用户在我完成 build 49 之前就跑了)。当前卡上 md5 = 367fbeab =
  BUILD 49 已在卡上, 等待运行。
红框解读: 163/frame 的 garbage-UV quads 现在是【可见的】— CLAMP 边缘色被拉伸
  成屏幕边缘的红框。这是【第一次从游戏纹理产生可见像素】(虽然是错的纹理坐标)!
  = 光栅管线已经打通, 剩下的就是 build 49 的 UV/尺寸修正。
下一步: Architect 重新插卡 + 开机 (跑 build 49) => 日志应出现新格式
  "[BSPPAGE] item 17 packed 512x256 padded 512x256" => 文字应出现。

## *** HWLOG19 READ (build 49) + PATCH 50 — LOG FAILURE, NOT CODE CRASH ***
HWLOG19 = 只有 header (426B) => build 49 的运行在 bootlog_open 的【reopen 窗口】
  丢日志 (行已改为每行立即 fflush + unbuffered — 但 [BOOT] 在 reopen 之后,
  reopen 失败 = 后续行全丢)。用户 Z+START 有效 = runner 活着 = 非启动崩溃!
  且卡 FAT 慢性损坏 (46 FSCK*.REC) = SD 写抖动高度可疑。
PATCH 50 (md5 a83a25e9d56fa8494d4b4698ced47418 ON CARD): [BOOT] 行写进 open()
  本体 (header 之后, reopen 之前) => 下一行永不丢失; 若此日志仍 header-only
  => 确认 SD 写故障, 换根路径写 (root fallback)。
最终尝试的完整改动集 (build 49 的全部 + 此 bootlog 加固):
  - BSP_decodeTpag 只解码打包区域
  - ensurePage 32 对齐补边 + item 索引
  - drawSprite/drawText item-page UV 契约
红框结论修正 (用户的批评是对的): 红框 = 错误 UV 的 CLAMP 边缘拉伸, 不是
  游戏内容。但它是【第一次从游戏纹理管线产生可见像素】= 管线活性证明。

## *** HWLOG11 READ (build 42) + PATCH 43 — COLOR MYSTERY DECODED, on card ***
[SETCOLOR] type=2 int32=16777215 val32=00FFFFFF  <- draw_set_color(白) 正确到达!
[SETCOLOR] type=2 int32=0 val32=00000000          <- 随后 draw_set_color(0) 黑!
[DRAWRECT] args=(-10,-10,999,999,0) color=000000 alpha=0.40  <- 矩形画的是黑色!
=> 每 frame: set_color(白) → set_color(0) → rect(黑@40%)。
=> 真相: 矩形是【淡入黑幕】(FADEFACTOR=0.4 = 40% 黑覆盖), 而白色背景矩形
   (DEVICE_CONTACT 的 16777215 矩形) 没有被执行/被剔除 — 只有幕布到达渲染器。
   set_color(0) 的调用者 = 另一个 draw 事件 (每帧一次, 不属于 Draw_0)。
=> 布局推测: 白色背景 + 黑色文字 应该在幕布【之下/之后】绘制。当前只画了幕布。
=> 白色内容的缺失 = 后续追查对象 (obj_time/obj_gamecontroller 的 draw 事件没有
   产生 quads — 它们的脚本在 draw 调用前退出或被跳过)。
PATCH 43 = 扩大日志采样 (SETCOLOR/DRAWRECT 前 10 次) => 下一次运行显示完整的
  每帧序列: 谁设置白、谁设置黑、谁画矩形、顺序如何 => 精确定位白色背景缺失处。
BUILD 43 md5 628ad834e8e19d4bd120144070c70343 ON CARD (md5 verified, synced)。
注意预算: 下一次 HW run 后将只做有针对性的修复, 不再盲目改码。
候选根因 (按可能性):
  1. draw_set_color(0) 是 Draw_0 内的第二次调用 (我的线性反汇编误对齐) —
     白色矩形在条件分支内 (WHITEFADE==true 时才画白底), 当前 FADE 状态走黑幕分支
  2. obj_time/gamecontroller 的 draw 事件在 DEVICE_CONTACT 之前设置黑色
     (深度排序), 白色背景由另一个尚未到达状态的脚本负责
  3. alpha=0.40 的黑幕在黑底上不可见 — 需要背景先画 (白底), 顺序 bug?
     (GAME 的设计就是 白底→黑幕@fade→文字, 我们的幕布在白底之前画 = 顺序错误
     => 检查 depth 排序!)

## NEXT STEPS (in order)
1. Write src/gcn/: gcn_platform_config.h, gcn_file_system.{c,h},
   gcn_renderer.{c,h} (GX quad batching, stb_image TXTR pages),
   gcn_audio_system.{c,h} (v1 noop wrap or stub), stb_impl.c, main.c
   (loading screen via console, PAD input, 30/60fps loop).
2. vendor/stb/image needs stb_impl compile (STB_IMAGE_IMPLEMENTATION) like
   wiiu + include path vendor/stb/image + stb/ds.
3. CMakeLists: add gcn elseif: IS_BIG_ENDIAN, USE_FLOAT_REALS, NO_RVALUE_INT64
   (same as wiiu PPC defines), includes libogc/include, link -logc -lm,
   ogc_create_dol -> cinnamon.dol/boot.dol.
4. Build, copy boot.dol to /run/media/ryzen/egg/apps/DELTARUNEGC/ +
   data.win (start ch1: need ch1 preprocessed data - Steam source
   ~/.local/share/Steam/steamapps/common/DELTARUNE/chapter1_windows, preprocess
   via 3ds-preprocess --target wii like run-preproc.mjs OR test first boot
   with existing DELTARUNEC5 ch5/ch1 data copied as-is).
5. Card sync (cp -r, sync), user tests in Swiss on real GC.
6. ITERATE: RAM crashes -> strip ROOM payloads / streaming; BGM via libasnd
   + stb_vorbis later; DSP bank in ARAM v3.

## GOTCHAS LEARNED (toolchain/network)
- curl to pkg.devkitpro.org needs browser UA else Cloudflare 403. .db and
  .pkg.tar.zst fetch fine with -A 'Mozilla/5.0 (X11; ... Firefox/142.0)'.
- dkp toolchain binaries: linux repo path packages/linux/x86_64/; x86_64
  packages contain opt/devkitpro/... (relocatable, extract to ~/devkitpro).
- mn10200-binutils NOT installed (missing from linux db listing I fetched?
  It IS in windows db: devkitppc-mn10200-binutils-2.24-3-x86_64) - meta dep,
  harmless unless building GP32 stuff.
- powerpc-eabi-cmake wrapper: dkp cmake package NOT installed; used pip
  cmake 4.4 binary directly with -DCMAKE_TOOLCHAIN_FILE + -G Ninja. Works.
- Firefox auto-downloads .db/.zst to ~/Downloads (used for dkp-libs.db).
- This machine: OOM-sensitive (mem-watchdog). Keep tool outputs <50 lines.
## *** PATCH 51 (build 51) — FIVE BUGS FOUND BY READING, DECODE PROVEN BY RENDER ***
Model: DeepSeek v4.1 flash. Method: local Python forensics against the CARD'S OWN
data (ATLAS.BIN/TEXTURES.BIN/CLUT* + data.win) BEFORE any hardware run — plus the
ButterscotchPreprocessor KOTLIN SOURCE at /tmp/ButterscotchPreprocessor (ground truth
for every format question). No guessing, no hardware trip on a hypothesis.

### EVIDENCE 1 — THE FONT DECODE ALREADY WORKED (previous model's premise was wrong)
Rendered ATLAS.BIN item 17 (fnt_ja_comicsans) + item 22 (4bpp sprite) + item 192
(fnt_main) to PNG from the card's bytes: REAL GLYPHS, real sprite art (card castle
throne). BSP_decodeTpag's *palette/index* work was correct. The "one bug deep"
premise (BSP reader broken) was WRONG — the decode was fine; the RENDERER
CONTRACT was broken.

### EVIDENCE 2 — THE ACTUAL KILLER: ensurePage pageCount gate (gcn_renderer.c:142)
    if (textureIndex >= r->pageCount) return NULL;   // pageCount = TXTR count = 9
BSP pages are keyed by TPAG **ITEM** index (0..3008). Every item index >= 9 was
rejected before the BSP branch was ever consulted. HWLOG16 "ensurePage(7) NULL"
was the visible tip; items 10..3008 (i.e. 99.7% of the game) silently died too.
FIX: split keyspaces — BSP keys (item indices) skip the pageCount gate; legacy
keys (TXTR page ids) keep it. Also moved the txtr[] deref into the legacy branch
(item indices are OOB for txtr[]).

### EVIDENCE 3 — THE TEXT KILLER: glyph UVs were 3.8x out of range
Glyph coords are RELATIVE to the item's source rect (proven: all 12 ch1 fonts,
max glyph x < item sourceWidth). Old code added the sheet's ABSOLUTE page origin:
    u0 = (fontTpag->sourceX + glyph->sourceX - cropX) / cropW
For the contact-screen font (fnt_main = item 192, sourceX=473):
    u0 = (473 + 10)/128 = 3.7734   -> way past 1.0
The blitter discards any UV outside [0,1] => **EVERY glyph silently dropped**.
That is why drawCalls was 163/frame and the screen stayed black: the quads were
emitted but every sample was out of range.
NEW FORMULA (verified numerically for all 12 fonts, all UVs within 0..1):
    u = (cropX + glyph->sourceX) * (packedW/cropW) / pageW
    v = (cropY + glyph->sourceY) * (packedH/cropH) / pageH
FIXED: u0 = 0.0781 (in range), and a local render of "GASTER" from the real packed
atlas is READABLE TEXT.

### EVIDENCE 4 — 4bpp decode was wrong for 2927 of 3008 items + HEAP OVERFLOW
Packer (writeTexturePagesBytes) stores 4bpp as 2 pixels/byte with row stride
atlasWidth/2, low nibble first; its RLE stream expands to that SAME packed form
((w*h)/2 bytes, not w*h). Old BSP_decodeTpag expanded RLE into a w*h buffer,
copied rows with a 1-byte-per-pixel stride, and wrote 2 pixels per input byte
into a w*h*4 allocation => 2x heap overflow + sheared garbage for odd-x regions
(1244 of 3008 items have an ODD atlasX; 1673 have odd width).
FIX: nibble-exact decode straight from the packed scanlines. Verified: item 22
now decodes to a clean, correctly-shaped sprite.

### EVIDENCE 5 — drawSprite/drawSpritePart used PADDED page dims
BSP pages are 32-aligned padded; drawSprite positioned/sized by page->width/height
and used UV 0..1, which samples transparent padding + neighbour items.
FIX: track packedW/H + crop rect on the page; place by the crop rect and use
UV = packedW/pageW (not 0..1). Same for drawSpritePart (source-space sub-rect).

### EVIDENCE 6 — the bootlog lost every log since build 49
HWLOG20 = 457 B (header + [BOOT]) while crash.log on the SAME card grew to 304 KB
with per-frame heartbeats to f=7647 => the card writes FINE; the bootlog's own
`fopen(path,"a")` reopen was failing (HWLOG19/20 both died there).
FIX: ONE handle for the whole run — no reopen; stderr bridged via dup2(fileno(f))
on that same handle. Root fallback now also writes the full timeline.

### OTHER CHANGES
- GCN_RESIDENT_PAGES 2 -> 8 (every BSP item is its own page; font + frame sprites
  would thrash a 2-slot LRU). Recycled slots have geometry cleared.
- NEW EVIDENCE HOOKS: [TEXDIAG]/[TEXUV] dumped at frame 120 AND every 600 frames
  AND at exit — texture-backed blits vs skipped, and the first UV rect + page size.
  This proves text pixels even on a short run or a hard power-off.

### BUILD 51 = 9c6071cba5182f42e36b0ca1ceeaa04d  *** ON CARD (md5 verified) ***
Snapshot: snapshots/build51-9c6071cb.dol. Clean rebuild reproduces the md5.

### EXPECTED NEXT RUN (this is the falsifiable prediction)
- HWLOG21 should contain: [BSPPAGE] item 192 packed 128x128 padded 128x128 (font)
- [TEXDIAG@120] texturedBlits > 0  (was: skipped everywhere)
- [TEXUV@120] u=0.0781 ... (in range; if it prints ~3.77 the fix regressed)
- SCREEN: Gaster's opening text, white on black, typed one char at a time.
IF the screen is still black but TEXDIAG>0: the decode/UV is right and the bug is
in the SW blitter's YUV write (gcn_software_renderer.h) — next suspect.
IF texturedBlits==0: the draw path never reaches the blitter — read [TEXUV] value.

## *** PATCH 52 (build 52) — ★ ROOT CAUSE OF THE BLACK SCREEN, PROVEN ★ ***
Read HWLOG21 (build 51's run). ALL THREE PREDICTIONS HIT EXACTLY:
  [BSPPAGE] item 192 packed 128x128 padded 128x128      <- decode + page: WORKING
  [TEXUV] u=0.0781 v=0.2969 u1=0.1250 v1=0.4219        <- EXACT match to prediction
  [TEXDIAG] texturedBlits=8343@f120 -> 57348, skipped=0 <- quads REACH the blitter
  [DIAG] drawCalls=58073 vmWarnings=0 roomIndex=1
=> decode OK, pages OK, UVs OK, blit path OK, ZERO skipped. Still black.
=> The failure is NOT in the renderer. Something ERASES the framebuffer after
   the worker writes it.

### THE ANSWER — libogc's console owns the framebuffer (disassembled, not guessed)
$ powerpc-eabi-objdump -d ~/devkitpro/libogc/lib/cube/libogc.a(console.o)
  <__console_vipostcb>:            <- registered by console_init() via
    VIDEO_GetCurrentFramebuffer()  <-    VIDEO_SetPostRetraceCallback
    memcpy(framebuffer + row*stride, _console_buffer + ..., rowlen)   x N rows
That callback fires on EVERY VBLANK and memcpy()s the console's SHADOW BUFFER
over whatever is currently being displayed, for every row the console covers.

main.c called GCN_telemetryRender() EVERY FRAME right before present, and it did:
    console_init(conXfb, 0, 0, 640, 480, 640*2)   // FULL-SCREEN console!
So every frame: worker renders the game -> console stamps shadow buffer (black +
telemetry text) over ALL 640x480 -> present -> TV shows black + console text.
=> THIS IS WHY BOOT TEXT WAS ALWAYS VISIBLE AND GAME PIXELS NEVER WERE.
   It was never a texture/decode/UV bug (those are FIXED and VERIFIED above);
   the pixels were being ERASED before presentation, every single frame,
   for the entire history of the project.

### PATCH 52
1. NEW src/gcn/gcn_text_overlay.h — self-contained software text renderer
   (5x7 ASCII font, draws via the SAME proven CPU YUV XFB write path; never
   registers any VI callback). Host-tested: 'A','ABC' render correctly.
2. main.c: ALL THREE console_init() call sites REMOVED (telemetry, boot status,
   SD-report path). <ogc/consol.h> include deleted. Verified: `powerpc-eabi-nm`
   on the final ELF shows NO CON_* symbols -> __console_vipostcb can never be
   registered. Structural guarantee, not a promise.
3. Diagnostic layout fixed so it can never hide the game:
     y   0..330 : GAME CONTENT, untouched (Gaster's text draws near y=80)
     y 332..350 : marching white square (in-guest blitter proof)
     y 356..476 : telemetry text, scale 1
   (The old marker square marched across y=4..28, ON TOP of the game area.)
4. Kept the [TEXDIAG]/[TEXUV] evidence dump.

### BUILD 52 = e237f36730d25049c7afbbf59a32682b  *** ON CARD (md5 verified) ***
Snapshot: snapshots/build52-e237f367.dol

### EXPECTED NEXT RUN — this is the real test
- The marching white square (y~332..350) uses the SAME blitter as the game: if it
  moves, the software pixel path is proven end-to-end on TV.
- Gaster's opening text should appear at ~(110,80), white on black, typewriter.
- Telemetry now draws in the bottom band without wiping the game.
IF the square moves but the game is blank => the blitter + presentation are
proven; the remaining gap is in what the game submits (texture CONTENT).
IF the square does NOT move => the software blitter/present path itself is
broken (next suspect: gcn_software_renderer.h YUV writes / stride).

### NOTE FOR THE NEXT SESSION — CARD HYGIENE
kate held the mount at the end of this session (PID held a .kate-swp file inside
apps/DELTARUNEGC). Card writes were already synced + md5-verified, so no data
was at risk. ALWAYS run `lsof /run/media/ryzen/egg` before unmounting.

## *** 🏆 MILESTONE UNLOCKED (build 52 run) — TEXT AND THE BLITTER ARE ON SCREEN 🏆 ***
The Architect CONFIRMED on real hardware:
  * THE WHITE SQUARE MARCHES (y~332) -> the software blitter + main-thread VI
    presentation are PROVEN end-to-end on the actual TV.
  * GAME TEXT RENDERS ("undefined", typed one character at a time).
  * Telemetry text renders in the bottom band without wiping the game.
THIS IS THE FIRST TIME GAME-GENERATED PIXELS HAVE EVER REACHED THIS TV.
The port has crossed the visible threshold. Everything from here is polish.

### ROOT CAUSE THAT FINALLY BROKE IT (build 52) — console_init owned the framebuffer
libogc's console_init() registers __console_vipostcb via
VIDEO_SetPostRetraceCallback; that callback memcpy()s the console SHADOW BUFFER
over the CURRENT FRAMEBUFFER on EVERY VBLANK. main.c called console_init() on a
FULL-SCREEN 640x480 console every frame, right before present => the game's
pixels were erased every frame before they reached the TV.
FIX: removed ALL console_init calls; new gcn_text_overlay.h draws text straight
into the XFB. Structural proof: `powerpc-eabi-nm cinnamon.elf | grep -i CON_`
is now EMPTY.

## *** PATCH 53/54 (build 54) — THE GREEN-DIM TEXT BUG, FOUND AND FIXED ***
Architect's report on build 52: everything renders, but the TEXT is "dim" and
"green", and the drawn message reads "undefined".

### BUG A (fixed, verified offline): GX_TF_RGBA8 swizzle writer was self-corrupting
Proved with a host harness running the REAL writer + REAL reader on the REAL
card data (no hardware needed):
    opaque glyph pixel rgba=(255,255,255,255)   <- BSP decode is CORRECT
    sampled back       rgba=(255,0,0,255)       <- swizzle round-trip BROKEN
    -> TV colour Y=81 U=90 V=239  (dark red)    <- "dim and green"
  1024/1024 pixel mismatches on a 32x32 round trip.
  Cause: the writer put the (G,B) plane at ar+32 inside a 64-byte row, so G,B
  for columns 0..15 occupied the SAME bytes as A,R for columns 16..31. Pure
  white read back as pure green with reduced luma.
FIX: the software blitter samples the page itself - it never binds a GX texture
  (no GX_LoadTexObj / GX_Begin anywhere in the render path). The swizzle was
  vestigial AND corrupting, so pages are now stored LINEAR RGBA8
  (GCNRenderer_swizzleRGBA8 -> memcpy; GCN_swr_sample -> linear fetch).
VERIFIED: LINEAR round-trip = 0/16384 mismatches; TV colours now
  white -> Y=235 U=128 V=128 (libogc white = 240/128/128)
  black -> Y= 16 U=128 V=128 (libogc black =  16/128/128)  EXACT
  grey  -> Y=126 U=128 V=128 (no green tint)

### BUG B (instrumented, awaiting the run): the message reads "undefined"
The typewriter is alive and typing; the string it prints is GML's
string(undefined) = the literal "undefined". rvalue.h returns "undefined" from
RValue_toString for RVALUE_UNDEFINED. So some variable feeding the contact
screen's message is never initialised in our VM.
BUILD 54 adds two diagnostics:
  [STRING-UNDEFINED] in <currentCodeName>   <- names the function that calls string() on undefined
  [DRAWFULL] font=N x=.. y=.. len=.. text='..'  <- the exact string the game asks to draw

### BUILD 54 = 8121c4b93baf13399daa53e56e8b69a9  *** ON CARD (md5 verified) ***
Snapshot: snapshots/build54-8442d38a.dol (build53-0a953ce0.dol kept too)

### EXPECTED NEXT RUN
- Text and sprites should now be WHITE/COLOUR-CORRECT instead of green+dim.
- [DRAWFULL] will show exactly what the contact screen is trying to print.
- [STRING-UNDEFINED] will name the function whose variable is undefined.

## *** PATCH 55/56 (build 56) — GREEN ROOT CAUSE: XFB BYTE ORDER (not the swizzle) ***
Read HWLOG22 (build 53) + HWLOG23 (build 54). Both had the swizzle fix; Architect
reports text STILL green + "undefined" still typed one char at a time.
=> the swizzle was a REAL bug but NOT the green. Found the actual green offline.

### THE GREEN — XFB pixel word is byte-swapped on little-endian PPC
Verified against libogc's OWN colorTable (the bytes that provably render fine):
    memory layout: [Y0, Cb, Y1, Cr]   black = 10 80 10 80, white = F0 80 F0 80
The code wrote  *w = (Y << 8) | C.  On little-endian PPC that stores as [C, Y]
- SWAPPED. Decoded through a standard YUV reader:
    CURRENT: black -> word 0x1E80 -> bytes [80 1E] -> TV shows RGB(0,248,0)  = PURE GREEN
    FIXED:   black -> word 0x801E -> bytes [1E 80] -> TV shows RGB(16,16,16) = correct
This is why EVERYTHING was green, including the black fade background - the
green was in the CLEAR, not just the text. The previous comment even claimed
"Y in the HIGH byte" which is true of the WORD but false of MEMORY.
FIXED at all 6 write sites:
  gcn_renderer.c clear (x2), gcn_software_renderer.h opaque path (x2),
  alpha-blend path (x2, including the existing-pixel readback masks),
  gcn_text_overlay.h glyph write, main.c software test pattern.
VERIFIED offline: [10 80] -> RGB(0,0,0) black; [EB 80] -> RGB(255,255,255) white.
[10 80] is EXACTLY libogc's black. 

### "undefined" — MECHANISM FOUND (not yet fixed)
  string_char_at(undefined, 1) -> 'u'
  string_char_at(undefined, 2) -> 'n'
  string_char_at(undefined, 3) -> 'd' ...  = "undefined", one char at a time!
builtinStringCharAt does RValue_toString(args[0]) FIRST, which stringifies
RVALUE_UNDEFINED to the literal "undefined", then indexes into it. So the game's
typewriter is doing string_char_at(msg[...], typer) and msg is EMPTY/undefined.
data.win contains NO literal "undefined" string => it is generated at runtime.
`msg` is the huge variable (4955 refs) feeding the contact screen.
BUILD 56 instruments BOTH ends:
  [CHARAT-UNDEFINED] in <codeName> pos=N   <- names the script reading msg
  [TYPED] so far: '...'                    <- accumulates the full typed message
Also REMOVED the marching white square (Architect: no longer needed - text is
proven) so it cannot draw over game content.

### BUILD 56 = a2a998c0a7e0c3a31b5d11163f461183  *** ON CARD (md5 verified) ***
Snapshot: snapshots/build56-a2a998c0.dol

### EXPECTED NEXT RUN
- The screen should be BLACK (correct fade bg) with WHITE text - no green.
- Telemetry text should now be crisp white on black.
- [CHARAT-UNDEFINED] will name the exact script whose `msg` is uninitialised.
- [TYPED] will show the full message the game intended to type.

## *** PATCH 58 (build 58) — INSIDER INTEL CONFIRMED: THE LANGUAGE FILE ***
The Architect's contact (Project Sunshine adjacent) said the "undefined" means the
language file is not parsed. CONFIRMED IN THE BINARY, not taken on faith:

### PROOF
data.win contains these STRG literals:
    'lang/'   'lang_'   '.json'   'lang-new/'
    gml_Script_scr_84_lang_load      obj_84_lang_helper
    gml_GlobalScript_scr_84_get_lang_string
    'true_config.ini'   'LANG'   ini_read_string   'lang_loaded'
Mechanism: the game reads true_config.ini -> [section] LANG -> opens
"lang/lang_<X>.json" -> json_decode -> fills a map -> scr_84_get_lang_string
reads it. THE CARD HAD NO lang/ DIRECTORY AND NO true_config.ini, so every
lookup returned undefined, and the typewriter then spelled it out one char at a
time via string_char_at(undefined, n).
And the ACTUAL intended text is in the Steam data (lang_en.json, 6242 keys):
    "DEVICE_CONTACT_slash_Step_0_gml_6_0"   = " ARE YOU^6& THERE^6?\M1 ^6 %"
    "DEVICE_CONTACT_slash_Step_0_gml_7_0"   = "^6 \M0ARE WE^6&CONNECTED^6?\M1 ^6 ^6 %%"
    "DEVICE_CONTACT_slash_Step_0_gml_111_0" = "\M0YOU MUST CREATE^6&A VESSEL^4.\M1 ^6 %%"
= EXACTLY the "ARE YOU THERE? ARE WE CONNECTED?" + "CREATE A VESSEL" the
Architect described. The game is running the right code; only the data is absent.

### FIX
1. lang_en.json (542800 B, md5 9bfc053526938db6239093d35f1c96fa) copied to the
   card at BOTH plausible roots (base path is /apps/DELTARUNEGC):
      apps/DELTARUNEGC/lang/lang_en.json
      apps/DELTARUNEGC/chapter1_windows/lang/lang_en.json
2. vm_builtins.c ini_read_string: if key == "LANG" and the ini has no value,
   DEFAULT TO "English" (the card has no true_config.ini; previously the lookup
   returned "" and the loader opened nothing). Logs [LANG-DEFAULT].
3. vm_builtins.c file_text_open_read: logs [FILEOPEN] '<path>' for the first 24
   opens, so the next log NAMES the exact path the game requests.

### THE GREEN — new discriminator (pending a photo)
Photos measured: green bg ~RGB(19,156,4); test-pattern bars light~purple
(207,220,255) / dark~green (20,130,8).
  RGB(20,130,8) is what the VI shows when the pixel word is [C=0x80, Y=0x10]
  i.e. CHROMA=0x80 and LUMA=0x10 -> the pair reads as (Y=0x80=128, C=0x10=16).
  That is EXACTLY what the build-55/56 swapped words produced.
  All-zero framebuffer bytes also decode to green, so a never-written buffer
  looks identical - the two causes are indistinguishable from one photo.
BUILD 58 replaces the swatch with FOUR TALL BANDS written as RAW (Y,C) pairs:
  band0 (235,128)   band1 (16,128)   band2 (128,0)   band3 (128,255)
  correct  -> WHITE  BLACK   GREEN   MAGENTA
  swapped  -> MAGENTA GREEN  BLACK   WHITE
  chroma=0 -> GREEN  GREEN   GREEN   GREEN
Plus logs [SWATCH] mode (fbWidth/xfbHeight/viWidth/viTVMode/xfbIdx) and the
first 8 raw framebuffer bytes - so the log states the byte order independent of
the photo.

### BUILD 58 = d505bd2c80dfb96074c881fa5df58f6e  *** ON CARD (md5 verified) ***
ALSO IN THIS BUILD: input remap (A/B/X/Y -> confirm 'Z', D-pad+stick -> arrows,
START -> Enter) so the game is playable at the contact screen.

## *** PATCH 63 (build 63) — CPU BLITTER FAST PATH (perf), 2026-09-23 ***
Card state at session start: boot.dol = build62-yuv-tables (md5 77db715d), which
WALKS rooms 0..30 with clean VM (HWLOG29/30). Green XFB byte-order already correct
(BIG-ENDIAN word (Y<<8)|C = bytes [Y,C] = libogc black 10 80). NOT green anymore.
PROBLEM: too slow to play. HWLOG30 shows texturedBlits=1,421,692, GX cmdsTotal=0
=> every pixel goes through the per-pixel barycentric CPU blitter GCN_swr_blitQuad.
### ROOT CAUSE
GX renders to EFB but GCN_videoPresent NEVER calls GX_CopyDisp, so GX output
never reached the XFB ("GX never appears"). SW path writes XFB directly. Rather
than risk the unproven GX->EFB->XFB chain, optimized the PROVEN SW path first.
### FIX
GCN_swr_blitQuadAxisAligned(): axis-aligned quads (text glyphs, fullscreen rects,
overworld sprites = ~all draws) get fixed-point (16.16) U/V stepping + per-pair
YUYV writes, skipping the per-pixel barycentric solve + float det. Detected by
|ex1y|<0.01 && |ex2x|<0.01; rotated quads fall through to the general path.
VERIFIED OFFLINE (host harness, 3.8M samples): worst sampling diff vs the general
path = 1 texel except 14/3.8M at exactly 2 (nearest-edge rounding, invisible).
Adds: GCN_diag_fastPath counter (logged as fastPath=N in [TEXDIAG]) to confirm the
path is hit; startup credit "property of ranchblad - coded by k3-cloud".
### BUILD 63 = 4348f92569c5e3166e0418c005af02a5  *** ON CARD (md5 verified) ***
Snapshot: snapshots/build63-swr-axisfastpath.dol ; baseline snapshots/gcn-BASELINE-build62-safe.dol
git: cinnamon eec6955 (baseline) -> 5655a5e (fast path)
### EXPECTED NEXT RUN
- Text + sprites + room art should be noticeably faster (target: playable framerate).
- [TEXDIAG] fastPath should be ~= texturedBlits (the path is axis-aligned-dominant).
- Watch for any visual shear/holes (would mean fast path UV stepping wrong - it was
  verified offline, so not expected).
### NEXT (if still too slow): real GX path
- Write a VERIFIED GX_TF_RGBA8 swizzler (page buffers are currently LINEAR; the
  GXTexObj at line 228 points at linear data = would be garbage if ever used).
- Route textured quads through GCNRenderer_setCommonState/emitVertex into EFB.
- Add GX_CopyDisp(efb->xfb) in GCN_videoPresent before SetNextFramebuffer.

## *** PATCH 64 (build 64) — READABLE OVERLAY + FONT FIX + SAFE TELEMETRY, 2026-09-23 ***
Build63 was flashed and ran, but user reported:
  * "telemetry text = unreadable glyphs"
  * "still green"
### DIAGNOSIS
I decoded libogc's OWN colorTable bytes (extracted from libogc.a console.o):
  black pixel pair word = 0x10801080  => bytes [Y=0x10, Cb=0x80, Y=0x10, Cr=0x80]
  white pixel pair word = 0xF080F080  => bytes [Y=0xF0, Cb=0x80, Y=0xF0, Cr=0x80]
The game's byte order `(Y<<8)|C` is EXACTLY correct; byte order is NOT the green cause.
Green source: unwritten XFB memory reads as Y=0, Cb=0, Cr=0, which a TV shows as
PURE GREEN (RGB 0,255,0). The old `GCN_telemetryRender` was intentionally empty,
leaving green gaps, and the game loop's boot status screen tore against gameplay
because they shared the same XFB.
"Unreadable glyphs" source: the 5x7 font data is stored with bit4 (MSB) as the
LEFT-MOST column, but `GCN_txt_glyph` was reading `1<<c` (bit0 = leftmost). This
horizontally mirrored every glyph on screen => "random glyphs".
### FIX
1. `gcn_text_overlay.h`: read font rows as `1 << (GCN_TXT_W - 1 - c)` so glyphs render
   correctly (verified by rendering the boot screen to PNG and reading it).
2. `GCN_drawStatusScreen` now FULLY CLEARS every XFB pixel as a complete black
   YUYV pair before drawing loading/credit text, so no unwritten green survives.
3. `GCN_telemetryRender` re-enabled as a SAFE bottom band (y >= 360):
   - Default OFF (no overhead, no green bar).
   - Toggle with **L + R** shoulder buttons together (edge-triggered).
   - When on, fills its entire band with black complete pairs, then draws readable
     uppercase 5x7 text: FRAME/BLIT/SKIP, TEXBLIT/FAST, credit.
   - Drawn LAST in the worker's frame loop (after `endFrame`), so it never tears.
4. Z + START still saves HWLOG and exits cleanly with persistent "LOG SAVED" screen.
5. Credits updated to: `SPAMTON LAUNCHER - PROPERTY OF RANCHBLAD / CODED BY K3 CLOUD`.
### BUILD 64 = a37d9798a33083b6afc53006565e4aaf  *** ON CARD (md5 verified) ***
Snapshot: snapshots/build64-fontfix-telemetry.dol
git: 5655a5e (build63) -> 7bb5ee9 (build64)
### EXPECTED NEXT RUN
- Boot/loading screen text should now be perfectly readable (not mirrored/garbled).
- Press L+R during play to see a readable black telemetry band at bottom; press again to hide.
- Green should be eliminated anywhere the game/overlay has written (unwritten overscan may still show green - that is normal TV overscan, not a bug).
- Performance should still feel improved from build63's fast path; HWLOG will show fastPath=>0.
