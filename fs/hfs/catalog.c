/*
 * linux/fs/hfs/catalog.c
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * This file may be distributed under the terms of the GNU Public License.
 *
 * This file contains the functions related to the catalog B-tree.
 *
 * "XXX" in a comment is a note to myself to consider changing something.
 *
 * Cache code shamelessly stolen from 
 *     linux/fs/inode.c Copyright (C) 1991, 1992  Linus Torvalds
 *
 * In function preconditions the term "valid" applied to a pointer to
 * a structure means that the pointer is non-NULL and the structure it
 * points to has all fields initialized to consistent values.
 *
 * The code in this file initializes some structures by calling
 * memset(&foo, 0, sizeof(foo)).  This produces the desired behavior
 * only due to the non-ANSI assumption that the machine representation
 */

#include "hfs.h"

/*================ Variable-like macros ================*/

#define NUM_FREE_ENTRIES 8

/* Number of hash table slots */
#define CCACHE_NR 128

/* Max number of entries in memory */
#define CCACHE_MAX 1024

/* Number of entries to fit in a single page on an i386 */
#define CCACHE_INC ((PAGE_SIZE - sizeof(void *))/sizeof(struct hfs_cat_entry))

/*================ File-local data types ================*/

/* The catalog record for a file */
typedef struct {
	hfs_byte_t	Flags;		/* Flags such as read-only */
	hfs_byte_t	Typ;		/* file version number = 0 */
	hfs_finfo_t	UsrWds;		/* data used by the Finder */
	hfs_lword_t	FlNum;		/* The CNID */
	hfs_word_t	StBlk;		/* obsolete */
	hfs_lword_t	LgLen;		/* The logical EOF of the data fork*/
	hfs_lword_t	PyLen;		/* The physical EOF of the data fork */
	hfs_word_t	RStBlk;		/* obsolete */
	hfs_lword_t	RLgLen;		/* The logical EOF of the rsrc fork */
	hfs_lword_t	RPyLen;		/* The physical EOF of the rsrc fork */
	hfs_lword_t	CrDat;		/* The creation date */
	hfs_lword_t	MdDat;		/* The modified date */
	hfs_lword_t	BkDat;		/* The last backup date */
	hfs_fxinfo_t	FndrInfo;	/* more data for the Finder */
	hfs_word_t	ClpSize;	/* number of bytes to allocate
					   when extending files */
	hfs_byte_t	ExtRec[12];	/* first extent record
					   for the data fork */
	hfs_byte_t	RExtRec[12];	/* first extent record
					   for the resource fork */
	hfs_lword_t	Resrv;		/* reserved by Apple */
} FIL_REC;

/* the catalog record for a directory */
typedef struct {
	hfs_word_t	Flags;		/* flags */
	hfs_word_t	Val;		/* Valence: number of files and
					   dirs in the directory */
	hfs_lword_t	DirID;		/* The CNID */
	hfs_lword_t	CrDat;		/* The creation date */
	hfs_lword_t	MdDat;		/* The modification date */
	hfs_lword_t	BkDat;		/* The last backup date */
	hfs_dinfo_t	UsrInfo;	/* data used by the Finder */
	hfs_dxinfo_t	FndrInfo;	/* more data used by Finder */
	hfs_byte_t	Resrv[16];	/* reserved by Apple */
} DIR_REC;

/* the catalog record for a thread */
typedef struct {
	hfs_byte_t		Reserv[8];	/* reserved by Apple */
	hfs_lword_t		ParID;		/* CNID of parent directory */
	struct hfs_name		CName;		/* The name of this entry */
} THD_REC;

/* A catalog tree record */
struct hfs_cat_rec {
	hfs_byte_t		cdrType;	/* The type of entry */
	hfs_byte_t		cdrResrv2;	/* padding */
	union {
		FIL_REC fil;
		DIR_REC dir;
		THD_REC thd;
	} u;
};


struct allocation_unit {
	struct allocation_unit *next;
	struct hfs_cat_entry entries[CCACHE_INC];
};

/*================ File-local variables ================*/
 
static LIST_HEAD(entry_in_use);
static LIST_HEAD(entry_dirty); /* all the dirty entries */
static LIST_HEAD(entry_unused);
static struct list_head hash_table[CCACHE_NR];

spinlock_t entry_lock = SPIN_LOCK_UNLOCKED;

static struct {
        int nr_entries;
        int nr_free_entries;
} entries_stat;

static struct allocation_unit *allocation = NULL;

/*================ File-local functions ================*/

/*
 * brec_to_id
 *
 * Get the CNID from a brec
 */
static inline hfs_u32 brec_to_id(struct hfs_brec *brec)
{
	struct hfs_cat_rec *rec = brec->data;

	return hfs_get_nl((rec->cdrType==HFS_CDR_FIL) ?
				rec->u.fil.FlNum : rec->u.dir.DirID);
}

/*
 * hashfn()
 *
 * hash an (struct mdb *) and a (struct hfs_cat_key *) to an integer.
 */
static inline unsigned int hashfn(const struct hfs_mdb *mdb,
				  const struct hfs_cat_key *key)
{
#define LSB(X) (((unsigned char *)(&X))[3])
	return ((unsigned int)LSB(mdb->create_date) ^ 
		(unsigned int)key->ParID[3] ^
		hfs_strhash(&key->CName)) % CCACHE_NR;
#undef LSB
}

/*
 * hash()
 *
 * hash an (struct mdb *) and a (struct hfs_cat_key *)
 * to a pointer to a slot in the hash table.
 */
static inline struct list_head *hash(struct hfs_mdb *mdb,
				     const struct hfs_cat_key *key)
{
	return hash_table + hashfn(mdb, key);
}

static inline void insert_hash(struct hfs_cat_entry *entry)
{
	struct list_head *head = hash(entry->mdb, &entry->key);
	list_add(&entry->hash, head);
}

static inline void remove_hash(struct hfs_cat_entry *entry)
{
	list_del(&entry->hash);
	INIT_LIST_HEAD(&entry->hash);
}

/*
 * wait_on_entry()
 *
 * Sleep until a locked entry is unlocked.
 */
static inline void wait_on_entry(struct hfs_cat_entry * entry)
{
	while ((entry->state & HFS_LOCK)) {
		hfs_sleep_on(&entry->wait);
	}
}

/*
 * lock_entry()
 *
 * Obtain an exclusive lock on an entry.
 */
static void lock_entry(struct hfs_cat_entry * entry)
{
	wait_on_entry(entry);
	spin_lock(&entry_lock);
	entry->state |= HFS_LOCK;
	spin_unlock(&entry_lock);
}

/*
 * lock_entry()
 *
 * Relinquish an exclusive lock on an entry.
 */
static void unlock_entry(struct hfs_cat_entry * entry)
{
	spin_lock(&entry_lock);
	entry->state &= ~HFS_LOCK;
	spin_unlock(&entry_lock);
	hfs_wake_up(&entry->wait);
}

/*
 * clear_entry()
 *
 * Zero all the fields of an entry and place it on the free list.
 */
static void clear_entry(struct hfs_cat_entry * entry)
{
	wait_on_entry(entry);
	/* zero all but the wait queue */
	memset(&entry->wait, 0,
	       sizeof(*entry) - offsetof(struct hfs_cat_entry, wait));
	INIT_LIST_HEAD(&entry->hash);
	INIT_LIST_HEAD(&entry->list);
	INIT_LIST_HEAD(&entry->dirty);
}

/* put entry on mdb dirty list. this only does it if it's on the hash
 * list. we also add it to the global dirty list as well. */
void hfs_cat_mark_dirty(struct hfs_cat_entry *entry)
{
        struct hfs_mdb *mdb = entry->mdb;

	spin_lock(&entry_lock);
	if (!(entry->state & HFS_DIRTY)) {
	        entry->state |= HFS_DIRTY;

		/* Only add valid (ie hashed) entries to the
		 * dirty list */
		if (!list_empty(&entry->hash)) {
		        list_del(&entry->list);
			list_add(&entry->list, &mdb->entry_dirty);
			INIT_LIST_HEAD(&entry->dirty);
			list_add(&entry->dirty, &entry_dirty);
		}
	}
	spin_unlock(&entry_lock);
}

/* prune all entries */
static void dispose_list(struct list_head *head)
{
        struct list_head *next;
	int count = 0;

	next = head->next;
	for (;;) {
		struct list_head * tmp = next;

		next = next->next;
		if (tmp == head)
			break;
		hfs_cat_prune(list_entry(tmp, struct hfs_cat_entry, list));
		count++;
	}
}

/*
 * try_to_free_entries works by getting the underlying 
 * cache system to release entries. it gets called with the entry lock
 * held. 
 *
 * count can be up to 2 due to both a resource and data fork being
 * listed. we can unuse dirty entries as well.
 */
#define CAN_UNUSE(tmp) (((tmp)->count < 3) && ((tmp)->state <= HFS_DIRTY))
static int try_to_free_entries(const int goal) 
{
        struct list_head *tmp, *head = &entry_in_use;
	LIST_HEAD(freeable);
	int found = 0, depth = goal << 1;

	/* try freeing from entry_in_use */
	while ((tmp = head->prev) != head && depth--) {
	        struct hfs_cat_entry *entry = 
		  list_entry(tmp, struct hfs_cat_entry, list);
		list_del(tmp);
		if (CAN_UNUSE(entry)) {
		        list_del(&entry->hash);
			INIT_LIST_HEAD(&entry->hash);
			list_add(tmp, &freeable);
			if (++found < goal)
			       continue;
			break;
		}
		list_add(tmp, head);
	}

	if (found < goal) { /* try freeing from global dirty list */
	        head = &entry_dirty;
		depth = goal << 1;
		while ((tmp = head->prev) != head && depth--) {
		        struct hfs_cat_entry *entry = 
			  list_entry(tmp, struct hfs_cat_entry, dirty);
			list_del(tmp);
			if (CAN_UNUSE(entry)) {
			        list_del(&entry->hash);
				INIT_LIST_HEAD(&entry->hash);
				list_del(&entry->list);
				INIT_LIST_HEAD(&entry->list);
				list_add(&entry->list, &freeable);
				if (++found < goal)
				  continue;
				break;
			}
			list_add(tmp, head);
		}
	}
		
	if (found) {
	        spin_unlock(&entry_lock);
		dispose_list(&freeable);
		spin_lock(&entry_lock);
	}

	return found;
}
  
/* init_once */
static inline void init_once(struct hfs_cat_entry *entry)
{
	init_waitqueue(&entry->wait);
	INIT_LIST_HEAD(&entry->hash);
	INIT_LIST_HEAD(&entry->list);
	INIT_LIST_HEAD(&entry->dirty);
}

/*
 * grow_entries()
 *
 * Try to allocate more entries, adding them to the free list. this returns
 * with the spinlock held if successful 
 */
static struct hfs_cat_entry *grow_entries(struct hfs_mdb *mdb)
{
	struct allocation_unit *tmp;
	struct hfs_cat_entry * entry;
	int i;

	spin_unlock(&entry_lock);
	if ((entries_stat.nr_entries < CCACHE_MAX) &&
	    HFS_NEW(tmp)) {
	        spin_lock(&entry_lock);
		memset(tmp, 0, sizeof(*tmp));
		tmp->next = allocation;
		allocation = tmp;
		entry = tmp->entries;
		for (i = 1; i < CCACHE_INC; i++) {
		        entry++;
			init_once(entry);
		        list_add(&entry->list, &entry_unused);
		}
		init_once(tmp->entries);

		entries_stat.nr_entries += CCACHE_INC;
		entries_stat.nr_free_entries += CCACHE_INC - 1;
		return tmp->entries;
	}

	/* allocation failed. do some pruning and try again */
	spin_lock(&entry_lock);
	try_to_free_entries(entries_stat.nr_entries >> 2);
	{
		struct list_head *tmp = entry_unused.next;
		if (tmp != &entry_unused) {
			entries_stat.nr_free_entries--;
			list_del(tmp);
			entry = list_entry(tmp, struct hfs_cat_entry, list);
			return entry;
		}
	}
	spin_unlock(&entry_lock);

	return NULL;
}

/*
 * __read_entry()
 *
 * Convert a (struct hfs_cat_rec) to a (struct hfs_cat_entry).
 */
