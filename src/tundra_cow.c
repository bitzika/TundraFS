#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "../include/tundra_fs.h"
#include "../include/tundra_cow.h"

extern FILE *disk;
extern pthread_mutex_t tundra_lock;

#define COW_POOL_SIZE 1024
static int64_t cow_pool[COW_POOL_SIZE];
static int cow_pool_idx = 0;

/* Предвыделение COW-блоков */
static void cow_pool_init(FILE *f) {
    for (int i = 0; i < COW_POOL_SIZE; i++) {
        cow_pool[i] = find_free_block();
        if (cow_pool[i] > 0) mark_block(cow_pool[i], 1);
    }
    cow_pool_idx = 0;
}

static int64_t cow_pool_get(void) {
    if (cow_pool_idx < COW_POOL_SIZE) return cow_pool[cow_pool_idx++];
    return find_free_block();
}

int cow_has_active_snapshots(void) {
    int64_t snap_block = COW_START_BLOCK;
    cow_snapshot_header_t header;
    fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
    fread(&header, sizeof(header), 1, disk);
    return header.magic == 0x434F57534E415000LL;
}

int cow_create_snapshot(FILE *f, const char *name) {
    disk = f;
    int64_t snap_block = COW_START_BLOCK;
    cow_snapshot_header_t header;
    
    for (int i = 0; i < COW_MAX_SNAPSHOTS; i++) {
        fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
        fread(&header, sizeof(header), 1, disk);
        if (header.magic != 0x434F57534E415000LL) break;
        snap_block += 2;
    }
    
    memset(&header, 0, sizeof(header));
    header.magic = 0x434F57534E415000LL;
    header.id = time(NULL);
    header.timestamp = header.id;
    header.parent_id = -1;
    header.cow_bitmap_start = snap_block + 1;
    memcpy(header.name, name, strlen(name) < 256 ? strlen(name) : 255);
    
    cow_bitmap_t bitmap;
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.snapshot_id = header.id;
    
    int64_t sb[4096];
    fseek(disk, 0, SEEK_SET); fread(sb, sizeof(sb), 1, disk);
    bitmap.total_blocks = sb[5];
    
    fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
    fwrite(&header, sizeof(header), 1, disk);
    fwrite(&bitmap, sizeof(bitmap), 1, disk);
    fflush(disk);
    
    cow_pool_init(f);
    printf("Snapshot '%s' created (id=%ld)\n", name, header.id);
    return 0;
}

int cow_list_snapshots(FILE *f) {
    disk = f;
    int64_t snap_block = COW_START_BLOCK;
    int count = 0;
    
    printf("Snapshots:\n");
    for (int i = 0; i < COW_MAX_SNAPSHOTS; i++) {
        cow_snapshot_header_t header;
        fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
        fread(&header, sizeof(header), 1, disk);
        if (header.magic != 0x434F57534E415000LL) break;
        
        char time_str[32];
        time_t t = header.timestamp;
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&t));
        printf("  [%d] id=%ld name=%.32s time=%s\n", i, header.id, (char*)header.name, time_str);
        count++; snap_block += 2;
    }
    if (count == 0) printf("  (no snapshots)\n");
    return count;
}

int cow_write_block(FILE *f, int64_t block_num, const uint8_t *data) {
    disk = f;
    int64_t snap_block = COW_START_BLOCK;
    
    for (int i = 0; i < COW_MAX_SNAPSHOTS; i++) {
        cow_snapshot_header_t header;
        fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
        fread(&header, sizeof(header), 1, disk);
        if (header.magic != 0x434F57534E415000LL) break;
        
        cow_bitmap_t bitmap;
        fseek(disk, header.cow_bitmap_start * BLOCK_SIZE, SEEK_SET);
        fread(&bitmap, sizeof(bitmap), 1, disk);
        
        int byte_idx = block_num / 8;
        int bit_idx = block_num % 8;
        
        if (!(bitmap.bitmap[byte_idx] & (1 << bit_idx))) {
            uint8_t old_data[BLOCK_SIZE];
            fseek(disk, block_num * BLOCK_SIZE, SEEK_SET);
            fread(old_data, BLOCK_SIZE, 1, disk);
            
            int64_t cow_block = cow_pool_get();
            if (cow_block > 0) {
                fseek(disk, cow_block * BLOCK_SIZE, SEEK_SET);
                fwrite(old_data, BLOCK_SIZE, 1, disk);
                
                bitmap.bitmap[byte_idx] |= (1 << bit_idx);
                fseek(disk, header.cow_bitmap_start * BLOCK_SIZE, SEEK_SET);
                fwrite(&bitmap, sizeof(bitmap), 1, disk);
                header.block_count++;
                fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
                fwrite(&header, sizeof(header), 1, disk);
            }
        }
        snap_block += 2;
    }
    
    fseek(disk, block_num * BLOCK_SIZE, SEEK_SET);
    fwrite(data, BLOCK_SIZE, 1, disk);
    return 0;
}

int cow_rollback(FILE *f, int64_t snapshot_id) {
    disk = f;
    int64_t snap_block = COW_START_BLOCK;
    
    for (int i = 0; i < COW_MAX_SNAPSHOTS; i++) {
        cow_snapshot_header_t header;
        fseek(disk, snap_block * BLOCK_SIZE, SEEK_SET);
        fread(&header, sizeof(header), 1, disk);
        if (header.magic != 0x434F57534E415000LL) break;
        
        if (header.id == snapshot_id) {
            printf("Rolling back to '%s'...\n", (char*)header.name);
            cow_bitmap_t bitmap;
            fseek(disk, header.cow_bitmap_start * BLOCK_SIZE, SEEK_SET);
            fread(&bitmap, sizeof(bitmap), 1, disk);
            
            int restored = 0;
            for (int64_t b = 0; b < bitmap.total_blocks; b++) {
                int byte_idx = b / 8;
                int bit_idx = b % 8;
                if (bitmap.bitmap[byte_idx] & (1 << bit_idx)) {
                    uint8_t cow_data[BLOCK_SIZE];
                    int64_t cow_data_block = header.cow_bitmap_start + 1 + b;
                    fseek(disk, cow_data_block * BLOCK_SIZE, SEEK_SET);
                    fread(cow_data, BLOCK_SIZE, 1, disk);
                    fseek(disk, b * BLOCK_SIZE, SEEK_SET);
                    fwrite(cow_data, BLOCK_SIZE, 1, disk);
                    restored++;
                }
            }
            fflush(disk);
            printf("Restored %d blocks\n", restored);
            return 0;
        }
        snap_block += 2;
    }
    printf("Snapshot %ld not found\n", snapshot_id);
    return -1;
}
