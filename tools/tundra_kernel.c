#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TundraFS");

struct tundra_disk_inode {
    __le64 inode_num; __le32 type; uint8_t perms[20]; __le64 size;
    __le64 owner; __le64 created_time; __le64 modified_time; __le64 accessed_time;
    __le64 tier; __le32 link_count; __le32 flags; __le32 extent_count; __le32 reserved2;
    struct { __le64 start_block; __le64 length; __le64 logical_start; } extents[6];
    uint8_t _pad[16];
} __attribute__((packed));

struct tundra_htree_node {
    __le32 depth; __le32 count;
    __le64 blocks[16];
    struct { __le64 inode; __le32 hash; __le16 name_len; char name[248]; } entries[8];
} __attribute__((packed));

struct tundra_inode_info {
    struct inode vfs_inode;
    struct tundra_disk_inode disk_inode;
};

static inline struct tundra_inode_info *TUNDRA_I(struct inode *inode) {
    return container_of(inode, struct tundra_inode_info, vfs_inode);
}

static int tundra_read_folio(struct file *file, struct folio *folio) {
    struct inode *inode = folio->mapping->host;
    struct super_block *sb = inode->i_sb;
    int64_t block = folio_pos(folio) >> sb->s_blocksize_bits;
    struct buffer_head *bh = sb_bread(sb, block);
    if (!bh) return -EIO;
    memcpy(folio_address(folio), bh->b_data, folio_size(folio));
    brelse(bh);
    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;
}

static struct address_space_operations tundra_aops = { .read_folio = tundra_read_folio };

static struct inode *tundra_iget(struct super_block *sb, unsigned long ino) {
    struct inode *inode = iget_locked(sb, ino);
    if (!inode) return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW)) return inode;

    struct tundra_inode_info *ti = TUNDRA_I(inode);
    struct buffer_head *bh = sb_bread(sb, 1 + (ino * 256 / sb->s_blocksize));
    if (!bh) { iget_failed(inode); return ERR_PTR(-EIO); }
    memcpy(&ti->disk_inode, bh->b_data + ((ino * 256) % sb->s_blocksize), 256);
    brelse(bh);

    inode->i_mode = (le32_to_cpu(ti->disk_inode.type) == 2) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    inode->i_size = le64_to_cpu(ti->disk_inode.size);
    inode->i_ino = ino;
    inode_set_mtime(inode, le64_to_cpu(ti->disk_inode.modified_time), 0);
    inode_set_ctime(inode, le64_to_cpu(ti->disk_inode.created_time), 0);
    inode_set_atime(inode, le64_to_cpu(ti->disk_inode.accessed_time), 0);
    inode->i_mapping->a_ops = &tundra_aops;
    unlock_new_inode(inode);
    return inode;
}

static struct dentry *tundra_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    return NULL;
}

static int tundra_iterate(struct file *file, struct dir_context *ctx) {
    struct inode *dir = file_inode(file);
    struct super_block *sb = dir->i_sb;
    struct tundra_inode_info *ti = TUNDRA_I(dir);

    if (!dir_emit_dots(file, ctx)) return 0;

    int64_t root_block = le64_to_cpu(ti->disk_inode.extents[0].start_block);
    struct buffer_head *bh = sb_bread(sb, root_block);
    if (!bh) return -EIO;

    struct tundra_htree_node *node = (struct tundra_htree_node *)bh->b_data;

    for (int i = 0; i < le32_to_cpu(node->count) && i < 8; i++) {
        if (node->entries[i].inode == 0) continue;
        if (node->entries[i].name_len == 1 && node->entries[i].name[0] == '.') continue;
        if (node->entries[i].name_len == 2 && node->entries[i].name[0] == '.' && node->entries[i].name[1] == '.') continue;

        struct inode *inode = tundra_iget(sb, le64_to_cpu(node->entries[i].inode));
        if (IS_ERR(inode)) continue;

        dir_emit(ctx, node->entries[i].name, le16_to_cpu(node->entries[i].name_len),
                 inode->i_ino, inode->i_mode >> 12);
        iput(inode);
    }

    brelse(bh);
    return 0;
}

static struct inode_operations tundra_dir_iops = { .lookup = tundra_lookup };
static struct file_operations tundra_dir_ops = { .iterate_shared = tundra_iterate, .llseek = generic_file_llseek };
static struct super_operations tundra_sops = { .statfs = simple_statfs };

static int tundra_fill_super(struct super_block *sb, void *data, int silent) {
    sb->s_magic = 0x54554E445241;
    sb->s_op = &tundra_sops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = 4096;
    sb->s_blocksize_bits = 12;

    struct inode *root = tundra_iget(sb, 1);
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

static int __init tundra_init(void) { register_filesystem(&tundra_fs_type); return 0; }
static void __exit tundra_exit(void) { unregister_filesystem(&tundra_fs_type); }
module_init(tundra_init); module_exit(tundra_exit);
