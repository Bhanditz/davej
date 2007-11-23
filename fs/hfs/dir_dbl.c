/*
 * linux/fs/hfs/dir_dbl.c
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * This file may be distributed under the terms of the GNU Public License.
 *
 * This file contains the inode_operations and file_operations
 * structures for HFS directories.
 *
 * Based on the minix file system code, (C) 1991, 1992 by Linus Torvalds
 *
 * "XXX" in a comment is a note to myself to consider changing something.
 *
 * In function preconditions the term "valid" applied to a pointer to
 * a structure means that the pointer is non-NULL and the structure it
 * points to has all fields initialized to consistent values.
 */

#include "hfs.h"
#include <linux/hfs_fs_sb.h>
#include <linux/hfs_fs_i.h>
#include <linux/hfs_fs.h>

/*================ Forward declarations ================*/

static int dbl_lookup(struct inode *, struct dentry *);
static int dbl_readdir(struct file *, void *, filldir_t);
static int dbl_create(struct inode *, struct dentry *, int);
static int dbl_mkdir(struct inode *, struct dentry *, int);
static int dbl_mknod(struct inode *, struct dentry *, int, int);
static int dbl_unlink(struct inode *, struct dentry *);
static int dbl_rmdir(struct inode *, struct dentry *);
static int dbl_rename(struct inode *, struct dentry *,
		      struct inode *, struct dentry *);

/*================ Global variables ================*/

#define DOT_LEN			1
#define DOT_DOT_LEN		2
#define ROOTINFO_LEN		8
#define PCNT_ROOTINFO_LEN	9

const struct hfs_name hfs_dbl_reserved1[] = {
	{DOT_LEN,		"."},
	{DOT_DOT_LEN,		".."},
	{0,			""},
};

const struct hfs_name hfs_dbl_reserved2[] = {
	{ROOTINFO_LEN,		"RootInfo"},
	{PCNT_ROOTINFO_LEN,	"%RootInfo"},
	{0,			""},
};

#define DOT		(&hfs_dbl_reserved1[0])
#define DOT_DOT		(&hfs_dbl_reserved1[1])
#define ROOTINFO	(&hfs_dbl_reserved2[0])
#define PCNT_ROOTINFO	(&hfs_dbl_reserved2[1])

static struct file_operations hfs_dbl_dir_operations = {
	NULL,			/* lseek - default */
	hfs_dir_read,		/* read - invalid */
	NULL,			/* write - bad */
	dbl_readdir,		/* readdir */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* mmap - none */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	file_fsync,		/* fsync - default */
        NULL,			/* fasync - default */
        NULL,			/* check_media_change - none */
        NULL			/* revalidate - none */
};

struct inode_operations hfs_dbl_dir_inode_operations = {
	&hfs_dbl_dir_operations,/* default directory file-ops */
	dbl_create,		/* create */
	dbl_lookup,		/* lookup */
	NULL,			/* link */
	dbl_unlink,		/* unlink */
	NULL,			/* symlink */
	dbl_mkdir,		/* mkdir */
	dbl_rmdir,		/* rmdir */
	dbl_mknod,		/* mknod */
	dbl_rename,		/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* readpage */
	NULL,			/* writepage */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL,			/* permission */
	NULL			/* smap */
};


/*================ File-local functions ================*/

/*
 * is_hdr()
 */
static int is_hdr(struct inode *dir, const char *name, int len)
{
	int retval = 0;

	if (name[0] == '%') {
		struct hfs_cat_entry *entry = HFS_I(dir)->entry;
		struct hfs_cat_entry *victim;
		struct hfs_name cname;
		struct hfs_cat_key key;

		hfs_nameout(dir, &cname, name+1, len-1);
		hfs_cat_build_key(entry->cnid, &cname, &key);
		if ((victim = hfs_cat_get(entry->mdb, &key))) {
			hfs_cat_put(victim);
			retval = 1;
		}
	}
	return retval;
}

/*
 * dbl_lookup()
 *
 * This is the lookup() entry in the inode_operations structure for
 * HFS directories in the AppleDouble scheme.  The purpose is to
 * generate the inode corresponding to an entry in a directory, given
 * the inode for the directory and the name (and its length) of the
 * entry.
 */
