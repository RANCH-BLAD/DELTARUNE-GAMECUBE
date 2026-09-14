#include "gcn_renderer.h"

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

#define GCN_MAX_QUADS 4096
#define GCN_RESIDENT_PAGES 2
#define GCN_MAX_PAGE_DIM 1024
#define GCN_VERTICES_PER_QUAD 6

typedef struct {
    GXTexObj texObj;
    uint32_t width, height;
    uint32_t ownerTexture; // TXTR index; UINT32_MAX = free slot
    uint32_t lastUsedStamp;
    bool ready;
} GCNTexturePage;

typedef struct {
    int32_t textureIndex; // -1 = white texture
    float p00x, p00y, p10x, p10y, p01x, p01y;
    float u0, v0, u1, v1;
    uint32_t color0, color1;
    float alpha;
    bool gradient;
} GCNQuadCommand;

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

// GX_TF_RGBA8 swizzle: 32x32 tiles; each tile row = 32 bytes (A,R) + 32 (G,B).
static void GCNRenderer_swizzleRGBA8(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst) {
    uint32_t tilesX = width / 32;
    uint32_t tilesY = height / 32;
    for (uint32_t ty = 0; ty < tilesY; ++ty) {
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            uint8_t* tile = dst + (ty * tilesX + tx) * (32 * 64);
            for (uint32_t row = 0; row < 32; ++row) {
                uint8_t* ar = tile + row * 64;
                uint8_t* gb = ar + 32;
                const uint8_t* srcRow = src + ((ty * 32 + row) * width + tx * 32) * 4;
                for (uint32_t col = 0; col < 32; ++col) {
                    const uint8_t* px = srcRow + col * 4;
                    ar[col * 2 + 0] = px[3]; // A
                    ar[col * 2 + 1] = px[0]; // R
                    gb[col * 2 + 0] = px[1]; // G
                    gb[col * 2 + 1] = px[2]; // B
                }
            }
        }
    }
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

