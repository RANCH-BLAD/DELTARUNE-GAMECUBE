// ===[ RAM DISK devoptab + LZSS decompressor (GCN ramfs) ]===
// The embedded blob (data_win.lzss, incbin'd) decompresses at boot into a
// malloc'd buffer; the "ram:" device serves it read-only. Zero-copy views
// (txtr blobs, buffer_load, file_text) point into the decompressed buffer.
#include <gccore.h>
#include <sys/iosupport.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdbool.h>

// Compressed blob (from gcn_ramfs_data.S):
extern const uint8_t gcn_ramfs_start[];
extern const uint8_t gcn_ramfs_end[];

// Decompressed data.win (set by GCN_ramfs_init):
static uint8_t* ramfs_data = NULL;
static uint32_t ramfs_size = 0;

uint8_t* GCN_ramfs_data(void) { return ramfs_data; }
uint32_t GCN_ramfs_size(void) { return ramfs_size; }

// === LZSS decode (matches tools/lzss_compress.py) ===
static void lzss_decode(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstLen) {
    uint32_t sp = 4; // skip uncompressed-size header
    uint32_t out = 0;
    while (out < dstLen && sp < srcLen) {
        uint8_t flags = src[sp++];
        for (int bit = 0; bit < 8 && out < dstLen; ++bit) {
            if (flags & (1 << bit)) {
                if (sp + 2 > srcLen) return;
                uint16_t v = (uint16_t) (src[sp] | (src[sp + 1] << 8));
                sp += 2;
                uint32_t dist = (v >> 4) + 1;
                uint32_t length = (v & 0xF) + 3;
                for (uint32_t k = 0; k < length && out < dstLen; ++k) {
                    dst[out] = dst[out - dist];
                    out++;
                }
            } else {
                if (sp >= srcLen) return;
                dst[out++] = src[sp++];
            }
        }
    }
}

typedef struct {
    uint64_t offset; // read cursor
} RamfsFile;

static ssize_t ramfs_read_r(struct _reent* r, void* fd, char* ptr, size_t len) {
    (void) r;
    RamfsFile* f = (RamfsFile*) fd;
    if (ramfs_data == NULL) return 0;
    if (f->offset >= ramfs_size) return 0;
    uint32_t remaining = ramfs_size - (uint32_t) f->offset;
    if (len > remaining) len = remaining;
    memcpy(ptr, ramfs_data + f->offset, len);
    f->offset += len;
    return (ssize_t) len;
}

static off_t ramfs_seek_r(struct _reent* r, void* fd, off_t pos, int dir) {
    (void) r;
    RamfsFile* f = (RamfsFile*) fd;
    if (dir == SEEK_SET) f->offset = (uint64_t) pos;
    else if (dir == SEEK_CUR) f->offset += (uint64_t) pos;
    else f->offset = (uint64_t) ramfs_size - (uint64_t) pos;
    if (f->offset > ramfs_size) f->offset = ramfs_size;
    return (off_t) f->offset;
}

static int ramfs_close_r(struct _reent* r, void* fd) {
    (void) r; (void) fd; // newlib owns the fileStruct allocation
    return 0;
}

static int ramfs_open_r(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
    (void) r; (void) path; (void) flags; (void) mode;
    RamfsFile* f = (RamfsFile*) fileStruct;
    f->offset = 0;
    return 0;
}

static int ramfs_fstat_r(struct _reent* r, void* fd, struct stat* st) {
    (void) r; (void) fd;
    memset(st, 0, sizeof(struct stat));
    st->st_size = (off_t) ramfs_size;
    st->st_mode = S_IFREG;
    return 0;
}

static int ramfs_stat_r(struct _reent* r, const char* path, struct stat* st) {
    (void) r; (void) path;
    memset(st, 0, sizeof(struct stat));
    st->st_size = (off_t) ramfs_size;
    st->st_mode = S_IFREG;
    return 0;
}

static const devoptab_t ramfs_devoptab = {
    .name = "ram",
    .structSize = sizeof(RamfsFile),
    .open_r = ramfs_open_r,
    .close_r = ramfs_close_r,
    .write_r = NULL,
    .read_r = ramfs_read_r,
    .seek_r = ramfs_seek_r,
    .fstat_r = ramfs_fstat_r,
    .stat_r = ramfs_stat_r,
    .link_r = NULL,
    .unlink_r = NULL,
    .chdir_r = NULL,
    .rename_r = NULL,
    .mkdir_r = NULL,
    .dirStateSize = 0,
    .diropen_r = NULL,
    .dirreset_r = NULL,
    .dirnext_r = NULL,
    .dirclose_r = NULL,
    .statvfs_r = NULL,
    .ftruncate_r = NULL,
    .fsync_r = NULL,
    .deviceData = NULL,
    .chmod_r = NULL,
    .fchmod_r = NULL,
    .rmdir_r = NULL,
    .lstat_r = NULL,
    .utimes_r = NULL,
};

void GCN_ramfs_init(void) {
    if (ramfs_data != NULL) { AddDevice(&ramfs_devoptab); return; }
#ifndef GCN_HAS_RAMFS
    AddDevice(&ramfs_devoptab);
    return;
#else
    uint32_t compressedSize = (uint32_t) ((const uint8_t*) gcn_ramfs_end - gcn_ramfs_start);
    // uncompressed size = first 4 bytes of the lzss stream
    uint32_t uncompressedSize = (uint32_t) gcn_ramfs_start[0]
        | ((uint32_t) gcn_ramfs_start[1] << 8)
        | ((uint32_t) gcn_ramfs_start[2] << 16)
        | ((uint32_t) gcn_ramfs_start[3] << 24);
    uint8_t* buf = (uint8_t*) malloc(uncompressedSize ? uncompressedSize : 1);
    if (buf == NULL) return;
    lzss_decode(gcn_ramfs_start, compressedSize, buf, uncompressedSize);
    ramfs_data = buf;
    ramfs_size = uncompressedSize;
    AddDevice(&ramfs_devoptab);
#endif
}