#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DEBUG

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/uio.h>
#include <linux/version.h>

#include "simple.h"

#define f_dentry f_path.dentry

static DEFINE_MUTEX(simplefs_sb_lock);
static DEFINE_MUTEX(simplefs_inodes_lock);
static DEFINE_MUTEX(simplefs_directory_lock);

static struct kmem_cache *sinode_cachep;

static inline struct simplefs_super_block *SIMPLEFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct simplefs_inode *SIMPLEFS_INODE(struct inode *inode)
{
	return inode->i_private;
}

static void simplefs_sb_sync(struct super_block *vsb)
{
	struct buffer_head *bh;
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

	bh = sb_bread(vsb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);

	bh->b_data = (char *)sb;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

static int simplefs_sb_get_a_free_ino(struct super_block *vsb, uint64_t *ino)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	int i;
	int ret = 0;

	mutex_lock(&simplefs_sb_lock);

	i = find_next_zero_bit((const unsigned long *)&sb->inodes_bitmap,
		SIMPLEFS_MAX_FILESYSTEM_OBJECTS,
		SIMPLEFS_START_INO);
	if (unlikely(i >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS)) {
		pr_err("No more free ino available");
		ret = -ENOSPC;
		goto end;
	}

	*ino = i;

	sb->inodes_bitmap |= BIT_ULL(i);

	simplefs_sb_sync(vsb);

 end:
	mutex_unlock(&simplefs_sb_lock);
	return ret;
}

static void simplefs_inode_add(struct super_block *vsb,
			       struct simplefs_inode *sinode)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	struct buffer_head *bh;
	struct simplefs_inode *inode_iterator;

	mutex_lock(&simplefs_inodes_lock);

	bh = sb_bread(vsb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);

	inode_iterator = (struct simplefs_inode *)bh->b_data;

	mutex_lock(&simplefs_sb_lock);

	inode_iterator += sinode->inode_no;

	memcpy(inode_iterator, sinode, sizeof(struct simplefs_inode));
	sb->inodes_count++;

	mark_buffer_dirty(bh);
	simplefs_sb_sync(vsb);
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_lock);
}

static void simplefs_inode_delete(struct super_block *vsb,
				  struct simplefs_inode *inode)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

	mutex_lock(&simplefs_sb_lock);

	sb->inodes_count--;

	sb->inodes_bitmap &= ~BIT_ULL(inode->inode_no);

	simplefs_sb_sync(vsb);

	mutex_unlock(&simplefs_sb_lock);
}

static int simplefs_sb_get_a_freeblock(struct super_block *vsb, uint64_t *out)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	int free_block;
	int ret = 0;

	mutex_lock(&simplefs_sb_lock);

	free_block = find_next_bit((const unsigned long *)&sb->free_blocks,
		SIMPLEFS_MAX_FILESYSTEM_OBJECTS,
		SIMPLEFS_ROOTDIR_DATABLOCK_NUMBER + 1);

	if (unlikely(free_block >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS)) {
		pr_err("No more free blocks available\n");
		ret = -ENOSPC;
		goto end;
	}

	*out = free_block;

	sb->free_blocks &= ~BIT_ULL(free_block);

	simplefs_sb_sync(vsb);

 end:
	mutex_unlock(&simplefs_sb_lock);
	return ret;
}

static int simplefs_sb_put_a_freeblock(struct super_block *vsb, uint64_t free)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	int ret = 0;

	mutex_lock(&simplefs_sb_lock);

	sb->free_blocks |= BIT_ULL(free);

	simplefs_sb_sync(vsb);

	mutex_unlock(&simplefs_sb_lock);
	return ret;
}

static const char *simplefs_follow_link(struct dentry *dentry, void **cookie)
{
	struct simplefs_inode *inode = SIMPLEFS_INODE(dentry->d_inode);
	struct buffer_head *bh;

	bh = sb_bread(dentry->d_inode->i_sb, inode->data_block_number);
	if (bh) {
		*cookie = bh;
		return (char *)bh->b_data;
	}
	return NULL;
}

static void simplefs_put_link(struct inode *unused, void *cookie)
{
	struct buffer_head *bh = cookie;
	brelse(bh);
}

static const struct inode_operations simplefs_symlink_inode_operations = {
	.readlink = generic_readlink,
	.follow_link = simplefs_follow_link,
	.put_link = simplefs_put_link,
};

