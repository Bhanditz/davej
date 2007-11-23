/*
 *  linux/mm/swapfile.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 */

#include <linux/malloc.h>
#include <linux/smp_lock.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/blkdev.h> /* for blk_size */
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/shm.h>

#include <asm/pgtable.h>

unsigned int nr_swapfiles = 0;

struct swap_list_t swap_list = {-1, -1};

struct swap_info_struct swap_info[MAX_SWAPFILES];

#define SWAPFILE_CLUSTER 256

static inline int scan_swap_map(struct swap_info_struct *si)
{
	unsigned long offset;
	/* 
	 * We try to cluster swap pages by allocating them
	 * sequentially in swap.  Once we've allocated
	 * SWAPFILE_CLUSTER pages this way, however, we resort to
	 * first-free allocation, starting a new cluster.  This
	 * prevents us from scattering swap pages all over the entire
	 * swap partition, so that we reduce overall disk seek times
	 * between swap pages.  -- sct */
	if (si->cluster_nr) {
		while (si->cluster_next <= si->highest_bit) {
			offset = si->cluster_next++;
			if (si->swap_map[offset])
				continue;
			si->cluster_nr--;
			goto got_page;
		}
	}
	si->cluster_nr = SWAPFILE_CLUSTER;

	/* try to find an empty (even not aligned) cluster. */
	offset = si->lowest_bit;
 check_next_cluster:
	if (offset+SWAPFILE_CLUSTER-1 <= si->highest_bit)
	{
		int nr;
		for (nr = offset; nr < offset+SWAPFILE_CLUSTER; nr++)
			if (si->swap_map[nr])
			{
				offset = nr+1;
				goto check_next_cluster;
			}
		/* We found a completly empty cluster, so start
		 * using it.
		 */
		goto got_page;
	}
	/* No luck, so now go finegrined as usual. -Andrea */
	for (offset = si->lowest_bit; offset <= si->highest_bit ; offset++) {
		if (si->swap_map[offset])
			continue;
	got_page:
		if (offset == si->lowest_bit)
			si->lowest_bit++;
		if (offset == si->highest_bit)
			si->highest_bit--;
		si->swap_map[offset] = 1;
		nr_swap_pages--;
		si->cluster_next = offset+1;
		return offset;
	}
	return 0;
}

pte_t get_swap_page(void)
{
	struct swap_info_struct * p;
	unsigned long offset;
	pte_t entry = __pte(0);
	int type, wrapped = 0;

	type = swap_list.next;
	if (type < 0)
		goto out;
	if (nr_swap_pages == 0)
		goto out;

	while (1) {
		p = &swap_info[type];
		if ((p->flags & SWP_WRITEOK) == SWP_WRITEOK) {
			offset = scan_swap_map(p);
			if (offset) {
				entry = SWP_ENTRY(type,offset);
				type = swap_info[type].next;
				if (type < 0 ||
					p->prio != swap_info[type].prio) {
						swap_list.next = swap_list.head;
				} else {
					swap_list.next = type;
				}
				goto out;
			}
		}
		type = p->next;
		if (!wrapped) {
			if (type < 0 || p->prio != swap_info[type].prio) {
				type = swap_list.head;
				wrapped = 1;
			}
		} else
			if (type < 0)
				goto out;	/* out of swap space */
	}
out:
	return entry;
}


void swap_free(pte_t entry)
{
	struct swap_info_struct * p;
	unsigned long offset, type;

	if (!pte_val(entry))
		goto out;

	type = SWP_TYPE(entry);
	if (type & SHM_SWP_TYPE)
		goto out;
	if (type >= nr_swapfiles)
		goto bad_nofile;
	p = & swap_info[type];
	if (!(p->flags & SWP_USED))
		goto bad_device;
	if (p->prio > swap_info[swap_list.next].prio)
		swap_list.next = swap_list.head;
	offset = SWP_OFFSET(entry);
	if (offset >= p->max)
		goto bad_offset;
	if (!p->swap_map[offset])
		goto bad_free;
	if (p->swap_map[offset] < SWAP_MAP_MAX) {
		if (!--p->swap_map[offset]) {
			if (offset < p->lowest_bit)
				p->lowest_bit = offset;
			if (offset > p->highest_bit)
				p->highest_bit = offset;
			nr_swap_pages++;
		}
	}
out:
	return;

bad_nofile:
	printk("swap_free: Trying to free nonexistent swap-page\n");
	goto out;
bad_device:
	printk("swap_free: Trying to free swap from unused swap-device\n");
	goto out;
bad_offset:
	printk("swap_free: offset exceeds max\n");
	goto out;
bad_free:
	pte_ERROR(entry);
	goto out;
}

/* needs the big kernel lock */
pte_t acquire_swap_entry(struct page *page)
{
	struct swap_info_struct * p;
	unsigned long offset, type;
	pte_t entry;

	if (!test_bit(PG_swap_entry, &page->flags))
		goto new_swap_entry;

	/* We have the old entry in the page offset still */
	if (!page->offset)
		goto new_swap_entry;
	entry = get_pagecache_pte(page);
	type = SWP_TYPE(entry);
	if (type & SHM_SWP_TYPE)
		goto new_swap_entry;
	if (type >= nr_swapfiles)
		goto new_swap_entry;
	p = type + swap_info;
	if ((p->flags & SWP_WRITEOK) != SWP_WRITEOK)
		goto new_swap_entry;
	offset = SWP_OFFSET(entry);
	if (offset >= p->max)
		goto new_swap_entry;
	/* Has it been re-used for something else? */
	if (p->swap_map[offset])
		goto new_swap_entry;

	/* We're cool, we can just use the old one */
	p->swap_map[offset] = 1;
	nr_swap_pages--;
	return entry;

new_swap_entry:
	return get_swap_page();
}

/*
 * The swap entry has been read in advance, and we return 1 to indicate
 * that the page has been used or is no longer needed.
 *
 * Always set the resulting pte to be nowrite (the same as COW pages
 * after one process has exited).  We don't know just how many PTEs will
 * share this swap entry, so be cautious and let do_wp_page work out
 * what to do if a write is requested later.
 */
static inline void unuse_pte(struct vm_area_struct * vma, unsigned long address,
	pte_t *dir, pte_t entry, struct page* page)
{
	pte_t pte = *dir;

	if (pte_none(pte))
		return;
	if (pte_present(pte)) {
		/* If this entry is swap-cached, then page must already
                   hold the right address for any copies in physical
                   memory */
		if (pte_page(pte) != page)
			return;
		/* We will be removing the swap cache in a moment, so... */
		set_pte(dir, pte_mkdirty(pte));
		return;
	}
	if (pte_val(pte) != pte_val(entry))
		return;
	set_pte(dir, pte_mkdirty(mk_pte(page, vma->vm_page_prot)));
	swap_free(entry);
	get_page(mem_map + MAP_NR(page));
	++vma->vm_mm->rss;
}

static inline void unuse_pmd(struct vm_area_struct * vma, pmd_t *dir,
	unsigned long address, unsigned long size, unsigned long offset,
	pte_t entry, struct page* page)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*dir))
		return;
	if (pmd_bad(*dir)) {
		pmd_ERROR(*dir);
		pmd_clear(dir);
		return;
	}
	pte = pte_offset(dir, address);
	offset += address & PMD_MASK;
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		unuse_pte(vma, offset+address-vma->vm_start, pte, entry, page);
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));
}

static inline void unuse_pgd(struct vm_area_struct * vma, pgd_t *dir,
	unsigned long address, unsigned long size,
	pte_t entry, struct page* page)
{
	pmd_t * pmd;
	unsigned long offset, end;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return;
	}
	pmd = pmd_offset(dir, address);
	offset = address & PGDIR_MASK;
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	if (address >= end)
		BUG();
	do {
		unuse_pmd(vma, pmd, address, end - address, offset, entry,
			  page);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
}

static void unuse_vma(struct vm_area_struct * vma, pgd_t *pgdir,
			pte_t entry, struct page* page)
{
	unsigned long start = vma->vm_start, end = vma->vm_end;

	if (start >= end)
		BUG();
	do {
		unuse_pgd(vma, pgdir, start, end - start, entry, page);
		start = (start + PGDIR_SIZE) & PGDIR_MASK;
		pgdir++;
	} while (start && (start < end));
}

static void unuse_process(struct mm_struct * mm,
			pte_t entry, struct page* page)
{
	struct vm_area_struct* vma;

	/*
	 * Go through process' page directory.
	 */
	if (!mm)
		return;
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		pgd_t * pgd = pgd_offset(mm, vma->vm_start);
		unuse_vma(vma, pgd, entry, page);
	}
	return;
}

/*
 * We completely avoid races by reading each swap page in advance,
 * and then search for the process using it.  All the necessary
 * page table adjustments can then be made atomically.
 */
static int try_to_unuse(unsigned int type)
{
	struct swap_info_struct * si = &swap_info[type];
	struct task_struct *p;
	struct page *page;
	pte_t entry;
	int i;

	while (1) {
		/*
		 * Find a swap page in use and read it in.
		 */
		for (i = 1; i < si->max ; i++) {
			if (si->swap_map[i] > 0 && si->swap_map[i] != SWAP_MAP_BAD) {
				goto found_entry;
			}
		}
		break;

	found_entry:
		entry = SWP_ENTRY(type, i);

		/* Get a page for the entry, using the existing swap
                   cache page if there is one.  Otherwise, get a clean
                   page and read the swap into it. */
		page = read_swap_cache(entry);
		if (!page) {
			/*
			 * Continue searching if the entry became unused.
			 */
			if (si->swap_map[i] == 0)
				continue;
  			return -ENOMEM;
		}
		read_lock(&tasklist_lock);
		for_each_task(p)
			unuse_process(p->mm, entry, page);
		read_unlock(&tasklist_lock);
		shm_unuse(entry, page);
		/* Now get rid of the extra reference to the temporary
                   page we've been using. */
		if (PageSwapCache(page))
			delete_from_swap_cache(page);
		__free_page(page);
		/*
		 * Check for and clear any overflowed swap map counts.
		 */
		if (si->swap_map[i] != 0) {
			if (si->swap_map[i] != SWAP_MAP_MAX)
				pte_ERROR(entry);
			si->swap_map[i] = 0;
			nr_swap_pages++;
		}
	}
	return 0;
}

asmlinkage long sys_swapoff(const char * specialfile)
{
	struct swap_info_struct * p = NULL;
	struct dentry * dentry;
	struct file filp;
	int i, type, prev;
	int err = -EPERM;
	
	lock_kernel();
	if (!capable(CAP_SYS_ADMIN))
		goto out;

	dentry = namei(specialfile);
	err = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out;

	prev = -1;
	for (type = swap_list.head; type >= 0; type = swap_info[type].next) {
		p = swap_info + type;
		if ((p->flags & SWP_WRITEOK) == SWP_WRITEOK) {
			if (p->swap_file) {
				if (p->swap_file == dentry)
				  break;
			} else {
				if (S_ISBLK(dentry->d_inode->i_mode)
				    && (p->swap_device == dentry->d_inode->i_rdev))
				  break;
			}
		}
		prev = type;
	}
	err = -EINVAL;
	if (type < 0)
		goto out_dput;

	if (prev < 0) {
		swap_list.head = p->next;
	} else {
		swap_info[prev].next = p->next;
	}
	if (type == swap_list.next) {
		/* just pick something that's safe... */
		swap_list.next = swap_list.head;
	}
	p->flags = SWP_USED;
	err = try_to_unuse(type);
	if (err) {
		/* re-insert swap space back into swap_list */
		for (prev = -1, i = swap_list.head; i >= 0; prev = i, i = swap_info[i].next)
			if (p->prio >= swap_info[i].prio)
				break;
		p->next = i;
		if (prev < 0)
			swap_list.head = swap_list.next = p - swap_info;
		else
			swap_info[prev].next = p - swap_info;
		p->flags = SWP_WRITEOK;
		goto out_dput;
	}
	if(p->swap_device){
		memset(&filp, 0, sizeof(filp));		
		filp.f_dentry = dentry;
		filp.f_mode = 3; /* read write */
		/* open it again to get fops */
		if( !blkdev_open(dentry->d_inode, &filp) &&
		   filp.f_op && filp.f_op->release){
			filp.f_op->release(dentry->d_inode,&filp);
			filp.f_op->release(dentry->d_inode,&filp);
		}
	}
	dput(dentry);

	dentry = p->swap_file;
	p->swap_file = NULL;
	nr_swap_pages -= p->pages;
	p->swap_device = 0;
	vfree(p->swap_map);
	p->swap_map = NULL;
	p->flags = 0;
	err = 0;

out_dput:
	dput(dentry);
out:
	unlock_kernel();
	return err;
}

int get_swaparea_info(char *buf)
{
	char * page = (char *) __get_free_page(GFP_KERNEL);
	struct swap_info_struct *ptr = swap_info;
	int i, j, len = 0, usedswap;

	if (!page)
		return -ENOMEM;

	len += sprintf(buf, "Filename\t\t\tType\t\tSize\tUsed\tPriority\n");
	for (i = 0 ; i < nr_swapfiles ; i++, ptr++) {
		if (ptr->flags & SWP_USED) {
			char * path = d_path(ptr->swap_file, page, PAGE_SIZE);

			len += sprintf(buf + len, "%-31s ", path);

			if (!ptr->swap_device)
				len += sprintf(buf + len, "file\t\t");
			else
				len += sprintf(buf + len, "partition\t");

			usedswap = 0;
			for (j = 0; j < ptr->max; ++j)
				switch (ptr->swap_map[j]) {
					case SWAP_MAP_BAD:
					case 0:
						continue;
					default:
						usedswap++;
				}
			len += sprintf(buf + len, "%d\t%d\t%d\n", ptr->pages << (PAGE_SHIFT - 10), 
				usedswap << (PAGE_SHIFT - 10), ptr->prio);
		}
	}
	free_page((unsigned long) page);
	return len;
}

int is_swap_partition(kdev_t dev) {
	struct swap_info_struct *ptr = swap_info;
	int i;

	for (i = 0 ; i < nr_swapfiles ; i++, ptr++) {
		if (ptr->flags & SWP_USED)
			if (ptr->swap_device == dev)
				return 1;
	}
	return 0;
}

/*
 * Written 01/25/92 by Simmule Turner, heavily changed by Linus.
 *
 * The swapon system call
 */
