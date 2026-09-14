#pragma once

#include "../file_system.h"

typedef struct {
    FileSystem base;
    char* basePath;      // e.g. "sd:/apps/DELTARUNEGC" (game data root)
    char* savePath;      // e.g. "sd:/apps/DELTARUNEGC" (saves live next to data)
} GCNFileSystem;

void GCNFileSystem_setBasePath(const char* basePath);
const char* GCNFileSystem_getBasePath(void);
GCNFileSystem* GCNFileSystem_create(const char* basePath, const char* savePath);
void GCNFileSystem_destroy(GCNFileSystem* fs);
