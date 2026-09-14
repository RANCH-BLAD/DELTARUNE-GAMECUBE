#include "../data_win.h"
#include "../vm.h"
#include "../runner.h"
#include "../runner_keyboard.h"

#include "gcn_file_system.h"
#include "gcn_renderer.h"
#include "gcn_audio_system.h"

#include <gccore.h>
#include <fat.h>
#include <ogc/consol.h>
#include <ogc/video.h>
#include <ogc/video_types.h>
#include <ogc/gx.h>
#include <ogc/pad.h>
#include <ogc/system.h>
#include <ogc/lwp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <fcntl.h>

// EGG: the launcher's soul. SPAMTON G SPAMTON was here. [E.G.G. SYSTEM ONLINE]
#define GCN_GAME_DIR "/apps/DELTARUNEGC"
#define GCN_CRASH_LOG_DEFAULT "/apps/DELTARUNEGC/crash.log"
#define GCN_CRASH_LOG_SD "fat:/apps/DELTARUNEGC/crash.log"
#define GCN_FIFO_SIZE (256 * 1024)
#define GCN_WORKER_STACK_SIZE (512 * 1024)

static void* gXfb[2] = { NULL, NULL };
static u32 gXfbIndex = 0;
static GXRModeObj* gRMode = NULL;
static void* gFifo = NULL;
static lwp_t gWorkerThread = LWP_THREAD_NULL;
static volatile bool gWorkerDone = false;
static int gWorkerResult = 0;
static char gStatusLine[80] = "starting...";

// On-screen status (last message shown at bottom of screen each frame).
static void GCN_setStatus(const char* message) {
    snprintf(gStatusLine, sizeof(gStatusLine), "%s", message);
}

// Boot-stage diagnostics written DIRECTLY to files (stderr may not exist).
static void GCN_writeDiagFile(const char* text) {
    const char* logPaths[] = {
        "/DELTA_SD_REPORT.TXT",
        "fat:/DELTA_SD_REPORT.TXT"
    };
    for (size_t i = 0; i < sizeof(logPaths) / sizeof(logPaths[0]); ++i) {
        FILE* f = fopen(logPaths[i], "w");
        if (f != NULL) {
            fputs(text, f);
            fclose(f);
        }
    }
    FILE* marker = fopen("/DELTA_WAS_HERE.TXT", "w");
    if (marker != NULL) {
        fputs("deltarune gc diagnostic build\n", marker);
        fclose(marker);
    }
}