static void __read_entry(struct hfs_cat_entry *entry,
			 const struct hfs_cat_rec *cat)
{
	entry->type = cat->cdrType;

	if (cat->cdrType == HFS_CDR_DIR) {
		struct hfs_dir *dir = &entry->u.dir;

		entry->cnid = hfs_get_nl(cat->u.dir.DirID);

		dir->magic = HFS_DIR_MAGIC;
		dir->flags = hfs_get_ns(cat->u.dir.Flags);
		memcpy(&entry->info.dir.dinfo, &cat->u.dir.UsrInfo, 16);
		memcpy(&entry->info.dir.dxinfo, &cat->u.dir.FndrInfo, 16);
		entry->create_date = hfs_get_nl(cat->u.dir.CrDat);
		entry->modify_date = hfs_get_nl(cat->u.dir.MdDat);
		entry->backup_date = hfs_get_nl(cat->u.dir.BkDat);
		dir->dirs = dir->files = 0;
	} else if (cat->cdrType == HFS_CDR_FIL) {
		struct hfs_file *fil = &entry->u.file;

		entry->cnid = hfs_get_nl(cat->u.fil.FlNum);

		fil->magic = HFS_FILE_MAGIC;

		fil->data_fork.fork = HFS_FK_DATA;
		fil->data_fork.entry = entry;
		fil->data_fork.lsize = hfs_get_hl(cat->u.fil.LgLen);
		fil->data_fork.psize = hfs_get_hl(cat->u.fil.PyLen) >>
						     HFS_SECTOR_SIZE_BITS;
		hfs_extent_in(&fil->data_fork, cat->u.fil.ExtRec);

		fil->rsrc_fork.fork = HFS_FK_RSRC;
		fil->rsrc_fork.entry = entry;
		fil->rsrc_fork.lsize = hfs_get_hl(cat->u.fil.RLgLen);
		fil->rsrc_fork.psize = hfs_get_hl(cat->u.fil.RPyLen) >>
						     HFS_SECTOR_SIZE_BITS;
		hfs_extent_in(&fil->rsrc_fork, cat->u.fil.RExtRec);

		memcpy(&entry->info.file.finfo, &cat->u.fil.UsrWds, 16);
		memcpy(&entry->info.file.fxinfo, &cat->u.fil.FndrInfo, 16);

		entry->create_date = hfs_get_nl(cat->u.fil.CrDat);
		entry->modify_date = hfs_get_nl(cat->u.fil.MdDat);
		entry->backup_date = hfs_get_nl(cat->u.fil.BkDat);
		fil->clumpablks = (hfs_get_hs(cat->u.fil.ClpSize)
					/ entry->mdb->alloc_blksz)
						>> HFS_SECTOR_SIZE_BITS;
		fil->flags = cat->u.fil.Flags;
	} else {
		hfs_warn("hfs_fs: entry is neither file nor directory!\n");
	}
}

/*
 * count_dir_entries()
 *
 * Count the number of files and directories in a given directory.
 */
static inline void count_dir_entries(struct hfs_cat_entry *entry,
				     struct hfs_brec *brec)
{
	int error = 0;
	hfs_u32 cnid;
	hfs_u8 type;

	if (!hfs_cat_open(entry, brec)) {
		while (!(error = hfs_cat_next(entry, brec, 1, &cnid, &type))) {
			if (type == HFS_CDR_FIL) {
				++entry->u.dir.files;
			} else if (type == HFS_CDR_DIR) {
				++entry->u.dir.dirs;
			}
		} /* -ENOENT is normal termination */
	}
	if (error != -ENOENT) {
		entry->cnid = 0;
	}
}

/*
 * read_entry()
 *
 * Convert a (struct hfs_brec) to a (struct hfs_cat_entry).
 */
static inline void read_entry(struct hfs_cat_entry *entry,
			      struct hfs_brec *brec)
{
	int need_count;
	struct hfs_cat_rec *rec = brec->data;

	__read_entry(entry, rec);

	need_count = (rec->cdrType == HFS_CDR_DIR) && rec->u.dir.Val;

	hfs_brec_relse(brec, NULL);

	if (need_count) {
		count_dir_entries(entry, brec);
	}
}

/*
 * __write_entry()
 *
 * Convert a (struct hfs_cat_entry) to a (struct hfs_cat_rec).
 */
static void __write_entry(const struct hfs_cat_entry *entry,
			  struct hfs_cat_rec *cat)
{
	if (entry->type == HFS_CDR_DIR) {
		const struct hfs_dir *dir = &entry->u.dir;

		hfs_put_ns(dir->flags,             cat->u.dir.Flags);
		hfs_put_hs(dir->dirs + dir->files, cat->u.dir.Val);
		hfs_put_nl(entry->cnid,            cat->u.dir.DirID);
		hfs_put_nl(entry->create_date,     cat->u.dir.CrDat);
		hfs_put_nl(entry->modify_date,     cat->u.dir.MdDat);
		hfs_put_nl(entry->backup_date,     cat->u.dir.BkDat);
		memcpy(&cat->u.dir.UsrInfo, &entry->info.dir.dinfo, 16);
		memcpy(&cat->u.dir.FndrInfo, &entry->info.dir.dxinfo, 16);
	} else if (entry->type == HFS_CDR_FIL) {
		const struct hfs_file *fil = &entry->u.file;

		cat->u.fil.Flags = fil->flags;
		hfs_put_nl(entry->cnid,            cat->u.fil.FlNum);
		memcpy(&cat->u.fil.UsrWds, &entry->info.file.finfo, 16);
		hfs_put_hl(fil->data_fork.lsize, cat->u.fil.LgLen);
		hfs_put_hl(fil->data_fork.psize << HFS_SECTOR_SIZE_BITS,
 							cat->u.fil.PyLen);
		hfs_put_hl(fil->rsrc_fork.lsize, cat->u.fil.RLgLen);
		hfs_put_hl(fil->rsrc_fork.psize << HFS_SECTOR_SIZE_BITS,
 							cat->u.fil.RPyLen);
		hfs_put_nl(entry->create_date,     cat->u.fil.CrDat);
		hfs_put_nl(entry->modify_date,     cat->u.fil.MdDat);
		hfs_put_nl(entry->backup_date,     cat->u.fil.BkDat);
		memcpy(&cat->u.fil.FndrInfo, &entry->info.file.fxinfo, 16);
		hfs_put_hs((fil->clumpablks * entry->mdb->alloc_blksz)
				<< HFS_SECTOR_SIZE_BITS, cat->u.fil.ClpSize);
		hfs_extent_out(&fil->data_fork, cat->u.fil.ExtRec);
		hfs_extent_out(&fil->rsrc_fork, cat->u.fil.RExtRec);
	} else {
		hfs_warn("__write_entry: invalid entry\n");
	}
}

/*
 * write_entry()
 *
 * Write a modified entry back to the catalog B-tree.
 */
static void write_entry(struct hfs_cat_entry * entry)
{
	struct hfs_brec brec;
	int error;

	if (!(entry->state & HFS_DELETED)) {
		error = hfs_bfind(&brec, entry->mdb->cat_tree,
				  HFS_BKEY(&entry->key), HFS_BFIND_WRITE);
		if (!error) {
			if ((entry->state & HFS_KEYDIRTY)) {
				/* key may have changed case due to a rename */
				entry->state &= ~HFS_KEYDIRTY;
				if (brec.key->KeyLen != entry->key.KeyLen) {
					hfs_warn("hfs_write_entry: key length "
						 "changed!\n");
					error = 1;
				} else {
					memcpy(brec.key, &entry->key,
					       entry->key.KeyLen);
				}
			} else if (entry->cnid != brec_to_id(&brec)) {
				hfs_warn("hfs_write_entry: CNID "
					 "changed unexpectedly!\n");
				error = 1;
			}
			if (!error) {
				__write_entry(entry, brec.data);
			}
			hfs_brec_relse(&brec, NULL);
		}
		if (error) {
			hfs_warn("hfs_write_entry: unable to write "
				 "entry %08x\n", entry->cnid);
		}
	}
}


