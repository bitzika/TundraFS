#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/file.h>
#include "../include/tundra_fs.h"
#include "../include/tundra_cow.h"
#include "../include/tundra_trim.h"
int64_t find_free_block(void);
static int64_t find_free_inode_impl(void);
void mark_block(int64_t block, int used);
void read_inode(int64_t num, inode_t *inode);

pthread_mutex_t tundra_lock = PTHREAD_MUTEX_INITIALIZER;
FILE *disk = NULL;

#define DIR_LOCKS 64
pthread_mutex_t dir_locks[DIR_LOCKS];
#define DELAYED_MAX 64

static struct { int64_t block; uint8_t data[BLOCK_SIZE]; int pending; } delayed_writes[DELAYED_MAX];

/* Allocation Groups: диск делится на независимые AG по 16 ГБ, у каждой
   свой битмап (mmap лениво, по требованию). Снимает потолок ~2ТБ старого
   плоского битмапа и даёт параллелизм между операциями в разных AG. */
typedef struct {
    uint8_t *bitmap;
    int64_t data_start;
    int64_t data_blocks;
    int mapped;
} ag_runtime_t;

static ag_runtime_t *ag_table = NULL;
static int64_t num_ags_g = 0;
static int64_t ag_region_start_g = 0;
static int64_t ag_size_blocks_g = AG_SIZE_BLOCKS;
static int64_t total_blocks_g = 0;
#define MAX_AG_LOCKS 4096
static pthread_mutex_t ag_locks[MAX_AG_LOCKS];
static int ag_locks_initialized = 0;

/* flock() — блокировка на уровне ядра ОС, привязанная к файлу диска, а не
   к памяти процесса. pthread_mutex (ag_locks/tundra_lock) синхронизирует
   только потоки ВНУТРИ одного процесса — а stress_test запускает каждую
   операцию как ОТДЕЛЬНЫЙ процесс через system(), так что pthread-локи между
   ними бесполезны. flock реально синхронизирует разные процессы. */
static void disk_lock(void) { flock(fileno(disk), LOCK_EX); }
static void disk_unlock(void) { flock(fileno(disk), LOCK_UN); }

static void ag_locks_init(void) {
    if (ag_locks_initialized) return;
    for (int i = 0; i < MAX_AG_LOCKS; i++) pthread_mutex_init(&ag_locks[i], NULL);
    ag_locks_initialized = 1;
}

/* Каждая AG получает свой независимый лок (по остатку от MAX_AG_LOCKS —
   при миллионах AG на йоттабайтных томах несколько AG будут делить один
   лок, но это всё равно на порядки лучше единственного глобального лока
   на весь диск, который мы использовали раньше). */
static pthread_mutex_t *ag_lock_for(int64_t ag_idx) {
    return &ag_locks[ag_idx % MAX_AG_LOCKS];
}

static void ag_ensure_mapped(int64_t a) {
    if (ag_table[a].mapped) return;
    int64_t ag_start = ag_region_start_g + a * ag_size_blocks_g;
    int64_t bitmap_off = (ag_start + AG_HEADER_BLOCKS) * BLOCK_SIZE;
    int fd = fileno(disk);
    void *p = mmap(NULL, AG_BITMAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bitmap_off);
    if (p == MAP_FAILED) { fprintf(stderr, "AG %ld bitmap mmap failed\n", a); exit(1); }
    ag_table[a].bitmap = (uint8_t*)p;
    ag_table[a].data_start = ag_start + AG_HEADER_BLOCKS + AG_BITMAP_BLOCKS;
    int64_t remaining = total_blocks_g - ag_table[a].data_start;
    int64_t cap = ag_size_blocks_g - AG_HEADER_BLOCKS - AG_BITMAP_BLOCKS;
    ag_table[a].data_blocks = remaining < cap ? remaining : cap;
    ag_table[a].mapped = 1;
}

void ag_init(void) {
    ag_locks_init();
    int64_t sb[4096];
    fseek(disk, 0, SEEK_SET); fread(sb, sizeof(sb), 1, disk);
    total_blocks_g = sb[5];
    ag_region_start_g = sb[40];
    ag_size_blocks_g = sb[41];
    num_ags_g = sb[42];
    if (num_ags_g < 1) num_ags_g = 1;
    ag_table = calloc(num_ags_g, sizeof(ag_runtime_t));
}

void ag_cleanup(void) {
    if (!ag_table) return;
    for (int64_t a = 0; a < num_ags_g; a++)
        if (ag_table[a].mapped) munmap(ag_table[a].bitmap, AG_BITMAP_BYTES);
    free(ag_table);
    ag_table = NULL;
}

/* Динамические диапазоны инодов */
static inode_range_t inode_ranges[MAX_INODE_RANGES];
static int num_inode_ranges = 0;
static int64_t total_inodes_dynamic = 0;

int64_t pack_ascii(const char *str) {
    int64_t r = 0;
    for (int i = 0; i < 8 && str[i]; i++) r += (int64_t)(unsigned char)str[i] << (8*(7-i));
    return r;
}

/* Загрузка диапазонов инодов из суперблока */
void load_inode_ranges(void) {
    int64_t sb[4096];
    fseek(disk, 0, SEEK_SET); fread(sb, sizeof(sb), 1, disk);

    num_inode_ranges = sb[32];
    if (num_inode_ranges < 1) num_inode_ranges = 1;

    for (int i = 0; i < num_inode_ranges && i < MAX_INODE_RANGES; i++) {
        inode_ranges[i].start_block = sb[33 + i*3];
        inode_ranges[i].block_count = sb[34 + i*3];
        inode_ranges[i].inode_count = sb[35 + i*3];
        inode_ranges[i].first_inode = (i == 0) ? 0 : (inode_ranges[i-1].first_inode + inode_ranges[i-1].inode_count);
        total_inodes_dynamic += inode_ranges[i].inode_count;
    }
}