static int simplefs_iterate(struct file *filp, struct dir_context *ctx)
{
	loff_t pos;
	struct inode *inode;
	struct super_block *sb;
	struct buffer_head *bh;
	struct simplefs_inode *sinode;
	struct simplefs_dir_record *record;
	int i;

	pos = ctx->pos;
	inode = filp->f_dentry->d_inode;
	sb = inode->i_sb;

	pr_debug("%s: readdir [%s] with pos:%llu\n", __func__,  filp->f_dentry->d_name.name,
		pos);

	if (pos) {
		return 0;
	}

	sinode = SIMPLEFS_INODE(inode);

	if (unlikely(!S_ISDIR(sinode->mode))) {
		pr_err("inode [%llu][%lu] for fs object [%s] not a directory\n",
		       sinode->inode_no, inode->i_ino,
		       filp->f_dentry->d_name.name);
		return -ENOTDIR;
	}

	bh = sb_bread(sb, sinode->data_block_number);
	BUG_ON(!bh);

	record = (struct simplefs_dir_record *)bh->b_data;

	pr_debug("%s: dir [%s] has %llu children\n", __func__,  filp->f_dentry->d_name.name,
		sinode->dir_children_count);

	for (i = 0; i < sinode->dir_children_count; i++, record++) {
		dir_emit(ctx, record->filename, SIMPLEFS_FILENAME_MAXLEN,
			 record->inode_no, DT_UNKNOWN);
		ctx->pos += sizeof(*record);
		pos += sizeof(*record);
	}
	brelse(bh);

	return 0;
}

struct simplefs_inode *inode_no_to_sinode(struct super_block *sb,
					  uint64_t inode_no)
{
	struct simplefs_super_block *sfs_sb = SIMPLEFS_SB(sb);
	struct simplefs_inode *sinode = NULL;
	struct simplefs_inode *inode_buffer = NULL;

	struct buffer_head *bh;

	BUG_ON(inode_no >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS);
	BUG_ON(!(sfs_sb->inodes_bitmap & BIT_ULL(inode_no)));

	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);

	sinode = (struct simplefs_inode *)bh->b_data;

	sinode += inode_no;
	inode_buffer = kmem_cache_alloc(sinode_cachep, GFP_KERNEL);
	memcpy(inode_buffer, sinode, sizeof(*inode_buffer));

	brelse(bh);
	return inode_buffer;
}

static int simplefs_inode_save(struct super_block *sb,
			struct simplefs_inode *sinode)
{
	struct simplefs_inode *inode_iterator;
	struct buffer_head *bh;

	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);

	inode_iterator = (struct simplefs_inode *)bh->b_data;

	memcpy(inode_iterator + sinode->inode_no, sinode, sizeof(*inode_iterator));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	brelse(bh);

	pr_debug("%s: inode [%llu] updated\n", __func__, sinode->inode_no);
	return 0;
}

const struct file_operations simplefs_file_operations = {
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
};

const struct file_operations simplefs_dir_operations = {
	.owner = THIS_MODULE,
	.iterate = simplefs_iterate,
};

static int simplefs_get_block(struct inode *inode, sector_t iblock,
		       struct buffer_head *bh_result, int create)
{
	struct simplefs_inode *si = SIMPLEFS_INODE(inode);
	map_bh(bh_result, inode->i_sb, le32_to_cpu(si->data_block_number));
	bh_result->b_size = SIMPLEFS_DEFAULT_BLOCK_SIZE;

	pr_debug("%s, %ld %lld\n", __func__, inode->i_ino, si->data_block_number);

	return 0;
}

static ssize_t
simplefs_direct_IO(struct kiocb *iocb, struct iov_iter *iter, loff_t offset)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	ssize_t ret;

	ret = blockdev_direct_IO(iocb, inode, iter, offset, simplefs_get_block);
	return ret;
}

static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, simplefs_get_block, wbc);
}

static int simplefs_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, simplefs_get_block);
}

static int simplefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned len, unsigned copied,
			      struct page *page, void *fsdata)
{
	return generic_write_end(file, mapping, pos, len, copied, page, fsdata);
}

static int
simplefs_write_begin(struct file *file, struct address_space *mapping,
		     loff_t pos, unsigned len, unsigned flags,
		     struct page **pagep, void **fsdata)
{
	return block_write_begin(mapping, pos, len, flags, pagep,
				simplefs_get_block);
}

static const struct address_space_operations simplefs_aops = {
	.readpage = simplefs_readpage,
	.writepage = simplefs_writepage,
	.write_begin = simplefs_write_begin,
	.write_end = simplefs_write_end,
	.direct_IO = simplefs_direct_IO,
};

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags);

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl);

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode);
static int simplefs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *dentry);
static int simplefs_symlink(struct inode *dir, struct dentry *dentry,
			    const char *symname);