static struct hfs_cat_entry *find_entry(struct hfs_mdb *mdb,
					const struct hfs_cat_key *key)
{
	struct list_head *tmp, *head = hash(mdb, key);
	struct hfs_cat_entry * entry;

	tmp = head;
	for (;;) {
		tmp = tmp->next;
		entry = NULL;
		if (tmp == head)
			break;
		entry = list_entry(tmp, struct hfs_cat_entry, hash);
		if (entry->mdb != mdb)
			continue;
		if (hfs_cat_compare(&entry->key, key))
			continue;
		entry->count++;
		break;
	}

	return entry;
}


/* be careful. this gets called with the spinlock held. */
static struct hfs_cat_entry *get_new_entry(struct hfs_mdb *mdb,
					   const struct hfs_cat_key *key,
					   const int read)
{
	struct hfs_cat_entry *entry;
	struct list_head *head = hash(mdb, key);
	struct list_head *tmp = entry_unused.next;

	if (tmp != &entry_unused) {
		list_del(tmp);
		entries_stat.nr_free_entries--;
		entry = list_entry(tmp, struct hfs_cat_entry, list);
add_new_entry:
		list_add(&entry->list, &entry_in_use);
		list_add(&entry->hash, head);
		entry->mdb = mdb;
		entry->count = 1;
		memcpy(&entry->key, key, sizeof(*key));
		entry->state = HFS_LOCK;
		spin_unlock(&entry_lock);

		if (read) {
		   struct hfs_brec brec;

		   if (hfs_bfind(&brec, mdb->cat_tree,
				 HFS_BKEY(key), HFS_BFIND_READ_EQ)) {
		        /* uh oh. we failed to read the record */
		        entry->state |= HFS_DELETED;
		        goto read_fail;
		   }

		   read_entry(entry, &brec);
		   
		   /* error */
		   if (!entry->cnid) {
		        goto read_fail;
		   }

		   /* we don't have to acquire a spinlock here or
		    * below for the unlocking bits as we're the first
		    * user of this entry. */
		   entry->state &= ~HFS_LOCK;
		   hfs_wake_up(&entry->wait);
		}

		return entry;
	}

	/*
	 * Uhhuh.. We need to expand. Note that "grow_entries()" will
	 * release the spinlock, but will return with the lock held
	 * again if the allocation succeeded.
	 */
	entry = grow_entries(mdb);
	if (entry) {
		/* We released the lock, so.. */
		struct hfs_cat_entry * old = find_entry(mdb, key);
		if (!old)
			goto add_new_entry;
		list_add(&entry->list, &entry_unused);
		entries_stat.nr_free_entries++;
		spin_unlock(&entry_lock);
		wait_on_entry(old);
		return old;
	}

	return entry;


read_fail:
	remove_hash(entry);
	entry->state &= ~HFS_LOCK;
	hfs_wake_up(&entry->wait);
	hfs_cat_put(entry);
	return NULL;
}

/*
 * get_entry()
 *
 * Try to return an entry for the indicated file or directory.
 * If ('read' == 0) then no attempt will be made to read it from disk
 * and a locked, but uninitialized, entry is returned.
 */
static struct hfs_cat_entry *get_entry(struct hfs_mdb *mdb,
				       const struct hfs_cat_key *key,
				       const int read)
{
	struct hfs_cat_entry * entry;

	spin_lock(&entry_lock);
	if (!entries_stat.nr_free_entries &&
	    (entries_stat.nr_entries >= CCACHE_MAX))
		goto restock;

search:
	entry = find_entry(mdb, key);
	if (!entry) {
	        return get_new_entry(mdb, key, read);
	}
	spin_unlock(&entry_lock);
	wait_on_entry(entry);
	return entry;

restock:
	try_to_free_entries(8);
	goto search;
}

/* 
 * new_cnid()
 *
 * Allocate a CNID to use for a new file or directory.
 */
static inline hfs_u32 new_cnid(struct hfs_mdb *mdb)
{
	/* If the create succeeds then the mdb will get dirtied */
	return htonl(mdb->next_id++);
}

/*
 * update_dir()
 *
 * Update counts, times and dirt on a changed directory
 */
static void update_dir(struct hfs_mdb *mdb, struct hfs_cat_entry *dir,
		       int is_dir, int count)
{
	/* update counts */
	if (is_dir) {
		mdb->dir_count += count;
		dir->u.dir.dirs += count;
		if (dir->cnid == htonl(HFS_ROOT_CNID)) {
			mdb->root_dirs += count;
		}
	} else {
		mdb->file_count += count;
		dir->u.dir.files += count;
		if (dir->cnid == htonl(HFS_ROOT_CNID)) {
			mdb->root_files += count;
		}
	}
	
	/* update times and dirt */
	dir->modify_date = hfs_time();
	hfs_cat_mark_dirty(dir);
}

/*
 * Add a writer to dir, excluding readers.
 */
static inline void start_write(struct hfs_cat_entry *dir)
{
	if (dir->u.dir.readers || dir->u.dir.read_wait) {
		hfs_sleep_on(&dir->u.dir.write_wait);
	}
	++dir->u.dir.writers;
}

/*
 * Add a reader to dir, excluding writers.
 */
static inline void start_read(struct hfs_cat_entry *dir)
{
	if (dir->u.dir.writers || dir->u.dir.write_wait) {
		hfs_sleep_on(&dir->u.dir.read_wait);
	}
	++dir->u.dir.readers;
}

/*
 * Remove a writer from dir, possibly admitting readers.
 */
static inline void end_write(struct hfs_cat_entry *dir)
{
	if (!(--dir->u.dir.writers)) {
		hfs_wake_up(&dir->u.dir.read_wait);
	}
}

/*
 * Remove a reader from dir, possibly admitting writers.
 */
static inline void end_read(struct hfs_cat_entry *dir)
{
	if (!(--dir->u.dir.readers)) {
		hfs_wake_up(&dir->u.dir.write_wait);
	}
}

/*
 * create_entry()
 *
 * Add a new file or directory to the catalog B-tree and
 * return a (struct hfs_cat_entry) for it in '*result'.
 */
