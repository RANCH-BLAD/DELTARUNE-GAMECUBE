#pragma once

#include "../renderer.h"

typedef struct GCNRenderer GCNRenderer;

Renderer* GCNRenderer_create(void);
bool GCNRenderer_isReady(Renderer* renderer);
const char* GCNRenderer_getStartupError(Renderer* renderer);
void GCNRenderer_setDataWinFile(Renderer* renderer, const char* dataWinPath);
bool GCNRenderer_openDataWinFile(Renderer* renderer, const char* dataWinPath);

typedef struct {
    int32_t textureIndex; // -1 = white texture
    float p00x, p00y, p10x, p10y, p01x, p01y;
    float u0, v0, u1, v1;
    uint32_t color0, color1;
    float alpha;
    bool gradient;
} GCNQuadCommand;

// Demo-mode (no SD): solid-quad render test for Dolphin
GCNQuadCommand* GCNRenderer_demoCommands(void);
uint32_t GCNRenderer_demoBuildFrame(GCNQuadCommand* cmds, uint32_t frame);
void GCNRenderer_renderDemoFrame(GCNQuadCommand* cmds, uint32_t count);
