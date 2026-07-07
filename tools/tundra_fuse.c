#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../include/tundra_fs.h"

static FILE *disk = NULL;

int64_t pack_ascii(const char *str) {
    int64_t r = 0;
    for (int i = 0; i < 8 && str[i]; i++) r += (int64_t)(unsigned char)str[i] << (8*(7-i));
    return r;
}

uint32_t htree_hash(const char *name, int len) {
    uint64_t h = HTREE_HASH_SEED;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)name[i]; h *= 0x9E3779B97F4A7C15; }
    return (uint32_t)(h ^ (h >> 32));
}

void read_inode(int64_t num, inode_t *inode) {
    fseek(disk, SUPERBLOCK_BLOCKS*BLOCK_SIZE + num*INODE_SIZE, SEEK_SET);
    fread(inode, sizeof(inode_t), 1, disk);
}

int64_t extent_get_block(inode_t *inode, int64_t lb) {
    for (int i = 0; i < inode->extent_count; i++) {
        extent_t *e = &inode->extents[i];
        if (lb >= e->logical_start && lb < e->logical_start+e->length)
            return e->start_block + (lb - e->logical_start);
    }
    return -1;
}

int64_t htree_lookup(int64_t dir_inode, const char *name) {
    inode_t dir; read_inode(dir_inode, &dir);
    if (dir.type != INODE_DIR) return -1;
    htree_node_t node;
    int64_t rb = dir.extents[0].start_block;
    fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fread(&node, sizeof(node), 1, disk);
    int name_len = strlen(name);
    int cnt = node.count > HTREE_ROOT_ENTRIES ? HTREE_ROOT_ENTRIES : node.count;
    for (int i = 0; i < cnt; i++)
        if (node.entries[i].inode && node.entries[i].name_len == name_len &&
            !strncmp(node.entries[i].name, name, name_len)) return node.entries[i].inode;
    if (node.depth > 0)
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++)
            if (node.blocks[i]) {
                htree_leaf_t leaf;
                fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, BLOCK_SIZE, 1, disk);
                for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
                    if (leaf.entries[j].inode && leaf.entries[j].name_len == name_len &&
                        !strncmp(leaf.entries[j].name, name, name_len)) return leaf.entries[j].inode;
            }
    return -1;
}

int64_t resolve_path(const char *path) {
    if (path[0] != '/') return -1;
    if (strcmp(path, "/") == 0) return 1;
    char work[4096]; strcpy(work, path+1);
    int64_t cur = 1;
    for (char *t = strtok(work, "/"); t; t = strtok(NULL, "/"))
        if ((cur = htree_lookup(cur, t)) < 0) return -1;
    return cur;
}

mode_t perms_to_mode(uint8_t *perms) {
    mode_t m = 0;
    if (perms[1*5+0]) m |= S_IRUSR; if (perms[1*5+1]) m |= S_IWUSR; if (perms[1*5+2]) m |= S_IXUSR;
    if (perms[2*5+0]) m |= S_IRGRP; if (perms[2*5+1]) m |= S_IWGRP; if (perms[2*5+2]) m |= S_IXGRP;
    if (perms[3*5+0]) m |= S_IROTH; if (perms[3*5+1]) m |= S_IWOTH; if (perms[3*5+2]) m |= S_IXOTH;
    return m;
}

void write_inode(int64_t num, inode_t *inode) {
    fseek(disk, SUPERBLOCK_BLOCKS*BLOCK_SIZE + num*INODE_SIZE, SEEK_SET);
    fwrite(inode, sizeof(inode_t), 1, disk); fflush(disk);
}

static uint8_t *bitmap_cache = NULL;
#include <pthread.h>
#include <fcntl.h>

static pthread_mutex_t bitmap_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_image_path[4096];

static volatile int flush_in_progress = 0;
typedef struct { uint8_t *snapshot; } flush_arg_t;

void *bitmap_flush_thread(void *arg) {
    flush_arg_t *fa = (flush_arg_t *)arg;
    int64_t bmp = (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE) * BLOCK_SIZE;
    int fd = open(g_image_path, O_WRONLY);
    if (fd >= 0) {
        pwrite(fd, fa->snapshot, BITMAP_BLOCKS * BLOCK_SIZE, bmp);
        fsync(fd);
        close(fd);
    }
    flush_in_progress = 0;
    free(fa->snapshot);
    free(fa);
    return NULL;
}

