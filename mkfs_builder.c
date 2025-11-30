
// mkfs_builder.c — creates a raw MiniVSFS image
// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder

#define _FILE_OFFSET_BITS 64
#include "minivsfs.h"
#include <getopt.h>

static void usage(const char* prog){
    fprintf(stderr,
        "Usage:\n"
        "  %s --image <out.img> --size-kib <180..4096> --inodes <128..512>\n"
        "\n"
        "Creates a MiniVSFS image per the spec (block size=4096, inode size=128).\n",
        prog);
}

int main(int argc, char** argv){
    const char* image_path = NULL;
    long long size_kib = -1;
    long long inode_count = -1;

    static struct option long_opts[] = {
        {"image",    required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes",   required_argument, 0, 'n'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt, longidx;
    while((opt = getopt_long(argc, argv, "i:s:n:h", long_opts, &longidx)) != -1){
        switch(opt){
            case 'i': image_path = optarg; break;
            case 's': size_kib = atoll(optarg); break;
            case 'n': inode_count = atoll(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if(!image_path){ usage(argv[0]); die("--image is required"); }
    if(size_kib < 180 || size_kib > 4096 || (size_kib % 4) != 0){
        die("--size-kib must be in [180..4096] and a multiple of 4");
    }
    if(inode_count < 128 || inode_count > 512){
        die("--inodes must be in [128..512]");
    }

    crc32_init();
    const uint64_t total_blocks = (uint64_t)size_kib / 4u; // size_kib*1024/4096
    const uint64_t inode_table_bytes = (uint64_t)inode_count * INODE_SIZE;
    const uint64_t inode_table_blocks = (inode_table_bytes + BS - 1) / BS;

    // Layout
    const uint64_t sb_blk  = 0;
    const uint64_t ibm_blk = 1;
    const uint64_t dbm_blk = 2;
    const uint64_t itb_blk = 3;
    const uint64_t data_start = itb_blk + inode_table_blocks;
    if(total_blocks <= data_start){
        die("Image too small to hold metadata + at least one data block");
    }
    const uint64_t data_blocks = total_blocks - data_start;

    // Allocate image buffer
    const uint64_t image_bytes = total_blocks * (uint64_t)BS;
    uint8_t* img = (uint8_t*)calloc(1, image_bytes);
    if(!img) die("calloc failed");

    // Fill superblock
    superblock_t sb = {0};
    sb.magic               = 0x4D565346u; // "MVSF"
    sb.version             = 1u;
    sb.block_size          = BS;
    sb.total_blocks        = total_blocks;
    sb.inode_count         = (uint64_t)inode_count;
    sb.inode_bitmap_start  = ibm_blk;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start   = dbm_blk;
    sb.data_bitmap_blocks  = 1;
    sb.inode_table_start   = itb_blk;
    sb.inode_table_blocks  = inode_table_blocks;
    sb.data_region_start   = data_start;
    sb.data_region_blocks  = data_blocks;
    sb.root_inode          = ROOT_INO; // 1-indexed
    sb.mtime_epoch         = (uint64_t)time(NULL);
    sb.flags               = 0;
    superblock_crc_finalize(&sb);
    memcpy(block_ptr(img, sb_blk), &sb, sizeof(sb));

    // Inode bitmap: allocate inode #1 (root)
    uint8_t* ibm = block_ptr(img, ibm_blk);
    bm_set(ibm, 0); // inode #1

    // Data bitmap: allocate first data block for root directory
    uint8_t* dbm = block_ptr(img, dbm_blk);
    bm_set(dbm, 0); // first data block (index 0 in data region)

    // Inode table
    uint8_t* itbl = block_ptr(img, itb_blk);
    inode_t root = {0};
    root.mode = 0040000; // directory
    root.links = 2;      // . and ..
    root.uid = 0; root.gid = 0;
    root.size_bytes = 2u * sizeof(dirent64_t); // "." and ".."
    root.atime = root.mtime = root.ctime = (uint64_t)time(NULL);
    for(int i=0;i<DIRECT_MAX;i++) root.direct[i] = 0;
    root.direct[0] = (uint32_t)sb.data_region_start; // absolute block index
    root.reserved_0 = root.reserved_1 = root.reserved_2 = 0;
    root.proj_id = 17;  // arbitrary project/group id
    root.uid16_gid16 = 0;
    root.xattr_ptr = 0;
    inode_crc_finalize(&root);
    memcpy(itbl + (ROOT_INO - 1) * INODE_SIZE, &root, sizeof(root));

    // Root directory contents in its first data block
    uint8_t* dblk0 = block_ptr(img, sb.data_region_start);
    dirent64_t dot  = {0};
    dot.inode_no = ROOT_INO;
    dot.type = 2; // dir
    memset(dot.name, 0, sizeof(dot.name));
    dot.name[0] = '.';
    dirent_checksum_finalize(&dot);
    memcpy(dblk0 + 0 * sizeof(dirent64_t), &dot, sizeof(dot));

    dirent64_t dotdot = {0};
    dotdot.inode_no = ROOT_INO;
    dotdot.type = 2;
    memset(dotdot.name, 0, sizeof(dotdot.name));
    dotdot.name[0] = '.'; dotdot.name[1] = '.';
    dirent_checksum_finalize(&dotdot);
    memcpy(dblk0 + 1 * sizeof(dirent64_t), &dotdot, sizeof(dotdot));

    // Save image
    FILE* f = fopen(image_path, "wb");
    if(!f){
        fprintf(stderr, "Failed to open output '%s': %s\n", image_path, strerror(errno));
        free(img);
        return 1;
    }
    size_t wr = fwrite(img, 1, image_bytes, f);
    if(wr != image_bytes){
        fprintf(stderr, "Short write: wrote %zu of %llu bytes\n", wr, (unsigned long long)image_bytes);
        fclose(f); free(img); return 1;
    }
    fclose(f);
    free(img);
    fprintf(stderr, "Created MiniVSFS image '%s' (%llu blocks, %llu bytes)\n",
            image_path, (unsigned long long)total_blocks, (unsigned long long)image_bytes);
    return 0;
}
