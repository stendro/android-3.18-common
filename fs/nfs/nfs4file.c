/*
 *  linux/fs/nfs/file.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 */
#include <linux/nfs_fs.h>
#include "internal.h"
#include "fscache.h"
#include "pnfs.h"

#ifdef CONFIG_NFS_V4_2
#include "nfs42.h"
#endif

#define NFSDBG_FACILITY		NFSDBG_FILE

static int
nfs4_file_open(struct inode *inode, struct file *filp)
{
	struct nfs_open_context *ctx;
	struct dentry *dentry = filp->f_path.dentry;
	struct dentry *parent = NULL;
	struct inode *dir;
	unsigned openflags = filp->f_flags;
	struct iattr attr;
	int opened = 0;
	int err;

	/*
	 * If no cached dentry exists or if it's negative, NFSv4 handled the
	 * opens in ->lookup() or ->create().
	 *
	 * We only get this far for a cached positive dentry.  We skipped
	 * revalidation, so handle it here by dropping the dentry and returning
	 * -EOPENSTALE.  The VFS will retry the lookup/create/open.
	 */

	dprintk("NFS: open file(%pd2)\n", dentry);

	if ((openflags & O_ACCMODE) == 3)
		return nfs_open(inode, filp);

	/* We can't create new files here */
	openflags &= ~(O_CREAT|O_EXCL);

	parent = dget_parent(dentry);
	dir = parent->d_inode;

	ctx = alloc_nfs_open_context(filp->f_path.dentry, filp->f_mode);
	err = PTR_ERR(ctx);
	if (IS_ERR(ctx))
		goto out;

	attr.ia_valid = ATTR_OPEN;
	if (openflags & O_TRUNC) {
		attr.ia_valid |= ATTR_SIZE;
		attr.ia_size = 0;
		nfs_wb_all(inode);
	}

	inode = NFS_PROTO(dir)->open_context(dir, ctx, openflags, &attr, &opened);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		switch (err) {
		default:
			goto out_put_ctx;
		case -ENOENT:
		case -ESTALE:
		case -EISDIR:
		case -ENOTDIR:
		case -ELOOP:
			goto out_drop;
		}
	}
	if (inode != dentry->d_inode)
		goto out_drop;

	nfs_set_verifier(dentry, nfs_save_change_attribute(dir));
	nfs_file_set_open_context(filp, ctx);
	nfs_fscache_open_file(inode, filp);
	err = 0;

out_put_ctx:
	put_nfs_open_context(ctx);
out:
	dput(parent);
	return err;

out_drop:
	d_drop(dentry);
	err = -EOPENSTALE;
	goto out_put_ctx;
}

static int
nfs4_file_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret;
	struct inode *inode = file_inode(file);

	do {
		ret = filemap_write_and_wait_range(inode->i_mapping, start, end);
		if (ret != 0)
			break;
		mutex_lock(&inode->i_mutex);
		ret = nfs_file_fsync_commit(file, start, end, datasync);
		if (!ret)
			ret = pnfs_layoutcommit_inode(inode, true);
		mutex_unlock(&inode->i_mutex);
		/*
		 * If nfs_file_fsync_commit detected a server reboot, then
		 * resend all dirty pages that might have been covered by
		 * the NFS_CONTEXT_RESEND_WRITES flag
		 */
		start = 0;
		end = LLONG_MAX;
	} while (ret == -EAGAIN);

	return ret;
}

#ifdef CONFIG_NFS_V4_2
static loff_t nfs4_file_llseek(struct file *filep, loff_t offset, int whence)
{
	loff_t ret;

	switch (whence) {
	case SEEK_HOLE:
	case SEEK_DATA:
		ret = nfs42_proc_llseek(filep, offset, whence);
		if (ret != -EOPNOTSUPP)
			return ret;
	default:
		return nfs_file_llseek(filep, offset, whence);
	}
}
#endif /* CONFIG_NFS_V4_2 */

const struct file_operations nfs4_file_operations = {
#ifdef CONFIG_NFS_V4_2
	.llseek		= nfs4_file_llseek,
#else
	.llseek		= nfs_file_llseek,
#endif
	.read		= new_sync_read,
	.write		= new_sync_write,
	.read_iter	= nfs_file_read,
	.write_iter	= nfs_file_write,
	.mmap		= nfs_file_mmap,
	.open		= nfs4_file_open,
	.flush		= nfs_file_flush,
	.release	= nfs_file_release,
	.fsync		= nfs4_file_fsync,
	.lock		= nfs_lock,
	.flock		= nfs_flock,
	.splice_read	= nfs_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.check_flags	= nfs_check_flags,
	.setlease	= simple_nosetlease,
};