void bitmap_flush_async(void) {
    if (flush_in_progress) return;
    flush_in_progress = 1;
    pthread_mutex_lock(&bitmap_mutex);
    flush_arg_t *fa = malloc(sizeof(flush_arg_t));
    fa->snapshot = malloc(BITMAP_BLOCKS * BLOCK_SIZE);
    memcpy(fa->snapshot, bitmap_cache, BITMAP_BLOCKS * BLOCK_SIZE);
    pthread_mutex_unlock(&bitmap_mutex);

    pthread_t tid;
    pthread_create(&tid, NULL, bitmap_flush_thread, fa);
    pthread_detach(tid);
}

void bitmap_load(void) {
    int64_t bmp = (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE) * BLOCK_SIZE;
    bitmap_cache = malloc(BITMAP_BLOCKS * BLOCK_SIZE);
    fseek(disk, bmp, SEEK_SET);
    fread(bitmap_cache, BITMAP_BLOCKS * BLOCK_SIZE, 1, disk);
}

void bitmap_flush(void) {
    int64_t bmp = (SUPERBLOCK_BLOCKS + INODE_BLOCKS_BASE) * BLOCK_SIZE;
    fseek(disk, bmp, SEEK_SET);
    fwrite(bitmap_cache, BITMAP_BLOCKS * BLOCK_SIZE, 1, disk);
    fflush(disk);
    fsync(fileno(disk));
}

int64_t find_free_block(void) {
    static int64_t last_pos = 0;
    for (int64_t i = last_pos; i < BITMAP_BLOCKS * BLOCK_SIZE; i++) {
        if (bitmap_cache[i] != 0xFF) {
            for (int b = 0; b < 8; b++)
                if (!(bitmap_cache[i] & (1<<b))) {
                    bitmap_cache[i] |= (1<<b);
                    last_pos = i;
                    return i*8 + b;
                }
        }
    }
    return -1;
}

int64_t find_free_inode(void) {
    for (int64_t i = 2; i < TOTAL_INODES_BASE; i++) {
        inode_t ino; read_inode(i, &ino);
        if (ino.type == INODE_FREE) return i;
    }
    return -1;
}

int64_t find_free_block_near(int64_t hint) {
    int64_t start_byte = hint > 0 ? hint/8 : 0;
    for (int64_t i = start_byte; i < BITMAP_BLOCKS * BLOCK_SIZE; i++) {
        if (bitmap_cache[i] != 0xFF) {
            for (int b = 0; b < 8; b++)
                if (!(bitmap_cache[i] & (1<<b))) {
                    bitmap_cache[i] |= (1<<b);
                    return i*8 + b;
                }
        }
    }
    return find_free_block();
}
int64_t get_or_alloc_block(inode_t *inode, int64_t lb) {
    int64_t phys = extent_get_block(inode, lb);
    if (phys >= 0) return phys;
    int64_t hint = (inode->extent_count > 0) ? inode->extents[inode->extent_count-1].start_block + inode->extents[inode->extent_count-1].length : 0;
    int64_t newblk = find_free_block_near(hint);
    if (newblk < 0) return -1;
    if (inode->extent_count > 0) {
        extent_t *last = &inode->extents[inode->extent_count - 1];
        if (last->start_block + last->length == newblk &&
            last->logical_start + last->length == lb) {
            last->length++;
            return newblk;
        }
    }
    if (inode->extent_count >= MAX_EXTENTS) return -1;
    extent_t *e = &inode->extents[inode->extent_count];
    e->start_block = newblk; e->length = 1; e->logical_start = lb;
    inode->extent_count++;
    return newblk;
}

