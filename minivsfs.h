
// minivsfs.h — shared definitions for MiniVSFS
// Spec per "[Summer 2025] CSE 321 Lab Term Project (Student)"
// Block size = 4096, Inode size = 128, 12 direct pointers, only root directory supported.

#ifndef MINIVSFS_H
#define MINIVSFS_H

#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

#define BS            4096u     // block size
#define INODE_SIZE     128u
#define DIRECT_MAX       12
#define ROOT_INO          1u

// ------------------------ On-disk structures (packed, little-endian) ------------------------
#pragma pack(push,1)
typedef struct {
    uint32_t magic;               // 0x4D565346 ("MVSF")
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;        // size_kib * 1024 / 4096
    uint64_t inode_count;         // number of inodes
    uint64_t inode_bitmap_start;  // block index
    uint64_t inode_bitmap_blocks; // 1
    uint64_t data_bitmap_start;   // block index
    uint64_t data_bitmap_blocks;  // 1
    uint64_t inode_table_start;   // block index of inode table region
    uint64_t inode_table_blocks;  // blocks occupied by inode table
    uint64_t data_region_start;   // block index of first data block
    uint64_t data_region_blocks;  // number of data blocks
    uint64_t root_inode;          // == 1 (1-indexed)
    uint64_t mtime_epoch;         // filesystem build time (unix epoch)
    uint32_t flags;               // 0
    uint32_t checksum;            // CRC32 over this struct excluding this field
} superblock_t;
#pragma pack(pop)

_Static_assert(sizeof(superblock_t) == 116, "superblock_t must be 116 bytes as per spec");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;                // 0100000 (file) or 0040000 (dir) in octal
    uint16_t links;               // link count
    uint32_t uid;                 // 0
    uint32_t gid;                 // 0
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];  // absolute block numbers
    uint32_t reserved_0;          // 0
    uint32_t reserved_1;          // 0
    uint32_t reserved_2;          // 0
    uint32_t proj_id;             // group id (arbitrary; default 321)
    uint32_t uid16_gid16;         // 0
    uint64_t xattr_ptr;           // 0
    uint64_t inode_crc;           // checksum (see finalize)
} inode_t;
#pragma pack(pop)

_Static_assert(sizeof(inode_t) == 128, "inode_t must be 128 bytes");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;           // 0 if free
    uint8_t  type;               // 1=file, 2=dir
    char     name[58];           // zero-padded, not necessarily null-terminated
    uint8_t  checksum;           // XOR of previous 63 bytes
} dirent64_t;
#pragma pack(pop)

_Static_assert(sizeof(dirent64_t) == 64, "dirent64_t must be 64 bytes");

// ------------------------ Checksums ------------------------
// CRC32 (IEEE-802.3) for superblock; Dirent checksum is XOR over first 63 bytes.
// inode_crc is 64-bit: we store CRC32 in the low 32 bits (high 32 bits zero) for simplicity.
static uint32_t CRC32_TAB[256];

static inline void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}

static inline uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

static inline void superblock_crc_finalize(superblock_t* sb){
    sb->checksum = 0;
    sb->checksum = crc32(sb, sizeof(*sb) - sizeof(sb->checksum));
}

static inline void dirent_checksum_finalize(dirent64_t* de){
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // ino(4) + type(1) + name(58)
    de->checksum = x;
}

static inline void inode_crc_finalize(inode_t* ino){
    ino->inode_crc = 0;
    // Store CRC32 across the first 120 bytes (128 - 8) into the low 32 bits.
    uint32_t c = crc32(ino, sizeof(*ino) - sizeof(ino->inode_crc));
    ino->inode_crc = (uint64_t)c; // upper 32 bits remain 0
}

// ------------------------ Bitmap helpers ------------------------
static inline void bm_set(uint8_t* bm, uint64_t idx){
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}
static inline void bm_clear(uint8_t* bm, uint64_t idx){
    bm[idx >> 3] &= (uint8_t)~(1u << (idx & 7u));
}
static inline int bm_test(const uint8_t* bm, uint64_t idx){
    return (bm[idx >> 3] >> (idx & 7u)) & 1u;
}
static inline int bm_find_first_zero(const uint8_t* bm, uint64_t nbits){
    for(uint64_t i=0;i<nbits;i++){
        if(!bm_test(bm,i)) return (int)i;
    }
    return -1;
}

// ------------------------ Misc helpers ------------------------
static inline uint8_t* block_ptr(uint8_t* base, uint64_t block_index){
    return base + (block_index * (uint64_t)BS);
}

static inline void die(const char* msg){
    fprintf(stderr, "Error: %s\n", msg);
    exit(EXIT_FAILURE);
}

#endif // MINIVSFS_H