static int create_entry(struct hfs_cat_entry *parent, struct hfs_cat_key *key,
			const struct hfs_cat_rec *record, int is_dir,
			hfs_u32 cnid, struct hfs_cat_entry **result)
{
	struct hfs_mdb *mdb = parent->mdb;
	struct hfs_cat_entry *entry;
	struct hfs_cat_key thd_key;
	struct hfs_cat_rec thd_rec;
	int error, has_thread;

	if (result) {
		*result = NULL;
	}

	/* keep readers from getting confused by changing dir size */
	start_write(parent);

	/* create a locked entry in the cache */
	entry = get_entry(mdb, key, 0);
	if (!entry) {
		/* The entry exists but can't be read */
		error = -EIO;
		goto done;
	}

	if (entry->cnid) {
		/* The (unlocked) entry exists in the cache */
		error = -EEXIST;
		goto bail2;
	}

	/* limit directory valence to signed 16-bit integer */
        if ((parent->u.dir.dirs + parent->u.dir.files) >= HFS_MAX_VALENCE) {
		error = -ENOSPC;
		goto bail1;
	}

	has_thread = is_dir || (record->u.fil.Flags & HFS_FIL_THD);

	if (has_thread) {
		/* init some fields for the thread record */
		memset(&thd_rec, 0, sizeof(thd_rec));
		thd_rec.cdrType = is_dir ? HFS_CDR_THD : HFS_CDR_FTH;
		memcpy(&thd_rec.u.thd.ParID, &key->ParID,
		       sizeof(hfs_u32) + sizeof(struct hfs_name));

		/* insert the thread record */
		hfs_cat_build_key(cnid, NULL, &thd_key);
		error = hfs_binsert(mdb->cat_tree, HFS_BKEY(&thd_key),
				    &thd_rec, 2 + sizeof(THD_REC));
		if (error) {
			goto bail1;
		}
	}

	/* insert the record */
	error = hfs_binsert(mdb->cat_tree, HFS_BKEY(key), record,
				is_dir ?  2 + sizeof(DIR_REC) :
					  2 + sizeof(FIL_REC));
	if (error) {
		if (has_thread && (error != -EIO)) {
			/* at least TRY to remove the thread record */
			(void)hfs_bdelete(mdb->cat_tree, HFS_BKEY(&thd_key));
		}
		goto bail1;
	}

	/* update the parent directory */
	update_dir(mdb, parent, is_dir, 1);

	/* complete the cache entry and return success */
	__read_entry(entry, record);
	unlock_entry(entry);
	if (result) {
		*result = entry;
	} else {
		hfs_cat_put(entry);
	}
	goto done;

bail1:
	entry->state |= HFS_DELETED;
	unlock_entry(entry);
bail2:
	hfs_cat_put(entry);
done:
	end_write(parent);
	return error;
}

/*================ Global functions ================*/

/* 
 * hfs_cat_put()
 *
 * Release an entry we aren't using anymore.
 *
 * NOTE: We must be careful any time we sleep on a non-deleted
 * entry that the entry is in a consistent state, since another
 * process may get the entry while we sleep. That is why we
 * 'goto repeat' after each operation that might sleep.
 */
void hfs_cat_put(struct hfs_cat_entry * entry)
{
	if (entry) {
	        wait_on_entry(entry);

		if (!entry->count) {/* just in case */
		  hfs_warn("hfs_cat_put: trying to free free entry: %p\n",
			   entry);
		  return;
		}

		spin_lock(&entry_lock);
		if (!--entry->count) {
repeat:		  
		        if ((entry->state & HFS_DELETED)) {
				if (entry->type == HFS_CDR_FIL) {
				  /* free all extents */
				  entry->u.file.data_fork.lsize = 0;
				  hfs_extent_adj(&entry->u.file.data_fork);
				  entry->u.file.rsrc_fork.lsize = 0;
				  hfs_extent_adj(&entry->u.file.rsrc_fork);
				}
				entry->state = 0;
			} else if (entry->type == HFS_CDR_FIL) {
		                /* clear out any cached extents */
			        if (entry->u.file.data_fork.first.next) {
				  hfs_extent_free(&entry->u.file.data_fork);
				  spin_unlock(&entry_lock);
				  wait_on_entry(entry);
				  spin_lock(&entry_lock);
				  goto repeat;
				}
				if (entry->u.file.rsrc_fork.first.next) {
				  hfs_extent_free(&entry->u.file.rsrc_fork);
				  spin_unlock(&entry_lock);
				  wait_on_entry(entry);
				  spin_lock(&entry_lock);
				  goto repeat;
				}
			}

			/* if we put a dirty entry, write it out. */
			if ((entry->state & HFS_DIRTY)) {
				list_del(&entry->dirty);
				INIT_LIST_HEAD(&entry->dirty);
				spin_unlock(&entry_lock);
				write_entry(entry);
				spin_lock(&entry_lock);
				entry->state &= ~HFS_DIRTY;
				goto repeat;
			}

			list_del(&entry->hash);
			list_del(&entry->list);
			spin_unlock(&entry_lock);
			clear_entry(entry);
			spin_lock(&entry_lock);
			list_add(&entry->list, &entry_unused);
			entries_stat.nr_free_entries++;
		}
		spin_unlock(&entry_lock);
	}
}

/* 
 * hfs_cat_get()
 *
 * Wrapper for get_entry() which always calls with ('read'==1).
 * Used for access to get_entry() from outside this file.
 */
struct hfs_cat_entry *hfs_cat_get(struct hfs_mdb *mdb,
				  const struct hfs_cat_key *key)
{
	return get_entry(mdb, key, 1);
}

/* invalidate all entries for a device */
static void invalidate_list(struct list_head *head, struct hfs_mdb *mdb,
			    struct list_head *dispose)
{
        struct list_head *next;

	next = head->next;
	for (;;) {
	        struct list_head *tmp = next;
		struct hfs_cat_entry * entry;
		
		next = next->next;
		if (tmp == head)
		        break;
		entry = list_entry(tmp, struct hfs_cat_entry, list);
		if (entry->mdb != mdb) {
			continue;
		}
		if (!entry->count) {
		        list_del(&entry->hash);
			INIT_LIST_HEAD(&entry->hash);
			list_del(&entry->dirty);
			INIT_LIST_HEAD(&entry->dirty);
			list_del(&entry->list);
			list_add(&entry->list, dispose);
			continue;
		}
		hfs_warn("hfs_fs: entry %p(%u:%lu) busy on removed device %s.\n",
			 entry, entry->count, entry->state,
			 hfs_mdb_name(entry->mdb->sys_mdb));
	}

}

/* 
 * hfs_cat_invalidate()
 *
 * Called by hfs_mdb_put() to remove all the entries
 * in the cache which are associated with a given MDB.
 */