static int simplefs_unlink(struct inode *dir, struct dentry *dentry);
static int simplefs_rmdir(struct inode *dir, struct dentry *dentry);

static struct inode_operations simplefs_inode_ops = {
	.create = simplefs_create,
	.lookup = simplefs_lookup,
	.mkdir = simplefs_mkdir,
	.rmdir = simplefs_rmdir,
	.link = simplefs_link,
	.symlink = simplefs_symlink,
	.unlink = simplefs_unlink,
};

static int simplefs_create_fs_object(struct inode *dir, struct dentry *dentry,
				     umode_t mode)
{
	struct inode *inode;
	struct simplefs_inode *sinode;
	struct super_block *sb;
	struct simplefs_inode *parent_dir_inode;
	struct buffer_head *bh;
	struct simplefs_dir_record *dir_contents_datablock;
	uint64_t free_ino;
	int ret;

	sb = dir->i_sb;

	ret = simplefs_sb_get_a_free_ino(sb, &free_ino);
	if (ret < 0)
		return ret;

	pr_debug("%s: creating new object, inode:%llu\n", __func__, free_ino);

	if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode))
		return -EINVAL;

	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	inode->i_state = I_NEW;
	inode->i_sb = sb;
	inode->i_op = &simplefs_inode_ops;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = free_ino;

	sinode = kmem_cache_alloc(sinode_cachep, GFP_KERNEL);
	sinode->inode_no = inode->i_ino;
	inode->i_private = sinode;
	sinode->mode = mode;
	sinode->nlink = inode->i_nlink;

	if (S_ISDIR(mode)) {
		pr_debug("%s: creating new dir, inode:%llu\n", __func__, free_ino);
		sinode->dir_children_count = 0;
		inode->i_fop = &simplefs_dir_operations;
	} else if (S_ISREG(mode)) {
		pr_debug("%s: creating new file, inode:%llu\n", __func__, free_ino);
		sinode->file_size = 0;
		inode->i_fop = &simplefs_file_operations;
		inode->i_mapping->a_ops = &simplefs_aops;
	} else if (S_ISLNK(mode)) {
		pr_debug("%s: creating new symlink, inode:%llu\n", __func__, free_ino);
		inode->i_op = &simplefs_symlink_inode_operations;
	}

	/* allocate data block for the new inode */
	ret = simplefs_sb_get_a_freeblock(sb, &sinode->data_block_number);
	if (ret < 0) {
		pr_err("could not get a freeblock\n");
		return ret;
	}

	/* add the new inode to inode table */
	simplefs_inode_add(sb, sinode);

	/* update the parent dir of this new inode */
	mutex_lock(&simplefs_directory_lock);

	parent_dir_inode = SIMPLEFS_INODE(dir);
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	BUG_ON(!bh);

	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;

	dir_contents_datablock += parent_dir_inode->dir_children_count;
	dir_contents_datablock->inode_no = sinode->inode_no;
	strcpy(dir_contents_datablock->filename, dentry->d_name.name);

	dir->i_size += sizeof(struct simplefs_dir_record);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	mutex_lock(&simplefs_inodes_lock);
	parent_dir_inode->dir_children_count++;
	simplefs_inode_save(sb, parent_dir_inode);
	mutex_unlock(&simplefs_inodes_lock);

	mutex_unlock(&simplefs_directory_lock);

	inode_init_owner(inode, dir, mode);
	insert_inode_hash(inode);
	d_instantiate_new(dentry, inode);

	return 0;
}

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode)
{
	return simplefs_create_fs_object(dir, dentry, S_IFDIR | mode);
}

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl)
{
	return simplefs_create_fs_object(dir, dentry, mode);
}

static int simplefs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct simplefs_inode *si = SIMPLEFS_INODE(inode);
	struct simplefs_inode *parent_dir_inode;
	struct inode *parent_inode = d_inode(dentry->d_parent);
	struct super_block *sb;
	struct buffer_head *bh;
	struct simplefs_dir_record *dir_contents_datablock;
	int err;

	inode->i_ctime = CURRENT_TIME_SEC;
	inode_inc_link_count(inode);
	ihold(inode);

	mutex_lock(&simplefs_directory_lock);

	parent_dir_inode = SIMPLEFS_INODE(parent_inode);
	sb = dir->i_sb;
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	BUG_ON(!bh);

	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;
	dir_contents_datablock += parent_dir_inode->dir_children_count;
	dir_contents_datablock->inode_no = inode->i_ino;
	strcpy(dir_contents_datablock->filename, dentry->d_name.name);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	mutex_lock(&simplefs_inodes_lock);


	parent_dir_inode->dir_children_count++;
	parent_inode->i_size += sizeof(struct simplefs_dir_record);
	err = simplefs_inode_save(sb, parent_dir_inode);

	si->nlink = inode->i_nlink;

	err = simplefs_inode_save(sb, si);

	mutex_unlock(&simplefs_inodes_lock);
	mutex_unlock(&simplefs_directory_lock);

	if (!err) {
		d_instantiate(dentry, inode);
		return 0;
	}
	inode_dec_link_count(inode);
	iput(inode);
	return err;
}

