#include "gcn_renderer.h"
#include <gccore.h>
#include <ogc/video_types.h>
#include "gcn_software_renderer.h"

#include "../data_win.h"
#include "../text_utils.h"

#include <gccore.h>
#include <ogc/gx.h>
#include <ogc/gx_struct.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <ogc/video_types.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#define STBI_NO_THREAD_LOCALS
#include <stb/image/stb_image.h>
#include "image_decoder.h"

#define GCN_MAX_QUADS 1024
// Every BSP sprite/text item is its OWN page, so the cache needs room for a
// font + the sprites of one frame without thrashing. 8 x 32-aligned pages,
// LRU-evicted; buffers are realloc'd only when they must grow.
#define GCN_RESIDENT_PAGES 8
#define GCN_MAX_PAGE_DIM 512
#define GCN_VERTICES_PER_QUAD 6

typedef struct {
    GXTexObj texObj;
    uint32_t width, height;
    float scale;           // originalWidth / width (1.0 = no downscale; 2.0 = 2048->1024)
    uint32_t ownerTexture; // TXTR index; UINT32_MAX = free slot
    uint32_t lastUsedStamp;
    bool ready;
    // === BSP item geometry (valid only for BSP-keyed pages) ===
    // The packed region (packedW x packedH) is packedW/page->width of the page,
    // and it covers the item's source-rect region [cropX, cropX+cropW) at
    // source scale. UV math MUST map source-rect coords through these.
    uint32_t packedW, packedH;
    uint16_t cropX, cropY, cropW, cropH;
} GCNTexturePage;



struct GCNRenderer {
    Renderer base;

    int32_t frameW, frameH;
    int32_t viewX, viewY, viewW, viewH;
    int32_t portX, portY, portW, portH;
    float viewScaleX, viewScaleY;

    GCNQuadCommand commands[GCN_MAX_QUADS];
    uint32_t commandCount;

    char* dataWinPath;
    FILE* dataWinFile;
    GCNTexturePage pages[GCN_RESIDENT_PAGES];
    uint8_t* pageBuffers[GCN_RESIDENT_PAGES];
    uint32_t pageBufferSizes[GCN_RESIDENT_PAGES];
    uint32_t pageCount;
    uint32_t useStamp;

    GXTexObj whiteTexObj;
};

static const char* gStartupError = NULL;

// Crash forensics: the main loop displays this so a frozen screen shows the
// exact renderer call that was running when everything stopped.
volatile const char* GCNRenderer_lastDrawCall = "none";
volatile uint32_t GCNRenderer_frameCounter = 0;
volatile uint32_t GCNRenderer_statsCommands = 0;
volatile uint32_t GCNRenderer_statsBlitted = 0;
volatile uint32_t GCNRenderer_statsSkipped = 0;
volatile uint32_t GCNRenderer_statsCmdsTotal = 0;
// TEXT-PIXEL EVIDENCE: how many texture-backed quads got blitted (vs skipped),
// plus the first UV rect + page dims, dumped in the exit timeline.
volatile uint32_t GCN_diag_texturedBlits = 0;
volatile uint32_t GCN_diag_fastPath = 0;
volatile float GCN_diag_firstU = -999.0f;
volatile float GCN_diag_firstV = -999.0f;
volatile float GCN_diag_firstU1 = -999.0f;
volatile float GCN_diag_firstV1 = -999.0f;
volatile uint32_t GCN_diag_firstTexW = 0;
volatile uint32_t GCN_diag_firstTexH = 0;

const char* GCNRenderer_getStartupError(Renderer* renderer) {
    (void) renderer;
    return gStartupError;
}

bool GCNRenderer_isReady(Renderer* renderer) {
    (void) renderer;
    return gStartupError == NULL;
}

void GCNRenderer_setDataWinFile(Renderer* renderer, const char* dataWinPath) {
    GCNRenderer* r = (GCNRenderer*) renderer;
    free(r->dataWinPath);
    r->dataWinPath = safeStrdup(dataWinPath);
}

// NOTE: the page buffer is kept LINEAR RGBA8.
// The software blitter samples it itself (GCN_swr_sample), so the GX_TF_RGBA8
// swizzle served no purpose here - and the previous swizzle writer was BROKEN:
// it wrote A,R for col 16..31 into the same bytes as G,B for col 0..15
// (ar spans 64B but gb was placed at ar+32), so every textured quad read back
// a scrambled colour. That is what made all game text dim and green while
// solid (untextured) quads stayed white. Verified offline: 1024/1024 pixel
// mismatches on a round-trip test; with linear storage it is 0/1024.
static void GCNRenderer_swizzleRGBA8(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst) {
    (void) width; (void) height;
    memcpy(dst, src, (size_t) width * (size_t) height * 4u);
}

static void GCNRenderer_initWhiteTexture(GCNRenderer* r) {
    static uint8_t white[2048] __attribute__((aligned(32)));
    memset(white, 0xFF, sizeof(white));
    DCFlushRange(white, sizeof(white));
    GX_InitTexObj(&r->whiteTexObj, white, 32, 32, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&r->whiteTexObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, 0, 0, GX_ANISO_1);
}

static GCNTexturePage* GCNRenderer_findResident(GCNRenderer* r, uint32_t textureIndex) {
    for (uint32_t slot = 0; slot < GCN_RESIDENT_PAGES; ++slot) {
        if (r->pages[slot].ready && r->pages[slot].ownerTexture == textureIndex) {
            r->pages[slot].lastUsedStamp = ++r->useStamp;
            return &r->pages[slot];
        }
    }
    return NULL;
}

// Free a decoded RGBA buffer with the right deallocator.
static void GCN_freePixels(uint8_t* pixels, bool fromStbi) {
    if (pixels == NULL) return;
    if (fromStbi) stbi_image_free(pixels);
    else free(pixels);
}