void hfs_cat_invalidate(struct hfs_mdb *mdb)
{
	LIST_HEAD(throw_away);

	spin_lock(&entry_lock);
	invalidate_list(&entry_in_use, mdb, &throw_away);
	invalidate_list(&mdb->entry_dirty, mdb, &throw_away);
	spin_unlock(&entry_lock);

	dispose_list(&throw_away); 
}

/*
 * hfs_cat_commit()
 *
 * Called by hfs_mdb_commit() to write dirty entries to the disk buffers.
 */
void hfs_cat_commit(struct hfs_mdb *mdb)
{
        struct list_head *tmp, *head = &mdb->entry_dirty;
	struct hfs_cat_entry * entry;

	spin_lock(&entry_lock);
	while ((tmp = head->prev) != head) {
	        entry = list_entry(tmp, struct hfs_cat_entry, list);
		  
		if ((entry->state & HFS_LOCK)) {
		        spin_unlock(&entry_lock);
			wait_on_entry(entry);
			spin_lock(&entry_lock);
		} else {
		       struct list_head *insert = &entry_in_use;

		       if (!entry->count)
			        insert = entry_in_use.prev;
		       /* remove from global dirty list */
		       list_del(&entry->dirty); 
		       INIT_LIST_HEAD(&entry->dirty);

		       /* add to in_use list */
		       list_del(&entry->list);
		       list_add(&entry->list, insert);

		       /* reset DIRTY, set LOCK */
		       entry->state ^= HFS_DIRTY | HFS_LOCK;
		       spin_unlock(&entry_lock);
		       write_entry(entry);
		       spin_lock(&entry_lock);
		       entry->state &= ~HFS_LOCK;
		       hfs_wake_up(&entry->wait);
		}
	}
	spin_unlock(&entry_lock);
}

/*
 * hfs_cat_free()
 *
 * Releases all the memory allocated in grow_entries().
 * Must call hfs_cat_invalidate() on all MDBs before calling this.
 */
void hfs_cat_free(void)
{
	struct allocation_unit *tmp;

	while (allocation) {
		tmp = allocation->next;
		HFS_DELETE(allocation);
		allocation = tmp;
	}
}

/*
 * hfs_cat_compare()
 *
 * Description:
 *   This is the comparison function used for the catalog B-tree.  In
 *   comparing catalog B-tree entries, the parent id is the most
 *   significant field (compared as unsigned ints).  The name field is
 *   the least significant (compared in "Macintosh lexical order",
 *   see hfs_strcmp() in string.c)
 * Input Variable(s):
 *   struct hfs_cat_key *key1: pointer to the first key to compare
 *   struct hfs_cat_key *key2: pointer to the second key to compare
 * Output Variable(s):
 *   NONE
 * Returns:
 *   int: negative if key1<key2, positive if key1>key2, and 0 if key1==key2
 * Preconditions:
 *   key1 and key2 point to "valid" (struct hfs_cat_key)s.
 * Postconditions:
 *   This function has no side-effects
 */
int hfs_cat_compare(const struct hfs_cat_key *key1,
		    const struct hfs_cat_key *key2)
{
	unsigned int parents;
	int retval;

	parents = hfs_get_hl(key1->ParID) - hfs_get_hl(key2->ParID);
	if (parents != 0) {
		retval = (int)parents;
	} else {
		retval = hfs_strcmp(&key1->CName, &key2->CName);
	}
	return retval;
}

/*
 * hfs_cat_build_key()
 *
 * Given the ID of the parent and the name build a search key.
 */
void hfs_cat_build_key(hfs_u32 parent, const struct hfs_name *cname,
		       struct hfs_cat_key *key)
{
	hfs_put_nl(parent, key->ParID);

	if (cname) {
		key->KeyLen = 6 + cname->Len;
		memcpy(&key->CName, cname, sizeof(*cname));
	} else {
		key->KeyLen = 6;
		memset(&key->CName, 0, sizeof(*cname));
	}
}

/*
 * hfs_cat_open()
 *
 * Given a directory on an HFS filesystem get its thread and
 * lock the directory against insertions and deletions.
 * Return 0 on success or an error code on failure.
 */
int hfs_cat_open(struct hfs_cat_entry *dir, struct hfs_brec *brec)
{
	struct hfs_cat_key key;
	int error;

	if (dir->type != HFS_CDR_DIR) {
		return -EINVAL;
	}
	
	/* Block writers */
	start_read(dir);

	/* Find the directory */
	hfs_cat_build_key(dir->cnid, NULL, &key);
	error = hfs_bfind(brec, dir->mdb->cat_tree,
			  HFS_BKEY(&key), HFS_BFIND_READ_EQ);

	if (error) {
		end_read(dir);
	}

	return error;
}

/*
 * hfs_cat_next()
 *
 * Given a catalog brec structure, replace it with the count'th next brec
 * in the same directory.
 * Return an error code if there is a problem, 0 if OK.
 * Note that an error code of -ENOENT means there are no more entries
 * in this directory.
 * The directory is "closed" on an error.
 */
int hfs_cat_next(struct hfs_cat_entry *dir, struct hfs_brec *brec,
		 hfs_u16 count, hfs_u32 *cnid, hfs_u8 *type)
{
	int error;

	if (!dir || !brec) {
		return -EINVAL;
	}

	/* Get the count'th next catalog tree entry */
	error = hfs_bsucc(brec, count);
	if (!error) {
		struct hfs_cat_key *key = (struct hfs_cat_key *)brec->key;
		if (hfs_get_nl(key->ParID) != dir->cnid) {
			hfs_brec_relse(brec, NULL);
			error = -ENOENT;
		}
	}
	if (!error) {
		*type = ((struct hfs_cat_rec *)brec->data)->cdrType;
		*cnid = brec_to_id(brec);
	} else {
		end_read(dir);
	}
	return error;
}

/*
 * hfs_cat_close()
 *
 * Given a catalog brec structure, replace it with the count'th next brec
 * in the same directory.
 * Return an error code if there is a problem, 0 if OK.
 * Note that an error code of -ENOENT means there are no more entries
 * in this directory.
 */
void hfs_cat_close(struct hfs_cat_entry *dir, struct hfs_brec *brec)
{
	if (dir && brec) {
		hfs_brec_relse(brec, NULL);
		end_read(dir);
	}
}

/*
 * hfs_cat_parent()
 *
 * Given a catalog entry, return the entry for its parent.
 * Uses catalog key for the entry to get its parent's ID
 * and then uses the parent's thread record to locate the
 * parent's actual catalog entry.
 */
struct hfs_cat_entry *hfs_cat_parent(struct hfs_cat_entry *entry)
{
	struct hfs_cat_entry *retval = NULL;
	struct hfs_mdb *mdb = entry->mdb;
	struct hfs_brec brec;
	struct hfs_cat_key key;
	int error;

