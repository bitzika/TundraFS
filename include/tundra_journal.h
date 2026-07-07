#ifndef TUNDRA_JOURNAL_H
#define TUNDRA_JOURNAL_H

#include <stdint.h>

#define JOURNAL_MAGIC 0x4A54554E44524100  /* "JTUNDRA\0" */
#define TX_BEGIN    1
#define TX_WRITE    2
#define TX_COMMIT   3
#define TX_CHECKPOINT 4

/*
 * Запись журнала (64 байта):
 * [0..7]   tx_id (ID транзакции)
 * [8..15]  type (тип операции)
 * [16..23] inode_num
 * [24..31] block_num
 * [32..39] offset
 * [40..47] size
 * [48..55] checksum (xor всех полей)
 * [56..63] reserved
 */
typedef struct {
    int64_t tx_id;
    int64_t type;
    int64_t inode_num;
    int64_t block_num;
    int64_t offset;
    int64_t size;
    int64_t checksum;
    int64_t reserved;
} journal_entry_t;

#define JOURNAL_ENTRY_SIZE 64
#define JOURNAL_ENTRIES_PER_BLOCK (4096 / JOURNAL_ENTRY_SIZE)

#endif