// All Cinnamon require()/fprintf(stderr) diagnostics land in this file on SD.
static void GCN_openCrashLog(void) {
    // dup2 trick: point stderr's file descriptor at our log file so ALL
    // require()/fprintf(stderr) diagnostics in the codebase land on SD.
    // freopen failed silently on the default devoptab; dup2 works at the
    // descriptor level.
    int logfd = open(GCN_CRASH_LOG_DEFAULT, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (logfd < 0) {
        logfd = open(GCN_CRASH_LOG_SD, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (logfd >= 0) {
        dup2(logfd, fileno(stderr));
        close(logfd);
        setvbuf(stderr, NULL, _IONBF, 0); // unbuffered: survives crashes
        fprintf(stderr, "=== DELTARUNE GC crash log ===\n");
    }
}

static void GCN_videoInit(void) {
    VIDEO_Init();
    gRMode = VIDEO_GetPreferredMode(NULL);
    gXfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gRMode));
    gXfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gRMode));
    VIDEO_Configure(gRMode);
    VIDEO_ClearFrameBuffer(gRMode, gXfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(gRMode, gXfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(gXfb[0]);
    VIDEO_SetBlack(GX_FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (gRMode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    gXfbIndex = 1;
}

// Minimal text on the XFB for boot status (no console.h dependency dance).
static void GCN_drawStatusScreen(void) {
    // Draw via console for simplicity: console renders into the active xfb.
    static bool consoleReady = false;
    if (!consoleReady) {
        console_init(gXfb[0], 20, 20, gRMode->fbWidth, gRMode->xfbHeight, gRMode->fbWidth * VI_DISPLAY_PIX_SZ);
        consoleReady = true;
    }
    printf("\x1b[2;0HDELTARUNE GameCube [Cinnamon]\n\x1b[4;0H%s\n", gStatusLine);
    VIDEO_SetNextFramebuffer(gXfb[0]);
    VIDEO_Flush();
}

static void GCN_videoPresent(void) {
    GX_SetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(gXfb[gXfbIndex], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(gXfb[gXfbIndex]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    gXfbIndex ^= 1;
}

static void GCN_gxInit(void) {
    gFifo = memalign(32, GCN_FIFO_SIZE);
    memset(gFifo, 0, GCN_FIFO_SIZE);
    GX_Init(gFifo, GCN_FIFO_SIZE);
    GX_SetDispCopyYScale((f32) gRMode->xfbHeight / (f32) gRMode->efbHeight);
    GX_SetScissor(0, 0, gRMode->fbWidth, gRMode->efbHeight);
    GX_SetDispCopySrc(0, 0, gRMode->fbWidth, gRMode->efbHeight);
    GX_SetDispCopyDst(gRMode->fbWidth, gRMode->xfbHeight);
    GX_SetCopyFilter(gRMode->aa, gRMode->sample_pattern, GX_TRUE, gRMode->vfilter);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetDispCopyGamma(GX_GM_1_0);
}

static void GCN_syncKey(RunnerKeyboardState* keyboard, bool* held, int32_t key, bool pressed) {
    if (pressed != *held) {
        if (pressed) RunnerKeyboard_onKeyDown(keyboard, key);
        else RunnerKeyboard_onKeyUp(keyboard, key);
        *held = pressed;
    }
}

typedef struct {
    bool left, right, up, down;
    bool a, b, x, y, start, z, l, r;
} GCNInputState;

static char parseStage[80];

static void GCN_parseProgress(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dataWin, void* userData) {
    (void) dataWin;
    (void) userData;
    snprintf(parseStage, sizeof(parseStage), "parsing %s (%d/%d)", chunkName, chunkIndex + 1, totalChunks);
    GCN_setStatus(parseStage);
    GCN_drawStatusScreen();
}

static int GCN_gameMain(void) {
    GCN_setStatus("mounting SD...");
    GCN_drawStatusScreen();

    if (!fatInitDefault()) {
        GCN_setStatus("SD mount FAILED (fatInitDefault)");
        GCN_drawStatusScreen();
        while (1) { usleep(1000); }
        return 1;
    }

    GCN_openCrashLog(); // after fat mount: log paths need the filesystem

    char dataWinPath[256];
    const char* roots[] = { "/apps/DELTARUNEGC", "fat:/apps/DELTARUNEGC", "sd:/apps/DELTARUNEGC" };
    const char* subdirs[] = { "/chapter1_windows/data.win", "/data.win" };
    FILE* probe = NULL;
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]) && probe == NULL; ++r) {
        for (size_t s = 0; s < sizeof(subdirs) / sizeof(subdirs[0]) && probe == NULL; ++s) {
            snprintf(dataWinPath, sizeof(dataWinPath), "%s%s", roots[r], subdirs[s]);
            snprintf(gStatusLine, sizeof(gStatusLine), "trying: %s", dataWinPath);
            probe = fopen(dataWinPath, "rb");
            if (probe != NULL) {
                GCNFileSystem_setBasePath(roots[r]);
                GCN_setStatus("data.win found!");
            GCN_drawStatusScreen();
            }
        }
    }
    if (probe != NULL) fclose(probe);
    else {
        // Build a full SD visibility report: on TV + written straight to files
        // (stderr/devoptab may not be usable at all).
        char report[4096];
        size_t off = 0;
        off += snprintf(report + off, sizeof(report) - off, "DELTARUNE GC SD report\n");
        const char* dumpRoots[] = { "/", "/apps", "/apps/DELTARUNEGC", "fat:/" };
        for (size_t r2 = 0; r2 < sizeof(dumpRoots) / sizeof(dumpRoots[0]); ++r2) {
            DIR* dir = opendir(dumpRoots[r2]);
            if (dir == NULL) {
                off += snprintf(report + off, sizeof(report) - off, "opendir(%s) FAILED\n", dumpRoots[r2]);
                continue;
            }
            off += snprintf(report + off, sizeof(report) - off, "listing %s:\n", dumpRoots[r2]);
            struct dirent* ent;
            int listed = 0;
            while ((ent = readdir(dir)) != NULL && listed < 30) {
                off += snprintf(report + off, sizeof(report) - off, "  %s\n", ent->d_name);
                listed++;
            }
            closedir(dir);
        }
        GCN_writeDiagFile(report);

        // Show the first lines ON SCREEN so we can see it without the card read:
        static bool consoleReady = false;
        if (!consoleReady) {
            console_init(gXfb[0], 20, 20, gRMode->fbWidth, gRMode->xfbHeight, gRMode->fbWidth * VI_DISPLAY_PIX_SZ);
            consoleReady = true;
        }
        printf("\x1b[2;0HDELTARUNE GC - data.win not found. SD report:\n%s\n", report);
        printf("\x1b[20;0H(full report also written to DELTA_SD_REPORT.TXT on SD)\n");
        VIDEO_SetNextFramebuffer(gXfb[0]);
        VIDEO_Flush();
        while (1) { VIDEO_WaitVSync(); }
        return 1;
    }

    GCN_setStatus("parsing data.win chunks...");
    GCN_drawStatusScreen();
    DataWin* dataWin = DataWin_parse(
        dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = false,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = false,
            .parseAudoHeadersOnly = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .lazyLoadRooms = true,
            .progressCallback = GCN_parseProgress,
            .progressCallbackUserData = NULL,
        }
    );

    if (dataWin == NULL) {
        GCN_setStatus("data.win PARSE FAILED (see crash.log)");
        while (1) { usleep(1000); }
        return 1;
    }

    GCN_setStatus("init systems...");
    GCN_drawStatusScreen();

    FileSystem* fileSystem = (FileSystem*) GCNFileSystem_create(GCNFileSystem_getBasePath(), GCNFileSystem_getBasePath());
    AudioSystem* audioSystem = GCNAudioSystem_create();
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);

    VMContext* vm = VM_create(dataWin);
    Renderer* renderer = GCNRenderer_create();
    GCNRenderer_setDataWinFile(renderer, dataWinPath);
    renderer->vtable->init(renderer, dataWin);

    if (!GCNRenderer_isReady(renderer)) {
        const char* error = GCNRenderer_getStartupError(renderer);
        GCN_setStatus(error != NULL ? error : "renderer init failed");
        while (1) { usleep(1000); }
        return 1;
    }
    GCNRenderer_openDataWinFile(renderer, dataWinPath);

    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);
    runner->osType = OS_WINDOWS; // DELTARUNE expects the Windows runner
    GCN_setStatus("init first room...");
    GCN_drawStatusScreen();
    Runner_initFirstRoom(runner);

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;

    GCN_setStatus("running");
    GCN_drawStatusScreen();

    GCNInputState inputPre = {0};
    uint32_t heartbeatFrame = 0;
    char heartLine[80];

    GCNInputState input = {0};

    while (!runner->shouldExit) {
        PAD_ScanPads();
        int buttonsHeld = PAD_ButtonsHeld(0);
        s8 stickX = PAD_StickX(0);
        s8 stickY = PAD_StickY(0);

        if (PAD_ButtonsDown(0) & PAD_BUTTON_START) break;
        heartbeatFrame++;
        if ((heartbeatFrame & 15) == 0) {
            extern volatile const char* GCNRenderer_lastDrawCall;
            extern volatile uint32_t GCNRenderer_frameCounter;
            snprintf(heartLine, sizeof(heartLine), "f:%u %s last:%s",
                GCNRenderer_frameCounter,
                (heartbeatFrame & 16) ? "[*]" : "[.]",
                GCNRenderer_lastDrawCall);
            GCN_setStatus(heartLine);
            fprintf(stderr, "f=%u last=%s\n", GCNRenderer_frameCounter, GCNRenderer_lastDrawCall);
        }

        GCN_syncKey(runner->keyboard, &input.left, VK_LEFT,
            (buttonsHeld & PAD_BUTTON_LEFT) || stickX < -40);
        GCN_syncKey(runner->keyboard, &input.right, VK_RIGHT,
            (buttonsHeld & PAD_BUTTON_RIGHT) || stickX > 40);
        GCN_syncKey(runner->keyboard, &input.up, VK_UP,
            (buttonsHeld & PAD_BUTTON_UP) || stickY > 40);
        GCN_syncKey(runner->keyboard, &input.down, VK_DOWN,
            (buttonsHeld & PAD_BUTTON_DOWN) || stickY < -40);
        GCN_syncKey(runner->keyboard, &input.a, 'Z', (buttonsHeld & PAD_BUTTON_A) != 0);
        GCN_syncKey(runner->keyboard, &input.b, 'X', (buttonsHeld & PAD_BUTTON_B) != 0);
        GCN_syncKey(runner->keyboard, &input.x, 'C', (buttonsHeld & PAD_BUTTON_X) != 0);
        GCN_syncKey(runner->keyboard, &input.y, 'C', (buttonsHeld & PAD_BUTTON_Y) != 0);
        GCN_syncKey(runner->keyboard, &input.start, VK_ENTER, (buttonsHeld & PAD_BUTTON_START) != 0);
        GCN_syncKey(runner->keyboard, &input.z, VK_SHIFT, (buttonsHeld & PAD_TRIGGER_Z) != 0);
        GCN_syncKey(runner->keyboard, &input.l, VK_PAGEDOWN, (buttonsHeld & PAD_TRIGGER_L) != 0);
        GCN_syncKey(runner->keyboard, &input.r, VK_PAGEUP, (buttonsHeld & PAD_TRIGGER_R) != 0);
        (void) stickX; (void) stickY;

        Runner_step(runner);
        runner->audioSystem->vtable->update(runner->audioSystem, 1.0f / 60.0f);

        float displayScaleX = 1.0f;
        float displayScaleY = 1.0f;
        Runner_computeViewDisplayScale(runner, gameW, gameH, &displayScaleX, &displayScaleY);

        renderer->vtable->beginFrame(renderer, gameW, gameH, 640, 480);
        Runner_drawViews(runner, gameW, gameH, displayScaleX, displayScaleY, false);
        renderer->vtable->endFrame(renderer);
        GCN_videoPresent();

        RunnerKeyboard_beginFrame(runner->keyboard);
    }

    audioSystem->vtable->destroy(audioSystem);
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    GCNFileSystem_destroy((GCNFileSystem*) fileSystem);
    VM_free(vm);
    DataWin_free(dataWin);
    return 0;
}

