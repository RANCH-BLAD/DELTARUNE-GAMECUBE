/* Embedded LZSS-compressed data.win (RAM disk for Dolphin / no-SD mode).
 * Decompressed at boot by GCN_ramfs_init into a malloc'd buffer.
 */
    .globl gcn_ramfs_start
    .globl gcn_ramfs_end
    .section .rodata
    .balign 32
gcn_ramfs_start:
    .incbin BIN_PATH
gcn_ramfs_end: