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

// YUV conversion tables: per-channel contributions, biased so the final sum
// is just three table lookups + adds. This is much cheaper than the full
// BT.601 multiply/shift formula on every pixel pair.
static int8_t gY_R[256], gY_G[256], gY_B[256];
static int8_t gU_R[256], gU_G[256], gU_B[256];
static int8_t gV_R[256], gV_G[256], gV_B[256];
static bool gYuvTablesBuilt = false;

static inline void GCN_swr_buildYuvTablesOnce(void) {
    if (gYuvTablesBuilt) return;
    for (int i = 0; i < 256; ++i) {
        gY_R[i] = (int8_t) (( 66 * i + 128) >> 8);
        gY_G[i] = (int8_t) ((129 * i) >> 8);
        gY_B[i] = (int8_t) (( 25 * i) >> 8);
        gU_R[i] = (int8_t) ((-38 * i + 128) >> 8);
        gU_G[i] = (int8_t) ((-74 * i) >> 8);
        gU_B[i] = (int8_t) ((112 * i) >> 8);
        gV_R[i] = (int8_t) ((112 * i + 128) >> 8);
        gV_G[i] = (int8_t) ((-94 * i) >> 8);
        gV_B[i] = (int8_t) ((-18 * i) >> 8);
    }
    gYuvTablesBuilt = true;
}

static inline void GCN_swr_rgb2yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t* y, uint8_t* u, uint8_t* v) {
    GCN_swr_buildYuvTablesOnce();
    int yy = 16 + gY_R[r] + gY_G[g] + gY_B[b];
    int uu = 128 + gU_R[r] + gU_G[g] + gU_B[b];
    int vv = 128 + gV_R[r] + gV_G[g] + gV_B[b];
    *y = (uint8_t) (yy < 0 ? 0 : (yy > 255 ? 255 : yy));
    *u = (uint8_t) (uu < 0 ? 0 : (uu > 255 ? 255 : uu));
    *v = (uint8_t) (vv < 0 ? 0 : (vv > 255 ? 255 : vv));
}

// Sample the page buffer at (x,y). The page is stored LINEAR RGBA8: the
// software blitter does its own sampling, so the GX_TF_RGBA8 swizzle is not
// used anywhere (see the note on GCNRenderer_swizzleRGBA8).
static inline uint32_t GCN_swr_sample(const uint8_t* buf, uint32_t width, uint32_t x, uint32_t y) {
    const uint8_t* p = buf + ((size_t) y * (size_t) width + (size_t) x) * 4u;
    uint8_t r = p[0];
    uint8_t g = p[1];
    uint8_t b = p[2];
    uint8_t a = p[3];
    return ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
}

