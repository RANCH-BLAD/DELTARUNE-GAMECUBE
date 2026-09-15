#pragma once

#include <gccore.h>
#include <ogc/video_types.h>
#include <math.h>

void* GCN_getXfb(u32 index);
u32 GCN_getXfbIndex(void);
GXRModeObj* GCN_getRMode(void);

// ===[ SOFTWARE RENDERER: blit quads straight into the XFB (YUV422) ]===
// The ONLY output path proven to reach this TV is CPU-written XFB pixels
// (the console text). GX output never appears. So rasterize on the CPU.

static inline void GCN_swr_rgb2yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t* y, uint8_t* u, uint8_t* v) {
    *y = (uint8_t) ((( 66 * r + 129 * g +  25 * b + 128) >> 8) +  16);
    *u = (uint8_t) (((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128);
    *v = (uint8_t) (((112 * r -  94 * g -  18 * b + 128) >> 8) + 128);
}

// Sample the GX_TF_RGBA8-swizzled page buffer at (x,y).
static inline uint32_t GCN_swr_sample(const uint8_t* buf, uint32_t width, uint32_t x, uint32_t y) {
    uint32_t tilesPerRow = width >> 5;
    const uint8_t* tile = buf + (((y >> 5) * tilesPerRow + (x >> 5)) * (32 * 64));
    const uint8_t* ar = tile + (y & 31) * 64;
    const uint8_t* gb = ar + 32;
    uint32_t xi = (x & 31) * 2;
    uint8_t a = ar[xi + 0];
    uint8_t r = ar[xi + 1];
    uint8_t g = gb[xi + 0];
    uint8_t b = gb[xi + 1];
    return ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
}

// Affine quad blit: p00 = (u0,v0), p10 = (u1,v0), p01 = (u0,v1).
// XFB pixel pair layout (libogc-verified): word0 = Y0<<8 | U, word1 = Y1<<8 | V.
static void GCN_swr_blitQuad(
    u16* xfb, uint32_t stride,
    const uint8_t* pageBuf, uint32_t pageW, uint32_t pageH, // NULL = solid
    float p00x, float p00y, float p10x, float p10y, float p01x, float p01y,
    float u0, float v0, float u1, float v1,
    uint8_t mr, uint8_t mg, uint8_t mb, uint8_t ma
) {
    float ex1x = p10x - p00x, ex1y = p10y - p00y; // du direction
    float ex2x = p01x - p00x, ex2y = p01y - p00y; // dv direction
    float denom = ex1x * ex2y - ex2x * ex1y;
    if (fabsf(denom) < 0.0001f) return;
    float invDen = 1.0f / denom;

    float ax0 = fminf(fminf(p00x, p10x), fminf(p01x, p01x + ex1x));
    float ax1 = fmaxf(fmaxf(p00x, p10x), fmaxf(p01x, p01x + ex1x));
    float ay0 = fminf(fminf(p00y, p10y), fminf(p01y, p01y + ex1y));
    float ay1 = fmaxf(fmaxf(p00y, p10y), fmaxf(p01y, p01y + ex1y));
    int32_t x0 = (int32_t) ax0; int32_t x1 = (int32_t) ax1 + 1;
    int32_t y0 = (int32_t) ay0; int32_t y1 = (int32_t) ay1 + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > 640) x1 = 640; if (y1 > 480) y1 = 480;
    if (x0 >= x1 || y0 >= y1) return;
    x0 &= ~1; // align pairs

    for (int32_t py = y0; py < y1; ++py) {
        float dy = (float) py + 0.5f - p00y;
        u16* row = xfb + py * 640;
        for (int32_t px = x0; px < x1; px += 2) {
            float dx0 = (float) px + 0.5f - p00x;
            float dx1 = (float) px + 1.5f - p00x;
            float uu0 = ( ex2y * dx0 - ex2x * dy) * invDen;
            float vv0 = (-ex1y * dx0 + ex1x * dy) * invDen;
            float uu1 = ( ex2y * dx1 - ex2x * dy) * invDen;
            // (vv1 ~ vv0 for near-horizontal pairs; recompute for safety)
            float vv1 = (-ex1y * dx1 + ex1x * dy) * invDen;

            if ((uu0 < 0.0f || uu0 > 1.0f || vv0 < 0.0f || vv0 > 1.0f) &&
                (uu1 < 0.0f || uu1 > 1.0f || vv1 < 0.0f || vv1 > 1.0f)) {
                continue;
            }

            uint32_t out[2];
            for (int sub = 0; sub < 2; ++sub) {
                float uu = sub ? uu1 : uu0;
                float vv = sub ? vv1 : vv0;
                uint8_t tr, tg, tb, ta;
                if (uu < 0.0f || uu > 1.0f || vv < 0.0f || vv > 1.0f) {
                    tr = tg = tb = ta = 0;
                } else if (pageBuf != NULL && pageW > 0) {
                    uint32_t sx = (uint32_t) ((u0 + (u1 - u0) * uu) * (float) (pageW - 1));
                    uint32_t sy = (uint32_t) ((v0 + (v1 - v0) * vv) * (float) (pageH - 1));
                    uint32_t px = GCN_swr_sample(pageBuf, pageW, sx, sy);
                    tr = (uint8_t) ((px >> 16) & 0xFF);
                    tg = (uint8_t) ((px >> 8) & 0xFF);
                    tb = (uint8_t) (px & 0xFF);
                    ta = (uint8_t) ((px >> 24) & 0xFF);
                } else {
                    tr = 255; tg = 255; tb = 255; ta = 255;
                }
                // modulate by vertex color + alpha
                tr = (uint8_t) (((uint32_t) tr * mr) >> 8);
                tg = (uint8_t) (((uint32_t) tg * mg) >> 8);
                tb = (uint8_t) (((uint32_t) tb * mb) >> 8);
                ta = (uint8_t) (((uint32_t) ta * ma) >> 8);
                out[sub] = ((uint32_t) ta << 24) | ((uint32_t) tr << 16) | ((uint32_t) tg << 8) | tb;
            }

            uint32_t c0 = out[0], c1 = out[1];
            uint8_t a0 = (uint8_t) (c0 >> 24), a1 = (uint8_t) (c1 >> 24);
            if (a0 == 0 && a1 == 0) continue;

            u16* w0 = row + px;
            u16* w1 = row + px + 1;
            // XFB pixel pair - VERIFIED against libogc console.c colorTable:
            // black 0x10801080 => u32 = Y1 Cb Y2 Cr? NO: the table words are
            // (Y<<8)|chroma in BOTH u16 slots => u16[2n] = (Y0<<8)|U,
            // u16[2n+1] = (Y1<<8)|V. Y in the HIGH byte of each u16.
            if (a0 == 255 && a1 == 255) {
                uint8_t y0v, uv0, vv0, y1v, uv1, vv1;
                GCN_swr_rgb2yuv((uint8_t)(c0 >> 16), (uint8_t)(c0 >> 8), (uint8_t)c0, &y0v, &uv0, &vv0);
                GCN_swr_rgb2yuv((uint8_t)(c1 >> 16), (uint8_t)(c1 >> 8), (uint8_t)c1, &y1v, &uv1, &vv1);
                *w0 = (u16) ((y0v << 8) | uv0);
                *w1 = (u16) ((y1v << 8) | vv1);
            } else {
                uint8_t yv, uv2, vv2;
                if (a0 > 0) {
                    uint8_t ey0 = (*w0 >> 8) & 0xFF; uint8_t euu = *w0 & 0xFF;
                    GCN_swr_rgb2yuv((uint8_t)(c0 >> 16), (uint8_t)(c0 >> 8), (uint8_t)c0, &yv, &uv2, &vv2);
                    uint8_t ny0 = (uint8_t) (((yv * a0) + (ey0 * (255 - a0))) / 255);
                    uint8_t nuu = (uint8_t) (((uv2 * a0) + (euu * (255 - a0))) / 255);
                    *w0 = (u16) ((ny0 << 8) | nuu);
                }
                if (a1 > 0) {
                    uint8_t ey1 = (*w1 >> 8) & 0xFF; uint8_t evv = *w1 & 0xFF;
                    GCN_swr_rgb2yuv((uint8_t)(c1 >> 16), (uint8_t)(c1 >> 8), (uint8_t)c1, &yv, &uv2, &vv2);
                    uint8_t ny1 = (uint8_t) (((yv * a1) + (ey1 * (255 - a1))) / 255);
                    uint8_t nvv = (uint8_t) (((vv2 * a1) + (evv * (255 - a1))) / 255);
                    *w1 = (u16) ((ny1 << 8) | nvv);
                }
            }
        }
    }
}