/* Добавление нового диапазона инодов */
int add_inode_range(void) {
    if (num_inode_ranges >= MAX_INODE_RANGES) return -1;

    int64_t start_block = find_free_block();
    if (start_block < 0) return -1;

    /* Выделяем INODE_RANGE_BLOCKS последовательных блоков */
    for (int64_t i = 0; i < INODE_RANGE_BLOCKS; i++) {
        mark_block(start_block + i, 1);
    }

    int idx = num_inode_ranges;
    inode_ranges[idx].start_block = start_block;
    inode_ranges[idx].block_count = INODE_RANGE_BLOCKS;
    inode_ranges[idx].inode_count = (INODE_RANGE_BLOCKS * BLOCK_SIZE) / INODE_SIZE;
    inode_ranges[idx].first_inode = total_inodes_dynamic;
    total_inodes_dynamic += inode_ranges[idx].inode_count;

    /* Обновляем суперблок */
    int64_t sb[4096];
    fseek(disk, 0, SEEK_SET); fread(sb, sizeof(sb), 1, disk);
    num_inode_ranges++;
    sb[32] = num_inode_ranges;
    sb[33 + idx*3] = start_block;
    sb[34 + idx*3] = INODE_RANGE_BLOCKS;
    sb[35 + idx*3] = inode_ranges[idx].inode_count;
    fseek(disk, 0, SEEK_SET); fwrite(sb, sizeof(sb), 1, disk);
    fflush(disk);

    /* Инициализируем новые иноды нулями */
    inode_t zero_inode;
    memset(&zero_inode, 0, sizeof(zero_inode));
    for (int64_t i = 0; i < inode_ranges[idx].inode_count; i++) {
        fseek(disk, start_block * BLOCK_SIZE + i * INODE_SIZE, SEEK_SET);
        fwrite(&zero_inode, sizeof(zero_inode), 1, disk);
    }
    fflush(disk);

    printf("  [INFO] Added inode range %d: %ld inodes at block %ld\n",
           idx, inode_ranges[idx].inode_count, start_block);
    return 0;
}

/* Поиск свободного инода по всем диапазонам */
int64_t find_free_inode(void) {
    return find_free_inode_impl();
}

/* Помечает inode как временно зарезервированный (type=-1), чтобы
   исключить гонку между поиском свободного inode и его записью. */
static void claim_inode_raw(int64_t global_ino) {
    for (int r = 0; r < num_inode_ranges; r++) {
        if (global_ino >= inode_ranges[r].first_inode &&
            global_ino < inode_ranges[r].first_inode + inode_ranges[r].inode_count) {
            int64_t offset = (global_ino - inode_ranges[r].first_inode) * INODE_SIZE;
            inode_t tmp; memset(&tmp, 0, sizeof(tmp));
            tmp.type = -1; tmp.inode_num = global_ino;
            fseek(disk, inode_ranges[r].start_block * BLOCK_SIZE + offset, SEEK_SET);
            fwrite(&tmp, sizeof(tmp), 1, disk); fflush(disk);
            return;
        }
    }
}

static int64_t find_free_inode_impl(void) {
    disk_lock();
    pthread_mutex_lock(&tundra_lock);
    inode_t inode;
    for (int r = 0; r < num_inode_ranges; r++) {
        for (int64_t i = (r == 0 ? 2 : 0); i < inode_ranges[r].inode_count; i++) {
            int64_t global_ino = inode_ranges[r].first_inode + i;
            int64_t offset = i * INODE_SIZE;
            fseek(disk, inode_ranges[r].start_block * BLOCK_SIZE + offset, SEEK_SET);
            fread(&inode, sizeof(inode), 1, disk);
            if (inode.type == INODE_FREE) {
                claim_inode_raw(global_ino);
                pthread_mutex_unlock(&tundra_lock);
                disk_unlock();
                return global_ino;
            }
        }
    }
    pthread_mutex_unlock(&tundra_lock);
    disk_unlock();
    if (add_inode_range() == 0) {
        int r = num_inode_ranges - 1;
        disk_lock();
        pthread_mutex_lock(&tundra_lock);
        claim_inode_raw(inode_ranges[r].first_inode);
        pthread_mutex_unlock(&tundra_lock);
        disk_unlock();
        return inode_ranges[r].first_inode;
    }
    return -1;
}

/* Чтение инода с учётом диапазонов */
void read_inode(int64_t num, inode_t *inode) {
    pthread_mutex_lock(&tundra_lock);
    for (int r = 0; r < num_inode_ranges; r++) {
        if (num >= inode_ranges[r].first_inode &&
            num < inode_ranges[r].first_inode + inode_ranges[r].inode_count) {
            int64_t offset = (num - inode_ranges[r].first_inode) * INODE_SIZE;
            fseek(disk, inode_ranges[r].start_block * BLOCK_SIZE + offset, SEEK_SET);
            fread(inode, sizeof(inode_t), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
            return;
        }
    }
    memset(inode, 0, sizeof(inode_t));
    pthread_mutex_unlock(&tundra_lock);
}

void write_inode(int64_t num, inode_t *inode) {
    pthread_mutex_lock(&tundra_lock);
    for (int r = 0; r < num_inode_ranges; r++) {
        if (num >= inode_ranges[r].first_inode &&
            num < inode_ranges[r].first_inode + inode_ranges[r].inode_count) {
            int64_t offset = (num - inode_ranges[r].first_inode) * INODE_SIZE;
            fseek(disk, inode_ranges[r].start_block * BLOCK_SIZE + offset, SEEK_SET);
            fwrite(inode, sizeof(inode_t), 1, disk);
            fflush(disk);
            pthread_mutex_unlock(&tundra_lock);
            return;
        }
    }
    pthread_mutex_unlock(&tundra_lock);
}

uint32_t htree_hash(const char *name, int len) {
    uint64_t h = HTREE_HASH_SEED;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)name[i]; h *= 0x9E3779B97F4A7C15; }
    return (uint32_t)(h ^ (h >> 32));
}

static void lock_dir(int64_t inode) { pthread_mutex_lock(&dir_locks[inode % DIR_LOCKS]); }
static void unlock_dir(int64_t inode) { pthread_mutex_unlock(&dir_locks[inode % DIR_LOCKS]); }

int64_t find_free_block(void) {
    disk_lock();
    int64_t result = -1;
    for (int64_t a = 0; a < num_ags_g; a++) {
        pthread_mutex_t *lk = ag_lock_for(a);
        pthread_mutex_lock(lk);
        ag_ensure_mapped(a);
        uint8_t *bm = ag_table[a].bitmap;
        int64_t n = ag_table[a].data_blocks;
        for (int64_t b = 0; b < n; b++) {
            if (!(bm[b/8] & (1<<(b%8)))) {
                bm[b/8] |= (1<<(b%8));
                pthread_mutex_unlock(lk);
                result = ag_table[a].data_start + b;
                break;
            }
        }
        if (result >= 0) break;
        pthread_mutex_unlock(lk);
    }
    disk_unlock();
    return result;
}

void mark_block(int64_t block, int used) {
    int64_t rel = block - ag_region_start_g;
    int64_t a = rel / ag_size_blocks_g;
    if (a < 0 || a >= num_ags_g) return;
    disk_lock();
    pthread_mutex_t *lk = ag_lock_for(a);
    pthread_mutex_lock(lk);
    ag_ensure_mapped(a);
    int64_t local = block - ag_table[a].data_start;
    if (local < 0 || local >= ag_table[a].data_blocks) { pthread_mutex_unlock(lk); disk_unlock(); return; }
    uint8_t *bm = ag_table[a].bitmap;
    if (used) bm[local/8] |= (1<<(local%8));
    else bm[local/8] &= ~(1<<(local%8));
    if (!used) trim_queue_add(block);
    pthread_mutex_unlock(lk);
    disk_unlock();
}

void delayed_write(int64_t block, const uint8_t *data) {
    pthread_mutex_lock(&tundra_lock);
    for (int i = 0; i < DELAYED_MAX; i++)
        if (!delayed_writes[i].pending) {
            delayed_writes[i].block=block;
            memcpy(delayed_writes[i].data, data, BLOCK_SIZE);
            delayed_writes[i].pending=1;
            break;
        }
    pthread_mutex_unlock(&tundra_lock);
}

void delayed_flush(void) {
    pthread_mutex_lock(&tundra_lock);
    for (int i = 0; i < DELAYED_MAX; i++)
        if (delayed_writes[i].pending) {
            fseek(disk, delayed_writes[i].block*BLOCK_SIZE, SEEK_SET);
            fwrite(delayed_writes[i].data, BLOCK_SIZE, 1, disk);
            delayed_writes[i].pending = 0;
        }
    fflush(disk);
    pthread_mutex_unlock(&tundra_lock);
}

int64_t extent_get_block(inode_t *inode, int64_t lb) {
    /* Прямые экстенты — ищем по диапазону, а не по индексу */
    for (int i = 0; i < inode->extent_count && i < DIRECT_BLOCKS; i++) {
        int64_t s = inode->extents[i].logical_start;
        int64_t n = inode->extents[i].length;
        if (lb >= s && lb < s + n) return inode->extents[i].start_block + (lb - s);
    }
    
    /* Single indirect (4): 512 блоков × 4KB = 2MB */
    if (inode->extents[SINGLE_INDIRECT].start_block) {
        int64_t s = inode->extents[SINGLE_INDIRECT].logical_start;
        int64_t n = inode->extents[SINGLE_INDIRECT].length;
        if (lb >= s && lb < s + n) {
            int64_t ind[PTRS_PER_BLOCK];
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, inode->extents[SINGLE_INDIRECT].start_block * BLOCK_SIZE, SEEK_SET);
            fread(ind, sizeof(ind), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
            return ind[lb - s];
        }
    }
    
    /* Double indirect (5): 512×512 блоков × 4KB = 1GB */
    if (inode->extents[DOUBLE_INDIRECT].start_block) {
        int64_t s = inode->extents[DOUBLE_INDIRECT].logical_start;
        int64_t n = inode->extents[DOUBLE_INDIRECT].length;
        if (lb >= s && lb < s + n) {
            int64_t off = lb - s;
            int64_t di = off / PTRS_PER_BLOCK;
            int64_t bi = off % PTRS_PER_BLOCK;
            int64_t dbl[PTRS_PER_BLOCK];
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, inode->extents[DOUBLE_INDIRECT].start_block * BLOCK_SIZE, SEEK_SET);
            fread(dbl, sizeof(dbl), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
            if (dbl[di]) {
                int64_t sgl[PTRS_PER_BLOCK];
                pthread_mutex_lock(&tundra_lock);
                fseek(disk, dbl[di] * BLOCK_SIZE, SEEK_SET);
                fread(sgl, sizeof(sgl), 1, disk);
                pthread_mutex_unlock(&tundra_lock);
                return sgl[bi];
            }
        }
    }
    
    /* Triple indirect (6): 512×512×512 блоков × 4KB = 512GB */
    if (inode->extents[TRIPLE_INDIRECT].start_block) {
        int64_t s = inode->extents[TRIPLE_INDIRECT].logical_start;
        int64_t n = inode->extents[TRIPLE_INDIRECT].length;
        if (lb >= s && lb < s + n) {
            int64_t off = lb - s;
            int64_t ti = off / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
            int64_t rem = off % (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
            int64_t di = rem / PTRS_PER_BLOCK;
            int64_t bi = rem % PTRS_PER_BLOCK;
            int64_t tpl[PTRS_PER_BLOCK];
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, inode->extents[TRIPLE_INDIRECT].start_block * BLOCK_SIZE, SEEK_SET);
            fread(tpl, sizeof(tpl), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
            if (tpl[ti]) {
                int64_t dbl[PTRS_PER_BLOCK];
                pthread_mutex_lock(&tundra_lock);
                fseek(disk, tpl[ti] * BLOCK_SIZE, SEEK_SET);
                fread(dbl, sizeof(dbl), 1, disk);
                pthread_mutex_unlock(&tundra_lock);
                if (dbl[di]) {
                    int64_t sgl[PTRS_PER_BLOCK];
                    pthread_mutex_lock(&tundra_lock);
                    fseek(disk, dbl[di] * BLOCK_SIZE, SEEK_SET);
                    fread(sgl, sizeof(sgl), 1, disk);
                    pthread_mutex_unlock(&tundra_lock);
                    return sgl[bi];
                }
            }
        }
    }
    return -1;
}

static int64_t alloc_index_block(void) {
    int64_t b = find_free_block();
    if (b < 0) return -1;
    int64_t zero[PTRS_PER_BLOCK]; memset(zero, 0, sizeof(zero));
    pthread_mutex_lock(&tundra_lock);
    fseek(disk, b*BLOCK_SIZE, SEEK_SET); fwrite(zero, sizeof(zero), 1, disk);
    pthread_mutex_unlock(&tundra_lock);
    return b;
}

static int extent_add_indirect(inode_t *inode, int64_t lb, int64_t phys) {
    int64_t base = DIRECT_BLOCKS;
    int64_t single_cap = PTRS_PER_BLOCK;
    int64_t double_cap = PTRS_PER_BLOCK * PTRS_PER_BLOCK;
    int64_t triple_cap = PTRS_PER_BLOCK * PTRS_PER_BLOCK * PTRS_PER_BLOCK;
    int64_t off = lb - base;

    if (off < single_cap) {
        extent_t *e = &inode->extents[SINGLE_INDIRECT];
        if (!e->start_block) {
            int64_t ib = alloc_index_block();
            if (ib < 0) return -1;
            e->start_block = ib; e->logical_start = base; e->length = 0;
        }
        int64_t ind[PTRS_PER_BLOCK];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, e->start_block*BLOCK_SIZE, SEEK_SET); fread(ind, sizeof(ind), 1, disk);
        ind[off] = phys;
        fseek(disk, e->start_block*BLOCK_SIZE, SEEK_SET); fwrite(ind, sizeof(ind), 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        if (off+1 > e->length) e->length = off+1;
        return 0;
    }
    off -= single_cap;

    if (off < double_cap) {
        extent_t *e = &inode->extents[DOUBLE_INDIRECT];
        if (!e->start_block) {
            int64_t ib = alloc_index_block();
            if (ib < 0) return -1;
            e->start_block = ib; e->logical_start = base+single_cap; e->length = 0;
        }
        int64_t di = off / PTRS_PER_BLOCK, bi = off % PTRS_PER_BLOCK;
        int64_t dbl[PTRS_PER_BLOCK];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, e->start_block*BLOCK_SIZE, SEEK_SET); fread(dbl, sizeof(dbl), 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        if (!dbl[di]) {
            int64_t ib2 = alloc_index_block();
            if (ib2 < 0) return -1;
            dbl[di] = ib2;
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, e->start_block*BLOCK_SIZE, SEEK_SET); fwrite(dbl, sizeof(dbl), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
        }
        int64_t sgl[PTRS_PER_BLOCK];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, dbl[di]*BLOCK_SIZE, SEEK_SET); fread(sgl, sizeof(sgl), 1, disk);
        sgl[bi] = phys;
        fseek(disk, dbl[di]*BLOCK_SIZE, SEEK_SET); fwrite(sgl, sizeof(sgl), 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        if (off+1 > e->length) e->length = off+1;
        return 0;
    }
    off -= double_cap;

    if (off < triple_cap) {
        extent_t *e = &inode->extents[TRIPLE_INDIRECT];
        if (!e->start_block) {
            int64_t ib = alloc_index_block();
            if (ib < 0) return -1;
            e->start_block = ib; e->logical_start = base+single_cap+double_cap; e->length = 0;
        }
        int64_t ti = off / (PTRS_PER_BLOCK*PTRS_PER_BLOCK);
        int64_t rem = off % (PTRS_PER_BLOCK*PTRS_PER_BLOCK);
        int64_t di = rem / PTRS_PER_BLOCK, bi = rem % PTRS_PER_BLOCK;
        int64_t tpl[PTRS_PER_BLOCK];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, e->start_block*BLOCK_SIZE, SEEK_SET); fread(tpl, sizeof(tpl), 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        if (!tpl[ti]) {
            int64_t ib2 = alloc_index_block();
            if (ib2 < 0) return -1;
            tpl[ti] = ib2;
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, e->start_block*BLOCK_SIZE, SEEK_SET); fwrite(tpl, sizeof(tpl), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
        }
        int64_t dbl[PTRS_PER_BLOCK];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, tpl[ti]*BLOCK_SIZE, SEEK_SET); fread(dbl, sizeof(dbl), 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        if (!dbl[di]) {
            int64_t ib3 = alloc_index_block();
            if (ib3 < 0) return -1;
            dbl[di] = ib3;
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, tpl[ti]*BLOCK_SIZE, SEEK_SET); fwrite(dbl, sizeof(dbl), 1, disk);
            pthread_mutex_unlock(&tundra_lock);
        }
        int64_t sgl[PTRS_PER_BLOCK];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, dbl[di]*BLOCK_SIZE, SEEK_SET); fread(sgl, sizeof(sgl), 1, disk);
        sgl[bi] = phys;
        fseek(disk, dbl[di]*BLOCK_SIZE, SEEK_SET); fwrite(sgl, sizeof(sgl), 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        if (off+1 > e->length) e->length = off+1;
        return 0;
    }
    return -1;
}