asmlinkage long sys_swapon(const char * specialfile, int swap_flags)
{
	struct swap_info_struct * p;
	struct dentry * swap_dentry;
	unsigned int type;
	int i, j, prev;
	int error = -EPERM;
	struct file filp;
	static int least_priority = 0;
	union swap_header *swap_header = 0;
	int swap_header_version;
	int nr_good_pages = 0;
	unsigned long maxpages;
	int swapfilesize;
	
	lock_kernel();
	if (!capable(CAP_SYS_ADMIN))
		goto out;
	memset(&filp, 0, sizeof(filp));
	p = swap_info;
	for (type = 0 ; type < nr_swapfiles ; type++,p++)
		if (!(p->flags & SWP_USED))
			break;
	if (type >= MAX_SWAPFILES)
		goto out;
	if (type >= nr_swapfiles)
		nr_swapfiles = type+1;
	p->flags = SWP_USED;
	p->swap_file = NULL;
	p->swap_device = 0;
	p->swap_map = NULL;
	p->lowest_bit = 0;
	p->highest_bit = 0;
	p->cluster_nr = 0;
	p->max = 1;
	p->next = -1;
	if (swap_flags & SWAP_FLAG_PREFER) {
		p->prio =
		  (swap_flags & SWAP_FLAG_PRIO_MASK)>>SWAP_FLAG_PRIO_SHIFT;
	} else {
		p->prio = --least_priority;
	}
	swap_dentry = namei(specialfile);
	error = PTR_ERR(swap_dentry);
	if (IS_ERR(swap_dentry))
		goto bad_swap_2;

	p->swap_file = swap_dentry;
	error = -EINVAL;

	if (S_ISBLK(swap_dentry->d_inode->i_mode)) {
		kdev_t dev = swap_dentry->d_inode->i_rdev;

		p->swap_device = dev;
		set_blocksize(dev, PAGE_SIZE);
		
		filp.f_dentry = swap_dentry;
		filp.f_mode = 3; /* read write */
		error = blkdev_open(swap_dentry->d_inode, &filp);
		if (error)
			goto bad_swap_2;
		set_blocksize(dev, PAGE_SIZE);
		error = -ENODEV;
		if (!dev || (blk_size[MAJOR(dev)] &&
		     !blk_size[MAJOR(dev)][MINOR(dev)]))
			goto bad_swap;
		error = -EBUSY;
		for (i = 0 ; i < nr_swapfiles ; i++) {
			if (i == type)
				continue;
			if (dev == swap_info[i].swap_device)
				goto bad_swap;
		}
		swapfilesize = 0;
		if (blk_size[MAJOR(dev)])
			swapfilesize = blk_size[MAJOR(dev)][MINOR(dev)]
				/ (PAGE_SIZE / 1024);
	} else if (S_ISREG(swap_dentry->d_inode->i_mode)) {
		error = -EBUSY;
		for (i = 0 ; i < nr_swapfiles ; i++) {
			if (i == type || !swap_info[i].swap_file)
				continue;
			if (swap_dentry->d_inode == swap_info[i].swap_file->d_inode)
				goto bad_swap;
		}
		swapfilesize = swap_dentry->d_inode->i_size / PAGE_SIZE;
	} else
		goto bad_swap;

	swap_header = (void *) __get_free_page(GFP_USER);
	if (!swap_header) {
		printk("Unable to start swapping: out of memory :-)\n");
		error = -ENOMEM;
		goto bad_swap;
	}

	lock_page(mem_map + MAP_NR(swap_header));
	rw_swap_page_nolock(READ, SWP_ENTRY(type,0), (char *) swap_header, 1);

	if (!memcmp("SWAP-SPACE",swap_header->magic.magic,10))
		swap_header_version = 1;
	else if (!memcmp("SWAPSPACE2",swap_header->magic.magic,10))
		swap_header_version = 2;
	else {
		printk("Unable to find swap-space signature\n");
		error = -EINVAL;
		goto bad_swap;
	}
	
	switch (swap_header_version) {
	case 1:
		memset(((char *) swap_header)+PAGE_SIZE-10,0,10);
		j = 0;
		p->lowest_bit = 0;
		p->highest_bit = 0;
		for (i = 1 ; i < 8*PAGE_SIZE ; i++) {
			if (test_bit(i,(char *) swap_header)) {
				if (!p->lowest_bit)
					p->lowest_bit = i;
				p->highest_bit = i;
				p->max = i+1;
				j++;
			}
		}
		nr_good_pages = j;
		p->swap_map = vmalloc(p->max * sizeof(short));
		if (!p->swap_map) {
			error = -ENOMEM;		
			goto bad_swap;
		}
		for (i = 1 ; i < p->max ; i++) {
			if (test_bit(i,(char *) swap_header))
				p->swap_map[i] = 0;
			else
				p->swap_map[i] = SWAP_MAP_BAD;
		}
		break;

	case 2:
		/* Check the swap header's sub-version and the size of
                   the swap file and bad block lists */
		if (swap_header->info.version != 1) {
			printk(KERN_WARNING
			       "Unable to handle swap header version %d\n",
			       swap_header->info.version);
			error = -EINVAL;
			goto bad_swap;
		}

		p->lowest_bit  = 1;
		p->highest_bit = swap_header->info.last_page - 1;
		p->max	       = swap_header->info.last_page;

		maxpages = SWP_OFFSET(SWP_ENTRY(0,~0UL));
		if (p->max >= maxpages)
			p->max = maxpages-1;

		error = -EINVAL;
		if (swap_header->info.nr_badpages > MAX_SWAP_BADPAGES)
			goto bad_swap;
		
		/* OK, set up the swap map and apply the bad block list */
		if (!(p->swap_map = vmalloc (p->max * sizeof(short)))) {
			error = -ENOMEM;
			goto bad_swap;
		}

		error = 0;
		memset(p->swap_map, 0, p->max * sizeof(short));
		for (i=0; i<swap_header->info.nr_badpages; i++) {
			int page = swap_header->info.badpages[i];
			if (page <= 0 || page >= swap_header->info.last_page)
				error = -EINVAL;
			else
				p->swap_map[page] = SWAP_MAP_BAD;
		}
		nr_good_pages = swap_header->info.last_page -
				swap_header->info.nr_badpages -
				1 /* header page */;
		if (error) 
			goto bad_swap;
	}
	
	if (swapfilesize && p->max > swapfilesize) {
		printk(KERN_WARNING
		       "Swap area shorter than signature indicates\n");
		error = -EINVAL;
		goto bad_swap;
	}
	if (!nr_good_pages) {
		printk(KERN_WARNING "Empty swap-file\n");
		error = -EINVAL;
		goto bad_swap;
	}
	p->swap_map[0] = SWAP_MAP_BAD;
	p->flags = SWP_WRITEOK;
	p->pages = nr_good_pages;
	nr_swap_pages += nr_good_pages;
	printk(KERN_INFO "Adding Swap: %dk swap-space (priority %d)\n",
	       nr_good_pages<<(PAGE_SHIFT-10), p->prio);

	/* insert swap space into swap_list: */
	prev = -1;
	for (i = swap_list.head; i >= 0; i = swap_info[i].next) {
		if (p->prio >= swap_info[i].prio) {
			break;
		}
		prev = i;
	}
	p->next = i;
	if (prev < 0) {
		swap_list.head = swap_list.next = p - swap_info;
	} else {
		swap_info[prev].next = p - swap_info;
	}
	error = 0;
	goto out;
bad_swap:
	if(filp.f_op && filp.f_op->release)
		filp.f_op->release(filp.f_dentry->d_inode,&filp);
bad_swap_2:
	if (p->swap_map)
		vfree(p->swap_map);
	dput(p->swap_file);
	p->swap_device = 0;
	p->swap_file = NULL;
	p->swap_map = NULL;
	p->flags = 0;
	if (!(swap_flags & SWAP_FLAG_PREFER))
		++least_priority;
out:
	if (swap_header)
		free_page((long) swap_header);
	unlock_kernel();
	return error;
}

void si_swapinfo(struct sysinfo *val)
{
	unsigned int i, j;

	val->freeswap = val->totalswap = 0;
	for (i = 0; i < nr_swapfiles; i++) {
		if ((swap_info[i].flags & SWP_WRITEOK) != SWP_WRITEOK)
			continue;
		for (j = 0; j < swap_info[i].max; ++j)
			switch (swap_info[i].swap_map[j]) {
				case SWAP_MAP_BAD:
					continue;
				case 0:
					++val->freeswap;
				default:
					++val->totalswap;
			}
	}
	val->freeswap <<= PAGE_SHIFT;
	val->totalswap <<= PAGE_SHIFT;
	return;
}