// Resolve a page-key into BSP item space.
//   * BSP path: keys are TPAG ITEM indices (0..tpag.count-1).
//   * legacy path: keys are TXTR page indices (0..pageCount-1).
// Anything with BSP data is a BSP key, so the legacy bounds check must not
// run first (HWLOG16: it rejected every item index >= TXTR count).
static GCNTexturePage* GCNRenderer_ensurePage(GCNRenderer* r, uint32_t textureIndex) {
    GCNRenderer_lastDrawCall = "ensurePage";
    extern bool BSP_hasTpag(int32_t);
    bool bspAvailable = BSP_hasTpag((int32_t) textureIndex);
    // BSP keys are item indices; ONLY the legacy path is bounded by pageCount,
    // and ONLY the legacy path indexes dataWin->txtr (item keys would be OOB).
    if (!bspAvailable) {
        if (textureIndex >= r->pageCount) return NULL;
        if (r->dataWinFile == NULL) return NULL;
        Texture* tex = &r->base.dataWin->txtr.textures[textureIndex];
        if (tex->blobOffset == 0 || tex->blobSize == 0) return NULL;
        if (tex->textureWidth > GCN_MAX_PAGE_DIM || tex->textureHeight > GCN_MAX_PAGE_DIM) return NULL;
    }
    GCNTexturePage* resident = GCNRenderer_findResident(r, textureIndex);
    if (resident != NULL) return resident;

    int32_t victim = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint32_t slot = 0; slot < GCN_RESIDENT_PAGES; ++slot) {
        if (!r->pages[slot].ready) {
            victim = (int32_t) slot;
            break;
        }
        if (r->pages[slot].lastUsedStamp < oldest) {
            oldest = r->pages[slot].lastUsedStamp;
            victim = (int32_t) slot;
        }
    }
    if (victim < 0) return NULL;
    GCNTexturePage* page = &r->pages[victim];
    page->ready = false;             // recycled slot: never expose stale geometry
    page->packedW = page->packedH = 0;
    page->cropX = page->cropY = page->cropW = page->cropH = 0;
    page->ownerTexture = UINT32_MAX;

    // === BSPACK PATH (ButterscotchPreprocessor pre-converted assets) ===
    // Linear CLUT data -> RGBA8, stored LINEAR (the software blitter samples
    // it directly; no GX, no swizzle). No '2zoq' decode.
    {
        extern bool BSP_hasTpag(int32_t);
        extern uint8_t* BSP_decodeTpag(int32_t, int32_t*, int32_t*);
        if (BSP_hasTpag((int32_t) textureIndex)) {
            int32_t bw = 0, bh = 0;
            uint8_t* linear = BSP_decodeTpag((int32_t) textureIndex, &bw, &bh);
            if (linear == NULL) {
                GCNRenderer_lastDrawCall = "bspDecodeFail";
                return NULL;
            }
            // Page buffer keeps the item's own dims but is 32-aligned so the
            // blitter's row math stays simple; padding is transparent.
            uint32_t pw = ((uint32_t) bw + 31u) & ~31u;
            uint32_t ph = ((uint32_t) bh + 31u) & ~31u;
            uint32_t needed = pw * ph * 4;
            if (r->pageBuffers[victim] == NULL || r->pageBufferSizes[victim] < needed) {
                if (r->pageBuffers[victim] != NULL) free(r->pageBuffers[victim]);
                r->pageBuffers[victim] = memalign(32, needed);
                r->pageBufferSizes[victim] = needed;
            }
            if (r->pageBuffers[victim] == NULL) { free(linear); return NULL; }
            uint8_t* padded = (uint8_t*) malloc(pw * ph * 4);
            if (padded == NULL) { free(linear); return NULL; }
            for (uint32_t y = 0; y < ph; ++y) {
                uint32_t* dst = (uint32_t*)(padded + y * pw * 4);
                if (y < (uint32_t) bh) {
                    const uint32_t* srcRow = (const uint32_t*)(linear + (uint32_t) y * (uint32_t) bw * 4);
                    for (uint32_t x = 0; x < pw; ++x)
                        dst[x] = x < (uint32_t) bw ? srcRow[x] : 0u;
                } else {
                    for (uint32_t x = 0; x < pw; ++x) dst[x] = 0u;
                }
            }
            GCNRenderer_swizzleRGBA8(padded, pw, ph, r->pageBuffers[victim]);
            DCFlushRange(r->pageBuffers[victim], needed);
            free(padded);
            free(linear);
            GX_InitTexObj(&page->texObj, r->pageBuffers[victim], pw, ph, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GX_InitTexObjLOD(&page->texObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, 0, 0, GX_ANISO_1);
            page->width = pw;
            page->height = ph;
            page->scale = 1.0f; // pre-scaled on the PC; source coords are 1:1
            page->ownerTexture = textureIndex;
            page->ready = true;
            page->lastUsedStamp = ++r->useStamp;
            // Record the BSP geometry so callers can map SOURCE-RECT coords
            // into this page: u = cropX + srcCoord scaled by packedW/cropW.
            {
                extern void BSP_tpagFrameRect(int32_t, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*);
                int32_t sx = 0, sy = 0, cx = 0, cy = 0, cw = 0, ch = 0;
                BSP_tpagFrameRect((int32_t) textureIndex, &sx, &sy, &cx, &cy, &cw, &ch);
                page->packedW = (uint32_t) bw;
                page->packedH = (uint32_t) bh;
                page->cropX = (uint16_t) cx;
                page->cropY = (uint16_t) cy;
                page->cropW = (uint16_t) (cw > 0 ? cw : bw);
                page->cropH = (uint16_t) (ch > 0 ? ch : bh);
            }
            {
                extern void GCN_bootlog(const char* fmt, ...);
                static int bspPageLogged = 0;
                if (bspPageLogged++ < 6)
                    GCN_bootlog("[BSPPAGE] item %u packed %dx%d padded %ux%u", textureIndex, bw, bh, pw, ph);
            }
            return page;
        }
        // no BSP data for this page: fall through to the legacy decode path
    }

    int w = 0, h = 0, channels = 0;
    uint32_t origW = 0, origH = 0;
    uint8_t* pixels = NULL;
    uint8_t* png = NULL;
    bool pixelsFromStbi = false; // 2zoq decodes come from plain malloc
    // Legacy (data.win TXTR page) path: bounded by pageCount, so this index is safe.
    if (textureIndex >= r->pageCount) return NULL;
    Texture* tex = &r->base.dataWin->txtr.textures[textureIndex];
    if (tex->blobData != NULL && tex->blobSize > 0) {
        // Parser already loaded the blob (parseTxtr=true path).
        // GameMaker 2022.9+ blobs are '2zoq' (BZip2+QOI):
        if (tex->blobSize >= 4 && tex->blobData[0] == '2' && tex->blobData[1] == 'z') {
            pixels = ImageDecoder_decodeToRgba(tex->blobData, tex->blobSize, true, &w, &h);
        } else {
            pixels = stbi_load_from_memory(tex->blobData, (int) tex->blobSize, &w, &h, &channels, 4);
            pixelsFromStbi = (pixels != NULL);
        }
    } else if (r->dataWinFile != NULL) {
        png = safeMalloc(tex->blobSize);
        if (png == NULL) return NULL;
        bool readOk = fseek(r->dataWinFile, (long) tex->blobOffset, SEEK_SET) == 0 &&
            fread(png, 1, tex->blobSize, r->dataWinFile) == tex->blobSize;
        if (!readOk) {
            free(png);
            return NULL;
        }
        if (tex->blobSize >= 4 && png[0] == '2' && png[1] == 'z') {
            pixels = ImageDecoder_decodeToRgba(png, tex->blobSize, true, &w, &h);
        } else {
            pixels = stbi_load_from_memory(png, (int) tex->blobSize, &w, &h, &channels, 4);
            pixelsFromStbi = (pixels != NULL);
        }
        free(png);
    }
    if (pixels == NULL || w <= 0 || h <= 0) return NULL;

    uint32_t pw = (uint32_t) w;
    uint32_t ph = (uint32_t) h;
    origW = pw; origH = ph;

    // Downscale oversized pages 2x until they fit (a 2048x2048 page becomes
    // 1024x1024 = 4MB - the max that fits MEM1 alongside the VM).
    while (pw > GCN_MAX_PAGE_DIM || ph > GCN_MAX_PAGE_DIM) {
        uint32_t nw = pw / 2, nh = ph / 2;
        uint8_t* half = safeMalloc(nw * nh * 4);
        if (half == NULL) { GCN_freePixels(pixels, pixelsFromStbi); return NULL; }
        for (uint32_t y = 0; y < nh; ++y) {
            const uint32_t* srcRow = (const uint32_t*) (pixels + (y * 2) * pw * 4);
            uint32_t* dstRow = (uint32_t*) (half + y * nw * 4);
            for (uint32_t x = 0; x < nw; ++x) dstRow[x] = srcRow[x * 2];
        }
        GCN_freePixels(pixels, pixelsFromStbi);
        pixels = half;
        pw = nw; ph = nh;
    }
    if ((pw & 31) != 0 || (ph & 31) != 0) {
        GCN_freePixels(pixels, pixelsFromStbi);
        return NULL;
    }

    // Lazy slot allocation: (re)alloc this slot's buffer for the page size.
    uint32_t needed = pw * ph * 4;
    if (r->pageBuffers[victim] == NULL || r->pageBufferSizes[victim] < needed) {
        if (r->pageBuffers[victim] != NULL) free(r->pageBuffers[victim]);
        r->pageBuffers[victim] = memalign(32, needed);
        if (r->pageBuffers[victim] == NULL) {
            r->pageBufferSizes[victim] = 0;
            GCN_freePixels(pixels, pixelsFromStbi);
            return NULL;
        }
        r->pageBufferSizes[victim] = needed;
    }

    GCNRenderer_swizzleRGBA8(pixels, pw, ph, r->pageBuffers[victim]);
    GCN_freePixels(pixels, pixelsFromStbi);

    DCFlushRange(r->pageBuffers[victim], needed);
    GX_InitTexObj(&page->texObj, r->pageBuffers[victim], pw, ph, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&page->texObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, 0, 0, GX_ANISO_1);
    page->width = pw;
    page->height = ph;
    page->scale = (float) origW / (float) pw;
    page->ownerTexture = textureIndex;
    page->ready = true;
    page->lastUsedStamp = ++r->useStamp;
    {
        extern void GCN_bootlog(const char* fmt, ...);
        GCN_bootlog("[FONTPAGE] page %u decoded %ux%u from %u-byte blob",
            textureIndex, pw, ph, (unsigned) tex->blobSize);
    }
    return page;
}

