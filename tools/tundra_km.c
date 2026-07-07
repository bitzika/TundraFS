#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>
#include <linux/iomap.h>
#include <linux/blkdev.h>

static int tundra_read_raw_block(struct super_block *sb, int64_t phys_block, void *buf, size_t len);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("TundraFS");
MODULE_DESCRIPTION("TundraFS Kernel Driver (iomap)");

#define TUNDRA_MAGIC 0x54554E4452410000ULL
#define MAX_EXTENTS 6
#define HTREE_ROOT_ENTRIES 8
#define HTREE_INDIRECT_ENTRIES 16

struct tundra_extent { __le64 start; __le64 len; __le64 log_start; } __packed;

struct tundra_disk_inode {
    __le64 inode_num; __le32 type; uint8_t perms[20]; __le64 size;
    __le64 owner; __le64 ctime; __le64 mtime; __le64 atime;
    __le64 tier; __le32 link_count; __le32 flags; __le32 extent_count; __le32 rsvd;
    struct tundra_extent extents[MAX_EXTENTS];
    uint8_t _pad[16];
} __packed;

struct tundra_htree_entry { __le64 inode; __le32 hash; __le16 name_len; char name[248]; } __packed;
struct tundra_htree_node {
    __le32 depth; __le32 count; __le64 blocks[HTREE_INDIRECT_ENTRIES];
    struct tundra_htree_entry entries[HTREE_ROOT_ENTRIES];
} __packed;

struct tundra_inode_info {
    struct inode vfs_inode;
    struct tundra_disk_inode raw;
};

static inline struct tundra_inode_info *TUNDRA_I(struct inode *inode) {
    return container_of(inode, struct tundra_inode_info, vfs_inode);
}

/* iomap: чтение страницы */
static int tundra_iomap_read_folio(struct file *file, struct folio *folio) {
    struct inode *inode = folio->mapping->host;
    struct tundra_inode_info *ti = TUNDRA_I(inode);
    struct super_block *sb = inode->i_sb;
    loff_t pos = folio_pos(folio);
    size_t count = folio_size(folio);
    void *buf = folio_address(folio);

    int64_t logical_block = pos >> sb->s_blocksize_bits;
    int64_t block_offset = pos & (sb->s_blocksize - 1);

    int64_t phys = -1;
    for (int i = 0; i < le32_to_cpu(ti->raw.extent_count); i++) {
        int64_t start = le64_to_cpu(ti->raw.extents[i].log_start);
        int64_t len = le64_to_cpu(ti->raw.extents[i].len);
        if (logical_block >= start && logical_block < start + len) {
            phys = le64_to_cpu(ti->raw.extents[i].start) + (logical_block - start);
            break;
        }
    }
    if (phys < 0) {
        folio_zero_range(folio, 0, count);
        folio_mark_uptodate(folio);
        folio_unlock(folio);
        return 0;
    }

    /* Прямое чтение через submit_bio */
    struct block_device *bdev = sb->s_bdev;
    struct bio *bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_NOFS);
    if (!bio) {
        mapping_set_error(folio->mapping, -EIO);
        folio_unlock(folio);
        return -ENOMEM;
    }

    bio->bi_iter.bi_sector = phys * (sb->s_blocksize >> 9);
    __bio_add_page(bio, &folio->page, count, block_offset);

    int err = submit_bio_wait(bio);
    bio_put(bio);

    if (err) {
        mapping_set_error(folio->mapping, -EIO);
        folio_unlock(folio);
        return err;
    }

    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;
}

static struct address_space_operations tundra_aops = {
    .read_folio = tundra_iomap_read_folio,
};

/* HTree lookup */
static int64_t tundra_htree_lookup(struct inode *dir, const char *name, int name_len) {
    struct tundra_inode_info *ti = TUNDRA_I(dir);
    struct super_block *sb = dir->i_sb;
    if (le32_to_cpu(ti->raw.type) != 2) return -1;
    int64_t root_block = le64_to_cpu(ti->raw.extents[0].start);
    struct tundra_htree_node *node = kmalloc(sb->s_blocksize, GFP_NOFS);
    if (!node) return -1;
    if (tundra_read_raw_block(sb, root_block, node, sb->s_blocksize)) { kfree(node); return -1; }

    int64_t result = -1;
    int cnt = le32_to_cpu(node->count);
    if (cnt > HTREE_ROOT_ENTRIES) cnt = HTREE_ROOT_ENTRIES;
    for (int i = 0; i < cnt; i++) {
        if (node->entries[i].inode && le16_to_cpu(node->entries[i].name_len) == name_len &&
            !strncmp(node->entries[i].name, name, name_len)) {
            result = le64_to_cpu(node->entries[i].inode);
            goto out;
        }
    }
    if (le32_to_cpu(node->depth) > 0) {
        struct tundra_htree_entry *entries = kmalloc(sb->s_blocksize, GFP_NOFS);
        if (!entries) goto out;
        for (int i = 0; i < HTREE_INDIRECT_ENTRIES; i++) {
            if (!node->blocks[i]) continue;
            if (tundra_read_raw_block(sb, le64_to_cpu(node->blocks[i]), entries, sb->s_blocksize)) continue;
            for (int j = 0; j < HTREE_INDIRECT_ENTRIES; j++) {
                if (entries[j].inode && le16_to_cpu(entries[j].name_len) == name_len &&
                    !strncmp(entries[j].name, name, name_len)) {
                    result = le64_to_cpu(entries[j].inode);
                    kfree(entries);
                    goto out;
                }
            }
        }
        kfree(entries);
    }
out:
    kfree(node);
    return result;
}