static GCNTexturePage* GCNRenderer_ensurePage(GCNRenderer* r, uint32_t textureIndex) {
    GCNRenderer_lastDrawCall = "ensurePage";
    if (textureIndex >= r->pageCount) return NULL;
    GCNTexturePage* resident = GCNRenderer_findResident(r, textureIndex);
    if (resident != NULL) return resident;
    if (r->dataWinFile == NULL) return NULL;

    Texture* tex = &r->base.dataWin->txtr.textures[textureIndex];
    if (tex->blobOffset == 0 || tex->blobSize == 0) return NULL;
    if (tex->textureWidth > GCN_MAX_PAGE_DIM || tex->textureHeight > GCN_MAX_PAGE_DIM) return NULL;

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

    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = NULL;
    uint8_t* png = NULL;
    if (tex->blobData != NULL && tex->blobSize > 0) {
        // Parser already loaded the blob (parseTxtr=true path).
        pixels = stbi_load_from_memory(tex->blobData, (int) tex->blobSize, &w, &h, &channels, 4);
    } else if (r->dataWinFile != NULL) {
        png = safeMalloc(tex->blobSize);
        if (png == NULL) return NULL;
        bool readOk = fseek(r->dataWinFile, (long) tex->blobOffset, SEEK_SET) == 0 &&
            fread(png, 1, tex->blobSize, r->dataWinFile) == tex->blobSize;
        if (!readOk) {
            free(png);
            return NULL;
        }
        pixels = stbi_load_from_memory(png, (int) tex->blobSize, &w, &h, &channels, 4);
        free(png);
    }
    if (pixels == NULL || w <= 0 || h <= 0) return NULL;

    uint32_t pw = (uint32_t) w;
    uint32_t ph = (uint32_t) h;

    // Downscale oversized pages 2x until they fit (a 2048x2048 page becomes
    // 1024x1024 = 4MB - the max that fits MEM1 alongside the VM).
    while (pw > GCN_MAX_PAGE_DIM || ph > GCN_MAX_PAGE_DIM) {
        uint32_t nw = pw / 2, nh = ph / 2;
        uint8_t* half = safeMalloc(nw * nh * 4);
        if (half == NULL) { stbi_image_free(pixels); return NULL; }
        for (uint32_t y = 0; y < nh; ++y) {
            const uint32_t* srcRow = (const uint32_t*) (pixels + (y * 2) * pw * 4);
            uint32_t* dstRow = (uint32_t*) (half + y * nw * 4);
            for (uint32_t x = 0; x < nw; ++x) dstRow[x] = srcRow[x * 2];
        }
        stbi_image_free(pixels);
        pixels = half;
        pw = nw; ph = nh;
    }
    if ((pw & 31) != 0 || (ph & 31) != 0) {
        stbi_image_free(pixels);
        return NULL;
    }

    // Lazy slot allocation: (re)alloc this slot's buffer for the page size.
    uint32_t needed = pw * ph * 4;
    if (r->pageBuffers[victim] == NULL || r->pageBufferSizes[victim] < needed) {
        if (r->pageBuffers[victim] != NULL) free(r->pageBuffers[victim]);
        r->pageBuffers[victim] = memalign(32, needed);
        if (r->pageBuffers[victim] == NULL) {
            r->pageBufferSizes[victim] = 0;
            stbi_image_free(pixels);
            return NULL;
        }
        r->pageBufferSizes[victim] = needed;
    }

    GCNRenderer_swizzleRGBA8(pixels, pw, ph, r->pageBuffers[victim]);
    stbi_image_free(pixels);

    DCFlushRange(r->pageBuffers[victim], needed);
    GX_InitTexObj(&page->texObj, r->pageBuffers[victim], pw, ph, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&page->texObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, 0, 0, GX_ANISO_1);
    page->width = pw;
    page->height = ph;
    page->ownerTexture = textureIndex;
    page->ready = true;
    page->lastUsedStamp = ++r->useStamp;
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
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= r->pageCount) return;
    GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) tpag->texturePageId);
    if (page == NULL) return;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    float w00x, w00y, w10x, w10y, w01x, w01y;
    GCNRenderer_transformPoint2D((float) tpag->targetX, (float) tpag->targetY, originX, originY, xscale, yscale, angleRad, &w00x, &w00y);
    GCNRenderer_transformPoint2D((float) tpag->targetX + (float) tpag->sourceWidth, (float) tpag->targetY, originX, originY, xscale, yscale, angleRad, &w10x, &w10y);
    GCNRenderer_transformPoint2D((float) tpag->targetX, (float) tpag->targetY + (float) tpag->sourceHeight, originX, originY, xscale, yscale, angleRad, &w01x, &w01y);

    GCNRenderer_appendQuadWorld(
        r,
        tpag->texturePageId,
        w00x, w00y, w10x, w10y, w01x, w01y,
        (float) tpag->sourceX / (float) page->width,
        (float) tpag->sourceY / (float) page->height,
        (float) (tpag->sourceX + tpag->sourceWidth) / (float) page->width,
        (float) (tpag->sourceY + tpag->sourceHeight) / (float) page->height,
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
        (float) (tpag->sourceX + srcOffX) / (float) page->width,
        (float) (tpag->sourceY + srcOffY) / (float) page->height,
        (float) (tpag->sourceX + srcOffX + srcW) / (float) page->width,
        (float) (tpag->sourceY + srcOffY + srcH) / (float) page->height,
        color, color, base->drawAlpha * alpha, false
    );
}