int htree_insert(int64_t dir_inode, const char *name, int64_t child_inode) {
    inode_t dir; read_inode(dir_inode, &dir);
    htree_node_t node;
    int64_t rb = dir.extents[0].start_block;
    fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fread(&node, sizeof(node), 1, disk);
    int name_len = strlen(name);

    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++)
        if (!node.entries[i].inode) {
            node.entries[i].inode = child_inode;
            node.entries[i].hash = htree_hash(name, name_len);
            node.entries[i].name_len = name_len;
            memcpy(node.entries[i].name, name, name_len);
            if (node.count <= i) node.count = i+1;
            fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fwrite(&node, sizeof(node), 1, disk); fflush(disk);
            return 0;
        }

    if (node.depth > 0) {
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++) {
            if (node.blocks[i]) {
                htree_leaf_t leaf;
                fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, sizeof(leaf), 1, disk);
                for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
                    if (!leaf.entries[j].inode) {
                        leaf.entries[j].inode = child_inode;
                        leaf.entries[j].hash = htree_hash(name, name_len);
                        leaf.entries[j].name_len = name_len;
                        memcpy(leaf.entries[j].name, name, name_len);
                        fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fwrite(&leaf, sizeof(leaf), 1, disk); fflush(disk);
                        return 0;
                    }
            }
        }
    }

    int64_t newleaf = find_free_block();
    if (newleaf < 0) return -1;
    htree_leaf_t leaf; memset(&leaf, 0, sizeof(leaf));
    leaf.entries[0].inode = child_inode;
    leaf.entries[0].hash = htree_hash(name, name_len);
    leaf.entries[0].name_len = name_len;
    memcpy(leaf.entries[0].name, name, name_len);
    fseek(disk, newleaf*BLOCK_SIZE, SEEK_SET); fwrite(&leaf, sizeof(leaf), 1, disk); fflush(disk);

    for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++)
        if (!node.blocks[i]) {
            node.blocks[i] = newleaf;
            node.depth = 1;
            fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fwrite(&node, sizeof(node), 1, disk); fflush(disk);
            return 0;
        }
    return -1;
}

void free_block(int64_t block) {
    bitmap_cache[block/8] &= ~(1 << (block%8));
}

int htree_remove_entry(int64_t dir_inode, const char *name) {
    inode_t dir; read_inode(dir_inode, &dir);
    htree_node_t node;
    int64_t rb = dir.extents[0].start_block;
    fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fread(&node, sizeof(node), 1, disk);
    int name_len = strlen(name);
    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++)
        if (node.entries[i].inode && node.entries[i].name_len == name_len &&
            !strncmp(node.entries[i].name, name, name_len)) {
            node.entries[i].inode = 0;
            node.entries[i].name_len = 0;
            fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fwrite(&node, sizeof(node), 1, disk); fflush(disk);
            return 0;
        }
    if (node.depth > 0)
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++)
            if (node.blocks[i]) {
                htree_leaf_t leaf;
                fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, sizeof(leaf), 1, disk);
                for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
                    if (leaf.entries[j].inode && leaf.entries[j].name_len == name_len &&
                        !strncmp(leaf.entries[j].name, name, name_len)) {
                        leaf.entries[j].inode = 0;
                        leaf.entries[j].name_len = 0;
                        fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fwrite(&leaf, sizeof(leaf), 1, disk); fflush(disk);
                        return 0;
                    }
            }
    return -1;
}

int htree_is_empty(int64_t dir_inode) {
    inode_t dir; read_inode(dir_inode, &dir);
    htree_node_t node;
    int64_t rb = dir.extents[0].start_block;
    fseek(disk, rb*BLOCK_SIZE, SEEK_SET); fread(&node, sizeof(node), 1, disk);
    for (int i = 0; i < HTREE_ROOT_ENTRIES; i++)
        if (node.entries[i].inode &&
            !(node.entries[i].name_len==1 && node.entries[i].name[0]=='.') &&
            !(node.entries[i].name_len==2 && node.entries[i].name[0]=='.' && node.entries[i].name[1]=='.'))
            return 0;
    return 1;
}