static int simplefs_symlink(struct inode *dir, struct dentry *dentry,
			    const char *symname)
{
	struct super_block *sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned l = strlen(symname) + 1;
	struct inode *inode;
	struct simplefs_inode *si;
	struct buffer_head *bh;
	char *buffer;

	if (l > sb->s_blocksize)
		goto out;

	err = simplefs_create_fs_object(dir, dentry, S_IFLNK | S_IRWXUGO);
	if (err < 0)
		goto out;

	inode = dentry->d_inode;
	inode->i_op = &simplefs_symlink_inode_operations;

	si = SIMPLEFS_INODE(inode);
	bh = sb_bread(sb, si->data_block_number);
	buffer = (char *)bh->b_data;
	memcpy(buffer, symname, l);
	inode->i_size = l - 1;
	si->file_size = l - 1;

	simplefs_inode_save(sb, si);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
 out:
	return err;

}

static int simplefs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct simplefs_inode *si = SIMPLEFS_INODE(inode);
	struct simplefs_inode *parent_dir_inode;
	struct buffer_head *bh;
	struct super_block *sb;
	struct simplefs_dir_record *dir_contents_datablock;
	int i;
	int err;

	sb = dir->i_sb;
	parent_dir_inode = SIMPLEFS_INODE(dir);
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	BUG_ON(!bh);

	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;

	for (i = 0; i < parent_dir_inode->dir_children_count; i++) {
		if (!strcmp
		    (dir_contents_datablock->filename, dentry->d_name.name))
			break;
		dir_contents_datablock++;
	}


	for (i = i + 1; i < parent_dir_inode->dir_children_count; i++) {
		memcpy(dir_contents_datablock, dir_contents_datablock + 1,
		       sizeof(struct simplefs_dir_record));
		dir_contents_datablock++;

	}

	parent_dir_inode->dir_children_count--;
	dir->i_size -= sizeof(struct simplefs_dir_record);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	/* update the inode table of parent */
	err = simplefs_inode_save(sb, parent_dir_inode);

	if (err)
		goto out;


	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	si->nlink = inode->i_nlink;
	err = simplefs_inode_save(sb, si);
 out:
	return err;

}

static int simplefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err = -ENOTEMPTY;

	if (!inode->i_size)
		err = simplefs_unlink(dir, dentry);
	return err;
}

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags)
{
	struct simplefs_inode *parent = SIMPLEFS_INODE(parent_inode);
	struct super_block *sb = parent_inode->i_sb;
	struct buffer_head *bh;
	struct simplefs_dir_record *record;
	int i;

	bh = sb_bread(sb, parent->data_block_number);
	BUG_ON(!bh);

	record = (struct simplefs_dir_record *)bh->b_data;

	mutex_lock(&simplefs_inodes_lock);
	mutex_lock(&simplefs_directory_lock);
	for (i = 0; i < parent->dir_children_count; i++, record++) {
		if (!strcmp(record->filename, child_dentry->d_name.name)) {
			struct inode *inode;
			struct simplefs_inode *sinode;

			sinode = inode_no_to_sinode(sb, record->inode_no);

			inode = iget_locked(sb, record->inode_no);
			if (inode->i_state & I_NEW) {
				pr_debug("%s: File [%s] ino [%llu] is existing and new inode is allocated for it\n",
					__func__, child_dentry->d_name.name, record->inode_no);
				sinode =
				    inode_no_to_sinode(sb, record->inode_no);
				inode->i_ino = record->inode_no;
				inode_init_owner(inode, parent_inode,
						 sinode->mode);
				set_nlink(inode, sinode->nlink);
				inode->i_sb = sb;
				inode->i_op = &simplefs_inode_ops;

				if (S_ISDIR(inode->i_mode)) {
					inode->i_fop = &simplefs_dir_operations;
					inode->i_size =
					    sinode->dir_children_count *
					    sizeof(struct simplefs_dir_record);
				} else if (S_ISREG(inode->i_mode)) {
					inode->i_fop =
					    &simplefs_file_operations;
					inode->i_mapping->a_ops =
					    &simplefs_aops;
					inode->i_size = sinode->file_size;
				} else if (S_ISLNK(inode->i_mode)) {
					inode->i_op =
					    &simplefs_symlink_inode_operations;
					inode->i_size = sinode->file_size;
				} else
					pr_err("Unknown inode type. Neither a directory nor a file\n");

				inode->i_atime = inode->i_mtime =
				    inode->i_ctime = CURRENT_TIME;

				inode->i_private = sinode;
				unlock_new_inode(inode);
			} else {
				pr_debug("%s: File [%s] ino [%llu] is existing and inode is also existing\n",
					__func__, child_dentry->d_name.name, record->inode_no);
			}

			mutex_unlock(&simplefs_directory_lock);
			mutex_unlock(&simplefs_inodes_lock);

			d_add(child_dentry, inode);
			return NULL;
		}
	}

	mutex_unlock(&simplefs_directory_lock);
	mutex_unlock(&simplefs_inodes_lock);

	pr_debug("%s: File [%s] is not existing\n", __func__, child_dentry->d_name.name);

	return NULL;
}