static void GCNRenderer_drawRectangle(Renderer* base, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GCNRenderer_lastDrawCall = "drawRectangle";
    GCNRenderer* r = (GCNRenderer*) base;
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
    if (base->drawFont < 0 || (uint32_t) base->drawFont >= dataWin->font.count) return;

    GCNRenderer* r = (GCNRenderer*) base;
    Font* font = &dataWin->font.fonts[base->drawFont];
    int32_t fontTpagIndex = font->tpagIndex;
    if (fontTpagIndex < 0 || (uint32_t) fontTpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* fontTpag = &dataWin->tpag.items[fontTpagIndex];
    if (fontTpag->texturePageId < 0 || (uint32_t) fontTpag->texturePageId >= r->pageCount) return;
    GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) fontTpag->texturePageId);
    if (page == NULL) return;

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
            if (glyph == NULL) continue;
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
            GCNRenderer_appendQuadWorld(
                r,
                fontTpag->texturePageId,
                w00x, w00y, w10x, w10y, w01x, w01y,
                (float) (fontTpag->sourceX + glyph->sourceX) / (float) page->width,
                (float) (fontTpag->sourceY + glyph->sourceY) / (float) page->height,
                (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) page->width,
                (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) page->height,
                color, color, base->drawAlpha * alpha, false
            );

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
    GCNRenderer_lastDrawCall = "renderCommands";
    GX_SetCopyClear((GXColor) { (uint8_t) clearR, (uint8_t) clearG, (uint8_t) clearB, (uint8_t) (clearA * 255.0f) }, GX_MAX_Z24);

    if (r->commandCount == 0) return;

    float scaleX = 640.0f / (float) r->frameW;
    float scaleY = 480.0f / (float) r->frameH;
    float scale = scaleX < scaleY ? scaleX : scaleY;
    if (scale < 1.0f) scale = 1.0f;
    float targetW = (float) r->frameW * scale;
    float targetH = (float) r->frameH * scale;
    float offsetX = (640.0f - targetW) * 0.5f;
    float offsetY = (480.0f - targetH) * 0.5f;

    Mtx44 projection;
    guOrtho(projection, 0.0f, 480.0f, 0.0f, 640.0f, 0.0f, 1000.0f);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    GX_SetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GX_SetScissor((u32) offsetX, (u32) offsetY, (u32) targetW, (u32) targetH);

    GCNRenderer_setCommonState(true);

    int32_t currentTexture = INT32_MIN;
    for (uint32_t i = 0; i < r->commandCount; ++i) {
        GCNQuadCommand* command = &r->commands[i];
        if (command->textureIndex != currentTexture) {
            GX_DrawDone();
            if (command->textureIndex < 0) {
                GX_LoadTexObj(&r->whiteTexObj, GX_TEXMAP0);
                currentTexture = command->textureIndex;
            } else {
                GCNTexturePage* page = GCNRenderer_ensurePage(r, (uint32_t) command->textureIndex);
                if (page == NULL) {
                    currentTexture = INT32_MIN;
                    continue;
                }
                GX_LoadTexObj(&page->texObj, GX_TEXMAP0);
                currentTexture = command->textureIndex;
            }
        }

        float p00x = offsetX + command->p00x * scale;
        float p00y = offsetY + command->p00y * scale;
        float p10x = offsetX + command->p10x * scale;
        float p10y = offsetY + command->p10y * scale;
        float p01x = offsetX + command->p01x * scale;
        float p01y = offsetY + command->p01y * scale;
        float p11x = p10x + (p01x - p00x);
        float p11y = p10y + (p01y - p00y);

        // game space Y-down -> GX ortho Y-up flip
        float y00 = 480.0f - p00y;
        float y10 = 480.0f - p10y;
        float y01 = 480.0f - p01y;
        float y11 = 480.0f - p11y;

        uint8_t r0 = GCNRenderer_colorR(command->color0);
        uint8_t g0 = GCNRenderer_bgrG(command->color0);
        uint8_t b0 = GCNRenderer_bgrB(command->color0);
        uint8_t r1 = command->gradient ? GCNRenderer_colorR(command->color1) : r0;
        uint8_t g1 = command->gradient ? GCNRenderer_bgrG(command->color1) : g0;
        uint8_t b1 = command->gradient ? GCNRenderer_bgrB(command->color1) : b0;
        uint8_t a = (uint8_t) (command->alpha * 255.0f);

        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, GCN_VERTICES_PER_QUAD);
            GCNRenderer_emitVertex(p00x, y00, command->u0, command->v0, r0, g0, b0, a);
            GCNRenderer_emitVertex(p10x, y10, command->u1, command->v0, r1, g1, b1, a);
            GCNRenderer_emitVertex(p01x, y01, command->u0, command->v1, r0, g0, b0, a);
            GCNRenderer_emitVertex(p01x, y01, command->u0, command->v1, r0, g0, b0, a);
            GCNRenderer_emitVertex(p10x, y10, command->u1, command->v0, r1, g1, b1, a);
            GCNRenderer_emitVertex(p11x, y11, command->u1, command->v1, r1, g1, b1, a);
        GX_End();
    }
    GX_DrawDone();
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

Renderer* GCNRenderer_create(void) {
    GCNRenderer* r = safeCalloc(1, sizeof(GCNRenderer));
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
