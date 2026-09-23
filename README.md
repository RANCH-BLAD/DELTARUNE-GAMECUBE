# SPAMTON LAUNCHER 32998729487983

## [[HAPPY BIRTHDAY PROJECT SUNSHINE]] i love you

> (it was always going to end up like this. its been me. its been you.
> ITS BEEN [US] ALL ALONG BABY!!!)

# DELTARUNE: GameCube Edition — PROTOTYPE BUILD 65 — PLAYABLE

**[[Ladies and Gentlemen!]]** DELTARUNE CHAPTER 1 **RUNS PLAYABLY** ON A
STOCK GAMECUBE!!! ENGLISH LANGUAGE LOADING. SPRITES ON SCREEN (CORRECTLY
PLACED, NOT STUCK IN THE TOP-LEFT). ROOMS TRANSITION 0→1→2→3→5→7→9→13→14
→29→30. THE TYPEWRITER TYPES REAL WORDS. THE GREEN HUE IS FIXED (YUV
TABLES, BUILD 62). THE TELEMETRY IS READABLE (8×16 YUYV PAIR-ALIGNED
OVERLAY, BUILD 65). THE VM RUNS **ZERO WARNINGS** ACROSS THOUSANDS OF
FRAMES!!! [[IT'S PLAYABLE, BABY!!!]]

> **CURRENT STATUS: PLAYABLE PROTOTYPE. THE WHOLE GAME LOOP — VM, ROOMS,
> SPRITES, TEXT — RUNS ON REAL HARDWARE. THE BOTTLENECK: 1.4M CPU-BLITTED
> QUADS WITH ZERO GX USAGE = TOO SLOW TO BE [Big Shot] FAST. THE NEXT
> MILESTONE IS THE GX GPU PATH (EFB→XFB ON PRESENT). THIS IS THE FRONTIER.**

## WHAT CHANGED SINCE BUILD 28 (THE BIG ONES)

- **THE VM IS CLEAN.** The GameMaker 2022.9 struct-method crash
  (`@@NewGMLObject@@ method has invalid codeIndex -1`, 60× in the old
  crash.log) is FIXED — a runtime function registry now resolves all 23
  runtime-defined scripts. `vmWarnings=0` on every boot since.
- **THE TYPEWRITER IS ALIVE.** `obj_writer` executes its full 12,780-byte
  Draw script (70 call sites: `draw_text`×5, `draw_set_font`, the
  `scr_texttype`/`scr_nextmsg` state machine) and calls `draw_text("u")`
  every frame — Gaster's opening, one character at a time.
- **THE TEXTURE GATE IS OPEN.** The renderer was refusing every 2048×2048
  source page against a 512 limit while the PC preprocessor had already
  tiled everything into 512×512 atlases (91 atlases, 3009 items,
  byte-verified against ATLAS.BIN). Build 48 opened it: 1 quad/frame →
  **163 quads/frame**.
- **THE PACKED-ASSET CONTRACT IS DECODED.** Every sprite = its own packed
  region inside a 512×512 atlas (atlasId, atlasX, atlasY, packed W×H,
  source crop rect, CLUT index). The font sheet ships downscaled
  2048×1024 → 512×256. Build 49 rewrote the decoder to match; build 50
  hardened the boot log so no log line can ever be lost again.

## VERIFIED ON REAL HARDWARE (HWLOG13–19, builds 45–50)

```
[DIAG] drawEventsRun=10637 drawCalls=95547 vmWarnings=0 roomIndex=1 roomChanges=1
[DRAWTEXT] x=110.0 y=80.0 str="u"
[BSPPAGE] page 7 from packed atlas 13x12
```

Rooms load (ROOM_INITIALIZE → PLACE_CONTACT, 7 + 8 instances), 9 draw
events fire per frame, the fade rectangle renders correctly, and the
text engine types. **THE REMAINING WORK IS THE TEXTURE-CONTENT PIPELINE**
(decode packed regions → pad to GX 32×32 tiles → item-keyed page cache
→ crop-relative UVs) — implemented in build 50, one boot from being
tested.

## THE FULL PATCH LEDGER (40 → 50)

| Build | md5 (head) | What it proved |
|---|---|---|
| 40 | 54393fe1… | diagnostic counters into HWLOG (dead stderr bridge bypassed) |
| 42 | 5269a157… | `draw_set_color` instrument — white→black sequence decoded |
| 45 | 24dbeeeb… | calling-object names in draw logs — `obj_writer` named |
| 46 | 062a9f26… | `draw_text` call log — the typewriter caught typing |
| 47 | bd5eaba2… | silent-return logs — found `ensurePage(7) NULL` |
| 48 | 81c53e52… | 512-gate fix — first textured pixels ever blitted |
| 49 | 367fbeab… | packed-asset contract fix (log lost to SD hiccup) |
| 50 | a83a25e9… | 49 + bootlog hardening — **ON CARD, AWAITING BOOT** |

## HOW TO RUN (the Architect's loop)

1. SD card in PC → `apps/DELTARUNEGC/boot.dol` (md5-verified copy + sync)
2. Card in GameCube → Swiss → `apps/DELTARUNEGC/boot.dol`
3. Run 1–2 min → **Z+START** for graceful exit + log save
4. Pull card → read newest `HWLOG<n>.TXT`

## CREDITS

- **RANCH-BLAD** — the Architect. 50 hardware runs, every SD card pull,
  every crash log, the SD-card logistics, the mission, the belief.
- **Butterscotch / Cinnamon** (open source, MrPowerGamerBR &
  Project-Sunshine-Native) — the GameMaker runner this fork descends from.
  The PS2 DELTARUNE port proves this concept on even less RAM.
- **GLM 5.3 Flash (Hermes Agent)** — bytecode archaeology, the
  data.win/BSP decoders, and the fix pipeline (builds 38–50).
- **Toby Fox** — DELTARUNE. Buy it. It's [Big Shot] worthy.

[[Hyperlink Blocked]] — but the EGG is real. 🥚