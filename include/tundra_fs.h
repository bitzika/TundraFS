#ifndef TUNDRA_FS_H
#define TUNDRA_FS_H
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define BLOCK_SIZE 4096
#define SUPERBLOCK_BLOCKS 1
#define INODE_BLOCKS_BASE 65536
#define BITMAP_BLOCKS 16384
#define JOURNAL_BLOCKS 65536
#define RESERVED_BLOCKS (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE + BITMAP_BLOCKS + JOURNAL_BLOCKS)

#define MAX_NAME 248
#define DIR_ENTRY_SIZE 256
#define MAX_EXTENTS 7

#define JOURNAL_MAGIC 0x4A54554E44524100
#define JNL_HEADER_SIZE 4096
#define JNL_ENTRY_SIZE 64
#define TX_BEGIN 1
#define TX_WRITE 2
#define TX_COMMIT 3

#define INODE_FREE 0
#define INODE_FILE 1
#define INODE_DIR  2
#define INODE_SIZE 512
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define TOTAL_INODES_BASE ((INODE_BLOCKS_BASE * BLOCK_SIZE) / INODE_SIZE)

#define HTREE_HASH_SEED 0x9E3779B97F4A7C15
#define HTREE_ROOT_ENTRIES 8
#define HTREE_INDIRECT_ENTRIES 16

#define MAX_INODE_RANGES 256
#define INODE_RANGE_BLOCKS 1024

#define DIRECT_BLOCKS 4
#define SINGLE_INDIRECT 4
#define DOUBLE_INDIRECT 5
#define TRIPLE_INDIRECT 6
#define PTRS_PER_BLOCK (BLOCK_SIZE / sizeof(int64_t))

/* ══════════ Allocation Groups ══════════
   Регион ДАННЫХ делится на независимые AG по 16 ГБ. У каждой AG свой
   заголовок + свой битмап. Снимает потолок ~2ТБ старого плоского битмапа. */
#define AG_SIZE_BYTES   (16LL * 1024 * 1024 * 1024)
#define AG_SIZE_BLOCKS  (AG_SIZE_BYTES / BLOCK_SIZE)
#define AG_BITMAP_BYTES (AG_SIZE_BLOCKS / 8)
#define AG_BITMAP_BLOCKS (AG_BITMAP_BYTES / BLOCK_SIZE)
#define AG_HEADER_BLOCKS 1
#define AG_MAGIC 0x4147535000000000LL

typedef struct {
    int64_t magic;
    int64_t ag_index;
    int64_t total_blocks;
    int64_t free_blocks;
    int64_t bitmap_start;
    int64_t data_start;
    int64_t reserved[10];
} __attribute__((packed)) ag_header_t;

typedef struct {
    int64_t start_block;
    int64_t block_count;
    int64_t inode_count;
    int64_t first_inode;
} inode_range_t;

typedef struct {
    int64_t start_block;
    int64_t length;
    int64_t logical_start;
} __attribute__((packed)) extent_t;

typedef struct {
    int64_t inode_num; int32_t type; uint8_t perms[20]; int64_t size;
    int64_t owner; int64_t created_time; int64_t modified_time; int64_t accessed_time;
    int64_t tier; int32_t link_count; int32_t flags; int32_t extent_count; int32_t reserved2;
    extent_t extents[MAX_EXTENTS]; uint8_t _pad[248];
} inode_t;
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch!");

typedef struct {
    int64_t inode; uint32_t hash; uint16_t name_len; char name[MAX_NAME]; uint8_t padding[2];
} __attribute__((packed)) htree_entry_t;

typedef struct {
    uint32_t depth; uint32_t count; int64_t blocks[HTREE_INDIRECT_ENTRIES]; htree_entry_t entries[HTREE_ROOT_ENTRIES];
    uint8_t _pad[BLOCK_SIZE - (8 + HTREE_INDIRECT_ENTRIES*(int)sizeof(int64_t) + HTREE_ROOT_ENTRIES*(int)sizeof(htree_entry_t))];
} __attribute__((packed)) htree_node_t;
_Static_assert(sizeof(htree_node_t) == BLOCK_SIZE, "htree_node_t size mismatch!");

typedef struct {
    htree_entry_t entries[HTREE_INDIRECT_ENTRIES];
} __attribute__((packed)) htree_leaf_t;

typedef struct {
    int64_t inode; uint16_t name_len; char name[MAX_NAME]; uint8_t padding[2];
} dir_entry_t;

typedef struct {
    int64_t tx_id; int64_t type; int64_t inode_num; int64_t block_num; int64_t offset; int64_t size; int64_t checksum; int64_t reserved;
} jnl_entry_t;

#define ORACLE_HISTORY 1024
typedef struct {
    int64_t magic; int64_t version; int64_t total_checks; int64_t blocks_failed; int64_t preemptive_saves; int64_t last_check_time; int64_t reserved[10];
    struct { int64_t block_num; int64_t error_count; int64_t last_error_time; int64_t read_latency_us; } errors[ORACLE_HISTORY];
} oracle_t;

extern pthread_mutex_t tundra_lock;

#endif