static int tundra_mkdir(const char *path, mode_t mode) {
    (void)mode;
    const char *slash = strrchr(path, '/');
    char dirpath[4096], name[256];
    if (slash == path) { strcpy(dirpath, "/"); }
    else { strncpy(dirpath, path, slash - path); dirpath[slash-path] = 0; }
    strcpy(name, slash + 1);

    int64_t parent_ino = resolve_path(dirpath);
    if (parent_ino < 0) return -ENOENT;

    int64_t new_ino = find_free_inode();
    if (new_ino < 0) return -ENOSPC;

    int64_t new_blk = find_free_block();
    if (new_blk < 0) return -ENOSPC;

    htree_node_t node; memset(&node, 0, sizeof(node));
    node.count = 2;
    node.entries[0].inode = new_ino; node.entries[0].name_len = 1; node.entries[0].name[0] = '.';
    node.entries[1].inode = parent_ino; node.entries[1].name_len = 2; node.entries[1].name[0] = '.'; node.entries[1].name[1] = '.';
    fseek(disk, new_blk*BLOCK_SIZE, SEEK_SET); fwrite(&node, sizeof(node), 1, disk); fflush(disk);

    inode_t inode; memset(&inode, 0, sizeof(inode));
    inode.inode_num = new_ino; inode.type = INODE_DIR; inode.size = BLOCK_SIZE;
    inode.link_count = 2;
    time_t now = time(NULL);
    inode.created_time = inode.modified_time = inode.accessed_time = now;
    inode.perms[1*5+0] = 1; inode.perms[1*5+1] = 1; inode.perms[1*5+2] = 1;
    inode.extents[0].start_block = new_blk; inode.extents[0].length = 1; inode.extents[0].logical_start = 0;
    inode.extent_count = 1;
    write_inode(new_ino, &inode);

    if (htree_insert(parent_ino, name, new_ino) < 0) return -ENOSPC;
    bitmap_flush();
    return 0;
}

static int tundra_unlink(const char *path) {
    int64_t ino = resolve_path(path);
    if (ino < 0) return -ENOENT;
    inode_t inode; read_inode(ino, &inode);
    if (inode.type != INODE_FILE) return -EISDIR;

    for (int e = 0; e < inode.extent_count; e++)
        for (int64_t j = 0; j < inode.extents[e].length; j++)
            free_block(inode.extents[e].start_block + j);

    memset(&inode, 0, sizeof(inode));
    inode.type = INODE_FREE;
    write_inode(ino, &inode);

    const char *slash = strrchr(path, '/');
    char dirpath[4096], name[256];
    if (slash == path) { strcpy(dirpath, "/"); }
    else { strncpy(dirpath, path, slash - path); dirpath[slash-path] = 0; }
    strcpy(name, slash + 1);
    int64_t parent_ino = resolve_path(dirpath);
    htree_remove_entry(parent_ino, name);

    bitmap_flush();
    return 0;
}

static int tundra_rmdir(const char *path) {
    int64_t ino = resolve_path(path);
    if (ino < 0) return -ENOENT;
    inode_t inode; read_inode(ino, &inode);
    if (inode.type != INODE_DIR) return -ENOTDIR;
    if (!htree_is_empty(ino)) return -ENOTEMPTY;

    for (int e = 0; e < inode.extent_count; e++)
        for (int64_t j = 0; j < inode.extents[e].length; j++)
            free_block(inode.extents[e].start_block + j);

    memset(&inode, 0, sizeof(inode));
    inode.type = INODE_FREE;
    write_inode(ino, &inode);

    const char *slash = strrchr(path, '/');
    char dirpath[4096], name[256];
    if (slash == path) { strcpy(dirpath, "/"); }
    else { strncpy(dirpath, path, slash - path); dirpath[slash-path] = 0; }
    strcpy(name, slash + 1);
    int64_t parent_ino = resolve_path(dirpath);
    htree_remove_entry(parent_ino, name);

    bitmap_flush();
    return 0;
}
static int tundra_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)mode; (void)fi;
    const char *slash = strrchr(path, '/');
    char dirpath[4096], name[256];
    if (slash == path) { strcpy(dirpath, "/"); }
    else { strncpy(dirpath, path, slash - path); dirpath[slash-path] = 0; }
    strcpy(name, slash + 1);

    int64_t dir_ino = resolve_path(dirpath);
    if (dir_ino < 0) return -ENOENT;

    int64_t new_ino = find_free_inode();
    if (new_ino < 0) return -ENOSPC;

    inode_t inode; memset(&inode, 0, sizeof(inode));
    inode.inode_num = new_ino; inode.type = INODE_FILE; inode.size = 0;
    inode.link_count = 1;
    time_t now = time(NULL);
    inode.created_time = inode.modified_time = inode.accessed_time = now;
    inode.perms[1*5+0] = 1; inode.perms[1*5+1] = 1; /* rw для owner */
    write_inode(new_ino, &inode);

    if (htree_insert(dir_ino, name, new_ino) < 0) return -ENOSPC;
    return 0;
}