static uint8_t GCNRenderer_colorR(uint32_t bgr) { return (uint8_t) (bgr & 0xFF); }
static uint8_t GCNRenderer_bgrG(uint32_t bgr) { return (uint8_t) ((bgr >> 8) & 0xFF); }
static uint8_t GCNRenderer_bgrB(uint32_t bgr) { return (uint8_t) ((bgr >> 16) & 0xFF); }

static void GCNRenderer_appendQuad(
    GCNRenderer* r,
    int32_t textureIndex,
    float g00x, float g00y,
    float g10x, float g10y,
    float g01x, float g01y,
    float u0, float v0, float u1, float v1,
    uint32_t color0, uint32_t color1,
    float alpha,
    bool gradient
) {
    if (r->commandCount >= GCN_MAX_QUADS) return;
#if defined(__has_include)
#if __has_include(<ogc/gx.h>)
    {
        extern volatile uint32_t GCN_diag_drawCalls;
        GCN_diag_drawCalls++;
    }
#endif
#endif

    float g11x = g10x + (g01x - g00x);
    float g11y = g10y + (g01y - g00y);
    float minX = fminf(fminf(g00x, g10x), fminf(g01x, g11x));
    float maxX = fmaxf(fmaxf(g00x, g10x), fmaxf(g01x, g11x));
    float minY = fminf(fminf(g00y, g10y), fminf(g01y, g11y));
    float maxY = fmaxf(fmaxf(g00y, g10y), fmaxf(g01y, g11y));
    float viewMinX = (float) r->portX;
    float viewMinY = (float) r->portY;
    float viewMaxX = viewMinX + (float) r->portW;
    float viewMaxY = viewMinY + (float) r->portH;
    if (maxX < viewMinX || minX > viewMaxX || maxY < viewMinY || minY > viewMaxY) return;

    GCNQuadCommand command;
    memset(&command, 0, sizeof(command));
    command.textureIndex = textureIndex;
    command.p00x = g00x; command.p00y = g00y;
    command.p10x = g10x; command.p10y = g10y;
    command.p01x = g01x; command.p01y = g01y;
    command.u0 = u0; command.v0 = v0; command.u1 = u1; command.v1 = v1;
    command.color0 = color0; command.color1 = color1;
    command.alpha = alpha;
    command.gradient = gradient;
    r->commands[r->commandCount++] = command;
}

static void GCNRenderer_worldToGame(GCNRenderer* r, float worldX, float worldY, float* outX, float* outY) {
    *outX = (float) r->portX + (worldX - (float) r->viewX) * r->viewScaleX;
    *outY = (float) r->portY + (worldY - (float) r->viewY) * r->viewScaleY;
}

static void GCNRenderer_appendQuadWorld(
    GCNRenderer* r,
    int32_t textureIndex,
    float w00x, float w00y,
    float w10x, float w10y,
    float w01x, float w01y,
    float u0, float v0, float u1, float v1,
    uint32_t color0, uint32_t color1,
    float alpha,
    bool gradient
) {
    float g00x, g00y, g10x, g10y, g01x, g01y;
    GCNRenderer_worldToGame(r, w00x, w00y, &g00x, &g00y);
    GCNRenderer_worldToGame(r, w10x, w10y, &g10x, &g10y);
    GCNRenderer_worldToGame(r, w01x, w01y, &g01x, &g01y);
    GCNRenderer_appendQuad(r, textureIndex,
        g00x, g00y, g10x, g10y, g01x, g01y,
        u0, v0, u1, v1, color0, color1, alpha, gradient);
}

// 2D transform used by sprites/text: translate->rotate->scale around origin.
static void GCNRenderer_transformPoint2D(
    float x, float y,
    float originX, float originY,
    float xscale, float yscale, float angleRad,
    float* outX, float* outY
) {
    float lx = (x - originX) * xscale;
    float ly = (y - originY) * yscale;
    float c = cosf(angleRad);
    float s = sinf(angleRad);
    *outX = lx * c - ly * s + originX;
    *outY = lx * s + ly * c + originY;
}

// ===[ Draw primitives ]=======================================================

