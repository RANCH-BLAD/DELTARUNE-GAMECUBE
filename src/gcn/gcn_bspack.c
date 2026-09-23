// ===[ ButterscotchPreprocessor packed-asset reader (GCN) ]===
// Reads the output of ButterscotchRunner/ButterscotchPreprocessor
// (ATLAS.BIN / TEXTURES.BIN / CLUT4.BIN / CLUT8.BIN) from the SD card
// and turns sprite pages into RGBA8 for the software blitter.
// All pre-converted on the PC: no '2zoq' decode, no 36MB spikes.
#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t offset;
    uint16_t width, height;
    uint8_t bpp;          // 4 or 8
    int32_t pixelDataSize;
    uint8_t compression;  // 0=raw 1=RLE
} BSPAtlas;

typedef struct {
    uint16_t atlasId, atlasX, atlasY, width, height;
    uint16_t cropX, cropY, cropW, cropH, clutIndex;
} BSPTpag;

static BSPAtlas* bspAtlases = NULL;
static int32_t bspAtlasCount = 0;
static BSPTpag* bspTpag = NULL;
static int32_t bspTpagCount = 0;
static uint8_t* bspTextures = NULL;   // TEXTURES.BIN resident
static uint32_t bspTexturesSize = 0;
static uint32_t* bspClut4 = NULL;     // 4bpp palettes (each 16 colors)
static int32_t bspClut4Count = 0;
static uint32_t* bspClut8 = NULL;     // 8bpp palettes (each 256 colors)
static int32_t bspClut8Count = 0;

static uint32_t readU32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t readU16(const uint8_t* p) { return p[0] | (p[1]<<8); }

// Undo PS2 CSM1 8bpp palette swizzle (inverse of the packer's pass).
static void unswizzlePalette8(uint32_t* pal) {
    for (uint32_t i = 0; i + 8 < 256; ++i) {
        if ((i & 0x18) == 8) {
            uint32_t tmp = pal[i];
            pal[i] = pal[i + 8];
            pal[i + 8] = tmp;
        }
    }
}

