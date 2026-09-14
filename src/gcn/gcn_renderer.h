#pragma once

#include "../renderer.h"

typedef struct GCNRenderer GCNRenderer;

Renderer* GCNRenderer_create(void);
bool GCNRenderer_isReady(Renderer* renderer);
const char* GCNRenderer_getStartupError(Renderer* renderer);
void GCNRenderer_setDataWinFile(Renderer* renderer, const char* dataWinPath);
bool GCNRenderer_openDataWinFile(Renderer* renderer, const char* dataWinPath);
