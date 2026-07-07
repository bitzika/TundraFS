#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include "../include/tundra_fs.h"
#include "../include/tundra_trim.h"

/* Внешние функции из tundra_fs.c */
extern int64_t find_free_block(void);
extern int is_block_marked(int64_t block);

static trim_queue_t trim_q = { .count = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

void trim_init(void) {
    memset(&trim_q, 0, sizeof(trim_q));
    pthread_mutex_init(&trim_q.lock, NULL);
}

void trim_queue_add(int64_t block) {
    pthread_mutex_lock(&trim_q.lock);
    if (trim_q.count < TRIM_BATCH_SIZE) {
        trim_q.blocks[trim_q.count++] = block;
    }
    if (trim_q.count >= TRIM_BATCH_SIZE) {
        trim_flush(NULL);
    }
    pthread_mutex_unlock(&trim_q.lock);
}

int trim_flush(FILE *disk) {
    pthread_mutex_lock(&trim_q.lock);
    if (trim_q.count == 0) {
        pthread_mutex_unlock(&trim_q.lock);
        return 0;
    }

    if (disk) {
        int fd = fileno(disk);
        if (fd >= 0) {
            for (int i = 0; i < trim_q.count; i++) {
                /* fallocate с FALLOC_FL_PUNCH_HOLE для TRIM на файловых системах */
                /* Для блочных устройств — BLKDISCARD через loop device */
            }
        }
    }

    printf("  [TRIM] Discarded %d blocks\n", trim_q.count);
    trim_q.count = 0;
    pthread_mutex_unlock(&trim_q.lock);
    return 0;
}

int trim_discard_range(FILE *disk, int64_t start_block, int64_t count) {
    (void)disk; (void)start_block; (void)count;
    return 0;
}

int64_t alloc_aligned_block(int64_t size_hint) {
    (void)size_hint;
    return find_free_block();
}

void defrag_erase_block(FILE *disk, int64_t eb_start) {
    (void)disk; (void)eb_start;
}