static void simplefs_destory_inode(struct inode *inode)
{
	struct simplefs_inode *sinode = SIMPLEFS_INODE(inode);

	if (!inode->i_nlink && !is_bad_inode(inode)) {
		pr_debug("%s: inode (%lu)\n", __func__, inode->i_ino);

		simplefs_sb_put_a_freeblock(inode->i_sb,
					    sinode->data_block_number);
		simplefs_inode_delete(inode->i_sb, sinode);
	}
	kmem_cache_free(sinode_cachep, sinode);
	free_inode_nonrcu(inode);
}

static const struct super_operations simplefs_sops = {
	.destroy_inode = simplefs_destory_inode,
};

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct buffer_head *bh;
	struct simplefs_super_block *sb_disk;
	int ret = -EPERM;

	bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);

	sb_disk = (struct simplefs_super_block *)bh->b_data;

	if (unlikely(sb_disk->magic != SIMPLEFS_MAGIC)) {
		pr_err("error: magic mismatch\n");
		goto release;
	}

	if (unlikely(sb_disk->block_size != SIMPLEFS_DEFAULT_BLOCK_SIZE)) {
		pr_err("simplefs seem to be formatted using a non-standard block size.\n");
		goto release;
	}

	sb->s_magic = SIMPLEFS_MAGIC;
	sb->s_fs_info = sb_disk;
	sb->s_maxbytes = SIMPLEFS_DEFAULT_BLOCK_SIZE;
	sb->s_op = &simplefs_sops;

	root_inode = new_inode(sb);
	root_inode->i_ino = SIMPLEFS_ROOTDIR_INODE_NUMBER;
	inode_init_owner(root_inode, NULL, S_IFDIR);
	root_inode->i_sb = sb;
	root_inode->i_op = &simplefs_inode_ops;
	root_inode->i_fop = &simplefs_dir_operations;
	root_inode->i_mapping->a_ops = &simplefs_aops;
	root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime =
	    CURRENT_TIME;

	root_inode->i_private =
	    inode_no_to_sinode(sb, SIMPLEFS_ROOTDIR_INODE_NUMBER);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto release;
	}

	ret = 0;
 release:
	brelse(bh);

	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	struct dentry *ret;

	ret = mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);

	if (unlikely(IS_ERR(ret)))
		pr_err("Error mounting simplefs\n");
	else
		pr_info("simplefs is mounted on [%s]\n", dev_name);

	return ret;
}

static void simplefs_kill_superblock(struct super_block *sb)
{
	kill_block_super(sb);
}

struct file_system_type simplefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = simplefs_mount,
	.kill_sb = simplefs_kill_superblock,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
	int ret;

	sinode_cachep = kmem_cache_create("sinode_cache",
					     sizeof(struct simplefs_inode),
					     0,
					     (SLAB_RECLAIM_ACCOUNT |
					      SLAB_MEM_SPREAD), NULL);
	if (!sinode_cachep)
		return -ENOMEM;

	ret = register_filesystem(&simplefs_fs_type);
	if (likely(ret == 0))
		pr_info("Sucessfully registered simplefs\n");
	else
		pr_err("Failed to register. Error:[%d]", ret);

	return ret;
}

static void __exit simplefs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&simplefs_fs_type);
	kmem_cache_destroy(sinode_cachep);

	if (ret)
		pr_err("Failed to unregister. Error:[%d]", ret);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("webing");