int extent_add_block(inode_t *inode, int64_t lb, int64_t phys) {
    if (inode->extent_count > 0 && inode->extent_count <= DIRECT_BLOCKS) {
        extent_t *last = &inode->extents[inode->extent_count - 1];
        if (last->start_block + last->length == phys &&
            last->logical_start + last->length == lb) {
            last->length++;
            return 0;
        }
    }
    if (inode->extent_count < DIRECT_BLOCKS) {
        extent_t *e = &inode->extents[inode->extent_count];
        e->start_block = phys; e->length = 1; e->logical_start = lb;
        inode->extent_count++;
        return 0;
    }
    return extent_add_indirect(inode, lb, phys);
}

void free_all_blocks(inode_t *inode) {
    delayed_flush();
    /* Прямые extents */
    for (int i = 0; i < inode->extent_count; i++)
        for (int64_t j = 0; j < inode->extents[i].length; j++)
            mark_block(inode->extents[i].start_block+j, 0);
    /* Single indirect */
    if (inode->extents[SINGLE_INDIRECT].start_block) {
        int64_t ind[PTRS_PER_BLOCK];
        fseek(disk, inode->extents[SINGLE_INDIRECT].start_block * BLOCK_SIZE, SEEK_SET);
        fread(ind, sizeof(ind), 1, disk);
        for (int i = 0; i < PTRS_PER_BLOCK; i++) if (ind[i]) mark_block(ind[i], 0);
        mark_block(inode->extents[SINGLE_INDIRECT].start_block, 0);
    }
    /* Double indirect */
    if (inode->extents[DOUBLE_INDIRECT].start_block) {
        int64_t dbl[PTRS_PER_BLOCK];
        fseek(disk, inode->extents[DOUBLE_INDIRECT].start_block * BLOCK_SIZE, SEEK_SET);
        fread(dbl, sizeof(dbl), 1, disk);
        for (int i = 0; i < PTRS_PER_BLOCK; i++) if (dbl[i]) {
            int64_t sgl[PTRS_PER_BLOCK];
            fseek(disk, dbl[i] * BLOCK_SIZE, SEEK_SET); fread(sgl, sizeof(sgl), 1, disk);
            for (int j = 0; j < PTRS_PER_BLOCK; j++) if (sgl[j]) mark_block(sgl[j], 0);
            mark_block(dbl[i], 0);
        }
        mark_block(inode->extents[DOUBLE_INDIRECT].start_block, 0);
    }
    /* Triple indirect */
    if (inode->extents[TRIPLE_INDIRECT].start_block) {
        int64_t tpl[PTRS_PER_BLOCK];
        fseek(disk, inode->extents[TRIPLE_INDIRECT].start_block * BLOCK_SIZE, SEEK_SET);
        fread(tpl, sizeof(tpl), 1, disk);
        for (int i = 0; i < PTRS_PER_BLOCK; i++) if (tpl[i]) {
            int64_t dbl[PTRS_PER_BLOCK];
            fseek(disk, tpl[i] * BLOCK_SIZE, SEEK_SET); fread(dbl, sizeof(dbl), 1, disk);
            for (int j = 0; j < PTRS_PER_BLOCK; j++) if (dbl[j]) {
                int64_t sgl[PTRS_PER_BLOCK];
                fseek(disk, dbl[j] * BLOCK_SIZE, SEEK_SET); fread(sgl, sizeof(sgl), 1, disk);
                for (int k = 0; k < PTRS_PER_BLOCK; k++) if (sgl[k]) mark_block(sgl[k], 0);
                mark_block(dbl[j], 0);
            }
            mark_block(tpl[i], 0);
        }
        mark_block(inode->extents[TRIPLE_INDIRECT].start_block, 0);
    }
    memset(inode->extents, 0, sizeof(inode->extents));
    inode->extent_count = 0;
}

