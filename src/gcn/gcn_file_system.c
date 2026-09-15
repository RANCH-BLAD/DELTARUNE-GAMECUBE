#include "gcn_file_system.h"

#include "../utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((weak)) void GCN_platformBootLog(const char* message) {
    (void) message;
}

static void GCNFileSystem_bootLog(const char* message) {
    GCN_platformBootLog(message);
}

static char* GCNFileSystem_buildFullPath(GCNFileSystem* fs, const char* relativePath) {
    if (relativePath == NULL) return NULL;
    if (strncmp(relativePath, "sd:/", 4) == 0) {
        return safeStrdup(relativePath);
    }

    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    bool needSlash = baseLen > 0 && fs->basePath[baseLen - 1] != '/' && relLen > 0 && relativePath[0] != '/';
    char* fullPath = safeMalloc(baseLen + relLen + (needSlash ? 2 : 1));
    memcpy(fullPath, fs->basePath, baseLen);
    size_t cursor = baseLen;
    if (needSlash) fullPath[cursor++] = '/';
    memcpy(fullPath + cursor, relativePath, relLen);
    fullPath[cursor + relLen] = '\0';
    return fullPath;
}

static char* GCNFileSystem_resolvePath(FileSystem* fs, const char* relativePath) {
    return GCNFileSystem_buildFullPath((GCNFileSystem*) fs, relativePath);
}

static bool GCNFileSystem_fileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = GCNFileSystem_buildFullPath((GCNFileSystem*) fs, relativePath);
    struct stat st;
    bool exists = fullPath != NULL && stat(fullPath, &st) == 0;
    free(fullPath);
    return exists;
}

// Set to true by readFileText when the returned pointer is a static view
// (ramfs blob) that the caller must NOT free.
bool gGCN_lastReadWasStatic = false;

static char* GCNFileSystem_readFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = GCNFileSystem_buildFullPath((GCNFileSystem*) fs, relativePath);
    if (fullPath == NULL) return NULL;

    // RAM MODE: any "ram:..." file IS the decompressed data.win. Return a
    // zero-copy static view (caller marked contentIsStatic, never frees).
    if (strncmp(fullPath, "ram:", 4) == 0) {
        extern uint8_t* GCN_ramfs_data(void);
        extern bool gGCN_lastReadWasStatic;
        free(fullPath);
        gGCN_lastReadWasStatic = true;
        return (char*) GCN_ramfs_data();
    }

    FILE* file = fopen(fullPath, "rb");
    if (file == NULL) {
        GCNFileSystem_bootLog("gcn_fs: readText open failed");
        free(fullPath);
        return NULL;
    }
    // Big read buffer: SD reads are slow sector-sized operations on GC.
    setvbuf(file, NULL, _IOFBF, 8u * 1024u);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        free(fullPath);
        return NULL;
    }

    char* text = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(text, 1, (size_t) size, file);
    text[bytesRead] = '\0';
    fclose(file);
    free(fullPath);
    return text;
}

static bool GCNFileSystem_writeFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    GCNFileSystem* gfs = (GCNFileSystem*) fs;
    char* fullPath = GCNFileSystem_buildFullPath(gfs, relativePath);
    if (fullPath == NULL) return false;

    FILE* file = fopen(fullPath, "wb");
    if (file == NULL) {
        free(fullPath);
        return false;
    }
    setvbuf(file, NULL, _IOFBF, 8u * 1024u);

    size_t length = strlen(contents);
    bool ok = fwrite(contents, 1, length, file) == length;
    fclose(file);
    free(fullPath);
    if (ok) GCNFileSystem_bootLog("gcn_fs: writeText ok");
    return ok;
}

static bool GCNFileSystem_deleteFile(FileSystem* fs, const char* relativePath) {
    GCNFileSystem* gfs = (GCNFileSystem*) fs;
    char* fullPath = GCNFileSystem_buildFullPath(gfs, relativePath);
    if (fullPath == NULL) return false;
    int rc = remove(fullPath);
    free(fullPath);
    return rc == 0;
}

static bool GCNFileSystem_readFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = GCNFileSystem_buildFullPath((GCNFileSystem*) fs, relativePath);
    if (fullPath == NULL) return false;

    // RAM MODE zero-copy (caller gets the decompressed blob; must not free)
    if (strncmp(fullPath, "ram:", 4) == 0) {
        extern uint8_t* GCN_ramfs_data(void);
        extern uint32_t GCN_ramfs_size(void);
        extern bool gGCN_lastReadWasStatic;
        free(fullPath);
        *outData = GCN_ramfs_data();
        *outSize = (int32_t) GCN_ramfs_size();
        gGCN_lastReadWasStatic = true;
        return true;
    }

    FILE* file = fopen(fullPath, "rb");
    if (file == NULL) {
        free(fullPath);
        return false;
    }
    setvbuf(file, NULL, _IOFBF, 8u * 1024u);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        free(fullPath);
        return false;
    }

    uint8_t* data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, file);
    fclose(file);

    *outData = data;
    *outSize = (int32_t) bytesRead;
    free(fullPath);
    return true;
}

static bool GCNFileSystem_writeFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    GCNFileSystem* gfs = (GCNFileSystem*) fs;
    char* fullPath = GCNFileSystem_buildFullPath(gfs, relativePath);
    if (fullPath == NULL) return false;

    FILE* file = fopen(fullPath, "wb");
    if (file == NULL) {
        free(fullPath);
        return false;
    }
    setvbuf(file, NULL, _IOFBF, 8u * 1024u);

    bool ok = fwrite(data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    free(fullPath);
    return ok;
}

static FileSystemVtable GCNFileSystem_vtable = {
    .resolvePath = GCNFileSystem_resolvePath,
    .fileExists = GCNFileSystem_fileExists,
    .readFileText = GCNFileSystem_readFileText,
    .writeFileText = GCNFileSystem_writeFileText,
    .deleteFile = GCNFileSystem_deleteFile,
    .readFileBinary = GCNFileSystem_readFileBinary,
    .writeFileBinary = GCNFileSystem_writeFileBinary,
};

static char gGCNBasePath[128] = "/apps/DELTARUNEGC";

void GCNFileSystem_setBasePath(const char* basePath) {
    if (basePath != NULL && strlen(basePath) < sizeof(gGCNBasePath)) {
        snprintf(gGCNBasePath, sizeof(gGCNBasePath), "%s", basePath);
    }
}

const char* GCNFileSystem_getBasePath(void) {
    return gGCNBasePath;
}

GCNFileSystem* GCNFileSystem_create(const char* basePath, const char* savePath) {
    GCNFileSystem* fs = safeCalloc(1, sizeof(GCNFileSystem));
    fs->base.vtable = &GCNFileSystem_vtable;
    fs->basePath = safeStrdup(basePath != NULL ? basePath : gGCNBasePath);
    fs->savePath = safeStrdup(savePath != NULL ? savePath : gGCNBasePath);
    return fs;
}

void GCNFileSystem_destroy(GCNFileSystem* fs) {
    if (fs == NULL) return;
    free(fs->basePath);
    free(fs->savePath);
    free(fs);
}