// ===[ FAST PATH: axis-aligned textured/solid quad ]===
// Nearly every draw in DELTARUNE (fullscreen rects, text glyphs, overworld
// sprites) is an UNROTATED, axis-aligned quad: p00->p10 is purely horizontal,
// p00->p01 is purely vertical. For those, texture U/V vary LINEARLY across the
// quad, so we can step them with a constant per-pixel increment and skip the
// per-pixel barycentric solve entirely. This is the dominant cost on a 486MHz
// Gekko (HWLOG30: texturedBlits hit 1.4M on one room walk), so this path is the
// difference between "runs" and "playable".
// Pixel-pair XFB write uses fixed-point YUV to avoid the table lookups too.
// Returns true when it handled the quad (caller must NOT fall through).
static bool GCN_swr_blitQuadAxisAligned(
    u16* xfb,
    const uint8_t* pageBuf, uint32_t pageW, uint32_t pageH, // NULL = solid
    float p00x, float p00y, float p10x, float p10y, float p01x, float p01y,
    float u0, float v0, float u1, float v1,
    uint8_t mr, uint8_t mg, uint8_t mb, uint8_t ma
) {
    (void) p10y; (void) p01x; // axis-aligned => these match p00y/p00x within eps
    float w = p10x - p00x;
    float h = p01y - p00y;
    if (w <= 0.0f || h <= 0.0f) return true; // degenerate: nothing to draw

    int32_t x0 = (int32_t) (p00x + 0.5f); int32_t x1 = (int32_t) (p00x + w + 0.5f);
    int32_t y0 = (int32_t) (p00y + 0.5f); int32_t y1 = (int32_t) (p00y + h + 0.5f);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > 640) x1 = 640; if (y1 > 480) y1 = 480;
    if (x0 >= x1 || y0 >= y1) return true;
    x0 &= ~1; // align pixel pairs

    // Per-pixel U/V increments in texture space, then mapped through the page.
    float du_pix = (u1 - u0) / w; // change in U per screen pixel
    float dv_pix = (v1 - v0) / h;

    // Fixed-point (16.16) page-space stepping: precompute texture pixel coords.
    // srcX at screen x = (u0 + du_pix*(x - p00x)) * (pageW-1); step = du_pix*(pageW-1).
    const float fw = (float) (pageW - 1);
    const float fh = (float) (pageH - 1);
    int32_t uStart = (int32_t) (((u0 + du_pix * ((float) x0 + 0.5f - p00x)) * fw) * 65536.0f);
    int32_t vStart = (int32_t) (((v0 + dv_pix * ((float) y0 + 0.5f - p00y)) * fh) * 65536.0f);
    int32_t uStep = (int32_t) (du_pix * fw * 65536.0f);
    int32_t vStep = (int32_t) (dv_pix * fh * 65536.0f);

    GCN_swr_buildYuvTablesOnce();
    int32_t vfp = vStart;
    const bool solidWhite = (pageBuf == NULL);
    const bool fullyOpaque = (ma == 255);
 extern volatile uint32_t GCN_diag_fastPath;
 GCN_diag_fastPath++;
 for (int32_t py = y0; py < y1; ++py, vfp += vStep) {
        u16* row = xfb + py * 640;
        uint32_t sy = (uint32_t) (vfp >> 16);
        if (sy >= pageH && !solidWhite) sy = pageH - 1;
        const uint8_t* srcRow = solidWhite ? NULL : (pageBuf + (size_t) sy * (size_t) pageW * 4u);
        int32_t ufp = uStart;
        for (int32_t px = x0; px < x1; px += 2, ufp += uStep * 2) {
            uint32_t c0, c1;
            uint8_t a0, a1;
            if (solidWhite) {
                a0 = a1 = ma;
                c0 = c1 = ((uint32_t) ma << 24) | 0x00FFFFFFu;
            } else {
                uint32_t sx0 = (uint32_t) (ufp >> 16);
                uint32_t sx1 = (uint32_t) ((ufp + uStep) >> 16);
                if (sx0 >= pageW) sx0 = pageW - 1;
                if (sx1 >= pageW) sx1 = pageW - 1;
                const uint8_t* s0 = srcRow + (size_t) sx0 * 4u;
                const uint8_t* s1 = srcRow + (size_t) sx1 * 4u;
                uint8_t r0 = s0[0], g0 = s0[1], b0 = s0[2]; a0 = s0[3];
                uint8_t r1 = s1[0], g1 = s1[1], b1 = s1[2]; a1 = s1[3];
                // modulate
                r0 = (uint8_t) (((uint32_t) r0 * mr) >> 8);
                g0 = (uint8_t) (((uint32_t) g0 * mg) >> 8);
                b0 = (uint8_t) (((uint32_t) b0 * mb) >> 8);
                a0 = (uint8_t) (((uint32_t) a0 * ma) >> 8);
                r1 = (uint8_t) (((uint32_t) r1 * mr) >> 8);
                g1 = (uint8_t) (((uint32_t) g1 * mg) >> 8);
                b1 = (uint8_t) (((uint32_t) b1 * mb) >> 8);
                a1 = (uint8_t) (((uint32_t) a1 * ma) >> 8);
                c0 = ((uint32_t) a0 << 24) | ((uint32_t) r0 << 16) | ((uint32_t) g0 << 8) | b0;
                c1 = ((uint32_t) a1 << 24) | ((uint32_t) r1 << 16) | ((uint32_t) g1 << 8) | b1;
            }
            if (a0 == 0 && a1 == 0) continue;

            u16* w0 = row + px;
            u16* w1 = row + px + 1;
            // XFB pixel pair, BIG-ENDIAN: word (Y<<8)|C stores bytes [Y, C] = YUYV.
            if (fullyOpaque && a0 == 255 && a1 == 255) {
                uint8_t y0v, uv0, vv0, y1v, uv1, vv1;
                GCN_swr_rgb2yuv((uint8_t)(c0 >> 16), (uint8_t)(c0 >> 8), (uint8_t)c0, &y0v, &uv0, &vv0);
                GCN_swr_rgb2yuv((uint8_t)(c1 >> 16), (uint8_t)(c1 >> 8), (uint8_t)c1, &y1v, &uv1, &vv1);
                *w0 = (u16) ((y0v << 8) | uv0);
                *w1 = (u16) ((y1v << 8) | vv1);
            } else {
                uint8_t yv, uv2, vv2;
                if (a0 > 0) {
                    uint8_t ey0 = (uint8_t) ((*w0 >> 8) & 0xFF); uint8_t euu = (uint8_t) (*w0 & 0xFF);
                    GCN_swr_rgb2yuv((uint8_t)(c0 >> 16), (uint8_t)(c0 >> 8), (uint8_t)c0, &yv, &uv2, &vv2);
                    uint8_t ny0 = (uint8_t) (((yv * a0) + (ey0 * (255 - a0))) / 255);
                    uint8_t nuu = (uint8_t) (((uv2 * a0) + (euu * (255 - a0))) / 255);
                    *w0 = (u16) ((ny0 << 8) | nuu);
                }
                if (a1 > 0) {
                    uint8_t ey1 = (uint8_t) ((*w1 >> 8) & 0xFF); uint8_t evv = (uint8_t) (*w1 & 0xFF);
                    GCN_swr_rgb2yuv((uint8_t)(c1 >> 16), (uint8_t)(c1 >> 8), (uint8_t)c1, &yv, &uv2, &vv2);
                    uint8_t ny1 = (uint8_t) (((yv * a1) + (ey1 * (255 - a1))) / 255);
                    uint8_t nvv = (uint8_t) (((vv2 * a1) + (evv * (255 - a1))) / 255);
                    *w1 = (u16) ((ny1 << 8) | nvv);
                }
            }
        }
    }
    return true;
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
    // FAST PATH: axis-aligned quad (no rotation/shear). p00->p10 horizontal,
    // p00->p01 vertical. Covers ~all DELTARUNE draws; avoids the barycentric
    // per-pixel solve below. Fall through to the general path for rotated quads.
    if (fabsf(ex1y) < 0.01f && fabsf(ex2x) < 0.01f) {
        GCN_swr_blitQuadAxisAligned(xfb, pageBuf, pageW, pageH,
            p00x, p00y, p10x, p10y, p01x, p01y, u0, v0, u1, v1, mr, mg, mb, ma);
        return;
    }
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
            // XFB pixel pair. PPC is BIG-ENDIAN: the word (Y<<8)|C is stored
            // with the high byte first, i.e. bytes [Y, C] - exactly the YUYV
            // order the VI reads and matching libogc's colorTable (black bytes
            // 10 80, white F0 80). Do NOT swap this to (C<<8)|Y: that stores
            // [C, Y] and makes everything pure green (Y=128,C=16 -> RGB(0,255,0)).
            // Fast opaque path: when both pixels are fully opaque we can skip
            // building the ARGB word and blending entirely. Just convert and
            // write the YUV pair straight to the XFB.
            if (a0 == 255 && a1 == 255) {
                uint8_t y0v, uv0, vv0, y1v, uv1, vv1;
                GCN_swr_rgb2yuv((uint8_t)(c0 >> 16), (uint8_t)(c0 >> 8), (uint8_t)c0, &y0v, &uv0, &vv0);
                GCN_swr_rgb2yuv((uint8_t)(c1 >> 16), (uint8_t)(c1 >> 8), (uint8_t)c1, &y1v, &uv1, &vv1);
                *w0 = (u16) ((y0v << 8) | uv0);   // bytes [Y0, Cb]
                *w1 = (u16) ((y1v << 8) | vv1);   // bytes [Y1, Cr]
            } else if (a0 == 0 && a1 == 0) {
                // Fully transparent: nothing to do.
            } else {
                uint8_t yv, uv2, vv2;
                if (a0 > 0) {
                    uint8_t ey0 = (uint8_t) ((*w0 >> 8) & 0xFF); uint8_t euu = (uint8_t) (*w0 & 0xFF);
                    GCN_swr_rgb2yuv((uint8_t)(c0 >> 16), (uint8_t)(c0 >> 8), (uint8_t)c0, &yv, &uv2, &vv2);
                    uint8_t ny0 = (uint8_t) (((yv * a0) + (ey0 * (255 - a0))) / 255);
                    uint8_t nuu = (uint8_t) (((uv2 * a0) + (euu * (255 - a0))) / 255);
                    *w0 = (u16) ((ny0 << 8) | nuu);
                }
                if (a1 > 0) {
                    uint8_t ey1 = (uint8_t) ((*w1 >> 8) & 0xFF); uint8_t evv = (uint8_t) (*w1 & 0xFF);
                    GCN_swr_rgb2yuv((uint8_t)(c1 >> 16), (uint8_t)(c1 >> 8), (uint8_t)c1, &yv, &uv2, &vv2);
                    uint8_t ny1 = (uint8_t) (((yv * a1) + (ey1 * (255 - a1))) / 255);
                    uint8_t nvv = (uint8_t) (((vv2 * a1) + (evv * (255 - a1))) / 255);
                    *w1 = (u16) ((ny1 << 8) | nvv);
                }
            }
        }
    }
}
