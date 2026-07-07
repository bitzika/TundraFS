#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/tundra_fs.h"

static FILE *disk = NULL;
int fix_mode = 0;

static int64_t ag_region_start, ag_size_blocks, num_ags;

int64_t pack_ascii(const char *str) {
    int64_t r = 0;
    for (int i = 0; i < 8 && str[i]; i++) r += (int64_t)(unsigned char)str[i] << (8*(7-i));
    return r;
}

/* Определяет, к какой AG относится блок и его локальное смещение внутри
   data-региона этой AG. Возвращает 0, если блок — не data-блок (это либо
   зарезервированная область в начале диска, либо заголовок/битмап самой AG). */
int is_block_data(int64_t block, int64_t *out_ag, int64_t *out_local) {
    if (block < ag_region_start) return 0;
    int64_t rel = block - ag_region_start;
    int64_t a = rel / ag_size_blocks;
    if (a >= num_ags) return 0;
    int64_t ag_start = ag_region_start + a * ag_size_blocks;
    int64_t data_start = ag_start + AG_HEADER_BLOCKS + AG_BITMAP_BLOCKS;
    if (block < data_start) return 0;
    *out_ag = a;
    *out_local = block - data_start;
    return 1;
}

int is_block_marked(int64_t block) {
    int64_t a, local;
    if (!is_block_data(block, &a, &local)) return 1; /* заголовок/битмап AG — всегда "занят" */
    int64_t ag_start = ag_region_start + a * ag_size_blocks;
    int64_t bitmap_off = (ag_start + AG_HEADER_BLOCKS) * BLOCK_SIZE;
    uint8_t byte;
    fseek(disk, bitmap_off + local/8, SEEK_SET); fread(&byte, 1, 1, disk);
    return (byte >> (local%8)) & 1;
}

void mark_block(int64_t block, int used) {
    int64_t a, local;
    if (!is_block_data(block, &a, &local)) return;
    int64_t ag_start = ag_region_start + a * ag_size_blocks;
    int64_t bitmap_off = (ag_start + AG_HEADER_BLOCKS) * BLOCK_SIZE;
    uint8_t byte;
    fseek(disk, bitmap_off + local/8, SEEK_SET); fread(&byte, 1, 1, disk);
    if (used) byte |= (1<<(local%8)); else byte &= ~(1<<(local%8));
    fseek(disk, bitmap_off + local/8, SEEK_SET); fwrite(&byte, 1, 1, disk); fflush(disk);
}