static void GCNRenderer_drawSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GCNRenderer_lastDrawCall = "drawSprite";
    GCNRenderer* r = (GCNRenderer*) base;
    DataWin* dataWin = base->dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
    // BSP path: the packer stores each item as its own packed region. Key the
    // page cache by TPAG ITEM INDEX and draw ONLY the packed region.
    //   * the region covers frame coords [cropX, cropX+cropW) x [cropY, +cropH)
    //   * UVs must span packedW/pageW .. packedH/pageH (NOT 0..1: the page is
    //     32-aligned padded, so 0..1 would sample transparent padding/neighbours
    //     and draw the sprite oversized).
    {
        extern bool BSP_hasTpag(int32_t);
        if (BSP_hasTpag(tpagIndex)) {
            GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) tpagIndex);
            if (page == NULL) return;
            int32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
            extern void BSP_tpagFrameRect(int32_t, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*);
            int32_t srcX = 0, srcY = 0;
            BSP_tpagFrameRect(tpagIndex, &srcX, &srcY, &cropX, &cropY, &cropW, &cropH);
            if (cropW <= 0) cropW = (int32_t) page->packedW;
            if (cropH <= 0) cropH = (int32_t) page->packedH;
            // Frame-relative placement of the packed content. The sprite must
            // appear with its origin at world (x,y), not at world (0,0).
            // cropX/Y is the content offset within the item; targetX/Y is the
            // item's position on the source page and is irrelevant here because
            // the packed page already isolates this item.
            float lx0 = ((float) cropX - originX) * xscale;
            float ly0 = ((float) cropY - originY) * yscale;
            float lx1 = lx0 + (float) cropW * xscale;
            float ly1 = ly0 + (float) cropH * yscale;
            float angleRad = -angleDeg * ((float) M_PI / 180.0f);
            float w00x, w00y, w10x, w10y, w01x, w01y;
            GCNRenderer_transformPoint2D(x + lx0, y + ly0, x, y, 1.0f, 1.0f, angleRad, &w00x, &w00y);
            GCNRenderer_transformPoint2D(x + lx1, y + ly0, x, y, 1.0f, 1.0f, angleRad, &w10x, &w10y);
            GCNRenderer_transformPoint2D(x + lx0, y + ly1, x, y, 1.0f, 1.0f, angleRad, &w01x, &w01y);
            float uEnd = (float) page->packedW / (float) page->width;
            float vEnd = (float) page->packedH / (float) page->height;
            GCNRenderer_appendQuadWorld(
                r,
                (uint32_t) tpagIndex,   // item-keyed page
                w00x, w00y, w10x, w10y, w01x, w01y,
                0.0f, 0.0f, uEnd, vEnd,
                color, color, base->drawAlpha * alpha, false
            );
            return;
        }
    }
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= r->pageCount) return;
    GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) tpag->texturePageId);
    if (page == NULL) return;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    float w00x, w00y, w10x, w10y, w01x, w01y;
    float lx0 = ((float) tpag->targetX - (float) originX) * xscale;
    float ly0 = ((float) tpag->targetY - (float) originY) * yscale;
    float lx1 = lx0 + (float) tpag->sourceWidth * xscale;
    float ly1 = ly0 + (float) tpag->sourceHeight * yscale;
    GCNRenderer_transformPoint2D(x + lx0, y + ly0, x, y, 1.0f, 1.0f, angleRad, &w00x, &w00y);
    GCNRenderer_transformPoint2D(x + lx1, y + ly0, x, y, 1.0f, 1.0f, angleRad, &w10x, &w10y);
    GCNRenderer_transformPoint2D(x + lx0, y + ly1, x, y, 1.0f, 1.0f, angleRad, &w01x, &w01y);

    GCNRenderer_appendQuadWorld(
        r,
        tpag->texturePageId,
        w00x, w00y, w10x, w10y, w01x, w01y,
        (float) tpag->sourceX / (float) (page->width * page->scale),
        (float) tpag->sourceY / (float) (page->height * page->scale),
        (float) (tpag->sourceX + tpag->sourceWidth) / (float) (page->width * page->scale),
        (float) (tpag->sourceY + tpag->sourceHeight) / (float) (page->height * page->scale),
        color, color, base->drawAlpha * alpha, false
    );
}

static void GCNRenderer_drawSpritePart(Renderer* base, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    GCNRenderer_lastDrawCall = "drawSpritePart";
    GCNRenderer* r = (GCNRenderer*) base;
    DataWin* dataWin = base->dataWin;
    (void) angleDeg;
    (void) pivotX;
    (void) pivotY;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
    extern bool BSP_hasTpag(int32_t);
    if (BSP_hasTpag(tpagIndex)) {
        // BSP: item-keyed page, packed region covers the item's source rect.
        // Source-space sub-rect (srcOffX/Y + srcW/H) maps into the packed region.
        GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) tpagIndex);
        if (page == NULL) return;
        int32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
        extern void BSP_tpagFrameRect(int32_t, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*);
        int32_t sx = 0, sy = 0;
        BSP_tpagFrameRect(tpagIndex, &sx, &sy, &cropX, &cropY, &cropW, &cropH);
        float invW = (float) page->packedW / (float) (cropW > 0 ? cropW : 1);
        float invH = (float) page->packedH / (float) (cropH > 0 ? cropH : 1);
        float g00x, g00y, g10x, g10y, g01x, g01y;
        float x1 = x + (float) srcW * xscale;
        float y1 = y + (float) srcH * yscale;
        GCNRenderer_worldToGame(r, x, y, &g00x, &g00y);
        GCNRenderer_worldToGame(r, x1, y, &g10x, &g10y);
        GCNRenderer_worldToGame(r, x, y1, &g01x, &g01y);
        GCNRenderer_appendQuad(r, tpagIndex,
            g00x, g00y, g10x, g10y, g01x, g01y,
            (float) (cropX + srcOffX) * invW / (float) page->width,
            (float) (cropY + srcOffY) * invH / (float) page->height,
            (float) (cropX + srcOffX + srcW) * invW / (float) page->width,
            (float) (cropY + srcOffY + srcH) * invH / (float) page->height,
            color, color, base->drawAlpha * alpha, false);
        return;
    }
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= r->pageCount) return;
    GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) tpag->texturePageId);
    if (page == NULL) return;

    float x1 = x + (float) srcW * xscale;
    float y1 = y + (float) srcH * yscale;
    float g00x, g00y, g10x, g10y, g01x, g01y;
    GCNRenderer_worldToGame(r, x, y, &g00x, &g00y);
    GCNRenderer_worldToGame(r, x1, y, &g10x, &g10y);
    GCNRenderer_worldToGame(r, x, y1, &g01x, &g01y);
    GCNRenderer_appendQuad(
        r,
        tpag->texturePageId,
        g00x, g00y, g10x, g10y, g01x, g01y,
        (float) (tpag->sourceX + srcOffX) / (float) (page->width * page->scale),
        (float) (tpag->sourceY + srcOffY) / (float) (page->height * page->scale),
        (float) (tpag->sourceX + srcOffX + srcW) / (float) (page->width * page->scale),
        (float) (tpag->sourceY + srcOffY + srcH) / (float) (page->height * page->scale),
        color, color, base->drawAlpha * alpha, false
    );
}

static void GCNRenderer_drawRectangle(Renderer* base, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GCNRenderer_lastDrawCall = "drawRectangle";
    GCNRenderer* r = (GCNRenderer*) base;
#if defined(__has_include)
#if __has_include(<ogc/gx.h>)
    {
        extern void GCN_bootlog(const char* fmt, ...);
        static int rectLogged = 0;
        if (rectLogged++ < 10)
            GCN_bootlog("[DRAWRECT] x1=%.1f y1=%.1f x2=%.1f y2=%.1f color=%06X alpha=%.2f outline=%d drawColor=%06X drawAlpha=%.2f",
                x1, y1, x2, y2, color, alpha, (int) outline, base->drawColor, base->drawAlpha);
    }
#endif
#endif
    if (outline) {
        r->base.vtable->drawLine(base, x1, y1, x2, y1, 1.0f, color, alpha);
        r->base.vtable->drawLine(base, x2, y1, x2, y2, 1.0f, color, alpha);
        r->base.vtable->drawLine(base, x2, y2, x1, y2, 1.0f, color, alpha);
        r->base.vtable->drawLine(base, x1, y2, x1, y1, 1.0f, color, alpha);
        return;
    }
    float g00x, g00y, g10x, g10y, g01x, g01y;
    GCNRenderer_worldToGame(r, x1, y1, &g00x, &g00y);
    GCNRenderer_worldToGame(r, x2, y1, &g10x, &g10y);
    GCNRenderer_worldToGame(r, x1, y2, &g01x, &g01y);
    GCNRenderer_appendQuad(r, -1, g00x, g00y, g10x, g10y, g01x, g01y,
        0.0f, 0.0f, 1.0f, 1.0f, color, color, base->drawAlpha * alpha, false);
}