	lock_entry(entry);
	if (!(entry->state & HFS_DELETED)) {
		hfs_cat_build_key(hfs_get_nl(entry->key.ParID), NULL, &key);
		error = hfs_bfind(&brec, mdb->cat_tree,
				  HFS_BKEY(&key), HFS_BFIND_READ_EQ);
		if (!error) {
			/* convert thread record to key */
			struct hfs_cat_rec *rec = brec.data;
			key.KeyLen = 6 + rec->u.thd.CName.Len;
			memcpy(&key.ParID, &rec->u.thd.ParID,
                       	       sizeof(hfs_u32) + sizeof(struct hfs_name));

                	hfs_brec_relse(&brec, NULL);

			retval = hfs_cat_get(mdb, &key);
		}
	}
	unlock_entry(entry);
	return retval;
}
	
/*
 * hfs_cat_create()
 *
 * Create a new file with the indicated name in the indicated directory.
 * The file will have the indicated flags, type and creator.
 * If successful an (struct hfs_cat_entry) is returned in '*result'.
 */
int hfs_cat_create(struct hfs_cat_entry *parent, struct hfs_cat_key *key,
		   hfs_u8 flags, hfs_u32 type, hfs_u32 creator,
		   struct hfs_cat_entry **result)
{
	struct hfs_cat_rec record;
	hfs_u32 id = new_cnid(parent->mdb);
	hfs_u32 mtime = hfs_time();

	/* init some fields for the file record */
	memset(&record, 0, sizeof(record));
	record.cdrType = HFS_CDR_FIL;
	record.u.fil.Flags = flags | HFS_FIL_USED;
	hfs_put_nl(id,      record.u.fil.FlNum);
	hfs_put_nl(mtime,   record.u.fil.CrDat);
	hfs_put_nl(mtime,   record.u.fil.MdDat);
	hfs_put_nl(0,       record.u.fil.BkDat);
	hfs_put_nl(type,    record.u.fil.UsrWds.fdType);
	hfs_put_nl(creator, record.u.fil.UsrWds.fdCreator);

	return create_entry(parent, key, &record, 0, id, result);
}

/*
 * hfs_cat_mkdir()
 *
 * Create a new directory with the indicated name in the indicated directory.
 * If successful an (struct hfs_cat_entry) is returned in '*result'.
 */
int hfs_cat_mkdir(struct hfs_cat_entry *parent, struct hfs_cat_key *key,
		  struct hfs_cat_entry **result)
{
	struct hfs_cat_rec record;
	hfs_u32 id = new_cnid(parent->mdb);
	hfs_u32 mtime = hfs_time();

	/* init some fields for the directory record */
	memset(&record, 0, sizeof(record));
	record.cdrType = HFS_CDR_DIR;
	hfs_put_nl(id,     record.u.dir.DirID);
	hfs_put_nl(mtime, record.u.dir.CrDat);
	hfs_put_nl(mtime, record.u.dir.MdDat);
	hfs_put_nl(0,     record.u.dir.BkDat);
	hfs_put_hs(0xff,  record.u.dir.UsrInfo.frView);

	return create_entry(parent, key, &record, 1, id, result);
}

/*
 * hfs_cat_delete()
 *
 * Delete the indicated file or directory.
 * The associated thread is also removed unless ('with_thread'==0).
 */
int hfs_cat_delete(struct hfs_cat_entry *parent, struct hfs_cat_entry *entry,
		   int with_thread)
{
	struct hfs_cat_key key;
	struct hfs_mdb *mdb = parent->mdb;
	int is_dir, error = 0;

	if (parent->mdb != entry->mdb) {
		return -EINVAL;
	}

	if (entry->type == HFS_CDR_FIL) {
		with_thread = (entry->u.file.flags&HFS_FIL_THD) && with_thread;
		is_dir = 0;
	} else {
		is_dir = 1;
	}

	/* keep readers from getting confused by changing dir size */
	start_write(parent);

	/* don't delete a busy directory */
	if (entry->type == HFS_CDR_DIR) {
		start_read(entry);

		if (entry->u.dir.files || entry->u.dir.dirs) {
			error = -ENOTEMPTY;
		}
	}

	/* try to delete the file or directory */
	if (!error) {
	        lock_entry(entry);
		if ((entry->state & HFS_DELETED)) {
			/* somebody beat us to it */
			error = -ENOENT;
		} else {
			error = hfs_bdelete(mdb->cat_tree,
					    HFS_BKEY(&entry->key));
		}
		unlock_entry(entry);
	}

	if (!error) {
		/* Mark the entry deleted and remove it from the cache */
		entry->state |= HFS_DELETED;
		remove_hash(entry);

		/* try to delete the thread entry if it exists */
		if (with_thread) {
			hfs_cat_build_key(entry->cnid, NULL, &key);
			(void)hfs_bdelete(mdb->cat_tree, HFS_BKEY(&key));
		}

		update_dir(mdb, parent, is_dir, -1);
	}

	if (entry->type == HFS_CDR_DIR) {
		end_read(entry);
	}
	end_write(parent);
	return error;
}

/*
 * hfs_cat_move()
 *
 * Rename a file or directory, possibly to a new directory.
 * If the destination exists it is removed and a
 * (struct hfs_cat_entry) for it is returned in '*result'.
 */
int hfs_cat_move(struct hfs_cat_entry *old_dir, struct hfs_cat_entry *new_dir,
		 struct hfs_cat_entry *entry, struct hfs_cat_key *new_key,
		 struct hfs_cat_entry **removed)
{
	struct hfs_cat_entry *dest;
	struct hfs_mdb *mdb;
	int error = 0;
	int is_dir, has_thread;

	if (removed) {
		*removed = NULL;
	}

	/* sanity checks */
	if (!old_dir || !new_dir) {
		return -EINVAL;
	}
	mdb = old_dir->mdb;
	if (mdb != new_dir->mdb) {
		return -EXDEV;
	}

	/* precompute a few things */
	if (entry->type == HFS_CDR_DIR) {
		is_dir = 1;
		has_thread = 1;
	} else if (entry->type == HFS_CDR_FIL) {
		is_dir = 0;
		has_thread = entry->u.file.flags & HFS_FIL_THD;
	} else {
		return -EINVAL;
	}

	while (mdb->rename_lock) {
		hfs_sleep_on(&mdb->rename_wait);
	}
	mdb->rename_lock = 1;

	/* keep readers from getting confused by changing dir size */
	start_write(new_dir);
	if (old_dir != new_dir) {
		start_write(old_dir);
	}