int htree_remove_entry(int64_t dir_inode, const char *name) {
    lock_dir(dir_inode);
    inode_t dir; read_inode(dir_inode, &dir);
    if (dir.type != INODE_DIR) { unlock_dir(dir_inode); return -1; }

    int64_t root_block = dir.extents[0].start_block;
    htree_node_t node;
    pthread_mutex_lock(&tundra_lock);
    fseek(disk, root_block*BLOCK_SIZE, SEEK_SET); fread(&node, BLOCK_SIZE, 1, disk);
    pthread_mutex_unlock(&tundra_lock);

    int name_len = strlen(name), removed = 0;

    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++) {
        if (node.entries[i].inode && node.entries[i].name_len == name_len &&
            !strncmp(node.entries[i].name, name, name_len)) {
            memset(&node.entries[i], 0, sizeof(node.entries[i]));
            if (node.count > 0) node.count--;
            removed = 1;
            goto save_root;
        }
    }

    if (node.depth > 0) {
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++) {
            if (!node.blocks[i]) continue;
            htree_leaf_t leaf; memset(&leaf, 0, sizeof(leaf));
            pthread_mutex_lock(&tundra_lock);
            fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, BLOCK_SIZE, 1, disk);
            pthread_mutex_unlock(&tundra_lock);
            for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++) {
                if (leaf.entries[j].inode && leaf.entries[j].name_len == name_len &&
                    !strncmp(leaf.entries[j].name, name, name_len)) {
                    memset(&leaf.entries[j], 0, sizeof(leaf.entries[j]));
                    removed = 1;
                    int empty = 1;
                    for (int k = 0; k < HTREE_INDIRECT_ENTRIES; k++)
                        if (leaf.entries[k].inode) { empty = 0; break; }
                    if (empty) { mark_block(node.blocks[i], 0); node.blocks[i] = 0; }
                    else delayed_write(node.blocks[i], (uint8_t*)&leaf);
                    goto save_root;
                }
            }
        }
    }

save_root:
    if (removed) {
        delayed_write(root_block, (uint8_t*)&node); delayed_flush();
        dir.size -= DIR_ENTRY_SIZE; dir.modified_time = time(NULL);
        write_inode(dir_inode, &dir);
    }
    unlock_dir(dir_inode);
    return removed ? 0 : -1;
}

int64_t htree_lookup(int64_t dir_inode, const char *name) {
    inode_t dir; read_inode(dir_inode, &dir);
    if (dir.type != INODE_DIR) return -1;
    int64_t rb = dir.extents[0].start_block, name_len = strlen(name);
    htree_node_t node;
    pthread_mutex_lock(&tundra_lock);
    fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fread(&node, BLOCK_SIZE, 1, disk);
    pthread_mutex_unlock(&tundra_lock);

    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++)
        if (node.entries[i].inode && node.entries[i].name_len == name_len &&
            !strncmp(node.entries[i].name, name, name_len)) return node.entries[i].inode;

    if (node.depth > 0)
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++)
            if (node.blocks[i]) {
                htree_leaf_t leaf; memset(&leaf, 0, sizeof(leaf));
                pthread_mutex_lock(&tundra_lock);
                fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, BLOCK_SIZE, 1, disk);
                pthread_mutex_unlock(&tundra_lock);
                for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
                    if (leaf.entries[j].inode && leaf.entries[j].name_len == name_len &&
                        !strncmp(leaf.entries[j].name, name, name_len)) return leaf.entries[j].inode;
            }
    return -1;
}

int htree_add_entry(int64_t dir_inode, const char *name, int64_t new_inode) {
    lock_dir(dir_inode);
    inode_t dir; read_inode(dir_inode, &dir);
    int64_t rb = dir.extents[0].start_block;
    uint32_t hash = htree_hash(name, strlen(name));
    int name_len = strlen(name);

    htree_node_t node;
    pthread_mutex_lock(&tundra_lock);
    fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fread(&node, BLOCK_SIZE, 1, disk);
    pthread_mutex_unlock(&tundra_lock);

    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++)
        if (node.entries[i].inode == 0) {
            node.entries[i].inode = new_inode; node.entries[i].hash = hash;
            node.entries[i].name_len = name_len; strncpy(node.entries[i].name, name, MAX_NAME-1);
            if (node.count < HTREE_ROOT_ENTRIES) node.count++;
            delayed_write(rb, (uint8_t*)&node); delayed_flush();
            dir.size += DIR_ENTRY_SIZE; dir.modified_time = time(NULL);
            write_inode(dir_inode, &dir); unlock_dir(dir_inode); return 0;
        }

    if (node.depth == 0) { node.depth = 1; memset(node.blocks, 0, sizeof(node.blocks)); }
    int idx = hash % HTREE_INDIRECT_ENTRIES;

    if (node.blocks[idx] == 0) {
        int64_t lb = find_free_block();
        if (lb < 0) { unlock_dir(dir_inode); return -1; }
        node.blocks[idx] = lb;
        htree_leaf_t leaf; memset(&leaf, 0, sizeof(leaf));
        leaf.entries[0].inode = new_inode; leaf.entries[0].hash = hash;
        leaf.entries[0].name_len = name_len; strncpy(leaf.entries[0].name, name, MAX_NAME-1);
        delayed_write(rb, (uint8_t*)&node); delayed_write(lb, (uint8_t*)&leaf); delayed_flush();
    } else {
        htree_leaf_t leaf; memset(&leaf, 0, sizeof(leaf));
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, node.blocks[idx]*BLOCK_SIZE, SEEK_SET); fread(&leaf, BLOCK_SIZE, 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
            if (leaf.entries[j].inode == 0) {
                leaf.entries[j].inode = new_inode; leaf.entries[j].hash = hash;
                leaf.entries[j].name_len = name_len; strncpy(leaf.entries[j].name, name, MAX_NAME-1);
                delayed_write(rb, (uint8_t*)&node); delayed_write(node.blocks[idx], (uint8_t*)&leaf); delayed_flush();
                goto done;
            }
        unlock_dir(dir_inode); return -1;
    }