static void GCNRenderer_drawLine(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    GCNRenderer_lastDrawCall = "drawLine";
    GCNRenderer* r = (GCNRenderer*) base;
    float g00x, g00y, g10x, g10y;
    GCNRenderer_worldToGame(r, x1, y1, &g00x, &g00y);
    GCNRenderer_worldToGame(r, x2, y2, &g10x, &g10y);
    width *= fmaxf(r->viewScaleX, r->viewScaleY);

    float dx = g10x - g00x;
    float dy = g10y - g00y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        GCNRenderer_appendQuad(r, -1, g00x, g00y, g00x + 1.0f, g00y, g00x, g00y + 1.0f,
            0.0f, 0.0f, 1.0f, 1.0f, color, color, base->drawAlpha * alpha, false);
        return;
    }
    float half = fmaxf(width, 1.0f) * 0.5f;
    float nx = -dy / len * half;
    float ny = dx / len * half;
    GCNRenderer_appendQuad(r, -1,
        g00x - nx, g00y - ny, g10x - nx, g10y - ny, g00x + nx, g00y + ny,
        0.0f, 0.0f, 1.0f, 1.0f, color, color, base->drawAlpha * alpha, false);
}

static void GCNRenderer_drawLineColor(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    GCNRenderer* r = (GCNRenderer*) base;
    float g00x, g00y, g10x, g10y;
    GCNRenderer_worldToGame(r, x1, y1, &g00x, &g00y);
    GCNRenderer_worldToGame(r, x2, y2, &g10x, &g10y);
    width *= fmaxf(r->viewScaleX, r->viewScaleY);

    float dx = g10x - g00x;
    float dy = g10y - g00y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        r->base.vtable->drawRectangle(base, g00x, g00y, g00x + 1.0f, g00y + 1.0f, color1, alpha, false);
        return;
    }
    float half = fmaxf(width, 1.0f) * 0.5f;
    float nx = -dy / len * half;
    float ny = dx / len * half;
    GCNRenderer_appendQuad(r, -1,
        g00x - nx, g00y - ny, g10x - nx, g10y - ny, g00x + nx, g00y + ny,
        0.0f, 0.0f, 1.0f, 1.0f, color1, color2, base->drawAlpha * alpha, true);
}

// ===[ Text ]==================================================================