static int tundra_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    int64_t ino = resolve_path(path);
    if (ino < 0) return -ENOENT;
    inode_t inode; read_inode(ino, &inode);
    if (inode.type != INODE_FILE) return -EISDIR;
    size_t remain = size, off = 0;
    int64_t start_blk = offset / BLOCK_SIZE, blk_off = offset % BLOCK_SIZE;
    for (int64_t b = start_blk; remain > 0; b++) {
        int64_t phys = get_or_alloc_block(&inode, b);
        if (phys < 0) break;
        size_t chunk = BLOCK_SIZE - blk_off;
        if (chunk > remain) chunk = remain;
        if (blk_off == 0 && chunk == BLOCK_SIZE) {
            fseek(disk, phys*BLOCK_SIZE, SEEK_SET);
            fwrite(buf+off, BLOCK_SIZE, 1, disk);
        } else {
            uint8_t data[BLOCK_SIZE];
            fseek(disk, phys*BLOCK_SIZE, SEEK_SET); fread(data, BLOCK_SIZE, 1, disk);
            memcpy(data+blk_off, buf+off, chunk);
            fseek(disk, phys*BLOCK_SIZE, SEEK_SET); fwrite(data, BLOCK_SIZE, 1, disk);
        }
        off += chunk; remain -= chunk; blk_off = 0;
    }
    fflush(disk);
    if ((int64_t)(offset + off) > inode.size) inode.size = offset + off;
    static int writes_since_flush = 0;
    if (++writes_since_flush >= 5000) { bitmap_flush_async(); writes_since_flush = 0; }
    inode.modified_time = time(NULL);
    write_inode(ino, &inode);
    return off;
}

static int tundra_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi; memset(stbuf, 0, sizeof(struct stat));
    int64_t ino = resolve_path(path);
    if (ino < 0) return -ENOENT;
    inode_t inode; read_inode(ino, &inode);
    stbuf->st_ino = inode.inode_num;
    stbuf->st_size = inode.size;
    stbuf->st_mode = (inode.type == INODE_DIR) ? (S_IFDIR | perms_to_mode(inode.perms)) : (S_IFREG | perms_to_mode(inode.perms));
    stbuf->st_nlink = inode.link_count;
    stbuf->st_uid = getuid(); stbuf->st_gid = getgid();
    stbuf->st_atime = inode.accessed_time; stbuf->st_mtime = inode.modified_time; stbuf->st_ctime = inode.created_time;
    stbuf->st_blksize = BLOCK_SIZE;
    stbuf->st_blocks = (inode.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    return 0;
}

static int tundra_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    int64_t di = resolve_path(path);
    if (di < 0) return -ENOENT;
    inode_t dir; read_inode(di, &dir);
    if (dir.type != INODE_DIR) return -ENOTDIR;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    htree_node_t node;
    fseek(disk, dir.extents[0].start_block*BLOCK_SIZE, SEEK_SET); fread(&node, sizeof(node), 1, disk);
    for (int i = 0; i < node.count && i < HTREE_ROOT_ENTRIES; i++)
        if (node.entries[i].inode && !(node.entries[i].name_len==1 && node.entries[i].name[0]=='.') &&
            !(node.entries[i].name_len==2 && node.entries[i].name[0]=='.' && node.entries[i].name[1]=='.'))
            filler(buf, node.entries[i].name, NULL, 0, 0);
    if (node.depth > 0)
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++)
            if (node.blocks[i]) {
                htree_leaf_t leaf;
                fseek(disk, node.blocks[i]*BLOCK_SIZE, SEEK_SET); fread(&leaf, BLOCK_SIZE, 1, disk);
                for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++)
                    if (leaf.entries[j].inode &&
                        !(leaf.entries[j].name_len==1 && leaf.entries[j].name[0]=='.') &&
                        !(leaf.entries[j].name_len==2 && leaf.entries[j].name[0]=='.' && leaf.entries[j].name[1]=='.'))
                        filler(buf, leaf.entries[j].name, NULL, 0, 0);
            }
    return 0;
}