static int dbl_lookup(struct inode * dir, struct dentry *dentry)
{
	struct hfs_name cname;
	struct hfs_cat_entry *entry;
	struct hfs_cat_key key;
	struct inode *inode = NULL;
	
	if (!dir || !S_ISDIR(dir->i_mode)) {
		goto done;
	}

	entry = HFS_I(dir)->entry;
	
	/* Perform name-mangling */
	hfs_nameout(dir, &cname, dentry->d_name.name, dentry->d_name.len);
 
	/* Check for "." */
	if (hfs_streq(&cname, DOT)) {
		/* this little trick skips the iget and iput */
		d_add(dentry, dir);
		return 0;
	}

	/* Check for "..". */
	if (hfs_streq(&cname, DOT_DOT)) {
		inode = hfs_iget(hfs_cat_parent(entry), HFS_DBL_DIR, dentry);
		goto done;
	}

	/* Check for "%RootInfo" if in the root directory. */
	if ((entry->cnid == htonl(HFS_ROOT_CNID)) &&
	    hfs_streq(&cname, PCNT_ROOTINFO)) {
		++entry->count; /* __hfs_iget() eats one */
		inode = hfs_iget(entry, HFS_DBL_HDR, dentry);
		goto done;
	}

	/* Do an hfs_iget() on the mangled name. */
	hfs_cat_build_key(entry->cnid, &cname, &key);
	inode = hfs_iget(hfs_cat_get(entry->mdb, &key), HFS_DBL_NORM, dentry);

	/* Try as a header if not found and first character is '%' */
	if (!inode && (dentry->d_name.name[0] == '%')) {
		hfs_nameout(dir, &cname, dentry->d_name.name+1,
			    dentry->d_name.len-1);
		hfs_cat_build_key(entry->cnid, &cname, &key);
		inode = hfs_iget(hfs_cat_get(entry->mdb, &key), 
				 HFS_DBL_HDR, dentry);
	}
	
done:
	dentry->d_op = &hfs_dentry_operations;
	d_add(dentry, inode);
	return 0;
}

/*
 * dbl_readdir()
 *
 * This is the readdir() entry in the file_operations structure for
 * HFS directories in the AppleDouble scheme.  The purpose is to
 * enumerate the entries in a directory, given the inode of the
 * directory and a (struct file *), the 'f_pos' field of which
 * indicates the location in the directory.  The (struct file *) is
 * updated so that the next call with the same 'dir' and 'filp'
 * arguments will produce the next directory entry.  The entries are
 * returned in 'dirent', which is "filled-in" by calling filldir().
 * This allows the same readdir() function be used for different
 * formats.  We try to read in as many entries as we can before
 * filldir() refuses to take any more.
 *
 * XXX: In the future it may be a good idea to consider not generating
 * metadata files for covered directories since the data doesn't
 * correspond to the mounted directory.	 However this requires an
 * iget() for every directory which could be considered an excessive
 * amount of overhead.	Since the inode for a mount point is always
 * in-core this is another argument for a call to get an inode if it
 * is in-core or NULL if it is not.
 */
static int dbl_readdir(struct file * filp,
		       void * dirent, filldir_t filldir)
{
	struct hfs_brec brec;
        struct hfs_cat_entry *entry;
	struct inode *dir = filp->f_dentry->d_inode;

	if (!dir || !dir->i_sb || !S_ISDIR(dir->i_mode)) {
		return -EBADF;
	}

        entry = HFS_I(dir)->entry;

	if (filp->f_pos == 0) {
		/* Entry 0 is for "." */
		if (filldir(dirent, DOT->Name, DOT_LEN, 0, dir->i_ino)) {
			return 0;
		}
		filp->f_pos = 1;
	}

	if (filp->f_pos == 1) {
		/* Entry 1 is for ".." */
		if (filldir(dirent, DOT_DOT->Name, DOT_DOT_LEN, 1,
			    hfs_get_hl(entry->key.ParID))) {
			return 0;
		}
		filp->f_pos = 2;
	}

	if (filp->f_pos < (dir->i_size - 1)) {
                hfs_u32 cnid;
                hfs_u8 type;

		if (hfs_cat_open(entry, &brec) ||
		    hfs_cat_next(entry, &brec, (filp->f_pos - 1) >> 1,
				 &cnid, &type)) {
			return 0;
		}

		while (filp->f_pos < (dir->i_size - 1)) {
			unsigned char tmp_name[HFS_NAMEMAX + 1];
			ino_t ino;
			int is_hdr = (filp->f_pos & 1);
			unsigned int len;

			if (is_hdr) {
				ino = ntohl(cnid) | HFS_DBL_HDR;
				tmp_name[0] = '%';
				len = 1 + hfs_namein(dir, tmp_name + 1,
				    &((struct hfs_cat_key *)brec.key)->CName);
			} else {
				if (hfs_cat_next(entry, &brec, 1,
							&cnid, &type)) {
					return 0;
				}
				ino = ntohl(cnid);
				len = hfs_namein(dir, tmp_name,
				    &((struct hfs_cat_key *)brec.key)->CName);
			}

			if (filldir(dirent, tmp_name, len, filp->f_pos, ino)) {
				hfs_cat_close(entry, &brec);
				return 0;
			}
			++filp->f_pos;
		}
		hfs_cat_close(entry, &brec);
	}

	if (filp->f_pos == (dir->i_size - 1)) {
		if (entry->cnid == htonl(HFS_ROOT_CNID)) {
			/* In root dir last entry is for "%RootInfo" */
			if (filldir(dirent, PCNT_ROOTINFO->Name,
				    PCNT_ROOTINFO_LEN, filp->f_pos,
				    ntohl(entry->cnid) | HFS_DBL_HDR)) {
				return 0;
			}
		}
		++filp->f_pos;
	}

	return 0;
}