	/* Don't move a directory inside itself */
	if (is_dir) {
		struct hfs_cat_key thd_key;
		struct hfs_brec brec;

		hfs_u32 id = new_dir->cnid;
		while (id != htonl(HFS_ROOT_CNID)) {
			if (id == entry->cnid) {
				error = -EINVAL;
			} else {
				hfs_cat_build_key(id, NULL, &thd_key);
				error = hfs_bfind(&brec, mdb->cat_tree,
						  HFS_BKEY(&thd_key),
						  HFS_BFIND_READ_EQ);
			}
			if (error) {
				goto done;
			} else {
				struct hfs_cat_rec *rec = brec.data;
				id = hfs_get_nl(rec->u.thd.ParID);
				hfs_brec_relse(&brec, NULL);
			}
		}
	}

restart:
	/* see if the destination exists, getting it if it does */
	dest = hfs_cat_get(mdb, new_key);

	if (!dest) {
		/* destination doesn't exist, so create it */
		struct hfs_cat_rec new_record;

		/* create a locked entry in the cache */
		dest = get_entry(mdb, new_key, 0);
		if (!dest) {
			error = -EIO;
			goto done;
		}
		if (dest->cnid) {
			/* The (unlocked) entry exists in the cache */
			goto have_distinct;
		}

		/* limit directory valence to signed 16-bit integer */
        	if ((new_dir->u.dir.dirs + new_dir->u.dir.files) >=
							HFS_MAX_VALENCE) {
			error = -ENOSPC;
			goto bail3;
		}

		/* build the new record */
		new_record.cdrType = entry->type;
		__write_entry(entry, &new_record);

		/* insert the new record */
		error = hfs_binsert(mdb->cat_tree, HFS_BKEY(new_key),
				    &new_record, is_dir ? 2 + sizeof(DIR_REC) :
							  2 + sizeof(FIL_REC));
		if (error == -EEXIST) {
			dest->state |= HFS_DELETED;
			unlock_entry(dest);
			hfs_cat_put(dest);
			goto restart;
		} else if (error) {
			goto bail3;
		}

		/* update the destination directory */
		update_dir(mdb, new_dir, is_dir, 1);
	} else if (entry != dest) {
have_distinct:
		/* The destination exists and is not same as source */
		lock_entry(dest);
		if ((dest->state & HFS_DELETED)) {
		        unlock_entry(dest);
			hfs_cat_put(dest);
			goto restart;
		}
		if (dest->type != entry->type) {
			/* can't move a file on top
			   of a dir nor vice versa. */
			error = is_dir ? -ENOTDIR : -EISDIR;
		} else if (is_dir && (dest->u.dir.dirs || dest->u.dir.files)) {
			/* directory to replace is not empty */
			error = -ENOTEMPTY;
		}

		if (error) {
			goto bail2;
		}
	} else {
		/* The destination exists but is same as source */
	        --entry->count;
		dest = NULL;
	}

	/* lock the entry */
	lock_entry(entry);
	if ((entry->state & HFS_DELETED)) {
		error = -ENOENT;
		goto bail1;
	}

	if (dest) {
		/* remove the old entry */
		error = hfs_bdelete(mdb->cat_tree, HFS_BKEY(&entry->key));

		if (error) {
			/* We couldn't remove the entry for the
			   original file, so nothing has changed. */
			goto bail1;
		}
		update_dir(mdb, old_dir, is_dir, -1);
	}

	/* update the thread of the dir/file we're moving */
	if (has_thread) {
		struct hfs_cat_key thd_key;
		struct hfs_brec brec;

		hfs_cat_build_key(entry->cnid, NULL, &thd_key);
		error = hfs_bfind(&brec, mdb->cat_tree,
				  HFS_BKEY(&thd_key), HFS_BFIND_WRITE);
		if (error == -ENOENT) {
			if (is_dir) {
				/* directory w/o a thread! */
				error = -EIO;
			} else {
				/* We were lied to! */
				entry->u.file.flags &= ~HFS_FIL_THD;
				hfs_cat_mark_dirty(entry);
			}
		}
		if (!error) {
			struct hfs_cat_rec *rec = brec.data;
			memcpy(&rec->u.thd.ParID, &new_key->ParID,
			       sizeof(hfs_u32) + sizeof(struct hfs_name));
			hfs_brec_relse(&brec, NULL);
		} else if (error == -ENOENT) {
			error = 0;
		} else if (!dest) {
			/* Nothing was changed */
			unlock_entry(entry);
			goto done;
		} else {
			/* Something went seriously wrong.
			   The dir/file has been deleted. */
			/* XXX try some recovery? */
			entry->state |= HFS_DELETED;
			remove_hash(entry);
			goto bail1;
		}
	}

	/* TRY to remove the thread for the pre-existing entry */
	if (dest && dest->cnid &&
	    (is_dir || (dest->u.file.flags & HFS_FIL_THD))) {
		struct hfs_cat_key thd_key;

		hfs_cat_build_key(dest->cnid, NULL, &thd_key);
		(void)hfs_bdelete(mdb->cat_tree, HFS_BKEY(&thd_key));
	}

	/* update directories */
	new_dir->modify_date = hfs_time();
	hfs_cat_mark_dirty(new_dir);

	/* update key */
	remove_hash(entry);
	memcpy(&entry->key, new_key, sizeof(*new_key));
	/* KEYDIRTY as case might differ */
	entry->state |= HFS_KEYDIRTY;
	insert_hash(entry);
	hfs_cat_mark_dirty(entry);
	unlock_entry(entry);

	/* delete any pre-existing or place-holder entry */
	if (dest) {
		dest->state |= HFS_DELETED;
		unlock_entry(dest);
		if (removed && dest->cnid) {
			*removed = dest;
		} else {
			hfs_cat_put(dest);
		}
	}
	goto done;

bail1:
	unlock_entry(entry);
bail2:
	if (dest) {
		if (!dest->cnid) {
			/* TRY to remove the new entry */
			(void)hfs_bdelete(mdb->cat_tree, HFS_BKEY(new_key));
			update_dir(mdb, new_dir, is_dir, -1);
bail3:
			dest->state |= HFS_DELETED;
		}
		unlock_entry(dest);
		hfs_cat_put(dest);
	}
done:
	if (new_dir != old_dir) {
		end_write(old_dir);
	}
	end_write(new_dir);
	mdb->rename_lock = 0;
	hfs_wake_up(&mdb->rename_wait);

	return error;
}

/*
 * Initialize the hash tables
 */
void hfs_cat_init(void)
{
	int i;
	struct list_head *head = hash_table;

        i = CCACHE_NR;
        do {
                INIT_LIST_HEAD(head);
                head++;
                i--;
        } while (i);
}