// PS2 RGBA (a<<24 | b<<16 | g<<8 | r, alpha 0..128) -> RGBA8 0..255
static inline uint32_t ps2rgba_to_rgba8(uint32_t c) {
    uint32_t r = c & 0xFF;
    uint32_t g = (c >> 8) & 0xFF;
    uint32_t b = (c >> 16) & 0xFF;
    uint32_t a = (c >> 24) & 0xFF;
    a = (a << 1) & 0xFF; // 0..128 -> 0..254 (approx inverse of (a+1)>>1)
    if (a == 0 && ((c >> 24) != 0)) a = 1;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

bool BSP_load(const char* dirPath) {
    char path[256];

    snprintf(path, sizeof(path), "%s/ATLAS.BIN", dirPath);
    FILE* fa = fopen(path, "rb");
    if (fa == NULL) return false;
    fseek(fa, 0, SEEK_END);
    long faSize = ftell(fa);
    fseek(fa, 0, SEEK_SET);
    uint8_t* meta = (uint8_t*) malloc(faSize);
    if (fread(meta, 1, faSize, fa) != (size_t) faSize) { fclose(fa); free(meta); return false; }
    fclose(fa);

    uint32_t tpagCount = readU16(meta + 1);
    bspAtlasCount = readU16(meta + 5);
    bspTpagCount = (int32_t) tpagCount;

    bspAtlases = (BSPAtlas*) calloc(bspAtlasCount, sizeof(BSPAtlas));
    uint8_t* cur = meta + 7;
    for (int32_t i = 0; i < bspAtlasCount; ++i) {
        bspAtlases[i].offset = (int32_t) readU32(cur);
        bspAtlases[i].width = readU16(cur + 4);
        bspAtlases[i].height = readU16(cur + 6);
        bspAtlases[i].bpp = cur[8];
        bspAtlases[i].pixelDataSize = (int32_t) readU32(cur + 9);
        bspAtlases[i].compression = cur[13];
        cur += 14;
    }

    bspTpag = (BSPTpag*) calloc(bspTpagCount, sizeof(BSPTpag));
    for (int32_t i = 0; i < bspTpagCount; ++i) {
        bspTpag[i].atlasId = readU16(cur + 0);
        bspTpag[i].atlasX = readU16(cur + 2);
        bspTpag[i].atlasY = readU16(cur + 4);
        bspTpag[i].width = readU16(cur + 6);
        bspTpag[i].height = readU16(cur + 8);
        bspTpag[i].cropX = readU16(cur + 10);
        bspTpag[i].cropY = readU16(cur + 12);
        bspTpag[i].cropW = readU16(cur + 14);
        bspTpag[i].cropH = readU16(cur + 16);
        bspTpag[i].clutIndex = readU16(cur + 18);
        cur += 20;
    }
    free(meta);

    // CLUTs
    snprintf(path, sizeof(path), "%s/CLUT4.BIN", dirPath);
    FILE* fc4 = fopen(path, "rb");
    if (fc4 != NULL) {
        fseek(fc4, 0, SEEK_END); long s = ftell(fc4); fseek(fc4, 0, SEEK_SET);
        bspClut4Count = (int32_t)(s / (16 * 4));
        bspClut4 = (uint32_t*) malloc(s);
        if (fread(bspClut4, 1, s, fc4) != (size_t) s) { bspClut4Count = 0; }
        fclose(fc4);
    }
    snprintf(path, sizeof(path), "%s/CLUT8.BIN", dirPath);
    FILE* fc8 = fopen(path, "rb");
    if (fc8 != NULL) {
        fseek(fc8, 0, SEEK_END); long s = ftell(fc8); fseek(fc8, 0, SEEK_SET);
        bspClut8Count = (int32_t)(s / (256 * 4));
        bspClut8 = (uint32_t*) malloc(s);
        if (fread(bspClut8, 1, s, fc8) != (size_t) s) { bspClut8Count = 0; }
        fclose(fc8);
        // un-swizzle each 8bpp palette (CSM1)
        for (int32_t p = 0; p < bspClut8Count; ++p) unswizzlePalette8(bspClut8 + p * 256);
    }

    // TEXTURES.BIN resident (4.5MB for ch1 - fits MEM1)
    snprintf(path, sizeof(path), "%s/TEXTURES.BIN", dirPath);
    FILE* ft = fopen(path, "rb");
    if (ft == NULL) return false;
    fseek(ft, 0, SEEK_END); bspTexturesSize = (uint32_t) ftell(ft); fseek(ft, 0, SEEK_SET);
    bspTextures = (uint8_t*) malloc(bspTexturesSize);
    if (fread(bspTextures, 1, bspTexturesSize, ft) != bspTexturesSize) {
        free(bspTextures); bspTextures = NULL; fclose(ft); return false;
    }
    fclose(ft);
    return true;
}

bool BSP_hasTpag(int32_t tpagIndex) {
    return bspTpag != NULL && tpagIndex >= 0 && tpagIndex < bspTpagCount
        && bspTpag[tpagIndex].atlasId != 0xFFFF;
}

// Decode tpag's pixels into a LINEAR RGBA8 buffer (caller frees).
// Returns the (possibly cropped) sprite's real w/h. NULL on failure.
//
// Packed-atlas layout (ButterscotchPreprocessor.writeTexturePagesBytes):
//   8bpp: 1 byte/pixel, row stride = atlasWidth
//   4bpp: 2 pixels/byte, low nibble first, row stride = atlasWidth/2
// RLE (compression==1) expands to that same PACKED byte form, so a 4bpp
// atlas' RLE stream is (w*h)/2 bytes, NOT w*h.
uint8_t* BSP_decodeTpag(int32_t tpagIndex, int32_t* outW, int32_t* outH) {
    if (!BSP_hasTpag(tpagIndex)) return NULL;
    BSPTpag* e = &bspTpag[tpagIndex];
    if (e->atlasId >= bspAtlasCount) return NULL;
    BSPAtlas* at = &bspAtlases[e->atlasId];

    uint32_t pw = e->width, ph = e->height;
    if (pw == 0 || ph == 0) return NULL;
    if (at->bpp != 4 && at->bpp != 8) return NULL;
    if (at->width == 0 || at->height == 0) return NULL;
    if ((uint32_t) e->atlasX + pw > (uint32_t) at->width) return NULL;
    if ((uint32_t) e->atlasY + ph > (uint32_t) at->height) return NULL;
    if (at->offset < 0 || (uint32_t) at->offset >= bspTexturesSize) return NULL;

    // Palette
    const uint32_t* pal = NULL;
    uint32_t palCount = 0;
    if (at->bpp == 4) {
        if (e->clutIndex >= bspClut4Count) return NULL;
        pal = bspClut4 + e->clutIndex * 16;
        palCount = 16;
    } else {
        if (e->clutIndex >= bspClut8Count) return NULL;
        pal = bspClut8 + e->clutIndex * 256;
        palCount = 256;
    }

    // Source scanlines (packed byte form).
    uint32_t rowBytes = (at->bpp == 4) ? ((uint32_t) at->width / 2u) : (uint32_t) at->width;
    const uint8_t* src = bspTextures + (uint32_t) at->offset;
    uint8_t* expanded = NULL;
    if (at->compression == 1) {
        uint32_t packedSize = rowBytes * (uint32_t) at->height;
        expanded = (uint8_t*) malloc(packedSize);
        if (expanded == NULL) return NULL;
        memset(expanded, 0, packedSize);
        uint32_t dataEnd = (uint32_t) at->offset + (uint32_t) at->pixelDataSize;
        if (dataEnd > bspTexturesSize) dataEnd = bspTexturesSize;
        uint32_t sp = (uint32_t) at->offset;
        uint32_t op = 0;
        while (op < packedSize && sp + 2 <= dataEnd) {
            uint8_t run = bspTextures[sp];
            uint8_t val = bspTextures[sp + 1];
            sp += 2;
            uint32_t n = run; // packer writes 1..255
            if (n > packedSize - op) n = packedSize - op;
            memset(expanded + op, val, n);
            op += n;
        }
        src = expanded;
    }

    // Expand the packed region straight to linear RGBA8 (nibble-exact for 4bpp,
    // so items starting at an odd atlasX decode correctly).
    uint8_t* out = (uint8_t*) malloc((size_t) pw * (size_t) ph * 4u);
    if (out == NULL) { if (expanded != NULL) free(expanded); return NULL; }
    for (uint32_t y = 0; y < ph; ++y) {
        const uint8_t* row = src + (size_t) ((uint32_t) e->atlasY + y) * (size_t) rowBytes;
        uint8_t* dst = out + (size_t) y * (size_t) pw * 4u;
        for (uint32_t x = 0; x < pw; ++x) {
            uint32_t sx = (uint32_t) e->atlasX + x;
            uint32_t ci;
            if (at->bpp == 4) {
                uint8_t b = row[sx >> 1];
                ci = (sx & 1u) ? ((uint32_t) (b >> 4) & 0xFu) : ((uint32_t) b & 0xFu);
            } else {
                ci = row[sx];
            }
            if (ci >= palCount) ci = 0;
            uint32_t c = pal[ci];
            // PS2 alpha 0..128 -> 0..255 (packer: ps2 = (a+1)>>1)
            uint32_t a8 = (c >> 24) & 0xFFu;
            a8 = a8 ? (a8 * 2u - 1u) : 0u;
            dst[x * 4 + 0] = (uint8_t) (c & 0xFFu);         // R
            dst[x * 4 + 1] = (uint8_t) ((c >> 8) & 0xFFu);  // G
            dst[x * 4 + 2] = (uint8_t) ((c >> 16) & 0xFFu); // B
            dst[x * 4 + 3] = (uint8_t) a8;                  // A
        }
    }
    if (expanded != NULL) free(expanded);

    // Return the PACKED item region. UV math in the renderer maps source-space
    // rects into this region via (coord - cropX) * width / cropW.
    *outW = (int32_t) pw;
    *outH = (int32_t) ph;
    return out;
}

int32_t BSP_tpagWidth(int32_t tpagIndex) { return BSP_hasTpag(tpagIndex) ? bspTpag[tpagIndex].cropW : 0; }
int32_t BSP_tpagHeight(int32_t tpagIndex) { return BSP_hasTpag(tpagIndex) ? bspTpag[tpagIndex].cropH : 0; }

// Frame-relative crop rect of the packed content (cropX, cropY within the
// original source rect; cropW/H = original source dims) + the item's source
// rect origin (srcX, srcY) in the original page space.
void BSP_tpagFrameRect(int32_t tpagIndex, int32_t* srcX, int32_t* srcY,
                       int32_t* cropX, int32_t* cropY, int32_t* cropW, int32_t* cropH) {
    if (!BSP_hasTpag(tpagIndex) || srcX == NULL) return;
    *srcX = bspTpag[tpagIndex].cropX;   // packer stores source rect origin here
    *srcY = bspTpag[tpagIndex].cropY;
    // NOTE: fields verified against data.win: crop=(cropX,cropY,cropW,cropH)
    *cropX = bspTpag[tpagIndex].cropX;
    *cropY = bspTpag[tpagIndex].cropY;
    *cropW = bspTpag[tpagIndex].cropW;
    *cropH = bspTpag[tpagIndex].cropH;
}
const char* BSP_lastError = "none";