static void* GCN_workerEntry(void* arg) {
    (void) arg;
    gWorkerResult = GCN_gameMain();
    gWorkerDone = true;
    return NULL;
}

static void GCN_drawTestPattern(void) {
    // Solid color bars via GX: validates video+GPU with zero dependencies.
    Mtx44 projection;
    guOrtho(projection, 0.0f, 480.0f, 0.0f, 640.0f, 0.0f, 1000.0f);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    GX_SetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetNumTevStages(1);
    GX_SetNumChans(1);
    GX_SetNumTexGens(0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GX_SetScissor(0, 0, 640, 480);

    const uint32_t colors[8] = {
        0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF, 0x808080
    };
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    for (int bar = 0; bar < 8; ++bar) {
        uint8_t r = (uint8_t) ((colors[bar] >> 16) & 0xFF);
        uint8_t g = (uint8_t) ((colors[bar] >> 8) & 0xFF);
        uint8_t b = (uint8_t) (colors[bar] & 0xFF);
        float x0 = bar * 80.0f;
        float x1 = x0 + 80.0f;
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position2f32(x0, 0.0f); GX_Color4u8(r, g, b, 255);
            GX_Position2f32(x1, 0.0f); GX_Color4u8(r, g, b, 255);
            GX_Position2f32(x1, 480.0f); GX_Color4u8(r, g, b, 255);
            GX_Position2f32(x0, 480.0f); GX_Color4u8(r, g, b, 255);
        GX_End();
    }
    GX_DrawDone();
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    GCN_videoInit();
    PAD_Init();
    GCN_gxInit();

    // Hold B while booting = GPU test pattern (no SD needed; also works in
    // Dolphin as a renderer sanity check).
    PAD_ScanPads();
    if (PAD_ButtonsHeld(0) & PAD_BUTTON_B) {
        while (1) {
            GCN_drawTestPattern();
            GCN_videoPresent();
            VIDEO_WaitVSync();
            PAD_ScanPads();
            if (PAD_ButtonsDown(0) & PAD_BUTTON_START) break;
        }
        return 0;
    }

    // The GameMaker VM recurses deeply; libogc's main thread stack is 8KB.
    // Run the whole game on a worker thread with a fat stack instead.
    void* workerStack = memalign(32, GCN_WORKER_STACK_SIZE);
    if (workerStack == NULL || LWP_CreateThread(&gWorkerThread, GCN_workerEntry, NULL, workerStack, GCN_WORKER_STACK_SIZE, 68) != 0) {
        GCN_setStatus("FATAL: worker thread creation failed");
        GCN_drawStatusScreen();
        while (1) { VIDEO_WaitVSync(); }
        return 1;
    }

    // Single-video-owner design: the worker owns ALL GX/VI access now.
    // main() just blocks on the worker (a frozen screen means the worker
    // printed its last stage before hanging - which is our diagnostic).
    while (!gWorkerDone) {
        usleep(1000);
    }
    LWP_JoinThread(gWorkerThread, NULL);
    free(workerStack);
    return gWorkerResult;
}