/*
 * dbl_create()
 *
 * This is the create() entry in the inode_operations structure for
 * AppleDouble directories.  The purpose is to create a new file in
 * a directory and return a corresponding inode, given the inode for
 * the directory and the name (and its length) of the new file.
 */
static int dbl_create(struct inode * dir, struct dentry *dentry,
		      int mode)
{
	int error;

	if (is_hdr(dir, dentry->d_name.name, dentry->d_name.len)) {
		error = -EEXIST;
	} else {
		error = hfs_create(dir, dentry, mode);
	}
	return error;
}

/*
 * dbl_mkdir()
 *
 * This is the mkdir() entry in the inode_operations structure for
 * AppleDouble directories.  The purpose is to create a new directory
 * in a directory, given the inode for the parent directory and the
 * name (and its length) of the new directory.
 */
static int dbl_mkdir(struct inode * parent, struct dentry *dentry,
		     int mode)
{
	int error;

	if (is_hdr(parent, dentry->d_name.name, dentry->d_name.len)) {
		error = -EEXIST;
	} else {
		error = hfs_mkdir(parent, dentry, mode);
	}
	return error;
}

/*
 * dbl_mknod()
 *
 * This is the mknod() entry in the inode_operations structure for
 * regular HFS directories.  The purpose is to create a new entry
 * in a directory, given the inode for the parent directory and the
 * name (and its length) and the mode of the new entry (and the device
 * number if the entry is to be a device special file).
 */
static int dbl_mknod(struct inode *dir, struct dentry *dentry,
		     int mode, int rdev)
{
	int error;

	if (is_hdr(dir, dentry->d_name.name, dentry->d_name.len)) {
		error = -EEXIST;
	} else {
		error = hfs_mknod(dir, dentry, mode, rdev);
	}
	return error;
}

/*
 * dbl_unlink()
 *
 * This is the unlink() entry in the inode_operations structure for
 * AppleDouble directories.  The purpose is to delete an existing
 * file, given the inode for the parent directory and the name
 * (and its length) of the existing file.
 */
static int dbl_unlink(struct inode * dir, struct dentry *dentry)
{
	int error;

	error = hfs_unlink(dir, dentry);
	if ((error == -ENOENT) && is_hdr(dir, dentry->d_name.name,
					 dentry->d_name.len)) {
		error = -EPERM;
	}
	return error;
}

/*
 * dbl_rmdir()
 *
 * This is the rmdir() entry in the inode_operations structure for
 * AppleDouble directories.  The purpose is to delete an existing
 * directory, given the inode for the parent directory and the name
 * (and its length) of the existing directory.
 */
static int dbl_rmdir(struct inode * parent, struct dentry *dentry)
{
	int error;

	error = hfs_rmdir(parent, dentry);
	if ((error == -ENOENT) && is_hdr(parent, dentry->d_name.name,
					 dentry->d_name.len)) {
		error = -ENOTDIR;
	}
	return error;
}

/*
 * dbl_rename()
 *
 * This is the rename() entry in the inode_operations structure for
 * AppleDouble directories.  The purpose is to rename an existing
 * file or directory, given the inode for the current directory and
 * the name (and its length) of the existing file/directory and the
 * inode for the new directory and the name (and its length) of the
 * new file/directory.
 * 
 * XXX: how do we handle must_be_dir?
 */
static int dbl_rename(struct inode *old_dir, struct dentry *old_dentry,
		      struct inode *new_dir, struct dentry *new_dentry)
{
	int error;

	if (is_hdr(new_dir, new_dentry->d_name.name, 
		   new_dentry->d_name.len)) {
		error = -EPERM;
	} else {
		error = hfs_rename(old_dir, old_dentry,
				   new_dir, new_dentry);
		if ((error == -ENOENT) /*&& !must_be_dir*/ &&
		    is_hdr(old_dir, old_dentry->d_name.name, 
			   old_dentry->d_name.len)) {
			error = -EPERM;
		}
	}
	return error;
}


/* due to the dcache caching negative dentries for non-existent files,
 * we need to drop those entries when a file silently gets created.
 * as far as i can tell, the calls that need to do this are the file
 * related calls (create, rename, and mknod). the directory calls
 * should be immune. the relevant calls in dir.c call drop_dentry 
 * upon successful completion. this allocates an array for %name
 * on the first attempt to access it. */
void hfs_dbl_drop_dentry(const ino_t type, struct dentry *dentry)
{
  unsigned char tmp_name[HFS_NAMEMAX + 1];
  struct dentry *de = NULL;

  switch (type) {
  case HFS_DBL_HDR:
   /* given %name, look for name. i don't think this happens. */
   de = hfs_lookup_dentry(dentry->d_name.name + 1, dentry->d_name.len - 1,
			   dentry->d_parent);
    break;
  case HFS_DBL_DATA:
    /* given name, look for %name */
    tmp_name[0] = '%';
    strncpy(tmp_name + 1, dentry->d_name.name, HFS_NAMELEN - 1);
    de = hfs_lookup_dentry(tmp_name, dentry->d_name.len + 1,
			     dentry->d_parent);
  }

  if (de) {
    if (!de->d_inode)
      d_drop(de);
    dput(de);
  }
}