#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/tundra_fs.h"

int64_t pack_ascii(const char *str) {
    int64_t result = 0;
    for (int i = 0; i < 8 && str[i]; i++)
        result += (int64_t)(unsigned char)str[i] << (8 * (7 - i));
    return result;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <filename> <total_blocks>\n", argv[0]); return 1; }
    const char *filename = argv[1];
    int64_t total_blocks = atoll(argv[2]);
    int64_t disk_gb = (total_blocks * BLOCK_SIZE) / (1024*1024*1024);
    int64_t total_inodes = TOTAL_INODES_BASE;

    if (total_blocks <= RESERVED_BLOCKS + AG_HEADER_BLOCKS + AG_BITMAP_BLOCKS + 1) {
        fprintf(stderr, "Disk too small: need at least %ld blocks\n",
                (int64_t)(RESERVED_BLOCKS + AG_HEADER_BLOCKS + AG_BITMAP_BLOCKS + 1));
        return 1;
    }

    int64_t ag_region_start = RESERVED_BLOCKS;
    int64_t ag_region_blocks = total_blocks - ag_region_start;
    int64_t ag_data_capacity = AG_SIZE_BLOCKS - AG_HEADER_BLOCKS - AG_BITMAP_BLOCKS;
    int64_t num_ags = (ag_region_blocks + AG_SIZE_BLOCKS - 1) / AG_SIZE_BLOCKS;

    int64_t ag0_start = ag_region_start;
    int64_t ag0_data_start = ag0_start + AG_HEADER_BLOCKS + AG_BITMAP_BLOCKS;

    printf("TundraFS mkfs v10 (Allocation Groups)\n");
    printf("  Disk: %ld GB (%ld blocks)\n", disk_gb, total_blocks);
    printf("  Inodes: %ld (base) + dynamic expansion up to 1 billion\n", (int64_t)total_inodes);
    printf("  AG size: %lld GB, AGs: %ld\n", (long long)(AG_SIZE_BYTES/(1024*1024*1024)), num_ags);

    FILE *f = fopen(filename, "wb");
    if (!f) { perror("fopen"); return 1; }

    int64_t sb[4096];
    memset(sb, 0, sizeof(sb));
    sb[0] = pack_ascii("TUNDRA");
    sb[2] = 2;
    sb[4] = BLOCK_SIZE;
    sb[5] = total_blocks;
    sb[6] = total_blocks - RESERVED_BLOCKS;
    sb[7] = 1;
    sb[8] = SUPERBLOCK_BLOCKS;
    sb[9] = total_inodes;
    sb[10] = SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE;
    sb[11] = SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE + BITMAP_BLOCKS;
    sb[12] = 0; sb[13] = 100;
    sb[17] = pack_ascii("GNU APL ");
    sb[18] = pack_ascii("TUNDRA  ");
    sb[19] = time(NULL);
    sb[20] = 20260701; sb[21] = 20060627;
    sb[32] = 1; sb[33] = SUPERBLOCK_BLOCKS; sb[34] = INODE_BLOCKS_BASE; sb[35] = total_inodes;
    sb[40] = ag_region_start; sb[41] = AG_SIZE_BLOCKS; sb[42] = num_ags;

    int64_t cs = 0;
    for (int i = 0; i < 30; i++) cs ^= sb[i];
    sb[30] = cs;

    fseek(f, 0, SEEK_SET);
    fwrite(sb, sizeof(sb), 1, f);

    fseek(f, SUPERBLOCK_BLOCKS * BLOCK_SIZE, SEEK_SET);
    int64_t chunk_size = (16 * 1024 * 1024) / sizeof(inode_t);
    inode_t *inode_chunk = calloc(chunk_size, sizeof(inode_t));

    inode_chunk[1].inode_num = 1; inode_chunk[1].type = INODE_DIR;
    inode_chunk[1].owner = pack_ascii("A       ");
    inode_chunk[1].created_time = inode_chunk[1].modified_time = inode_chunk[1].accessed_time = time(NULL);
    inode_chunk[1].link_count = 2;
    inode_chunk[1].extents[0].start_block = ag0_data_start;
    inode_chunk[1].extents[0].length = 1;
    inode_chunk[1].extent_count = 1;
    for (int s = 0; s < 4; s++) {
        inode_chunk[1].perms[s*5+0] = 1;
        if (s <= 1) { inode_chunk[1].perms[s*5+1]=1; inode_chunk[1].perms[s*5+2]=1; inode_chunk[1].perms[s*5+4]=1; }
    }
    fwrite(inode_chunk, sizeof(inode_t), chunk_size, f);
    memset(inode_chunk, 0, chunk_size * sizeof(inode_t));
    for (int64_t off = chunk_size; off < total_inodes; off += chunk_size) {
        int64_t n = (off + chunk_size > total_inodes) ? (total_inodes - off) : chunk_size;
        fwrite(inode_chunk, sizeof(inode_t), n, f);
    }
    free(inode_chunk);

    fseek(f, (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE) * BLOCK_SIZE, SEEK_SET);
    uint8_t *old_bmp = calloc(BITMAP_BLOCKS * BLOCK_SIZE, 1);
    fwrite(old_bmp, BITMAP_BLOCKS * BLOCK_SIZE, 1, f);
    free(old_bmp);

    fseek(f, (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE + BITMAP_BLOCKS) * BLOCK_SIZE, SEEK_SET);
    int64_t jnl_header[512];
    memset(jnl_header, 0, sizeof(jnl_header));
    jnl_header[0] = JOURNAL_MAGIC;
    jnl_header[2] = JOURNAL_BLOCKS * BLOCK_SIZE;
    fwrite(jnl_header, sizeof(jnl_header), 1, f);
    uint8_t *jnl_rest = calloc(JOURNAL_BLOCKS * BLOCK_SIZE - sizeof(jnl_header), 1);
    fwrite(jnl_rest, JOURNAL_BLOCKS * BLOCK_SIZE - sizeof(jnl_header), 1, f);
    free(jnl_rest);

    uint8_t *zero_bitmap = calloc(AG_BITMAP_BLOCKS * BLOCK_SIZE, 1);
    for (int64_t a = 0; a < num_ags; a++) {
        int64_t ag_start = ag_region_start + a * AG_SIZE_BLOCKS;
        int64_t ag_capacity = (a == num_ags - 1)
            ? (total_blocks - ag_start - AG_HEADER_BLOCKS - AG_BITMAP_BLOCKS)
            : ag_data_capacity;
        if (ag_capacity < 0) ag_capacity = 0;

        ag_header_t hdr; memset(&hdr, 0, sizeof(hdr));
        hdr.magic = AG_MAGIC; hdr.ag_index = a;
        hdr.total_blocks = ag_capacity; hdr.free_blocks = ag_capacity;
        hdr.bitmap_start = ag_start + AG_HEADER_BLOCKS;
        hdr.data_start = ag_start + AG_HEADER_BLOCKS + AG_BITMAP_BLOCKS;

        fseek(f, ag_start * BLOCK_SIZE, SEEK_SET);
        fwrite(&hdr, sizeof(hdr), 1, f);
        uint8_t pad[BLOCK_SIZE]; memset(pad, 0, sizeof(pad));
        fwrite(pad, BLOCK_SIZE - sizeof(hdr), 1, f);

        fseek(f, hdr.bitmap_start * BLOCK_SIZE, SEEK_SET);
        fwrite(zero_bitmap, AG_BITMAP_BLOCKS * BLOCK_SIZE, 1, f);
    }
    free(zero_bitmap);

    { uint8_t byte = 0x01;
      fseek(f, (ag0_start + AG_HEADER_BLOCKS) * BLOCK_SIZE, SEEK_SET);
      fwrite(&byte, 1, 1, f); }

    fseek(f, ag0_data_start * BLOCK_SIZE, SEEK_SET);
    htree_node_t root; memset(&root, 0, sizeof(root));
    root.count = 2;
    root.entries[0].inode = 1; root.entries[0].name_len = 1; root.entries[0].name[0] = '.';
    root.entries[1].inode = 1; root.entries[1].name_len = 2; root.entries[1].name[0]='.'; root.entries[1].name[1]='.';
    fwrite(&root, BLOCK_SIZE, 1, f);

    fseek(f, total_blocks * BLOCK_SIZE - 1, SEEK_SET);
    fputc(0, f);
    fclose(f);

    printf("  [OK] %ld inodes (base), %ld allocation groups\n", (int64_t)total_inodes, num_ags);
    printf("Done: %s\n", filename);
    return 0;
}