/* Сканирование HTree и сбор всех блоков */
void scan_htree(int64_t dir_block, int64_t *refs, int64_t total_blocks) {
    htree_node_t node;
    fseek(disk, dir_block * BLOCK_SIZE, SEEK_SET);
    fread(&node, sizeof(node), 1, disk);

    if (node.depth > 0) {
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++) {
            if (node.blocks[i] > 0 && node.blocks[i] < total_blocks) {
                refs[node.blocks[i]]++;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: %s <image> [--fix]\n", argv[0]); return 1; }
    if (argc > 2 && !strcmp(argv[2], "--fix")) fix_mode = 1;

    disk = fopen(argv[1], fix_mode ? "r+b" : "rb");
    if (!disk) { perror("fopen"); return 1; }

    printf("TundraFS fsck v6 (AG-aware)%s\n", fix_mode ? " (fix mode)" : "");

    int64_t sb[4096];
    fseek(disk, 0, SEEK_SET); fread(sb, sizeof(sb), 1, disk);
    int64_t total_blocks = sb[5];
    ag_region_start = sb[40];
    ag_size_blocks = sb[41];
    num_ags = sb[42];
    if (num_ags < 1) num_ags = 1;

    if (sb[0] != pack_ascii("TUNDRA")) { printf("  Superblock: FAIL\n"); fclose(disk); return 1; }
    printf("  Superblock: OK\n");
    printf("  AGs: %ld (region starts at block %ld, size %ld blocks each)\n",
           num_ags, ag_region_start, ag_size_blocks);

    /* Проверка журнала */
    int64_t jnl_start = (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE + BITMAP_BLOCKS) * BLOCK_SIZE;
    int64_t jnl_header[512];
    fseek(disk, jnl_start, SEEK_SET); fread(jnl_header, sizeof(jnl_header), 1, disk);

    int64_t tx_id = jnl_header[3], jnl_pos = jnl_header[1];
    printf("  Journal: tx_id=%ld, pos=%ld\n", tx_id, jnl_pos);

    if (jnl_pos > 4096 && fix_mode) {
        printf("  Journal replay: %ld entries to check\n", (jnl_pos - 4096) / 64);
        jnl_entry_t entry;
        int64_t last_tx = -1, in_tx = 0;

        for (int64_t off = 4096; off < jnl_pos; off += 64) {
            fseek(disk, jnl_start + off, SEEK_SET);
            fread(&entry, sizeof(entry), 1, disk);
            if (entry.type == TX_BEGIN) { in_tx = 1; last_tx = entry.tx_id; }
            else if (entry.type == TX_COMMIT) {
                if (entry.inode_num < 0) {
                    printf("  Rolling back tx %ld...\n", entry.tx_id);
                    for (int64_t off2 = 4096; off2 < off; off2 += 64) {
                        jnl_entry_t e2;
                        fseek(disk, jnl_start + off2, SEEK_SET);
                        fread(&e2, sizeof(e2), 1, disk);
                        if (e2.tx_id == entry.tx_id && e2.type == TX_WRITE && e2.block_num > 0)
                            mark_block(e2.block_num, 0);
                    }
                }
                in_tx = 0;
            }
        }
        if (in_tx && last_tx >= 0) {
            printf("  Rolling back unclosed tx %ld\n", last_tx);
            for (int64_t off = 4096; off < jnl_pos; off += 64) {
                jnl_entry_t e;
                fseek(disk, jnl_start + off, SEEK_SET);
                fread(&e, sizeof(e), 1, disk);
                if (e.tx_id == last_tx && e.type == TX_WRITE && e.block_num > 0)
                    mark_block(e.block_num, 0);
            }
        }
        jnl_header[1] = 4096; jnl_header[3] = 0;
        fseek(disk, jnl_start, SEEK_SET);
        fwrite(jnl_header, sizeof(jnl_header), 1, disk);
        fflush(disk);
        printf("  Journal cleaned\n");
    }

    int64_t *refs = calloc(total_blocks, sizeof(int64_t));
    for (int64_t i = 0; i < RESERVED_BLOCKS; i++) refs[i] = 1;

    int64_t num_ranges = sb[32];
    if (num_ranges < 1) num_ranges = 1;

    for (int r = 0; r < num_ranges && r < MAX_INODE_RANGES; r++) {
        int64_t range_start = sb[33 + r*3];
        int64_t range_count = sb[35 + r*3];

        for (int64_t i = 0; i < range_count; i++) {
            inode_t inode;
            fseek(disk, range_start * BLOCK_SIZE + i * INODE_SIZE, SEEK_SET);
            fread(&inode, sizeof(inode), 1, disk);
            if (inode.type == INODE_FREE) continue;

            /* Прямые extents */
            for (int e = 0; e < inode.extent_count && e < DIRECT_BLOCKS; e++)
                for (int64_t j = 0; j < inode.extents[e].length && j < total_blocks; j++)
                    if (inode.extents[e].start_block + j < total_blocks)
                        refs[inode.extents[e].start_block + j]++;

            /* Single indirect */
            if (inode.extents[SINGLE_INDIRECT].start_block > 0 &&
                inode.extents[SINGLE_INDIRECT].start_block < total_blocks) {
                int64_t ib = inode.extents[SINGLE_INDIRECT].start_block;
                refs[ib]++;
                int64_t ind[PTRS_PER_BLOCK];
                fseek(disk, ib*BLOCK_SIZE, SEEK_SET); fread(ind, sizeof(ind), 1, disk);
                for (int k = 0; k < PTRS_PER_BLOCK; k++)
                    if (ind[k] > 0 && ind[k] < total_blocks) refs[ind[k]]++;
            }

            /* Double indirect */
            if (inode.extents[DOUBLE_INDIRECT].start_block > 0 &&
                inode.extents[DOUBLE_INDIRECT].start_block < total_blocks) {
                int64_t db = inode.extents[DOUBLE_INDIRECT].start_block;
                refs[db]++;
                int64_t dbl[PTRS_PER_BLOCK];
                fseek(disk, db*BLOCK_SIZE, SEEK_SET); fread(dbl, sizeof(dbl), 1, disk);
                for (int di = 0; di < PTRS_PER_BLOCK; di++) {
                    if (dbl[di] <= 0 || dbl[di] >= total_blocks) continue;
                    refs[dbl[di]]++;
                    int64_t sgl[PTRS_PER_BLOCK];
                    fseek(disk, dbl[di]*BLOCK_SIZE, SEEK_SET); fread(sgl, sizeof(sgl), 1, disk);
                    for (int k = 0; k < PTRS_PER_BLOCK; k++)
                        if (sgl[k] > 0 && sgl[k] < total_blocks) refs[sgl[k]]++;
                }
            }

            /* Triple indirect */
            if (inode.extents[TRIPLE_INDIRECT].start_block > 0 &&
                inode.extents[TRIPLE_INDIRECT].start_block < total_blocks) {
                int64_t tb = inode.extents[TRIPLE_INDIRECT].start_block;
                refs[tb]++;
                int64_t tpl[PTRS_PER_BLOCK];
                fseek(disk, tb*BLOCK_SIZE, SEEK_SET); fread(tpl, sizeof(tpl), 1, disk);
                for (int ti = 0; ti < PTRS_PER_BLOCK; ti++) {
                    if (tpl[ti] <= 0 || tpl[ti] >= total_blocks) continue;
                    refs[tpl[ti]]++;
                    int64_t dbl[PTRS_PER_BLOCK];
                    fseek(disk, tpl[ti]*BLOCK_SIZE, SEEK_SET); fread(dbl, sizeof(dbl), 1, disk);
                    for (int di = 0; di < PTRS_PER_BLOCK; di++) {
                        if (dbl[di] <= 0 || dbl[di] >= total_blocks) continue;
                        refs[dbl[di]]++;
                        int64_t sgl[PTRS_PER_BLOCK];
                        fseek(disk, dbl[di]*BLOCK_SIZE, SEEK_SET); fread(sgl, sizeof(sgl), 1, disk);
                        for (int k = 0; k < PTRS_PER_BLOCK; k++)
                            if (sgl[k] > 0 && sgl[k] < total_blocks) refs[sgl[k]]++;
                    }
                }
            }

            if (inode.type == INODE_DIR && inode.extent_count > 0) {
                int64_t rb = inode.extents[0].start_block;
                if (rb > 0 && rb < total_blocks) scan_htree(rb, refs, total_blocks);
            }
        }
    }

    /* Сверка: только по настоящим data-блокам внутри AG. Заголовки и
       битмапы самих AG, а также зарезервированная область в начале диска,
       из проверки исключаются — это не пространство для файлов. */
    int errors = 0, fixed = 0;
    for (int64_t b = RESERVED_BLOCKS; b < total_blocks; b++) {
        int64_t a, local;
        if (!is_block_data(b, &a, &local)) continue;

        int marked = is_block_marked(b);
        if (marked && !refs[b]) {
            if (errors < 20) printf("  [ERROR] Block %ld (AG %ld) marked but not ref'd\n", b, a);
            errors++;
            if (fix_mode) { mark_block(b, 0); fixed++; }
        }
        if (!marked && refs[b]) {
            if (errors < 20) printf("  [ERROR] Block %ld (AG %ld) ref'd but not marked\n", b, a);
            errors++;
            if (fix_mode) { mark_block(b, 1); fixed++; }
        }
    }

    free(refs);

    if (fix_mode && fixed > 0) printf("  Fixed %d block(s)\n", fixed);
    printf("  Inodes/Bitmap: %s (%d errors)\n", errors ? "ERRORS" : "OK", errors);
    printf("fsck complete: %s\n", errors ? (fixed ? "FIXED" : "ERRORS FOUND") : "CLEAN");

    fclose(disk);
    return errors - fixed;
}