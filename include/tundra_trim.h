#ifndef TUNDRA_TRIM_H
#define TUNDRA_TRIM_H

#include <stdint.h>
#include <pthread.h>

#define TRIM_BATCH_SIZE 256
#define ERASE_BLOCK_SIZE 128
#define ERASE_BLOCK_SECTORS (ERASE_BLOCK_SIZE * 8)

/* Очередь TRIM-команд */
typedef struct {
    int64_t blocks[TRIM_BATCH_SIZE];
    int count;
    pthread_mutex_t lock;
} trim_queue_t;

/* Инициализация TRIM */
void trim_init(void);
void trim_queue_add(int64_t block);
int trim_flush(FILE *disk);
int trim_discard_range(FILE *disk, int64_t start_block, int64_t count);

/* Оптимизация под erase-блоки */
int64_t alloc_aligned_block(int64_t size_hint);
void defrag_erase_block(FILE *disk, int64_t erase_block_start);

#endif