static int tundra_read_raw_block(struct super_block *sb, int64_t phys_block, void *buf, size_t len) {
    struct block_device *bdev = sb->s_bdev;
    struct bio *bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_NOFS);
    if (!bio) return -ENOMEM;
    struct page *page = alloc_page(GFP_NOFS);
    if (!page) { bio_put(bio); return -ENOMEM; }
    bio->bi_iter.bi_sector = phys_block * (sb->s_blocksize >> 9);
    __bio_add_page(bio, page, sb->s_blocksize, 0);
    int err = submit_bio_wait(bio);
    bio_put(bio);
    if (err) { __free_page(page); return err; }
    memcpy(buf, page_address(page) + 0, len);
    __free_page(page);
    return 0;
}
static int tundra_read_inode(struct inode *inode) {
    struct super_block *sb = inode->i_sb;
    struct tundra_inode_info *ti = TUNDRA_I(inode);
    int64_t inode_block = 1 + (inode->i_ino * 256) / 4096;
    int offset = (inode->i_ino * 256) % 4096;
    char *buf = kmalloc(sb->s_blocksize, GFP_NOFS);
    if (!buf) return -ENOMEM;
    int err = tundra_read_raw_block(sb, inode_block, buf, sb->s_blocksize);
    if (err) { kfree(buf); return err; }
    memcpy(&ti->raw, buf + offset, sizeof(ti->raw));
    kfree(buf);
    inode->i_mode = (le32_to_cpu(ti->raw.type) == 2) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    inode->i_size = le64_to_cpu(ti->raw.size);
    inode_set_mtime(inode, le64_to_cpu(ti->raw.mtime), 0);
    inode_set_atime(inode, le64_to_cpu(ti->raw.atime), 0);
    inode_set_ctime(inode, le64_to_cpu(ti->raw.ctime), 0);
    return 0;
}

static struct inode *tundra_iget(struct super_block *sb, unsigned long ino) {
    struct inode *inode = iget_locked(sb, ino);
    if (!inode) return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW)) return inode;
    inode->i_mapping->a_ops = &tundra_aops;
    if (tundra_read_inode(inode)) { iget_failed(inode); return ERR_PTR(-EIO); }
    unlock_new_inode(inode);
    return inode;
}

static struct dentry *tundra_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    int64_t ino = tundra_htree_lookup(dir, dentry->d_name.name, dentry->d_name.len);
    if (ino < 0) return NULL;
    struct inode *inode = tundra_iget(dir->i_sb, ino);
    if (IS_ERR(inode)) return ERR_CAST(inode);
    return d_splice_alias(inode, dentry);
}

static int tundra_iterate(struct file *file, struct dir_context *ctx) {
    struct inode *dir = file_inode(file);
    struct tundra_inode_info *ti = TUNDRA_I(dir);
    struct super_block *sb = dir->i_sb;
    if (!dir_emit_dots(file, ctx)) return 0;

    int64_t root_block = le64_to_cpu(ti->raw.extents[0].start);
    pr_info("tundra: iterate root_block=%lld extent_count=%d size=%lld\n", root_block, ti->raw.extent_count, le64_to_cpu(ti->raw.size));
    struct tundra_htree_node *node = kmalloc(sb->s_blocksize, GFP_NOFS);
    if (!node) return -ENOMEM;
    int err = tundra_read_raw_block(sb, root_block, node, sb->s_blocksize);
    if (err) { kfree(node); return err; }

    for (int i = 0; i < le32_to_cpu(node->count) && i < HTREE_ROOT_ENTRIES; i++) {
        if (!node->entries[i].inode) continue;
        if (node->entries[i].name_len == 1 && node->entries[i].name[0] == '.') continue;
        if (node->entries[i].name_len == 2 && node->entries[i].name[0] == '.' && node->entries[i].name[1] == '.') continue;

        struct inode *inode = tundra_iget(sb, le64_to_cpu(node->entries[i].inode));
        if (!IS_ERR(inode)) {
            dir_emit(ctx, node->entries[i].name, le16_to_cpu(node->entries[i].name_len),
                     inode->i_ino, inode->i_mode >> 12);
            iput(inode);
        }
    }
    kfree(node);
    return 0;
}

static struct inode_operations tundra_dir_iops = { .lookup = tundra_lookup };
static struct file_operations tundra_dir_ops = { .iterate_shared = tundra_iterate, .llseek = generic_file_llseek };
static struct super_operations tundra_sops = { .statfs = simple_statfs };

static int tundra_fill_super(struct super_block *sb, void *data, int silent) {
    sb->s_magic = TUNDRA_MAGIC;
    sb->s_op = &tundra_sops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = 4096;
    sb->s_blocksize_bits = 12;

    struct inode *root = tundra_iget(sb, 1);
    if (!IS_ERR(root)) pr_info("tundra: root inode mode=0x%x ino=%lu\n", root->i_mode, root->i_ino); else pr_info("tundra: tundra_iget failed err=%ld\n", PTR_ERR(root));
    if (IS_ERR(root)) return PTR_ERR(root);
    root->i_op = &tundra_dir_iops;
    root->i_fop = &tundra_dir_ops;
    sb->s_root = d_make_root(root);
    if (!sb->s_root) { iput(root); return -ENOMEM; }
    return 0;
}

static struct dentry *tundra_mount(struct file_system_type *fs_type, int flags, const char *dev, void *data) {
    return mount_bdev(fs_type, flags, dev, data, tundra_fill_super);
}

static struct file_system_type tundra_fs_type = {
    .owner = THIS_MODULE, .name = "tundrafs", .mount = tundra_mount, .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init tundra_init(void) { return register_filesystem(&tundra_fs_type); }
static void __exit tundra_exit(void) { unregister_filesystem(&tundra_fs_type); }
module_init(tundra_init); module_exit(tundra_exit);
