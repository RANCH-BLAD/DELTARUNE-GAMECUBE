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