static void GCNRenderer_drawTextCommon(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg, bool gradient, int32_t c1, float alpha) {
    GCNRenderer_lastDrawCall = "drawText";
    DataWin* dataWin = base->dataWin;
    if (base->drawFont < 0 || (uint32_t) base->drawFont >= dataWin->font.count) {
        extern void GCN_bootlog(const char* fmt, ...);
        static int noFontLogged = 0;
        if (noFontLogged++ < 3) GCN_bootlog("[DRAWTEXT] SKIPPED drawFont=%d (invalid, fonts=%u) text='%.20s'",
            (int) base->drawFont, (unsigned) dataWin->font.count, text ? text : "");
        return;
    }

    GCNRenderer* r = (GCNRenderer*) base;
    Font* font = &dataWin->font.fonts[base->drawFont];
    int32_t fontTpagIndex = font->tpagIndex;
    if (fontTpagIndex < 0 || (uint32_t) fontTpagIndex >= dataWin->tpag.count) {
        extern void GCN_bootlog(const char* fmt, ...);
        static int ftLogged = 0;
        if (ftLogged++ < 3) GCN_bootlog("[DRAWTEXT] font %d tpagIndex=%d INVALID (tpags=%u)",
            (int) base->drawFont, (int) fontTpagIndex, (unsigned) dataWin->tpag.count);
        return;
    }

    TexturePageItem* fontTpag = &dataWin->tpag.items[fontTpagIndex];
    extern bool BSP_hasTpag(int32_t);
    bool bspFontPage = BSP_hasTpag((int32_t) fontTpagIndex);
    // BSP font pages are keyed by ITEM index, so the TXTR pageId bound must
    // not gate them (TPAG[17].pageId=2 while pageCount=TXTR count=9; and
    // item indices run to 3008).
    if (!bspFontPage && (fontTpag->texturePageId < 0 || (uint32_t) fontTpag->texturePageId >= r->pageCount)) {
        extern void GCN_bootlog(const char* fmt, ...);
        static int pgLogged = 0;
        if (pgLogged++ < 3) GCN_bootlog("[DRAWTEXT] pageId=%d INVALID (pageCount=%u)",
            (int) fontTpag->texturePageId, (unsigned) r->pageCount);
        return;
    }
    GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) fontTpagIndex);
    if (page == NULL) {
        extern void GCN_bootlog(const char* fmt, ...);
        static int epLogged = 0;
        if (epLogged++ < 3) GCN_bootlog("[DRAWTEXT] ensurePage(item %u) NULL", (unsigned) fontTpagIndex);
        return;
    }

    // DIAGNOSTIC: log the FULL string the game asks us to draw (capped).
    {
        extern void GCN_bootlog(const char* fmt, ...);
        static int fullLog = 0;
        if (fullLog++ < 6)
            GCN_bootlog("[DRAWFULL] font=%d x=%.1f y=%.1f len=%u text='%.64s'",
                (int) base->drawFont, x, y,
                (unsigned) (text != NULL ? strlen(text) : 0), text != NULL ? text : "");
    }

    PreprocessedText processed = TextUtils_preprocessGmlText(text);
    const char* processedText = processed.text;
    int32_t textLen = (int32_t) strlen(processedText);
    int32_t lineCount = TextUtils_countLines(processedText, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0.0f;
    if (base->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (base->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    float effXScale = xscale * font->scaleX;
    float effYScale = yscale * font->scaleY;
    float pivotX = x;
    float pivotY = y + valignOffset;

    // BSP font texture: the font's TPAG item is packed as its own region.
    // Glyph coords (glyph->sourceX/Y) are RELATIVE to the item's source rect
    // (verified against all 12 ch1 fonts: max glyph x < item sourceWidth).
    // The packed region covers [cropX, cropX+cropW) x [cropY, cropY+cropH) of
    // that source rect, so a source-relative coord maps to
    //   u = (cropX + glyphX) * packedW / cropW / width
    // The earlier (fontTpag->sourceX + glyphX - cropX)/cropW form added the
    // sheet's absolute page position (fnt_main sourceX=473) and pushed every
    // glyph UV past 1.0 => the blitter discarded all of them (no text, ever).
    bool bspFont = false;
    uint32_t fPackedW = 0, fPackedH = 0, fCropW = 0, fCropH = 0, fCropX = 0, fCropY = 0;
    {
        extern bool BSP_hasTpag(int32_t);
        extern void BSP_tpagFrameRect(int32_t, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*);
        if (BSP_hasTpag((int32_t) fontTpagIndex)) {
            int32_t sx = 0, sy = 0, cx = 0, cy = 0, cw = 0, ch = 0;
            BSP_tpagFrameRect(fontTpagIndex, &sx, &sy, &cx, &cy, &cw, &ch);
            bspFont = true;
            fCropX = (uint32_t) cx;
            fCropY = (uint32_t) cy;
            fCropW = (uint32_t) (cw > 0 ? cw : (int32_t) page->packedW);
            fCropH = (uint32_t) (ch > 0 ? ch : (int32_t) page->packedH);
            fPackedW = page->packedW ? page->packedW : fCropW;
            fPackedH = page->packedH ? page->packedH : fCropH;
        }
    }

    float cursorY = y + valignOffset;
    int32_t lineStart = 0;
    for (int32_t lineIdx = 0; lineIdx < lineCount; ++lineIdx) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(processedText[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processedText + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (base->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (base->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = x + halignOffset;
        int32_t pos = 0;
        while (pos < lineLen) {
            uint16_t ch = TextUtils_decodeUtf8(processedText + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == NULL) {
                extern void GCN_bootlog(const char* fmt, ...);
                static int ngLogged = 0;
                if (ngLogged++ < 4) GCN_bootlog("[DRAWTEXT] glyph U+%04X MISSING (glyphs=%u)", (unsigned) ch, (unsigned) font->glyphCount);
                continue;
            }
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float w00x, w00y, w10x, w10y, w01x, w01y;
            float lx0 = cursorX + glyph->offset;
            float ly0 = cursorY;
            float lx1 = lx0 + (float) glyph->sourceWidth;
            float ly1 = ly0 + (float) glyph->sourceHeight;
            GCNRenderer_transformPoint2D(lx0, ly0, pivotX, pivotY, effXScale, effYScale, angleRad, &w00x, &w00y);
            GCNRenderer_transformPoint2D(lx1, ly0, pivotX, pivotY, effXScale, effYScale, angleRad, &w10x, &w10y);
            GCNRenderer_transformPoint2D(lx0, ly1, pivotX, pivotY, effXScale, effYScale, angleRad, &w01x, &w01y);

            uint32_t color = gradient ? (uint32_t) c1 : base->drawColor;
            if (bspFont) {
                // Item-keyed BSP page. Glyph coords are source-rect-relative;
                // map them through the crop rect into the packed region.
                //   u = (cropX + glyphX) * (packedW / cropW) / pageW
                float invW = (float) fPackedW / (float) (fCropW ? fCropW : 1u);
                float invH = (float) fPackedH / (float) (fCropH ? fCropH : 1u);
                float u0 = (float) (fCropX + glyph->sourceX) * invW / (float) page->width;
                float v0 = (float) (fCropY + glyph->sourceY) * invH / (float) page->height;
                float u1 = (float) (fCropX + glyph->sourceX + glyph->sourceWidth) * invW / (float) page->width;
                float v1 = (float) (fCropY + glyph->sourceY + glyph->sourceHeight) * invH / (float) page->height;
                GCNRenderer_appendQuadWorld(
                    r,
                    (uint32_t) fontTpagIndex,   // item-keyed page
                    w00x, w00y, w10x, w10y, w01x, w01y,
                    u0, v0, u1, v1,
                    color, color, base->drawAlpha * alpha, false
                );
            } else {
            GCNRenderer_appendQuadWorld(
                r,
                fontTpag->texturePageId,
                w00x, w00y, w10x, w10y, w01x, w01y,
                (float) (fontTpag->sourceX + glyph->sourceX) / (float) (page->width * page->scale),
                (float) (fontTpag->sourceY + glyph->sourceY) / (float) (page->height * page->scale),
                (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) (page->width * page->scale),
                (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) (page->height * page->scale),
                color, color, base->drawAlpha * alpha, false
            );
            }

            cursorX += glyph->shift;
            if (pos < lineLen) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processedText + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        lineStart = TextUtils_skipNewline(processedText, lineEnd, textLen);
    }

    PreprocessedText_free(processed);
}

static void GCNRenderer_drawText(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    GCNRenderer_drawTextCommon(base, text, x, y, xscale, yscale, angleDeg, false, 0, base->drawAlpha);
}

static void GCNRenderer_drawTextColor(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha) {
    (void) c3;
    (void) c4;
    GCNRenderer_drawTextCommon(base, text, x, y, xscale, yscale, angleDeg, true, c1, alpha);
}

static void GCNRenderer_flush(Renderer* base) {
    (void) base; // commands are flushed in endFrame
}

static void GCNRenderer_clearScreen(Renderer* base, uint32_t color, float alpha) {
    (void) base; (void) color; (void) alpha; // v1: always cleared black in endFrame
}

// ===[ Surfaces: v1 unsupported ]==============================================

static int32_t GCNRenderer_createSpriteFromSurface(Renderer* base, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    (void) base; (void) surfaceID; (void) x; (void) y; (void) w; (void) h;
    (void) removeback; (void) smooth; (void) xorig; (void) yorig;
    return -1;
}

static void GCNRenderer_deleteSprite(Renderer* base, int32_t spriteIndex) {
    (void) base; (void) spriteIndex;
}

// ===[ Frame flow / GX emission ]==============================================

static void GCNRenderer_setCommonState(bool blendEnabled) {
    GX_SetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetBlendMode(
        blendEnabled ? GX_BM_BLEND : GX_BM_NONE,
        GX_BL_SRCALPHA,
        GX_BL_INVSRCALPHA,
        GX_LO_CLEAR
    );
    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetNumTevStages(1);
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_FALSE);
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void GCNRenderer_emitVertex(float x, float y, float u, float v, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    GX_Position2f32(x, y);
    GX_Color4u8(cr, cg, cb, ca);
    GX_TexCoord2f32(u, v);
}

static void GCNRenderer_renderCommands(GCNRenderer* r, uint32_t clearR, uint32_t clearG, uint32_t clearB, float clearA) {
    GCNRenderer_lastDrawCall = "swrRender";
    // SOFTWARE PATH: clear the XFB to black on CPU, then blit every quad
    // directly. No GX involvement at all - same pixel path as the console
    // text that provably reaches this TV.
    u16* xfb = (u16*) GCN_getXfb(GCN_getXfbIndex()); // back buffer of the moment
    GXRModeObj* rmode = GCN_getRMode();
    uint32_t stride = rmode->fbWidth;
    // XFB pixel-pair layout. PPC is BIG-ENDIAN: a u16 value is stored with its
    // HIGH byte first, so the word (Y<<8)|C lands in memory as [Y, C] - which
    // is exactly the YUYV order the VI reads and matches libogc's colorTable
    // (black = bytes 10 80, white = F0 80). This was correct all along; do NOT
    // "fix" it to (C<<8)|Y - that stores [C, Y] and paints the screen pure
    // green (Y=128,C=16 -> RGB(0,255,0)).
    // ===[ CLEAR + IMMEDIATE READ-BACK ]===
    // Write the whole frame black, then IMMEDIATELY verify by reading the same
    // pointer back. This distinguishes "our writes do not reach the memory the
    // VI scans" (pointer/stride/cache error) from "something overwrites us
    // later". Logged every 600 frames so drift is visible.
    for (uint32_t yyy = 0; yyy < 480; ++yyy) {
        u16* row = xfb + yyy * stride;
        for (uint32_t xxx = 0; xxx < 640; xxx += 2) {
            row[xxx] = (u16) ((16 << 8) | 0x80);      // [Y=16, Cb=128] = black
            row[xxx + 1] = (u16) ((16 << 8) | 0x80);  // [Y=16, Cr=128]
        }
    }
    {
        extern void GCN_bootlog(const char* fmt, ...);
        static int clearLogged = 0;
        if (clearLogged++ < 3) {
            uint16_t* p16 = (uint16_t*) xfb;
            uint16_t* midRow = (uint16_t*) (xfb + 240u * stride);
            GCN_bootlog("[CLEAR] stride=%u p16[0]=%04X p16[1]=%04X p16[639]=%04X mid[0]=%04X (want 1080 everywhere)",
                (unsigned) stride, (unsigned) p16[0], (unsigned) p16[1],
                (unsigned) p16[639], (unsigned) midRow[0]);
        }
    }
    GCNRenderer_statsCommands = r->commandCount;
    // ===[ SWATCH REMOVED ]===
    // The 4-band discriminator did its job as a diagnostic but the Architect
    // does not want it on screen. The [CLEAR] read-back log above now provides
    // the same information (raw pixel words straight out of the framebuffer)
    // without drawing anything. Keep the screen clean for the game.
    if (r->commandCount == 0) return;

    float scaleX = 640.0f / (float) r->frameW;
    float scaleY = 480.0f / (float) r->frameH;
    float scale = scaleX < scaleY ? scaleX : scaleY;
    float targetW = (float) r->frameW * scale;
    float targetH = (float) r->frameH * scale;
    float offsetX = (640.0f - targetW) * 0.5f;
    float offsetY = (480.0f - targetH) * 0.5f;

    for (uint32_t i = 0; i < r->commandCount; ++i) {
        GCNQuadCommand* command = &r->commands[i];

        float p00x = offsetX + command->p00x * scale;
        float p00y = offsetY + command->p00y * scale;
        float p10x = offsetX + command->p10x * scale;
        float p10y = offsetY + command->p10y * scale;
        float p01x = offsetX + command->p01x * scale;
        float p01y = offsetY + command->p01y * scale;

        uint8_t mr = GCNRenderer_colorR(command->color0);
        uint8_t mg = GCNRenderer_bgrG(command->color0);
        uint8_t mb = GCNRenderer_bgrB(command->color0);
        uint8_t ma = (uint8_t) (command->alpha * 255.0f);
        (void) command->color1;
        (void) command->gradient;

        const uint8_t* pageBuf = NULL;
        uint32_t pageW = 0, pageH = 0;
        if (command->textureIndex >= 0) {
            GCNTexturePage* page = GCNRenderer_findResident(r, (uint32_t) command->textureIndex);
            if (page == NULL) {
                GCNRenderer_statsSkipped++;
                // Try to load the page on the spot (render-time fetch):
                page = GCNRenderer_ensurePage(r, (uint32_t) command->textureIndex);
                if (page == NULL) { GCNRenderer_statsSkipped++; continue; }
            }
            for (uint32_t slot = 0; slot < GCN_RESIDENT_PAGES; ++slot) {
                if (r->pages[slot].ready && r->pages[slot].ownerTexture == (uint32_t) command->textureIndex) {
                    pageBuf = r->pageBuffers[slot];
                    pageW = page->width;
                    pageH = page->height;
                    break;
                }
            }
            if (pageBuf == NULL) { GCNRenderer_statsSkipped++; continue; }
        }
        GCNRenderer_statsBlitted++;

        GCN_swr_blitQuad(xfb, stride, pageBuf, pageW, pageH,
            p00x, p00y, p10x, p10y, p01x, p01y,
            command->u0, command->v0, command->u1, command->v1,
            mr, mg, mb, ma);

        // TEXT-PIXEL EVIDENCE: count blitted quads that actually wrote pixels
        // for a texture-backed command, and remember the first UV pair we see.
        if (pageBuf != NULL) {
            GCN_diag_texturedBlits++;
            if (GCN_diag_firstU < -900.0f) {
                GCN_diag_firstU = command->u0;
                GCN_diag_firstV = command->v0;
                GCN_diag_firstU1 = command->u1;
                GCN_diag_firstV1 = command->v1;
                GCN_diag_firstTexW = pageW;
                GCN_diag_firstTexH = pageH;
            }
        }
    }

    // (The marching diagnostic square was REMOVED: game text renders, so the
    //  blitter path is proven. Keeping it would only draw over the game.)

    r->commandCount = 0;
}

static void GCNRenderer_beginFrame(Renderer* base, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    (void) windowW;
    (void) windowH;
    GCNRenderer* r = (GCNRenderer*) base;
    r->frameW = gameW;
    r->frameH = gameH;
    r->viewX = 0; r->viewY = 0; r->viewW = gameW; r->viewH = gameH;
    r->portX = 0; r->portY = 0; r->portW = gameW; r->portH = gameH;
    r->viewScaleX = 1.0f; r->viewScaleY = 1.0f;
    r->commandCount = 0;
}

static void GCNRenderer_beginView(Renderer* base, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    (void) viewAngle;
    GCNRenderer* r = (GCNRenderer*) base;
    r->viewX = viewX; r->viewY = viewY;
    r->viewW = viewW != 0 ? viewW : 1;
    r->viewH = viewH != 0 ? viewH : 1;
    r->portX = portX; r->portY = portY;
    r->portW = portW; r->portH = portH;
    r->viewScaleX = (float) portW / (float) r->viewW;
    r->viewScaleY = (float) portH / (float) r->viewH;
}

static void GCNRenderer_endView(Renderer* base) { (void) base; }

static void GCNRenderer_beginGUI(Renderer* base, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    GCNRenderer_beginView(base, 0, 0, guiW, guiH, portX, portY, portW, portH, 0.0f);
}

static void GCNRenderer_endGUI(Renderer* base) { GCNRenderer_endView(base); }

static void GCNRenderer_endFrame(Renderer* base) {
    GCNRenderer_lastDrawCall = "endFrame";
    GCNRenderer* r = (GCNRenderer*) base;
    GCNRenderer_frameCounter++;
    GCNRenderer_renderCommands(r, 0, 0, 0, 1.0f);
}

// ===[ Init / destroy / vtable / create ]======================================

static void GCNRenderer_init(Renderer* base, DataWin* dataWin) {
    GCNRenderer* r = (GCNRenderer*) base;
    base->dataWin = dataWin;
    r->pageCount = dataWin->txtr.count;

    // Page buffers are allocated LAZILY at the page's real (downscaled) size
    // on first use and freed on eviction - avoids boot-time 16MB spikes that
    // OOM-killed the first build (ch1 has a 2048x2048 page; 4x4MB eager allocs
    // blew past MEM1 alongside the VM).
    for (uint32_t slot = 0; slot < GCN_RESIDENT_PAGES; ++slot) {
        r->pageBuffers[slot] = NULL;
        r->pageBufferSizes[slot] = 0;
        r->pages[slot].ownerTexture = UINT32_MAX;
        r->pages[slot].ready = false;
    }
    GCNRenderer_initWhiteTexture(r);
    gStartupError = NULL;
}

static void GCNRenderer_destroy(Renderer* base) {
    GCNRenderer* r = (GCNRenderer*) base;
    for (uint32_t slot = 0; slot < GCN_RESIDENT_PAGES; ++slot) {
        if (r->pageBuffers[slot] != NULL) {
            free(r->pageBuffers[slot]);
            r->pageBuffers[slot] = NULL;
        }
        r->pageBufferSizes[slot] = 0;
        r->pages[slot].ready = false;
    }
    if (r->dataWinFile != NULL) {
        fclose(r->dataWinFile);
        r->dataWinFile = NULL;
    }
    free(r->dataWinPath);
    r->dataWinPath = NULL;
    free(r);
}

static RendererVtable GCNRendererVtable = {
    .init = GCNRenderer_init,
    .destroy = GCNRenderer_destroy,
    .beginFrame = GCNRenderer_beginFrame,
    .endFrame = GCNRenderer_endFrame,
    .beginView = GCNRenderer_beginView,
    .endView = GCNRenderer_endView,
    .beginGUI = GCNRenderer_beginGUI,
    .endGUI = GCNRenderer_endGUI,
    .drawSprite = GCNRenderer_drawSprite,
    .drawSpritePart = GCNRenderer_drawSpritePart,
    .drawSpritePos = NULL,
    .drawRectangle = GCNRenderer_drawRectangle,
    .drawLine = GCNRenderer_drawLine,
    .drawTriangle = NULL,
    .drawLineColor = GCNRenderer_drawLineColor,
    .drawText = GCNRenderer_drawText,
    .drawTextColor = GCNRenderer_drawTextColor,
    .flush = GCNRenderer_flush,
    .clearScreen = GCNRenderer_clearScreen,
    .createSpriteFromSurface = GCNRenderer_createSpriteFromSurface,
    .deleteSprite = GCNRenderer_deleteSprite,
    .gpuSetBlendMode = NULL,
    .gpuSetBlendModeExt = NULL,
    .gpuSetBlendEnable = NULL,
    .gpuSetAlphaTestEnable = NULL,
    .gpuSetAlphaTestRef = NULL,
    .gpuSetColorWriteEnable = NULL,
    .gpuSetFog = NULL,
    .drawTile = NULL,
    .prewarmRoom = NULL,
    .drawTiled = NULL,
    .createSurface = NULL,
    .surfaceExists = NULL,
    .setSurfaceTarget = NULL,
    .resetSurfaceTarget = NULL,
    .getSurfaceWidth = NULL,
    .getSurfaceHeight = NULL,
    .drawSurface = NULL,
    .drawSurfacePart = NULL,
    .drawSurfaceStretched = NULL,
    .surfaceResize = NULL,
    .surfaceFree = NULL,
    .surfaceCopy = NULL,
    .drawTiledPart = NULL,
};

static GCNRenderer* gGCNRendererInstance = NULL;

Renderer* GCNRenderer_create(void) {
    GCNRenderer* r = safeCalloc(1, sizeof(GCNRenderer));
    gGCNRendererInstance = r;
    r->base.vtable = &GCNRendererVtable;
    r->base.drawColor = 0xFFFFFF;
    r->base.drawAlpha = 1.0f;
    r->base.drawFont = -1;
    for (uint32_t slot = 0; slot < GCN_RESIDENT_PAGES; ++slot) {
        r->pages[slot].ownerTexture = UINT32_MAX;
    }
    return (Renderer*) r;
}

// Opens the data.win file handle used by the streaming texture loader.
bool GCNRenderer_openDataWinFile(Renderer* renderer, const char* dataWinPath) {
    GCNRenderer* r = (GCNRenderer*) renderer;
    if (r->dataWinFile != NULL) {
        fclose(r->dataWinFile);
    }
    r->dataWinFile = fopen(dataWinPath, "rb");
    if (r->dataWinFile == NULL) return false;
    setvbuf(r->dataWinFile, NULL, _IOFBF, 64u * 1024u);
    return true;
}

static GCNRenderer* GCNRenderer_getInstance(void) {
    return gGCNRendererInstance;
}

// ===[ DEMO MODE: solid-quad render test (Dolphin, no SD needed) ]==========
GCNQuadCommand* GCNRenderer_demoCommands(void) {
    static GCNQuadCommand demo[4];
    return demo;
}

uint32_t GCNRenderer_demoBuildFrame(GCNQuadCommand* cmds, uint32_t frame) {
    // quad 0: full-screen dark backdrop
    memset(&cmds[0], 0, sizeof(GCNQuadCommand));
    cmds[0].textureIndex = -1;
    cmds[0].p00x = 0; cmds[0].p00y = 0;
    cmds[0].p10x = 640; cmds[0].p10y = 0;
    cmds[0].p01x = 0; cmds[0].p01y = 480;
    cmds[0].color0 = 0x404040; // dark grey (BGR)
    cmds[0].alpha = 1.0f;
    // quad 1: red square moving right
    float x = (float) ((frame * 3) % 600);
    memset(&cmds[1], 0, sizeof(GCNQuadCommand));
    cmds[1].textureIndex = -1;
    cmds[1].p00x = x; cmds[1].p00y = 100;
    cmds[1].p10x = x + 60; cmds[1].p10y = 100;
    cmds[1].p01x = x; cmds[1].p01y = 160;
    cmds[1].color0 = 0x0000FF; // red in BGR
    cmds[1].alpha = 1.0f;
    // quad 2: green square moving left
    float y = 200 + (float) ((frame * 2) % 200);
    memset(&cmds[2], 0, sizeof(GCNQuadCommand));
    cmds[2].textureIndex = -1;
    cmds[2].p00x = 200; cmds[2].p00y = y;
    cmds[2].p10x = 300; cmds[2].p10y = y;
    cmds[2].p01x = 200; cmds[2].p01y = y + 50;
    cmds[2].color0 = 0x00FF00; // green
    cmds[2].alpha = 1.0f;
    // quad 3: white square
    memset(&cmds[3], 0, sizeof(GCNQuadCommand));
    cmds[3].textureIndex = -1;
    cmds[3].p00x = 500; cmds[3].p00y = 300;
    cmds[3].p10x = 560; cmds[3].p10y = 300;
    cmds[3].p01x = 500; cmds[3].p01y = 360;
    cmds[3].color0 = 0xFFFFFF;
    cmds[3].alpha = 1.0f;
    return 4;
}

// Embedded 64x64 RGBA8 checkerboard to validate the texture sampler path.
static const uint8_t* demoTextureQuad = NULL;

void GCNRenderer_renderDemoFrame(GCNQuadCommand* cmds, uint32_t count) {
    GCNRenderer* r = GCNRenderer_getInstance();
    if (r == NULL) {
        // Demo path runs before the game creates its renderer - make one.
        r = (GCNRenderer*) GCNRenderer_create();
        r->frameW = 640;
        r->frameH = 480;
        r->viewX = 0; r->viewY = 0; r->viewW = 640; r->viewH = 480;
        r->portX = 0; r->portY = 0; r->portW = 640; r->portH = 480;
        r->viewScaleX = 1.0f; r->viewScaleY = 1.0f;
        r->commandCount = 0;
    }
    r->commands[0] = cmds[0];
    r->commands[1] = cmds[1];
    r->commands[2] = cmds[2];
    r->commands[3] = cmds[3];
    r->commandCount = count;
    // route through the same path the game uses:
    r->base.vtable->endFrame(&r->base);
    // then the TEXTURE TEST: a real sampled+blitted texture quad
    static uint8_t testTex[64 * 64 * 2] __attribute__((aligned(32)));
    static bool testTexInit = false;
    if (!testTexInit) {
        uint8_t rgba[64 * 64 * 4];
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                uint8_t* px = &rgba[(y * 64 + x) * 4];
                if ((x >> 3) & 1) { px[0] = 255; px[1] = 64; px[2] = 64; px[3] = 255; }
                else              { px[0] = 64; px[1] = 64; px[2] = 255; px[3] = 255; }
            }
        }
        GCNRenderer_swizzleRGBA8(rgba, 64, 64, testTex);
        DCFlushRange(testTex, sizeof(testTex));
        testTexInit = true;
    }
    {
        u16* xf = (u16*) GCN_getXfb(GCN_getXfbIndex());
        GXRModeObj* rmode = GCN_getRMode();
        GCN_swr_blitQuad(xf, rmode->fbWidth, testTex, 64, 64,
            500.0f, 300.0f, 564.0f, 300.0f, 500.0f, 364.0f,
            0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255);
    }
}