done:
    dir.size += DIR_ENTRY_SIZE; dir.modified_time = time(NULL);
    write_inode(dir_inode, &dir); unlock_dir(dir_inode); return 0;
}

int64_t resolve_path(const char *path) {
    if (path[0] != '/') return -1;
    if (strcmp(path, "/") == 0) return 1;
    char work[1024]; strcpy(work, path+1);
    int64_t cur = 1;
    for (char *t = strtok(work, "/"); t; t = strtok(NULL, "/"))
        if ((cur = htree_lookup(cur, t)) < 0) return -1;
    return cur;
}

int cmd_write(const char *path, const char *data) {
    int64_t len = strlen(data), blocks = (len+BLOCK_SIZE-1)/BLOCK_SIZE;
    char d[1024], f[256]; strcpy(d, path);
    char *s = strrchr(d, '/');
    if (s == d) { strcpy(f, s+1); strcpy(d, "/"); }
    else if (s) { strcpy(f, s+1); *s = 0; }
    else return -1;

    int64_t parent = resolve_path(d);
    if (parent < 0) return -1;

    int64_t ex = htree_lookup(parent, f);
    if (ex > 0) {
        htree_remove_entry(parent, f);
        inode_t old; read_inode(ex, &old);
        free_all_blocks(&old);
        memset(&old, 0, sizeof(old));
        write_inode(ex, &old);
    }

    int64_t ni = find_free_inode();
    if (ni < 0) return -1;

    inode_t nf; memset(&nf, 0, sizeof(nf));
    nf.inode_num = ni; nf.type = INODE_FILE; nf.size = len;
    nf.owner = pack_ascii("A       ");
    nf.created_time = nf.modified_time = nf.accessed_time = time(NULL);
    nf.link_count = 1;
    for (int i = 0; i < 4; i++) {
        nf.perms[i*5+0] = 1;
        if (i <= 1) { nf.perms[i*5+1]=1; nf.perms[i*5+2]=1; nf.perms[i*5+4]=1; }
    }

    for (int64_t b = 0; b < blocks; b++) {
        int64_t phys = find_free_block();
        if (phys < 0) { free_all_blocks(&nf); return -1; }
        if (extent_add_block(&nf, b, phys) < 0) {
            mark_block(phys, 0);
            free_all_blocks(&nf);
            return -1;
        }
        int64_t chunk = (b < blocks-1) ? BLOCK_SIZE : (len % BLOCK_SIZE ?: BLOCK_SIZE);
        uint8_t buf[BLOCK_SIZE]; memset(buf, 0, BLOCK_SIZE);
        memcpy(buf, data + b*BLOCK_SIZE, chunk);
        delayed_write(phys, buf);
    }

    delayed_flush();
    write_inode(ni, &nf);
    htree_add_entry(parent, f, ni);
    return 0;
}

int cmd_delete(const char *path) {
    char d[1024], f[256]; strcpy(d, path);
    char *s = strrchr(d, '/');
    if (s == d) { strcpy(f, s+1); strcpy(d, "/"); }
    else if (s) { strcpy(f, s+1); *s = 0; }
    else return -1;

    int64_t parent = resolve_path(d);
    if (parent < 0) return -1;

    int64_t ex = htree_lookup(parent, f);
    if (ex <= 0) { printf("Not found\n"); return -1; }

    inode_t victim; read_inode(ex, &victim);
    if (victim.type != INODE_FILE) { printf("Not a file\n"); return -1; }

    if (htree_remove_entry(parent, f) < 0) return -1;
    free_all_blocks(&victim);
    memset(&victim, 0, sizeof(victim));
    write_inode(ex, &victim);
    return 0;
}

int cmd_read(const char *path) {
    int64_t ino = resolve_path(path);
    if (ino < 0) { printf("Not found\n"); return -1; }
    inode_t inode; read_inode(ino, &inode);
    if (inode.type != INODE_FILE) { printf("Not a file\n"); return -1; }

    char *data = malloc(inode.size + 1);
    int64_t rem = inode.size, off = 0;
    for (int64_t b = 0; rem > 0; b++) {
        int64_t phys = extent_get_block(&inode, b);
        if (phys < 0) break;
        uint8_t buf[BLOCK_SIZE];
        pthread_mutex_lock(&tundra_lock);
        fseek(disk, phys*BLOCK_SIZE, SEEK_SET); fread(buf, BLOCK_SIZE, 1, disk);
        pthread_mutex_unlock(&tundra_lock);
        int64_t chunk = rem > BLOCK_SIZE ? BLOCK_SIZE : rem;
        memcpy(data+off, buf, chunk); off += chunk; rem -= chunk;
    }
    data[inode.size] = 0;
    printf("%s", data);
    free(data);
    return 0;
}

