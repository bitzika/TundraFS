#ifndef TUNDRA_COW_H
#define TUNDRA_COW_H
#include <stdint.h>
#include <time.h>

#define COW_MAX_SNAPSHOTS 64
#define COW_BITMAP_BLOCKS 256

/* Заголовок снапшота */
typedef struct {
    int64_t magic;          /* 0x434F57534E415000 = "COWSNAP\0" */
    int64_t id;             /* Номер снапшота */
    int64_t timestamp;      /* Время создания */
    int64_t parent_id;      /* Родительский снапшот (-1 если нет) */
    int64_t block_count;    /* Количество блоков в ФС на момент снапшота */
    int64_t cow_bitmap_start; /* Где лежит COW-битмап */
    int64_t name[32];       /* Имя снапшота (256 байт) */
    int64_t reserved[16];
} cow_snapshot_header_t;

/* COW-битмап: 1 бит на блок = этот блок скопирован при записи */
typedef struct {
    int64_t snapshot_id;
    int64_t total_blocks;
    uint8_t bitmap[COW_BITMAP_BLOCKS * 4096]; /* 1MB = 8M блоков */
} cow_bitmap_t;

/* Функции COW */
int cow_create_snapshot(FILE *disk, const char *name);
int cow_list_snapshots(FILE *disk);
int cow_rollback(FILE *disk, int64_t snapshot_id);
int cow_delete_snapshot(FILE *disk, int64_t snapshot_id);
int cow_write_block(FILE *disk, int64_t block_num, const uint8_t *data);
int cow_read_block(FILE *disk, int64_t snapshot_id, int64_t block_num, uint8_t *data);

#endif
int cow_has_active_snapshots(void);
#define COW_START_BLOCK (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE + BITMAP_BLOCKS + JOURNAL_BLOCKS + 1024)
extern int64_t find_free_block(void);
extern void mark_block(int64_t block, int used);