static int tundra_open(const char *path, struct fuse_file_info *fi) {
    int64_t ino = resolve_path(path);
    if (ino < 0) return -ENOENT;
    inode_t inode; read_inode(ino, &inode);
    if (inode.type != INODE_FILE) return -EISDIR;
    
    return 0;
}

static int tundra_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    int64_t ino = resolve_path(path);
    if (ino < 0) return -ENOENT;
    inode_t inode; read_inode(ino, &inode);
    if (inode.type != INODE_FILE) return -EISDIR;
    if (offset >= inode.size) return 0;
    if (offset + size > (size_t)inode.size) size = inode.size - offset;
    size_t remain = size, off = 0;
    int64_t start_blk = offset / BLOCK_SIZE, blk_off = offset % BLOCK_SIZE;
    for (int64_t b = start_blk; remain > 0; b++) {
        int64_t phys = extent_get_block(&inode, b);
        if (phys < 0) break;
        uint8_t data[BLOCK_SIZE];
        fseek(disk, phys*BLOCK_SIZE, SEEK_SET); fread(data, BLOCK_SIZE, 1, disk);
        size_t chunk = BLOCK_SIZE - blk_off;
        if (chunk > remain) chunk = remain;
        memcpy(buf+off, data+blk_off, chunk);
        off += chunk; remain -= chunk; blk_off = 0;
    }
    return off;
}

static int tundra_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize = BLOCK_SIZE;
    stbuf->f_frsize = BLOCK_SIZE;
    stbuf->f_blocks = 25600;
    stbuf->f_bfree = 25000;
    stbuf->f_bavail = 25000;
    stbuf->f_files = TOTAL_INODES_BASE;
    stbuf->f_ffree = TOTAL_INODES_BASE - 10;
    stbuf->f_namemax = MAX_NAME;
    return 0;
}

static void *tundra_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 1;
    return NULL;
}

static int tundra_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi;
    fflush(disk);
    return 0;
}
static int tundra_fallocate(const char *path, int mode, off_t offset, off_t length, struct fuse_file_info *fi) {
    return 0;
}
static struct fuse_operations tundra_ops = {
    .init    = tundra_init,
    .getattr = tundra_getattr,
    .readdir = tundra_readdir,
    .open    = tundra_open,
    .read    = tundra_read,
    .statfs  = tundra_statfs,
    .create  = tundra_create,
    .write   = tundra_write,
    .fsync   = tundra_fsync,
    .fallocate = tundra_fallocate,
    .mkdir   = tundra_mkdir,
    .unlink  = tundra_unlink,
    .rmdir   = tundra_rmdir,
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image> <mountpoint> [fuse_opts...]\n", argv[0]);
        return 1;
    }

    char *image_path = argv[1];
    strncpy(g_image_path, image_path, sizeof(g_image_path)-1);
    char *mountpoint = argv[2];

    disk = fopen(image_path, "r+b");
    if (!disk) { perror("fopen"); return 1; }

    int64_t magic;
    fseek(disk, 0, SEEK_SET); fread(&magic, 8, 1, disk);
    if (magic != pack_ascii("TUNDRA")) {
        fprintf(stderr, "Not a TundraFS image\n");
        fclose(disk);
        return 1;
    }
    fclose(disk);
    {
        char fsck_cmd[4096];
        snprintf(fsck_cmd, sizeof(fsck_cmd), "./tundra_fsck %s --fix > /dev/null 2>&1", image_path);
        system(fsck_cmd);
    }
    disk = fopen(image_path, "r+b");
    if (!disk) { perror("fopen"); return 1; }
    bitmap_load();

    printf("TundraFS FUSE: %s -> %s\n", image_path, mountpoint);

    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, argv[0]);
    /* Опции ДО mountpoint */
    for (int i = 3; i < argc; i++)
        fuse_opt_add_arg(&args, argv[i]);
    fuse_opt_add_arg(&args, mountpoint);

    int ret = fuse_main(args.argc, args.argv, &tundra_ops, NULL);
    fuse_opt_free_args(&args);
    bitmap_flush();
    fclose(disk);
    return ret;
}