int cmd_ls(const char *path) {
    int64_t di = resolve_path(path);
    if (di < 0) { printf("Not found\n"); return -1; }
    inode_t dir; read_inode(di, &dir);
    if (dir.type != INODE_DIR) { printf("Not a directory\n"); return -1; }

    htree_node_t node;
    pthread_mutex_lock(&tundra_lock);
    fseek(disk, dir.extents[0].start_block*BLOCK_SIZE, SEEK_SET); fread(&node, BLOCK_SIZE, 1, disk);
    pthread_mutex_unlock(&tundra_lock);

    int cnt = 0;
    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++)
        if (node.entries[i].inode) {
            inode_t e; read_inode(node.entries[i].inode, &e);
            printf("  %c inode=%-6ld size=%-8ld %s\n", e.type==INODE_DIR?'d':'-', node.entries[i].inode, e.size, node.entries[i].name);
            cnt++;
        }
    if (node.depth > 0)
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++)
            if (node.blocks[i]) {
                htree_leaf_t leaf; memset(&leaf, 0, sizeof(leaf));
                pthread_mutex_lock(&tundra_lock);
                fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, BLOCK_SIZE, 1, disk);
                pthread_mutex_unlock(&tundra_lock);
                for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
                    if (leaf.entries[j].inode) {
                        inode_t e; read_inode(leaf.entries[j].inode, &e);
                        printf("  %c inode=%-6ld size=%-8ld %s\n", e.type==INODE_DIR?'d':'-', leaf.entries[j].inode, e.size, leaf.entries[j].name);
                        cnt++;
                    }
            }
    printf("  %d entries\n", cnt);
    return 0;
}

int cmd_mkdir(const char *path) {
    char d[1024], n[256]; strcpy(d, path);
    char *s = strrchr(d, '/');
    if (s == d) { strcpy(n, s+1); strcpy(d, "/"); }
    else if (s) { strcpy(n, s+1); *s = 0; }
    else return -1;

    int64_t p = resolve_path(d);
    if (p < 0 || htree_lookup(p, n) > 0) return -1;

    int64_t ni = find_free_inode(), nb = find_free_block();
    if (ni < 0 || nb < 0) return -1;

    inode_t nd; memset(&nd, 0, sizeof(nd));
    nd.inode_num = ni; nd.type = INODE_DIR; nd.size = 2*DIR_ENTRY_SIZE;
    nd.owner = pack_ascii("A       ");
    nd.created_time = nd.modified_time = nd.accessed_time = time(NULL);
    nd.link_count = 2;
    nd.extents[0].start_block = nb; nd.extents[0].length = 1; nd.extent_count = 1;
    for (int i = 0; i < 4; i++) {
        nd.perms[i*5+0] = 1;
        if (i <= 1) { nd.perms[i*5+1]=1; nd.perms[i*5+2]=1; nd.perms[i*5+4]=1; }
    }
    write_inode(ni, &nd);

    htree_node_t child; memset(&child, 0, sizeof(child));
    child.count = 2;
    child.entries[0].inode = ni; child.entries[0].name_len = 1; child.entries[0].name[0] = '.';
    child.entries[1].inode = p; child.entries[1].name_len = 2; child.entries[1].name[0] = '.'; child.entries[1].name[1] = '.';
    delayed_write(nb, (uint8_t*)&child); delayed_flush();
    htree_add_entry(p, n, ni);
    return 0;
}

int cmd_snapshot(int argc, char **argv);
int cmd_rollback(int argc, char **argv);
int cmd_snaplist(int argc, char **argv);
int main(int argc, char **argv) {
    for (int i = 0; i < DIR_LOCKS; i++) pthread_mutex_init(&dir_locks[i], NULL);
    if (argc < 3) { printf("tundra_fs <write|read|ls|mkdir|delete> <image> [args]\n"); return 1; }

    disk = fopen(argv[2], "r+b");
    if (!disk) { perror("fopen"); return 1; }

    load_inode_ranges();
    ag_init();
    trim_init();
    fprintf(stderr, "TundraFS: %ld inodes in %d ranges\n", total_inodes_dynamic, num_inode_ranges);

    int ret = 0;
    if (!strcmp(argv[1], "snapshot") && argc > 3) { ret = cmd_snapshot(argc, argv); ag_cleanup(); fclose(disk); return ret; }
    if (!strcmp(argv[1], "rollback") && argc > 3) { ret = cmd_rollback(argc, argv); ag_cleanup(); fclose(disk); return ret; }
    if (!strcmp(argv[1], "snaplist")) { ret = cmd_snaplist(argc, argv); ag_cleanup(); fclose(disk); return ret; }
    if (!strcmp(argv[1], "write") && argc > 4) ret = cmd_write(argv[3], argv[4]);
    else if (!strcmp(argv[1], "read") && argc > 3) ret = cmd_read(argv[3]);
    else if (!strcmp(argv[1], "ls")) ret = cmd_ls(argc > 3 ? argv[3] : "/");
    else if (!strcmp(argv[1], "mkdir") && argc > 3) ret = cmd_mkdir(argv[3]);
    else if (!strcmp(argv[1], "delete") && argc > 3) ret = cmd_delete(argv[3]);

    trim_flush(disk);
    ag_cleanup(); fclose(disk);
    return ret;
}

int cmd_snapshot(int argc, char **argv) {
    if (argc < 4) { printf("Usage: %s snapshot <image> <name>\n", argv[0]); return 1; }
    return cow_create_snapshot(disk, argv[3]);
}

int cmd_rollback(int argc, char **argv) {
    if (argc < 4) { printf("Usage: %s rollback <image> <snapshot_id>\n", argv[0]); return 1; }
    return cow_rollback(disk, atoll(argv[3]));
}

int cmd_snaplist(int argc, char **argv) {
    return cow_list_snapshots(disk);
}
