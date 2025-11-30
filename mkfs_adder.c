
// mkfs_adder.c — adds a file from the working directory into a MiniVSFS image
// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder

#define _FILE_OFFSET_BITS 64
#include "minivsfs.h"
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h> 

static void usage(const char* prog){
    fprintf(stderr,
        "Usage:\n"
        "  %s --input <in.img> --output <out.img> --file <path>\n",
        prog);
}

static off_t file_size_path(const char* path){
    struct stat st;
    if(stat(path, &st) != 0) return -1;
    return st.st_size;
}

static int read_file(const char* path, uint8_t** out_buf, size_t* out_len){
    *out_buf = NULL; *out_len = 0;
    FILE* f = fopen(path, "rb");
    if(!f) return -1;
    if(fseek(f, 0, SEEK_END) != 0){ fclose(f); return -1; }
    long long sz = ftell(f);
    if(sz < 0){ fclose(f); return -1; }
    if(fseek(f, 0, SEEK_SET) != 0){ fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if(!buf){ fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if(rd != (size_t)sz){ free(buf); return -1; }
    *out_buf = buf; *out_len = (size_t)sz;
    return 0;
}

static const char* base_name(const char* path){
    const char* s = strrchr(path, '/');
#ifdef _WIN32
    const char* s2 = strrchr(path, '\\');
    if(!s || (s2 && s2 > s)) s = s2;
#endif
    return s ? s + 1 : path;
}

int main(int argc, char** argv){
    const char* input_path  = NULL;
    const char* output_path = NULL;
    const char* file_path   = NULL;

    static struct option long_opts[] = {
        {"input",  required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file",   required_argument, 0, 'f'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt, longidx;
    while((opt = getopt_long(argc, argv, "i:o:f:h", long_opts, &longidx)) != -1){
        switch(opt){
            case 'i': input_path = optarg; break;
            case 'o': output_path = optarg; break;
            case 'f': file_path = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if(!input_path || !output_path || !file_path){
        usage(argv[0]); return 1;
    }

    crc32_init();

    // Load image
    FILE* fin = fopen(input_path, "rb");
    if(!fin){
        fprintf(stderr, "Failed to open input '%s': %s\n", input_path, strerror(errno));
        return 1;
    }
    if(fseek(fin, 0, SEEK_END) != 0){ fclose(fin); die("seek failed"); }
    long long fsz = ftell(fin);
    if(fsz < 0){ fclose(fin); die("ftell failed"); }
    if(fseek(fin, 0, SEEK_SET) != 0){ fclose(fin); die("seek failed"); }
    uint8_t* img = (uint8_t*)malloc((size_t)fsz);
    if(!img){ fclose(fin); die("malloc failed"); }
    size_t rd = fread(img, 1, (size_t)fsz, fin);
    fclose(fin);
    if(rd != (size_t)fsz){
        free(img); die("short read on input image");
    }

    // Parse superblock
    if((size_t)fsz < BS) { free(img); die("input image too small"); }
    superblock_t sb = {0};
    memcpy(&sb, block_ptr(img, 0), sizeof(sb));
    if(sb.magic != 0x4D565346u || sb.version != 1u || sb.block_size != BS){
        free(img); die("invalid superblock (magic/version/block size mismatch)");
    }
    // Optional: verify checksum
    uint32_t chk = 0;
    superblock_t tmp = sb; tmp.checksum = 0;
    chk = crc32(&tmp, sizeof(tmp) - sizeof(tmp.checksum));
    if(chk != sb.checksum){
        fprintf(stderr, "Warning: superblock checksum mismatch (expected 0x%08x got 0x%08x)\n",
                sb.checksum, chk);
    }

    // Bitmaps and tables
    uint8_t* ibm = block_ptr(img, sb.inode_bitmap_start);
    uint8_t* dbm = block_ptr(img, sb.data_bitmap_start);
    uint8_t* itbl = block_ptr(img, sb.inode_table_start);

    // Read file to add
    uint8_t* file_buf = NULL; size_t file_len = 0;
    if(read_file(file_path, &file_buf, &file_len) != 0){
        free(img); die("failed to read --file");
    }
    const char* fname = base_name(file_path);
    size_t fname_len = strlen(fname);
    if(fname_len == 0 || fname_len > 58){
        free(file_buf); free(img); die("file name must be 1..58 bytes");
    }

    // Compute required blocks
    uint64_t needed_blocks = (file_len + BS - 1) / BS;
    if(needed_blocks > DIRECT_MAX){
        fprintf(stderr, "Warning: file too large for MiniVSFS (%zu bytes, needs %llu blocks, max %d)\n",
                file_len, (unsigned long long)needed_blocks, DIRECT_MAX);
        free(file_buf); free(img); return 2;
    }
    if(needed_blocks == 0) needed_blocks = 1; // allocate one block for empty file

    // Allocate an inode (first-fit)
    int free_inode_idx = bm_find_first_zero(ibm, sb.inode_count);
    if(free_inode_idx < 0){
        free(file_buf); free(img); die("no free inodes");
    }
    bm_set(ibm, (uint64_t)free_inode_idx);
    uint32_t new_ino_no = (uint32_t)(free_inode_idx + 1);

    // Allocate data blocks (first-fit) from data region
    uint32_t direct[DIRECT_MAX] = {0};
    uint64_t allocated = 0;
    for(uint64_t bit = 0; bit < sb.data_region_blocks && allocated < needed_blocks; bit++){
        if(!bm_test(dbm, bit)){
            bm_set(dbm, bit);
            direct[allocated] = (uint32_t)(sb.data_region_start + bit); // absolute block
            allocated++;
        }
    }
    if(allocated < needed_blocks){
        free(file_buf); free(img); die("not enough free data blocks");
    }

    // Write file data into allocated blocks
    size_t remaining = file_len;
    size_t off = 0;
    for(uint64_t i=0;i<needed_blocks;i++){
        uint8_t* blk = block_ptr(img, direct[i]);
        size_t chunk = remaining < BS ? remaining : BS;
        if(chunk > 0) memcpy(blk, file_buf + off, chunk);
        if(chunk < BS){ // zero the rest (already zero, but be explicit)
            memset(blk + chunk, 0, BS - chunk);
        }
        off += chunk;
        if(remaining >= chunk) remaining -= chunk;
    }

    // Build inode for the new file
    inode_t ino = {0};
    ino.mode = 0100000; // regular file
    ino.links = 1;
    ino.uid = 0; ino.gid = 0;
    ino.size_bytes = (uint64_t)file_len;
    ino.atime = ino.mtime = ino.ctime = (uint64_t)time(NULL);
    for(int i=0;i<DIRECT_MAX;i++) ino.direct[i]=0;
    for(uint64_t i=0;i<needed_blocks;i++) ino.direct[i] = direct[i];
    ino.reserved_0 = ino.reserved_1 = ino.reserved_2 = 0;
    ino.proj_id = 17;
    ino.uid16_gid16 = 0;
    ino.xattr_ptr = 0;
    inode_crc_finalize(&ino);
    memcpy(itbl + (new_ino_no - 1) * INODE_SIZE, &ino, sizeof(ino));

    // Insert directory entry into root directory; grow root if needed
    inode_t root = {0};
    memcpy(&root, itbl + (ROOT_INO - 1) * INODE_SIZE, sizeof(root));
    // Count entries and find free slot
    int slot_found = 0;
    uint32_t chosen_block_abs = 0;
    size_t chosen_offset_in_block = 0;
    size_t dir_entries_count = 0;

    for(int di=0; di<DIRECT_MAX && !slot_found; di++){
        uint32_t absb = root.direct[di];
        if(absb == 0) break; // stop at first zero — allocated contiguously
        uint8_t* b = block_ptr(img, absb);
        for(int s=0;s<(int)(BS/sizeof(dirent64_t));s++){
            dirent64_t ent = {0};
            memcpy(&ent, b + s*sizeof(dirent64_t), sizeof(ent));
            if(ent.inode_no == 0){
                slot_found = 1;
                chosen_block_abs = absb;
                chosen_offset_in_block = (size_t)s * sizeof(dirent64_t);
                break;
            }else{
                dir_entries_count++;
            }
        }
    }

    // If no free slot, allocate a new block for root directory
    if(!slot_found){
        // find next free direct slot in root
        int di_empty = -1;
        for(int di=0; di<DIRECT_MAX; di++){
            if(root.direct[di] == 0){ di_empty = di; break; }
        }
        if(di_empty < 0){
            free(file_buf); free(img); die("root directory out of direct pointers");
        }
        // allocate a data block for root
        int bit = bm_find_first_zero(dbm, sb.data_region_blocks);
        if(bit < 0){
            free(file_buf); free(img); die("no free data blocks for root dir growth");
        }
        bm_set(dbm, (uint64_t)bit);
        uint32_t absb = (uint32_t)(sb.data_region_start + (uint64_t)bit);
        root.direct[di_empty] = absb;
        chosen_block_abs = absb;
        chosen_offset_in_block = 0;
        slot_found = 1;
        // zero-init new block
        memset(block_ptr(img, absb), 0, BS);
    }

    // Prepare and write new directory entry
    dirent64_t newde = {0};
    newde.inode_no = new_ino_no;
    newde.type = 1; // file
    memset(newde.name, 0, sizeof(newde.name));
    memcpy(newde.name, fname, fname_len); // truncate validated above
    dirent_checksum_finalize(&newde);
    uint8_t* dirblk = block_ptr(img, chosen_block_abs);
    memcpy(dirblk + chosen_offset_in_block, &newde, sizeof(newde));

    // Update root inode size (count real used entries)
    // We add 1 new entry; include "." and ".." if present
    size_t new_count = 0;
    for(int di=0; di<DIRECT_MAX; di++){
        uint32_t absb = root.direct[di];
        if(!absb) break;
        uint8_t* b = block_ptr(img, absb);
        for(int s=0;s<(int)(BS/sizeof(dirent64_t));s++){
            dirent64_t ent = {0};
            memcpy(&ent, b + s*sizeof(dirent64_t), sizeof(ent));
            if(ent.inode_no != 0) new_count++;
        }
    }
    root.size_bytes = (uint64_t)new_count * sizeof(dirent64_t);
    root.links += 1; // per spec discussion: root link count increases by 1
    root.mtime = root.ctime = (uint64_t)time(NULL);
    inode_crc_finalize(&root);
    memcpy(itbl + (ROOT_INO - 1) * INODE_SIZE, &root, sizeof(root));

    // Save to output
    FILE* fout = fopen(output_path, "wb");
    if(!fout){
        fprintf(stderr, "Failed to open output '%s': %s\n", output_path, strerror(errno));
        free(file_buf); free(img); return 1;
    }
    size_t wr = fwrite(img, 1, (size_t)fsz, fout);
    fclose(fout);
    free(file_buf); free(img);
    if(wr != (size_t)fsz){
        fprintf(stderr, "Short write writing output image\n");
        return 1;
    }
    fprintf(stderr, "Added '%s' (inode #%u, %zu bytes, %llu blocks) to '%s' -> '%s'\n",
            fname, new_ino_no, file_len, (unsigned long long)needed_blocks, input_path, output_path);
    return 0;
}
