/*
 * PKSM (Anon-page KSM).
 *Copyright (C) 2012-2013
 * Authors:
 *	Figo.zhang (figo1802@gmail.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 * This is an improvement upon KSM/UKSM. Some basic data structures and routines
 * are borrowed from ksm.c and uksm.c .
 *
 * Its new features:
 * 1. Full system scan:
 *      It automatically scans all user processes' anonymous pages. Kernel-user
 *      interaction to submit a memory area to KSM is no longer needed.
 *
 * 2. High efficiency for anonymous page detection:
 *      It automatically detects anonymous page is created and freed, It will use a new
 *      algorithm and mechanism that it directly handle the anonymous page when it be created/freed
 *      by the linux kernel. It is no need to waster CPU time to traversing all of the VMA
 *      areas to find valid anonymous pages. KSM/UKSM will waster a lot of CPU time to traversing VMAs
 *      to find a valid anonymous page, PKSM no need considering it.
 *
 * 3. Full Zero Page consideration
 * 	Now pksmd consider full zero pages as special pages and merge them to an
 * 	special unswappable pksm zero page.
 *
 * 4. Check page content periodically
 * 	Pksm considers unstable page contents are volatile, which are add the a FIFO list. It check page
 * 	content by a hash value periodically. The default scanning cycles is 20 minutes.
 *
 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/memory.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/ksm.h>
#include <linux/hash.h>
#include <linux/freezer.h>
#include <linux/oom.h>
#include <linux/random.h>

#include <asm/tlbflush.h>
#include "internal.h"

#ifdef CONFIG_X86
#define CONFIG_PKSM_RHASH
#undef memcmp

#ifdef CONFIG_X86_32
#define memcmp memcmpx86_32
/*
 * Compare 4-byte-aligned address s1 and s2, with length n
 */
int memcmpx86_32(void *s1, void *s2, size_t n)
{
	size_t num = n / 4;
	register int res;

	__asm__ __volatile__
	(
	 "testl %3,%3\n\t"
	 "repe; cmpsd\n\t"
	 "je        1f\n\t"
	 "sbbl      %0,%0\n\t"
	 "orl       $1,%0\n"
	 "1:"
	 : "=&a" (res), "+&S" (s1), "+&D" (s2), "+&c" (num)
	 : "0" (0)
	 : "cc");

	return res;
}

/*
 * Check the page is all zero ?
 */
static int is_full_zero(const void *s1, size_t len)
{
	unsigned char same;

	len /= 4;

	__asm__ __volatile__
	("repe; scasl;"
	 "sete %0"
	 : "=qm" (same), "+D" (s1), "+c" (len)
	 : "a" (0)
	 : "cc");

	return same;
}
#elif defined(CONFIG_X86_64)
#define memcmp memcmpx86_64
/*
 * Compare 8-byte-aligned address s1 and s2, with length n
 */
int memcmpx86_64(void *s1, void *s2, size_t n)
{
	size_t num = n / 8;
	register int res;

	__asm__ __volatile__
	(
	 "testq %q3,%q3\n\t"
	 "repe; cmpsq\n\t"
	 "je        1f\n\t"
	 "sbbq      %q0,%q0\n\t"
	 "orq       $1,%q0\n"
	 "1:"
	 : "=&a" (res), "+&S" (s1), "+&D" (s2), "+&c" (num)
	 : "0" (0)
	 : "cc");

	return res;
}

static int is_full_zero(const void *s1, size_t len)
{
	unsigned char same;

	len /= 8;

	__asm__ __volatile__
	("repe; scasq;"
	 "sete %0"
	 : "=qm" (same), "+D" (s1), "+c" (len)
	 : "a" (0)
	 : "cc");

	return same;
}
#endif
#elif defined(CONFIG_ARM)
static int is_full_zero(const void *cs, size_t count)
{
	const unsigned long *src = cs;
	int i;

	count /= sizeof(*src);

	for (i = 0; i < count; i++) {
		if (src[i])
			return 0;
	}

	return 1;
}
#else
static int is_full_zero(const void *s1, size_t len)
{
	const unsigned long *src = s1;
	int i;

	len /= sizeof(*src);

	for (i = 0; i < len; i++) {
		if (src[i])
			return 0;
	}

	return 1;
}
#endif

struct stable_node_anon {
	struct hlist_node hlist;
	struct anon_vma *anon_vma;
};

/**
 * struct rmap_item - reverse mapping item for virtual addresses
 * @rmap_list: next rmap_item in mm_slot's singly-linked rmap_list
 * @anon_vma: pointer to anon_vma for this mm,address, when in stable tree
 * @mm: the memory structure this rmap_item is pointing into
 * @address: the virtual address this rmap_item tracks (+ flags in low bits)
 * @oldchecksum: previous checksum of the page at that virtual address
 * @node: rb node of this rmap_item in the unstable tree
 * @head: pointer to stable_node heading this list in the stable tree
 * @hlist: link into hlist of rmap_items hanging off that stable_node
 */
struct rmap_item {
	struct anon_vma *anon_vma;	/* point to the page anon_page */
	struct page  *page;
	unsigned long address;		/* + low bits used for flags below */
	struct list_head list; /*list for add new page(rmap_item)*/
	struct list_head del_list; /*list for del a page(rmap_item)*/
	struct hlist_head hlist; /*list for stable anon */
	union {
		struct rb_node node;	/* when node of unstable tree */
	};
	atomic_t _mapcount;
	unsigned long checksum;
	struct list_head update_list; /*list for unstable page checksum update*/
};


/**
 * struct mm_slot - ksm information per mm that is being scanned
 * @link: link to the mm_slots hash list
 * @mm_list: link into the mm_slots list, rooted in ksm_mm_head
 * @rmap_list: head for this mm_slot's singly-linked list of rmap_items
 * @mm: the mm that this information is valid for
 */
struct mm_slot {
	struct hlist_node link;
	struct list_head mm_list;
	struct rmap_item *rmap_list;
	struct mm_struct *mm;
};

/**
 * struct ksm_scan - cursor for scanning
 * @mm_slot: the current mm_slot we are scanning
 * @address: the next address inside that to be scanned
 * @rmap_list: link to the next rmap to be scanned in the rmap_list
 * @seqnr: count of completed full scans (needed when removing unstable node)
 *
 * There is only the one ksm_scan instance of this cursor structure.
 */
struct ksm_scan {
	struct mm_slot *mm_slot;
	unsigned long address;
	struct rmap_item **rmap_list;
	unsigned long seqnr;
};

#define SEQNR_MASK	0x0ff	/* low bits of unstable tree seqnr */

#define NEWLIST_FLAG  (1<<0)		/* rmap_item is at new_anon_page_list */
#define DELLIST_FLAG  (1<<1)      /*rmap_item at del_anon_page_list*/
#define INKSM_FLAG  (1<<2)      /*this page add to ksm subsystem*/
#define UNSTABLE_FLAG  (1<<3)  /* is a node of the unstable tree */
#define STABLE_FLAG (1<<4)		/* is listed from the stable tree */
#define CHECKSUM_LIST_FLAG (1<<5)  /*rmap_item in checksum list*/
#define INITCHECKSUM_FLAG (1<<6)
#define RESCAN_LIST_FLAG (1<<7)  /*rmap_item in pksm_rescan_page_list*/


#define PKSM_FAULT_SUCCESS 	0
#define PKSM_FAULT_DROP	1  /*drop this rmap_item*/
#define PKSM_FAULT_TRY      2 /*retry this rmap_item*/
#define PKSM_FAULT_KEEP    3 /*keep this rmap_item*/

/* The stable and unstable tree heads */
static struct rb_root root_stable_tree = RB_ROOT;
static struct rb_root root_unstable_tree = RB_ROOT;


#define MM_SLOTS_HASH_SHIFT 10
#define MM_SLOTS_HASH_HEADS (1 << MM_SLOTS_HASH_SHIFT)
static struct hlist_head mm_slots_hash[MM_SLOTS_HASH_HEADS];

static struct mm_slot ksm_mm_head = {
	.mm_list = LIST_HEAD_INIT(ksm_mm_head.mm_list),
};
static struct ksm_scan ksm_scan = {
	.mm_slot = &ksm_mm_head,
};

static struct kmem_cache *rmap_item_cache;
static struct kmem_cache *stable_anon_cache;
static struct kmem_cache *mm_slot_cache;

/* The number of nodes in the stable tree */
static unsigned long ksm_pages_shared;

/* The number of page slots additionally sharing those nodes */
static unsigned long ksm_pages_sharing;

/* The number of nodes in the unstable tree */
static unsigned long ksm_pages_unshared;

/* The number of rmap_items in use*/
static unsigned long ksm_rmap_items;

static unsigned long ksm_stable_nodes;

unsigned long ksm_pages_zero_sharing;

/* Number of pages ksmd should scan in one batch */
static unsigned int ksm_thread_pages_to_scan = 1000;

/* Milliseconds ksmd should sleep between batches */
static unsigned int ksm_thread_sleep_millisecs = 20;

/*Seconds pksm should update all unshared_pages by one period*/
static unsigned int pksm_unshared_page_update_period = 10;

/* Boolean to indicate whether to use deferred timer or not */
static bool use_deferred_timer;

#define KSM_RUN_STOP	0
#define KSM_RUN_MERGE	1
#define KSM_RUN_UNMERGE	2
static unsigned int ksm_run = KSM_RUN_MERGE;

/* The hash strength needed to hash a full page */
#define RSAD_STRENGTH_FULL		(PAGE_SIZE / sizeof(u32))

/* The random offsets in a page */
static u32 *pksm_random_table;
static u32 pksm_zero_random_checksum;

/*   32/3 < they < 32/2 */
#define shiftl	8
#define shiftr	12

#define HASH_FROM_TO(from, to) 				\
for (index = from; index < to; index++) {		\
	pos = pksm_random_table[index];			\
	hash += key[pos];				\
	hash += (hash << shiftl);			\
	hash ^= (hash >> shiftr);			\
}

#define ZERO_HASH_FROM_TO(from, to) 				\
for (index = from; index < to; index++) {		\
	pos = pksm_random_table[index];			\
	hash += 0;				\
	hash += (hash << shiftl);			\
	hash ^= (hash >> shiftr);			\
}

static DECLARE_WAIT_QUEUE_HEAD(ksm_thread_wait);
static DEFINE_MUTEX(ksm_thread_mutex);
static DEFINE_SPINLOCK(ksm_mmlist_lock);

//add by figo
static DEFINE_SPINLOCK(pksm_np_list_lock);
struct list_head new_anon_page_list = LIST_HEAD_INIT(new_anon_page_list);
struct list_head del_anon_page_list = LIST_HEAD_INIT(del_anon_page_list);
struct list_head pksm_rescan_page_list = LIST_HEAD_INIT(pksm_rescan_page_list);

struct list_head unstabletree_checksum_list = LIST_HEAD_INIT(unstabletree_checksum_list);
struct list_head pksm_volatile_page_list = LIST_HEAD_INIT(pksm_volatile_page_list);

#define KSM_KMEM_CACHE(__struct, __flags) kmem_cache_create("ksm_"#__struct,\
		sizeof(struct __struct), __alignof__(struct __struct),\
		(__flags), NULL)

static int __init ksm_slab_init(void)
{
	rmap_item_cache = KSM_KMEM_CACHE(rmap_item, 0);
	if (!rmap_item_cache)
		goto out;

	stable_anon_cache = KSM_KMEM_CACHE(stable_node_anon, 0);
	if (!stable_anon_cache)
		goto out_free1;

	mm_slot_cache = KSM_KMEM_CACHE(mm_slot, 0);
	if (!mm_slot_cache)
		goto out_free2;

	return 0;

out_free2:
	kmem_cache_destroy(stable_anon_cache);
out_free1:
	kmem_cache_destroy(rmap_item_cache);
out:
	return -ENOMEM;
}

static void __init ksm_slab_free(void)
{
	kmem_cache_destroy(mm_slot_cache);
	kmem_cache_destroy(stable_anon_cache);
	kmem_cache_destroy(rmap_item_cache);
	mm_slot_cache = NULL;
}

static inline struct stable_node_anon *alloc_stable_anon(void)
{
	struct stable_node_anon *node;
	node = kmem_cache_alloc(stable_anon_cache, GFP_KERNEL);
	if (!node)
		return NULL;

	ksm_stable_nodes++;
	return node;
}

static inline void free_stable_anon(struct stable_node_anon *stable_anon)
{
	if (stable_anon) {
		kmem_cache_free(stable_anon_cache, stable_anon);
		ksm_stable_nodes--;
	}
}

struct rmap_item *pksm_alloc_rmap_item(void)
{
	struct rmap_item *rmap_item;

	rmap_item = kmem_cache_zalloc(rmap_item_cache, GFP_KERNEL);
	if (rmap_item) {
		INIT_HLIST_HEAD(&rmap_item->hlist);
		INIT_LIST_HEAD(&rmap_item->list);
		INIT_LIST_HEAD(&rmap_item->del_list);
		INIT_LIST_HEAD(&rmap_item->update_list);
		RB_CLEAR_NODE(&rmap_item->node);
		ksm_rmap_items++;
		atomic_set(&rmap_item->_mapcount, 0);
	}
	return rmap_item;
}

void pksm_free_rmap_item(struct rmap_item *rmap_item)
{
	if (rmap_item) {
		ksm_rmap_items--;
		kmem_cache_free(rmap_item_cache, rmap_item);
	}
}

static struct page *page_trans_compound_anon(struct page *page)
{
	if (PageTransCompound(page)) {
		struct page *head = compound_trans_head(page);
		/*
		 * head may actually be splitted and freed from under
		 * us but it's ok here.
		 */
		if (PageAnon(head))
			return head;
	}
	return NULL;
}

static int check_valid_rmap_item(struct rmap_item *rmap_item)
{
	if (!rmap_item || !rmap_item->page || !PagePKSM(rmap_item->page) ||
		!(rmap_item->address & INKSM_FLAG))
		return 0;
	else
		return 1;
}

static inline struct rmap_item *page_stable_rmap_item(struct page *page)
{
	//return  PageKsm(page) ? ((unsigned long)page->mapping & ~PAGE_MAPPING_FLAGS) : NULL;

	if (!PageKsm(page))
		return NULL;

	if (!PagePKSM(page))
		return NULL;

	return page->pksm;
}

static inline struct mm_slot *alloc_mm_slot(void)
{
	if (!mm_slot_cache)	/* initialization failed */
		return NULL;
	return kmem_cache_zalloc(mm_slot_cache, GFP_KERNEL);
}

static inline void free_mm_slot(struct mm_slot *mm_slot)
{
	kmem_cache_free(mm_slot_cache, mm_slot);
}

static struct mm_slot *get_mm_slot(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	struct hlist_head *bucket;
	struct hlist_node *node;

	bucket = &mm_slots_hash[hash_ptr(mm, MM_SLOTS_HASH_SHIFT)];
	hlist_for_each_entry(mm_slot, node, bucket, link) {
		if (mm == mm_slot->mm)
			return mm_slot;
	}
	return NULL;
}

static void insert_to_mm_slots_hash(struct mm_struct *mm,
				    struct mm_slot *mm_slot)
{
	struct hlist_head *bucket;

	bucket = &mm_slots_hash[hash_ptr(mm, MM_SLOTS_HASH_SHIFT)];
	mm_slot->mm = mm;
	hlist_add_head(&mm_slot->link, bucket);
}

static inline int in_stable_tree(struct rmap_item *rmap_item)
{
	return rmap_item->address & STABLE_FLAG;
}

static inline int in_unstable_tree(struct rmap_item *rmap_item)
{
	return rmap_item->address & UNSTABLE_FLAG;
}

static void pksm_del_sharing_page_counter(struct page *page, int n)
{
	int i;
	struct rmap_item * rmap_item = (struct rmap_item *)(page->pksm);

	for (i = 0; i < n; i++) {
		ksm_pages_sharing--;
		atomic_dec(&rmap_item->_mapcount);
		__dec_zone_page_state(page, NR_PKSM_SHARING_PAGES);
	}
}

static void pksm_add_sharing_page_counter(struct page *page, int n)
{
	int i;
	struct rmap_item *rmap_item;
	if (!PageKsm(page))
		return ;

	rmap_item = (struct rmap_item *)(page->pksm);

	for (i = 0; i<n; i++) {
		ksm_pages_sharing++;
		atomic_inc(&rmap_item->_mapcount);
		__inc_zone_page_state(page, NR_PKSM_SHARING_PAGES);
	}
}

void pksm_unmap_sharing_page(struct page *page, struct mm_struct *mm, unsigned long address)
{
	int ksm_map, mapcount;
	struct rmap_item *rmap_item;

	if (!(PageKsm(page) && PagePKSM(page)))
		return ;

	rmap_item = (struct rmap_item *)(page->pksm);

	if (!check_valid_rmap_item(rmap_item))
		return ;

	ksm_map = atomic_read(&rmap_item->_mapcount);
	mapcount = page_mapcount(page);
	if (mapcount > ksm_map)
		return ;
	else
		goto free ;

free:
	if (ksm_map > 0)
		pksm_del_sharing_page_counter(page, 1);
}

/*
 * ksmd, and unmerge_and_remove_all_rmap_items(), must not touch an mm's
 * page tables after it has passed through ksm_exit() - which, if necessary,
 * takes mmap_sem briefly to serialize against them.  ksm_exit() does not set
 * a special flag: they can just back out as soon as mm_users goes to zero.
 * ksm_test_exit() is used throughout to make this test for exit: in some
 * places for correctness, in some places just to avoid unnecessary work.
 */
static inline bool ksm_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

#if 0
/*
 * We use break_ksm to break COW on a ksm page: it's a stripped down
 *
 *	if (get_user_pages(current, mm, addr, 1, 1, 1, &page, NULL) == 1)
 *		put_page(page);
 *
 * but taking great care only to touch a ksm page, in a VM_MERGEABLE vma,
 * in case the application has unmapped and remapped mm,addr meanwhile.
 * Could a ksm page appear anywhere else?  Actually yes, in a VM_PFNMAP
 * mmap of /dev/mem or /dev/kmem, where we would not want to touch it.
 */
static int break_ksm(struct vm_area_struct *vma, unsigned long addr)
{
	struct page *page;
	int ret = 0;

	do {
		cond_resched();
		page = follow_page(vma, addr, FOLL_GET);
		if (IS_ERR_OR_NULL(page))
			break;
		if (PageKsm(page))
			ret = handle_mm_fault(vma->vm_mm, vma, addr,
							FAULT_FLAG_WRITE);
		else
			ret = VM_FAULT_WRITE;
		put_page(page);
	} while (!(ret & (VM_FAULT_WRITE | VM_FAULT_SIGBUS | VM_FAULT_OOM)));
	/*
	 * We must loop because handle_mm_fault() may back out if there's
	 * any difficulty e.g. if pte accessed bit gets updated concurrently.
	 *
	 * VM_FAULT_WRITE is what we have been hoping for: it indicates that
	 * COW has been broken, even if the vma does not permit VM_WRITE;
	 * but note that a concurrent fault might break PageKsm for us.
	 *
	 * VM_FAULT_SIGBUS could occur if we race with truncation of the
	 * backing file, which also invalidates anonymous pages: that's
	 * okay, that truncation will have unmapped the PageKsm for us.
	 *
	 * VM_FAULT_OOM: at the time of writing (late July 2009), setting
	 * aside mem_cgroup limits, VM_FAULT_OOM would only be set if the
	 * current task has TIF_MEMDIE set, and will be OOM killed on return
	 * to user; and ksmd, having no mm, would never be chosen for that.
	 *
	 * But if the mm is in a limited mem_cgroup, then the fault may fail
	 * with VM_FAULT_OOM even if the current task is not TIF_MEMDIE; and
	 * even ksmd can fail in this way - though it's usually breaking ksm
	 * just to undo a merge it made a moment before, so unlikely to oom.
	 *
	 * That's a pity: we might therefore have more kernel pages allocated
	 * than we're counting as nodes in the stable tree; but ksm_do_scan
	 * will retry to break_cow on each pass, so should recover the page
	 * in due course.  The important thing is to not let VM_MERGEABLE
	 * be cleared while any such pages might remain in the area.
	 */
	return (ret & VM_FAULT_OOM) ? -ENOMEM : 0;
}
#endif

/*
 * Check that no O_DIRECT or similar I/O is in progress on the
 * page
 */
static int check_page_dio(struct page *page)
{
	int swapped = PageSwapCache(page);
	return (page_mapcount(page) +1+ swapped != page_count(page));
}

#if 0
static void break_cow(struct rmap_item *rmap_item)
{
	struct page *page;

	/*
	 * It is not an accident that whenever we want to break COW
	 * to undo, we also need to drop a reference to the anon_vma.
	 */
	//put_anon_vma(rmap_item->anon_vma);

	page = rmap_item->page;

	if ((atomic_read(&page->_count) < 1))
                    return;

	//rmap_walk_cow(page, pksm_break_ksm, NULL);
}
#endif

static struct page *get_ksm_page(struct rmap_item *rmap_item)
{
	struct page *page;
	void *expected_mapping;

	if (!check_valid_rmap_item(rmap_item))
		goto out;

	if (!(rmap_item->address & STABLE_FLAG))
		goto out;

	page = rmap_item->page;
	if (!page || !PageKsm(page))
		goto out;

	expected_mapping = (void *)rmap_item +
				(PAGE_MAPPING_ANON | PAGE_MAPPING_KSM);
	rcu_read_lock();
	if (page->mapping != expected_mapping) {
		goto stale;
	}
	if (!get_page_unless_zero(page))
		goto stale;
	if (!check_valid_rmap_item(rmap_item)) {
		put_page(page);
		goto stale;
	}
	if (page->mapping != expected_mapping) {
		put_page(page);
		goto stale;
	}
	rcu_read_unlock();
	return page;
stale:
	rcu_read_unlock();
out:
	return NULL;
}

static struct page *get_mergeable_page(struct rmap_item *rmap_item)
{

	struct page *page;

	if (!check_valid_rmap_item(rmap_item))
		goto out;

	if (!(rmap_item->address & UNSTABLE_FLAG))
		goto out;

	page = rmap_item->page;
	if (IS_ERR_OR_NULL(page))
		goto out;

	rcu_read_lock();
	if (!get_page_unless_zero(page))
		goto unlock;

	if (!check_valid_rmap_item(rmap_item))
		goto unlock;

	if (PageAnon(page)) {
		flush_dcache_page(page);
	} else {
		put_page(page);
		goto unlock;
	}

	rcu_read_unlock();
	return page;

unlock:
	rcu_read_unlock();
out:
	return NULL;
}

/*
 * Removing rmap_item from stable or unstable tree.
 * This function will clean the information from the stable/unstable tree.
 */
static void remove_rmap_item_from_tree(struct rmap_item *rmap_item, int free_anon)
{

	struct stable_node_anon *stable_anon;
	struct hlist_node *hlist , *tmp;

	if (!rmap_item)
		return ;

	if (rmap_item->address & STABLE_FLAG) {
		WARN_ON(RB_EMPTY_NODE(&rmap_item->node));
		if (!RB_EMPTY_NODE(&rmap_item->node)) {
			rmap_item->address &=~ STABLE_FLAG;
			rb_erase(&rmap_item->node, &root_stable_tree);
			RB_CLEAR_NODE(&rmap_item->node);
			ksm_pages_shared--;
		}
	} else if (rmap_item->address & UNSTABLE_FLAG) {
		WARN_ON(RB_EMPTY_NODE(&rmap_item->node));
		if (!RB_EMPTY_NODE(&rmap_item->node)) {
			rmap_item->address &=~ UNSTABLE_FLAG;
			rb_erase(&rmap_item->node, &root_unstable_tree);
			RB_CLEAR_NODE(&rmap_item->node);
			ksm_pages_unshared--;
		}

		if (rmap_item->address & CHECKSUM_LIST_FLAG) {
			list_del_init(&rmap_item->update_list);
			rmap_item->address &=~ CHECKSUM_LIST_FLAG;
		}
	}

	/*free stable_anon_node*/
	if (free_anon && !hlist_empty(&rmap_item->hlist)) {
			hlist_for_each_entry_safe(stable_anon, hlist, tmp, &rmap_item->hlist, hlist) {
				if (!stable_anon)
					continue;
				hlist_del(&stable_anon->hlist);
				put_anon_vma(stable_anon->anon_vma);
				free_stable_anon(stable_anon);
				cond_resched();
			}
	}
}

void pksm_clean_all_rmap_items(struct list_head *list)
{
	struct rmap_item *rmap_item , *n_item;

	list_for_each_entry_safe(rmap_item, n_item, list, del_list) {
		list_del(&rmap_item->del_list);
		remove_rmap_item_from_tree(rmap_item, 1);
		rmap_item->address &=~ INKSM_FLAG;
		rmap_item->address &=~ DELLIST_FLAG;
		pksm_free_rmap_item(rmap_item);
		cond_resched();
	}
}

static void pksm_free_all_rmap_items(void)
{
	struct rmap_item *rmap_item , *n_item;
	LIST_HEAD(l_del);

	spin_lock_irq(&pksm_np_list_lock);
	list_for_each_entry_safe(rmap_item, n_item, &del_anon_page_list, del_list) {
		if (!rmap_item)
			continue;
		list_move(&rmap_item->del_list, &l_del);
	}
	spin_unlock_irq(&pksm_np_list_lock);

	pksm_clean_all_rmap_items(&l_del);
}

#if 0
/*
 * Though it's very tempting to unmerge in_stable_tree(rmap_item)s rather
 * than check every pte of a given vma, the locking doesn't quite work for
 * that - an rmap_item is assigned to the stable tree after inserting ksm
 * page and upping mmap_sem.  Nor does it fit with the way we skip dup'ing
 * rmap_items from parent to child at fork time (so as not to waste time
 * if exit comes before the next scan reaches it).
 *
 * Similarly, although we'd like to remove rmap_items (so updating counts
 * and freeing memory) when unmerging an area, it's easier to leave that
 * to the next pass of ksmd - consider, for example, how ksmd might be
 * in cmp_and_merge_page on one of the rmap_items we would be removing.
 */
static int unmerge_ksm_pages(struct vm_area_struct *vma,
			     unsigned long start, unsigned long end)
{
	unsigned long addr;
	int err = 0;

	for (addr = start; addr < end && !err; addr += PAGE_SIZE) {
		if (ksm_test_exit(vma->vm_mm))
			break;
		if (signal_pending(current))
			err = -ERESTARTSYS;
		else
			err = break_ksm(vma, addr);
	}
	return err;
}

#ifdef CONFIG_SYSFS
/*
 * Only called through the sysfs control interface:
 */
static int unmerge_and_remove_all_rmap_items(void)
{
	struct mm_slot *mm_slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int err = 0;

	spin_lock(&ksm_mmlist_lock);
	ksm_scan.mm_slot = list_entry(ksm_mm_head.mm_list.next,
						struct mm_slot, mm_list);
	spin_unlock(&ksm_mmlist_lock);

	for (mm_slot = ksm_scan.mm_slot;
			mm_slot != &ksm_mm_head; mm_slot = ksm_scan.mm_slot) {
		mm = mm_slot->mm;
		down_read(&mm->mmap_sem);
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			if (ksm_test_exit(mm))
				break;
			if (!(vma->vm_flags & VM_MERGEABLE) || !vma->anon_vma)
				continue;
			err = unmerge_ksm_pages(vma,
						vma->vm_start, vma->vm_end);
			if (err)
				goto error;
		}

		//remove_trailing_rmap_items(mm_slot, &mm_slot->rmap_list);

		spin_lock(&ksm_mmlist_lock);
		ksm_scan.mm_slot = list_entry(mm_slot->mm_list.next,
						struct mm_slot, mm_list);
		if (ksm_test_exit(mm)) {
			hlist_del(&mm_slot->link);
			list_del(&mm_slot->mm_list);
			spin_unlock(&ksm_mmlist_lock);

			free_mm_slot(mm_slot);
			clear_bit(MMF_VM_MERGEABLE, &mm->flags);
			up_read(&mm->mmap_sem);
			mmdrop(mm);
		} else {
			spin_unlock(&ksm_mmlist_lock);
			up_read(&mm->mmap_sem);
		}
	}

	ksm_scan.seqnr = 0;
	return 0;

error:
	up_read(&mm->mmap_sem);
	spin_lock(&ksm_mmlist_lock);
	ksm_scan.mm_slot = &ksm_mm_head;
	spin_unlock(&ksm_mmlist_lock);
	return err;
}
#endif
#endif /* CONFIG_SYSFS */


static u32 pksm_calc_checksum(void *addr, u32 hash_strength)
{
	u32 hash = 0xdeadbeef;
	int index, pos;
	u32 *key = (u32 *)addr;
	HASH_FROM_TO(0, hash_strength);
	return hash;
}

static u32 pksm_calc_zero_page_checksum(u32 hash_strength)
{
	u32 hash = 0xdeadbeef;
	int index, pos;
	ZERO_HASH_FROM_TO(0, hash_strength);
	return hash;
}

static u32 calc_checksum(struct page *page)
{
	u32 checksum;
	void *addr = kmap_atomic(page);
	//checksum = jhash2(addr, PAGE_SIZE / 4, 17);
	checksum = pksm_calc_checksum(addr, RSAD_STRENGTH_FULL >> 4);
	kunmap_atomic(addr);
	return checksum;
}


static int memcmp_pages(struct page *page1, struct page *page2)
{
	char *addr1, *addr2;
	int ret;

	addr1 = kmap_atomic(page1);
	addr2 = kmap_atomic(page2);
	ret = memcmp(addr1, addr2, PAGE_SIZE);
	kunmap_atomic(addr2);
	kunmap_atomic(addr1);
	return ret;
}

static inline int pages_identical(struct page *page1, struct page *page2)
{
	return !memcmp_pages(page1, page2);
}

static int write_protect_page(struct vm_area_struct *vma, struct page *page,
			      pte_t *orig_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;
	pte_t *ptep;
	spinlock_t *ptl;
	int swapped;
	int err = PKSM_FAULT_DROP;

	addr = page_address_in_vma(page, vma);
	if (addr == -EFAULT)
		goto out;

	BUG_ON(PageTransCompound(page));
	ptep = page_check_address(page, mm, addr, &ptl, 0);
	if (!ptep)
		goto out;

	if (pte_write(*ptep) || pte_dirty(*ptep)) {
		pte_t entry;

		swapped = PageSwapCache(page);
		flush_cache_page(vma, addr, page_to_pfn(page));
		/*
		 * Ok this is tricky, when get_user_pages_fast() run it doesn't
		 * take any lock, therefore the check that we are going to make
		 * with the pagecount against the mapcount is racey and
		 * O_DIRECT can happen right after the check.
		 * So we clear the pte and flush the tlb before the check
		 * this assure us that no O_DIRECT can happen after the check
		 * or in the middle of the check.
		 */
		entry = ptep_clear_flush(vma, addr, ptep);
		/*
		 * Check that no O_DIRECT or similar I/O is in progress on the
		 * page
		 */
		if (page_mapcount(page) +1+ swapped != page_count(page)) {
			set_pte_at(mm, addr, ptep, entry);
			err = PKSM_FAULT_TRY;
			goto out_unlock;
		}

		if (pte_dirty(entry))
			set_page_dirty(page);
		entry = pte_mkclean(pte_wrprotect(entry));
		set_pte_at_notify(mm, addr, ptep, entry);
	}
	*orig_pte = *ptep;
	err = 0;

out_unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	return err;
}

/**
 * replace_page - replace page in vma by new ksm page
 * @vma:      vma that holds the pte pointing to page
 * @page:     the page we are replacing by kpage
 * @kpage:    the ksm page we replace page by
 * @orig_pte: the original value of the pte
 *
 * Returns 0 on success, -EFAULT on failure.
 */
static int replace_page(struct vm_area_struct *vma, struct page *page,
			struct page *kpage, pte_t orig_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;
	unsigned long addr;
	int err = PKSM_FAULT_DROP;
	pte_t entry;

	addr = page_address_in_vma(page, vma);
	if (addr == -EFAULT)
		goto out;

	pgd = pgd_offset(mm, addr);
	if (!pgd_present(*pgd))
		goto out;

	pud = pud_offset(pgd, addr);
	if (!pud_present(*pud))
		goto out;

	pmd = pmd_offset(pud, addr);
	BUG_ON(pmd_trans_huge(*pmd));
	if (!pmd_present(*pmd))
		goto out;

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte_same(*ptep, orig_pte)) {
		pte_unmap_unlock(ptep, ptl);
		goto out;
	}

	flush_cache_page(vma, addr, pte_pfn(*ptep));
	ptep_clear_flush(vma, addr, ptep);
	entry = mk_pte(kpage, vma->vm_page_prot);

	/* special treatment is needed for zero_page */
	if ((page_to_pfn(kpage) == pksm_zero_pfn) ||
				(page_to_pfn(kpage) == zero_pfn)) {
		entry = pte_mkspecial(entry);
		ksm_pages_zero_sharing++;
		__inc_zone_page_state(kpage, NR_PKSM_SHARING_PAGES);
		dec_mm_counter(mm, MM_ANONPAGES);
	} else {
		get_page(kpage);
		page_add_anon_rmap(kpage, vma, addr);
	}

	set_pte_at_notify(mm, addr, ptep, entry);

	page_remove_rmap(page);
	if (!page_mapped(page))
		try_to_free_swap(page);
	page_cache_release(page);

	pte_unmap_unlock(ptep, ptl);

	/*add ksm_pages_sharing++*/
	pksm_add_sharing_page_counter(kpage, 1);

	err = 0;

out:
	return err;
}

static int page_trans_compound_anon_split(struct page *page)
{
	int ret = 0;
	struct page *transhuge_head = page_trans_compound_anon(page);
	if (transhuge_head) {
		/* Get the reference on the head to split it. */
		if (get_page_unless_zero(transhuge_head)) {
			/*
			 * Recheck we got the reference while the head
			 * was still anonymous.
			 */
			if (PageAnon(transhuge_head))
				ret = split_huge_page(transhuge_head);
			else
				/*
				 * Retry later if split_huge_page run
				 * from under us.
				 */
				ret = 1;
			put_page(transhuge_head);
		} else
			/* Retry later if split_huge_page run from under us. */
			ret = 1;
	}
	return ret;
}

static inline void set_page_stable_ksm(struct page *page,
					struct rmap_item *rmap_item)
{
	page->mapping = (void *)rmap_item +
				(PAGE_MAPPING_ANON | PAGE_MAPPING_KSM);
}

static inline int pksm_flags_can_scan(unsigned long vm_flags)
{
	return !(vm_flags & (VM_PFNMAP | VM_IO  | VM_DONTEXPAND |
				  VM_RESERVED  | VM_HUGETLB | VM_INSERTPAGE |
				  VM_NONLINEAR | VM_MIXEDMAP | VM_SAO |
				  VM_SHARED  | VM_MAYSHARE | VM_GROWSUP
				  | VM_GROWSDOWN));
}

/*
 * What kind of VMA is considered ?
 */
static inline int vma_can_enter(struct vm_area_struct *vma)
{
	return pksm_flags_can_scan(vma->vm_flags);
}

static int pksm_rmap_walk(struct page *page, int (*rmap_one)(struct page *,
		struct vm_area_struct *, unsigned long, void *), void *arg)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	unsigned long address;
	int ret = PKSM_FAULT_DROP;

	VM_BUG_ON(!PageLocked(page));

	if (!PageAnon(page))
		goto out;

	anon_vma = page_lock_anon_vma(page);
	if (!anon_vma)
		goto out;

	list_for_each_entry(avc, &anon_vma->head, same_anon_vma) {
		struct vm_area_struct *vma = avc->vma;
		if (!vma_can_enter(vma))
			break;
		address = vma_address(page, vma);
		if (address == -EFAULT)
			break;
		ret = rmap_one(page, vma, address, arg);
		if (ret != PKSM_FAULT_SUCCESS)
			break;
	}
	page_unlock_anon_vma(anon_vma);
out:
	return ret;
}

static int pksm_writepect_pte(struct page *page, struct vm_area_struct *vma,
				 unsigned long addr, void *kpage)
{
	int err = PKSM_FAULT_DROP;
	pte_t orig_pte = __pte(0);

	if ((err = write_protect_page(vma, page, &orig_pte)) == 0) {
		if (!kpage) {
			set_page_stable_ksm(page, NULL);
			mark_page_accessed(page);
			err = 0;
		}  else if (pages_identical(page, kpage)) {
			err = replace_page(vma, page, kpage, orig_pte);
		}
	}

	if ((vma->vm_flags & VM_LOCKED) && kpage && !err) {
		munlock_vma_page(page);
		if (!PageMlocked(kpage)) {
			unlock_page(page);
			lock_page(kpage);
			mlock_vma_page(kpage);
			page = kpage;		/* for final unlock */
		}
	}

	return err;
}

static int try_to_merge_one_anon_page(struct page *page, struct page *kpage)
{
	int err =PKSM_FAULT_DROP;

	if (page == kpage)			/* ksm page forked */
		return 0;

	if (PageTransCompound(page) && page_trans_compound_anon_split(page))
		goto out;
	BUG_ON(PageTransCompound(page));
	if (!PageAnon(page))
		goto out;

	/*
	 * We need the page lock to read a stable PageSwapCache in
	 * write_protect_page().  We use trylock_page() instead of
	 * lock_page() because we don't want to wait here - we
	 * prefer to continue scanning and merging different pages,
	 * then come back to this page when it is unlocked.
	 */
	if (!trylock_page(page)) {
		err = PKSM_FAULT_TRY;
		goto out;
	}

	/*set write-protect and migrate pte*/
	err = pksm_rmap_walk(page, pksm_writepect_pte, kpage);
	if (err != PKSM_FAULT_SUCCESS)
		goto unlock;

	err = 0;

unlock:
	unlock_page(page);
out:
	return err;
}

static int try_to_merge_with_pksm_page(struct rmap_item *rmap_item,
				      struct page *page, struct page *kpage)
{
	int err = PKSM_FAULT_DROP;
	BUG_ON (rmap_item != page->pksm);
	err = try_to_merge_one_anon_page(page, kpage);
	return err;
}

/*
 * try_to_merge_two_pages - take two identical pages and prepare them
 * to be merged into one page.
 *
 * This function returns the kpage if we successfully merged two identical
 * pages into one ksm page, NULL otherwise.
 *
 * Note that this function upgrades page to ksm page: if one of the pages
 * is already a ksm page, try_to_merge_with_ksm_page should be used.
 */
static int try_to_merge_two_pages(struct rmap_item *rmap_item,
					   struct page *page,
					   struct rmap_item *tree_rmap_item,
					   struct page *tree_page)
{
	int err = PKSM_FAULT_DROP;

	BUG_ON(!rmap_item);
	BUG_ON(!page);
	BUG_ON(!tree_rmap_item);
	BUG_ON(!tree_page);
	BUG_ON (rmap_item != page->pksm);
	BUG_ON (tree_rmap_item != tree_page->pksm);

	err = try_to_merge_with_pksm_page(rmap_item, page, NULL);
	if (!err) {
		err = try_to_merge_with_pksm_page(tree_rmap_item,
							tree_page, page);
	}

	return err;
}

#ifdef CONFIG_PKSM_RHASH
static inline int hash_cmp(u32 new_val, u32 node_val)
{
	if (new_val > node_val)
		return 1;
	else if (new_val < node_val)
		return -1;
	else
		return 0;
}
#endif

/*
 * stable_tree_search - search for page inside the stable tree
 *
 * This function checks if there is a page inside the stable tree
 * with identical content to the page that we are scanning right now.
 *
 * This function returns the stable tree node of identical content if found,
 * NULL otherwise.
 */
static struct page *stable_tree_search(struct page *page)
{
#ifdef CONFIG_PKSM_RHASH
	struct rmap_item *rmap_item = (struct rmap_item *)(page->pksm);
#endif
	struct rb_node *parent = NULL;
	struct rb_node **new ;

	if (PageKsm(page))
		return NULL;

retry:
	new = &root_stable_tree.rb_node;

	while (*new) {
		struct rmap_item *tree_rmap_item;
		struct page *tree_page;
		int ret;

		cond_resched();

		tree_rmap_item = rb_entry(*new, struct rmap_item, node);

		if ((tree_rmap_item->address & DELLIST_FLAG) ||
			!tree_rmap_item->page) {
			remove_rmap_item_from_tree(tree_rmap_item, 0);
			//printk("%s:%d, page:0x%x:retry...\n",__func__, __LINE__, page);
			goto retry;
		}

		/*inc tree_page->_count ref*/
		tree_page = get_ksm_page(tree_rmap_item);
		if (!tree_page) {
			//printk("%s: get_ksm_page error, tree_page=0x%x, mapp=0x%x\n", __func__, tree_page, tree_page->mapping);
			return NULL;
		}

#ifdef CONFIG_PKSM_RHASH
		ret = hash_cmp(rmap_item->checksum, tree_rmap_item->checksum);
#else
		ret = memcmp_pages(page, tree_page);
#endif
		parent = *new;
		if (ret < 0) {
			put_page(tree_page);
			new = &parent->rb_left;
		} else if (ret > 0) {
			put_page(tree_page);
			new = &parent->rb_right;
		} else
			return tree_page;
	}

	return NULL;
}

/*
 * stable_tree_insert - insert rmap_item pointing to new ksm page
 * into the stable tree.
 *
 * This function returns the stable tree node just allocated on success,
 * NULL otherwise.
 */
static int stable_tree_insert(struct rmap_item *rmap_item,
											struct page *kpage)
{

	struct rb_node *parent = NULL;
	struct rb_node **new;

retry:
	new = &root_stable_tree.rb_node;

	while (*new) {
		struct rmap_item *tree_rmap_item;
		struct page *tree_page;
		int ret;

		cond_resched();
		tree_rmap_item = rb_entry(*new, struct rmap_item, node);

		/*rmap_item have add to dellist, drop it*/
		if ((tree_rmap_item->address & DELLIST_FLAG) ||
			!tree_rmap_item->page) {
			remove_rmap_item_from_tree(tree_rmap_item, 0);
			//printk("%s:%d, page:0x%x:retry...\n",__func__, __LINE__, kpage);
			goto retry;
		}

		tree_page = get_ksm_page(tree_rmap_item);
		if (!tree_page)
			return PKSM_FAULT_DROP;

#ifdef CONFIG_PKSM_RHASH
		ret = hash_cmp(rmap_item->checksum, tree_rmap_item->checksum);
#else
		ret = memcmp_pages(kpage, tree_page);
#endif

		put_page(tree_page);

		parent = *new;
		if (ret < 0)
			new = &parent->rb_left;
		else if (ret > 0)
			new = &parent->rb_right;
		else {
			/*
			 * It is not a bug that stable_tree_search() didn't
			 * find this node: because at that time our page was
			 * not yet write-protected, so may have changed since.
			 */
			return PKSM_FAULT_TRY;
		}
	}

	BUG_ON (rmap_item->address & UNSTABLE_FLAG);
	BUG_ON (rmap_item->address & STABLE_FLAG);

	rb_link_node(&rmap_item->node, parent, new);
	rb_insert_color(&rmap_item->node, &root_stable_tree);
	set_page_stable_ksm(kpage, rmap_item);
	rmap_item->address |= STABLE_FLAG;

	return 0;
}

/*
 * unstable_tree_search_insert - search for identical page,
 * else insert rmap_item into the unstable tree.
 *
 * This function searches for a page in the unstable tree identical to the
 * page currently being scanned; and if no identical page is found in the
 * tree, we insert rmap_item as a new object into the unstable tree.
 *
 * This function returns pointer to rmap_item found to be identical
 * to the currently scanned page, NULL otherwise.
 *
 * This function does both searching and inserting, because they share
 * the same walking algorithm in an rbtree.
 */
static
struct rmap_item *unstable_tree_search_insert(struct rmap_item *rmap_item,
					      struct page *page,
					      struct page **tree_pagep)

{

	struct rb_node *parent = NULL;
	struct rb_node **new;

	BUG_ON (rmap_item != page->pksm);

retry:
	new = &root_unstable_tree.rb_node;

	while (*new) {
		struct rmap_item *tree_rmap_item;
		struct page *tree_page;
		int ret;

		cond_resched();
		tree_rmap_item = rb_entry(*new, struct rmap_item, node);

		BUG_ON (rmap_item != page->pksm);

		if ((tree_rmap_item->address & DELLIST_FLAG) ||
			!tree_rmap_item->page) {
			remove_rmap_item_from_tree(tree_rmap_item, 0);
			//printk("%s:%d, page:0x%x:retry...\n",__func__, __LINE__, page);
			goto retry;
		}

		tree_page = get_mergeable_page(tree_rmap_item);
		if (IS_ERR_OR_NULL(tree_page))
			return NULL;

		BUG_ON (tree_rmap_item != tree_page->pksm);

		/*
		 * Don't substitute a ksm page for a forked page.
		 */
		if (page == tree_page) {
			put_page(tree_page);
			return NULL;
		}

#ifdef CONFIG_PKSM_RHASH
		ret = hash_cmp(rmap_item->checksum, tree_rmap_item->checksum);
#else
		ret = memcmp_pages(page, tree_page);
#endif

		parent = *new;
		if (ret < 0) {
			put_page(tree_page);
			new = &parent->rb_left;
		} else if (ret > 0) {
			put_page(tree_page);
			new = &parent->rb_right;
		} else {
			*tree_pagep = tree_page;
			return tree_rmap_item;
		}
	}

	BUG_ON (rmap_item != page->pksm);

	BUG_ON (rmap_item->address & UNSTABLE_FLAG);
	BUG_ON (rmap_item->address & STABLE_FLAG);

	if (!(rmap_item->address & UNSTABLE_FLAG)) {
		rmap_item->address |= UNSTABLE_FLAG;
		rb_link_node(&rmap_item->node, parent, new);
		rb_insert_color(&rmap_item->node, &root_unstable_tree);
		ksm_pages_unshared++;

		list_add_tail(&rmap_item->update_list, &unstabletree_checksum_list);
		rmap_item->address |= CHECKSUM_LIST_FLAG;
	}

	return NULL;
}

/*
 * stable_tree_append - add another rmap_item to the linked list of
 * rmap_items hanging off a given node of the stable tree, all sharing
 * the same ksm page.
 */
static void stable_tree_append(struct rmap_item *rmap_head, struct page *page)
{

	struct stable_node_anon *anon_node;
	struct rmap_item *rmap =(struct rmap_item *)(page->pksm);

	if (!check_valid_rmap_item(rmap_head))
		return ;
	if (!check_valid_rmap_item(rmap))
		return ;

	anon_node = alloc_stable_anon();

	if (PageKsm(page)) {
		anon_node->anon_vma = rmap->anon_vma;
	} else
		anon_node->anon_vma = page_rmapping(page);

	get_anon_vma(anon_node->anon_vma);

	/*add anon_node to rmap_head->hlist*/
	hlist_add_head(&anon_node->hlist, &rmap_head->hlist);

	if (!anon_node->hlist.next)
		ksm_pages_shared++;

}

/*---------------------------- zero page----------------------------*/
static inline int is_page_full_zero(struct page *page)
{
	char *addr;
	int ret;

	addr = kmap_atomic(page);
	ret = is_full_zero(addr, PAGE_SIZE);
	kunmap_atomic(addr);

	return ret;
}

static inline int find_zero_page_hash(struct page *page)
{
#ifdef CONFIG_PKSM_RHASH
	struct rmap_item *rmap_item= (struct rmap_item *)page->pksm;
	return (rmap_item->checksum == pksm_zero_random_checksum);
#else
	return is_page_full_zero(page);
#endif
}

static int pksm_merge_zero_page(struct page *page, struct vm_area_struct *vma,
				 unsigned long addr, void *kpage)
{
	int err = PKSM_FAULT_DROP;
	pte_t orig_pte = __pte(0);

	if ((err = write_protect_page(vma, page, &orig_pte)) == 0) {
		if (is_page_full_zero(page))
			err = replace_page(vma, page, kpage, orig_pte);
	}

	return err;
}


static
int try_to_merge_zero_page(struct page *page)
{
	struct page *zero_page = empty_pksm_zero_page;
	int err = PKSM_FAULT_DROP;

	if (PageTransCompound(page) && page_trans_compound_anon_split(page))
		goto out;
	BUG_ON(PageTransCompound(page));
	if (!PageAnon(page))
		goto out;

	/*
	 * We need the page lock to read a stable PageSwapCache in
	 * write_protect_page().  We use trylock_page() instead of
	 * lock_page() because we don't want to wait here - we
	 * prefer to continue scanning and merging different pages,
	 * then come back to this page when it is unlocked.
	 */
	if (!trylock_page(page))
		goto out;

	/*set write-protect and migrate pte*/
	err = pksm_rmap_walk(page, pksm_merge_zero_page, zero_page);
	if (err != PKSM_FAULT_SUCCESS)
		goto unlock;

	err = 0;

unlock:
	unlock_page(page);
out:
	return err;
}

int cmp_and_merge_zero_page(struct page *page)
{
	if (find_zero_page_hash(page)) {
		if (!try_to_merge_zero_page(page)) {
			return 0;
		}
	}

	return -EFAULT;
}

/*-------------------------------------------------------------------*/

/*
 * cmp_and_merge_page - first see if page can be merged into the stable tree;
 * if not, compare checksum to previous and if it's the same, see if page can
 * be inserted into the unstable tree, or merged with a page already there and
 * both transferred to the stable tree.
 *
 * @page: the page that we are searching identical page to.
 * @rmap_item: the reverse mapping into the virtual address of this page
 */
static int cmp_and_merge_page(struct page *page, struct rmap_item *rmap_item,
									int init_checksum)
{
	struct rmap_item *tree_rmap_item = NULL;
	struct page *tree_page = NULL;
	struct page *kpage;
	int err = PKSM_FAULT_SUCCESS;

	if (!check_valid_rmap_item(rmap_item)) {
		err = PKSM_FAULT_DROP;
		goto out;
	}

	if (PageKsm(page) || in_stable_tree(rmap_item)) {
		err = PKSM_FAULT_DROP;
		goto out;
	}

	remove_rmap_item_from_tree(rmap_item, 0);

	if (init_checksum)
		rmap_item->checksum = calc_checksum(page);

	if (!cmp_and_merge_zero_page(page))
		return 0;

	/* We first start with searching the page inside the stable tree */
	kpage = stable_tree_search(page);
	if (kpage) {
		BUG_ON (rmap_item != page->pksm);
		err = try_to_merge_with_pksm_page(rmap_item, page, kpage);
		if (!err) {
			lock_page(kpage);
			stable_tree_append(page_stable_rmap_item(kpage), page);
			unlock_page(kpage);
			err = 0;
		}
		put_page(kpage);
		goto out;
	}

	tree_rmap_item =
		unstable_tree_search_insert(rmap_item, page, &tree_page);
	if (tree_rmap_item) { /*if find the same page on unstable_tree*/
		/*unstable_rmap page map to the new page*/
		err = try_to_merge_two_pages(rmap_item, page,
						tree_rmap_item, tree_page);
		/*
		 * As soon as we merge this page, we want to remove the
		 * rmap_item of the page we have merged with from the unstable
		 * tree, and insert it instead as new node in the stable tree.
		 */
		if (!err) {
			kpage = page;
			BUG_ON (rmap_item != page->pksm);
			BUG_ON(rmap_item->page != kpage);

			remove_rmap_item_from_tree(tree_rmap_item, 0);

			/*insert new page to stable tree*/
			lock_page(kpage);
			err = stable_tree_insert(rmap_item, kpage);
			if (!err) {
				stable_tree_append(rmap_item, kpage);

				lock_page(tree_page);
				stable_tree_append(rmap_item, tree_page);
				unlock_page(tree_page);
				err = 0;
			}
			unlock_page(kpage);
		}
		put_page(tree_page);
	}

out:
	return err;
}

static unsigned int pksm_calc_update_pages_num(void)
{
	unsigned int need_scan = 0;

	if (ksm_pages_unshared < ksm_thread_pages_to_scan)
		need_scan = ksm_pages_unshared;
	else
		need_scan = (ksm_pages_unshared * ksm_thread_sleep_millisecs) /
		               (pksm_unshared_page_update_period *1000);

	return need_scan;
}
static void pksm_update_unstable_page_checksum(void)
{
	int scan = 0;
	unsigned long checksum;
	struct page *page;
	struct rmap_item *rmap_item, *n_item;
	LIST_HEAD(l_add);
	unsigned int need_scan = 0;

	need_scan = pksm_calc_update_pages_num();
	if (need_scan <= 0)
		return ;

	need_scan = min(need_scan, ksm_thread_pages_to_scan);

	list_for_each_entry_safe(rmap_item, n_item, &unstabletree_checksum_list, update_list) {
		if (!rmap_item)
			continue;

		BUG_ON(rmap_item->address & NEWLIST_FLAG);
		BUG_ON(!(rmap_item->address & CHECKSUM_LIST_FLAG));
		BUG_ON((rmap_item->address & STABLE_FLAG));

		page = rmap_item->page;
		if (!check_valid_rmap_item(rmap_item))
			goto out;

		if (!get_page_unless_zero(page))
			goto out;

		if (PageLocked(page))
			goto putpage;
		if (check_page_dio(page))
			goto putpage;

		checksum = calc_checksum(page);
		/*page content change? */
		if (rmap_item->checksum != checksum) {
			rmap_item->checksum = checksum;
			goto re_cmp;
		} else
			goto putpage;

re_cmp:
	remove_rmap_item_from_tree(rmap_item, 0);
	spin_lock(&pksm_np_list_lock);
	rmap_item->address &=~ INITCHECKSUM_FLAG;
	rmap_item->address |= RESCAN_LIST_FLAG;
	list_add_tail(&rmap_item->list, &pksm_rescan_page_list);
	spin_unlock(&pksm_np_list_lock);

putpage:
	put_page(page);
out:
	if (scan++ > need_scan)
		break;
	cond_resched();
	}
}

static void pksm_drop_rmap_item(struct rmap_item *rmap_item)
{
		struct page *page = (struct page *)(rmap_item->page);

		if (page && PagePKSM(page))
			__ClearPagePKSM(page);

		rmap_item->address = 0;
		remove_rmap_item_from_tree(rmap_item, 1);
		if (page)
			page->pksm = NULL;
		rmap_item->page = NULL;
		pksm_free_rmap_item(rmap_item);
}

/**
 * ksm_do_scan  - the ksm scanner main worker function.
 * @scan_npages - number of pages we want to scan before we return.
 */
static void ksm_do_scan(unsigned int scan_npages)
{
	struct rmap_item *rmap_item, *n_item;
	struct page *page;
	int init_checksum = 0;
	LIST_HEAD(l_add);
	int scan = 0;

	spin_lock_irq(&pksm_np_list_lock);
	list_for_each_entry_safe(rmap_item, n_item, &new_anon_page_list, list) {
		if (!rmap_item)
			continue;
		list_move(&rmap_item->list, &l_add);
		rmap_item->address &=~ NEWLIST_FLAG;
		rmap_item->address |= INKSM_FLAG;
		if (scan++ > scan_npages)
			break;
	}
	spin_unlock_irq(&pksm_np_list_lock);

	scan = 0;

	spin_lock(&pksm_np_list_lock);
	list_for_each_entry_safe(rmap_item, n_item, &pksm_rescan_page_list, list) {
		list_del_init(&rmap_item->list);
		rmap_item->address &=~RESCAN_LIST_FLAG;

		/*page have add del_list? free rmap_item later */
		if (rmap_item->address & DELLIST_FLAG)
			continue;
		list_add_tail(&rmap_item->list, &l_add);
		if (scan++ > scan_npages)
			break;
	}
	spin_unlock(&pksm_np_list_lock);

	list_for_each_entry_safe(rmap_item, n_item, &l_add, list) {
		list_del_init(&rmap_item->list);

		/*page have add del_list? free rmap_item later */
		if (rmap_item->address & DELLIST_FLAG)
			goto out;

		if (rmap_item->address & INITCHECKSUM_FLAG) {
			rmap_item->address &=~ INITCHECKSUM_FLAG;
			init_checksum = 1;
		} else
			init_checksum = 0;

		page = rmap_item->page;
		if (!check_valid_rmap_item(rmap_item))
			goto out;

		if (!(PageAnon(page)))
			goto out;

		flush_dcache_page(page);

		/*get ref for new page*/
		if (!get_page_unless_zero(page))
			goto out;

		if (PageLocked(page))
			goto rescan;

		if (check_page_dio(page))
			goto rescan;

		switch (cmp_and_merge_page(page, rmap_item, init_checksum)) {
			case PKSM_FAULT_SUCCESS:
			case PKSM_FAULT_KEEP:
				goto putpage;
			case PKSM_FAULT_DROP:
				pksm_drop_rmap_item(rmap_item);
				goto putpage;
			case PKSM_FAULT_TRY:
				goto rescan;
			}

rescan:
	spin_lock(&pksm_np_list_lock);
	rmap_item->address |= INITCHECKSUM_FLAG;
	rmap_item->address |= RESCAN_LIST_FLAG;
	list_add_tail(&rmap_item->list, &pksm_rescan_page_list);
	spin_unlock(&pksm_np_list_lock);

putpage:
	put_page(page);
out:
	cond_resched();
	}

	pksm_free_all_rmap_items();
	pksm_update_unstable_page_checksum();
}

static void process_timeout(unsigned long __data)
{
	wake_up_process((struct task_struct *)__data);
}

static signed long __sched deferred_schedule_timeout(signed long timeout)
{
	struct timer_list timer;
	unsigned long expire;

	__set_current_state(TASK_INTERRUPTIBLE);
	if (timeout < 0) {
		pr_err("schedule_timeout: wrong timeout value %lx\n",
							timeout);
		__set_current_state(TASK_RUNNING);
		goto out;
	}

	expire = timeout + jiffies;

	setup_deferrable_timer_on_stack(&timer, process_timeout,
			(unsigned long)current);
	mod_timer(&timer, expire);
	schedule();
	del_singleshot_timer_sync(&timer);

	/* Remove the timer from the object tracker */
	destroy_timer_on_stack(&timer);

	timeout = expire - jiffies;

out:
	return timeout < 0 ? 0 : timeout;
}

static int ksmd_should_run(void)
{
	return (ksm_run & KSM_RUN_MERGE);
}

static int ksm_scan_thread(void *nothing)
{
	set_freezable();
	set_user_nice(current, 5);

	while (!kthread_should_stop()) {
		mutex_lock(&ksm_thread_mutex);
		if (ksmd_should_run())
			ksm_do_scan(ksm_thread_pages_to_scan);
		mutex_unlock(&ksm_thread_mutex);

		try_to_freeze();

		if (ksmd_should_run()) {
			if (use_deferred_timer)
				deferred_schedule_timeout(
				msecs_to_jiffies(ksm_thread_sleep_millisecs));
			else
				schedule_timeout_interruptible(
				msecs_to_jiffies(ksm_thread_sleep_millisecs));
		} else {
			wait_event_freezable(ksm_thread_wait,
				ksmd_should_run() || kthread_should_stop());
		}
	}
	return 0;
}

int ksm_madvise(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, int advice, unsigned long *vm_flags)
{

	switch (advice) {
	case MADV_MERGEABLE:
		return 0;		/* just ignore the advice */
		break;

	case MADV_UNMERGEABLE:
#if 0
		if (!(*vm_flags & VM_MERGEABLE))
			return 0;		/* just ignore the advice */

		if (vma->anon_vma) {
			err = unmerge_ksm_pages(vma, start, end);
			if (err)
				return err;
		}

		*vm_flags &= ~VM_MERGEABLE;
#endif
		return 0;
		break;
	}

	return 0;
}

int __ksm_enter(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	int needs_wakeup;

	mm_slot = alloc_mm_slot();
	if (!mm_slot)
		return -ENOMEM;

	/* Check ksm_run too?  Would need tighter locking */
	needs_wakeup = list_empty(&ksm_mm_head.mm_list);

	spin_lock(&ksm_mmlist_lock);
	insert_to_mm_slots_hash(mm, mm_slot);
	/*
	 * Insert just behind the scanning cursor, to let the area settle
	 * down a little; when fork is followed by immediate exec, we don't
	 * want ksmd to waste time setting up and tearing down an rmap_list.
	 */
	list_add_tail(&mm_slot->mm_list, &ksm_scan.mm_slot->mm_list);
	spin_unlock(&ksm_mmlist_lock);

	set_bit(MMF_VM_MERGEABLE, &mm->flags);
	atomic_inc(&mm->mm_count);

	if (needs_wakeup)
		wake_up_interruptible(&ksm_thread_wait);

	return 0;
}

void __ksm_exit(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	int easy_to_free = 0;

	/*
	 * This process is exiting: if it's straightforward (as is the
	 * case when ksmd was never running), free mm_slot immediately.
	 * But if it's at the cursor or has rmap_items linked to it, use
	 * mmap_sem to synchronize with any break_cows before pagetables
	 * are freed, and leave the mm_slot on the list for ksmd to free.
	 * Beware: ksm may already have noticed it exiting and freed the slot.
	 */

	spin_lock(&ksm_mmlist_lock);
	mm_slot = get_mm_slot(mm);
	if (mm_slot && ksm_scan.mm_slot != mm_slot) {
		if (!mm_slot->rmap_list) {
			hlist_del(&mm_slot->link);
			list_del(&mm_slot->mm_list);
			easy_to_free = 1;
		} else {
			list_move(&mm_slot->mm_list,
				  &ksm_scan.mm_slot->mm_list);
		}
	}
	spin_unlock(&ksm_mmlist_lock);

	if (easy_to_free) {
		free_mm_slot(mm_slot);
		clear_bit(MMF_VM_MERGEABLE, &mm->flags);
		mmdrop(mm);
	} else if (mm_slot) {
		down_write(&mm->mmap_sem);
		up_write(&mm->mmap_sem);
	}
}

int pksm_add_new_anon_page(struct page *page, struct rmap_item *rmap_item, struct anon_vma *anon_vma)
{
	rmap_item = (struct rmap_item *)rmap_item;

	if (!rmap_item)
		return -EFAULT;
	if (!page || !anon_vma)
		return -EFAULT;

	/*is pksm page ? */
	if (PagePKSM(page))
		return -EFAULT;

	if (!(PageAnon(page)))
		return -EFAULT;

	if (PageKsm(page))
		return -EFAULT;

	anon_vma = (struct anon_vma *)((unsigned long)anon_vma & ~PAGE_MAPPING_FLAGS);
	rmap_item->anon_vma = anon_vma;

	SetPagePKSM(page);
	rmap_item->address |= NEWLIST_FLAG;
	rmap_item->address |= INITCHECKSUM_FLAG;
	page->pksm = rmap_item;
	rmap_item->page = page;

	spin_lock_irq(&pksm_np_list_lock);
	list_add_tail(&rmap_item->list, &new_anon_page_list);
	spin_unlock_irq(&pksm_np_list_lock);

	return 0;
}

int pksm_del_anon_page(struct page *page)
{

	struct rmap_item *rmap_item = NULL;
	int map;

	if (!PagePKSM(page))
		return -EFAULT;

	__ClearPagePKSM(page);

	rmap_item = (struct rmap_item *)(page->pksm);
	if (!rmap_item)
		return -EFAULT;

	if (page != rmap_item->page)
		return -EFAULT;

	map = atomic_read(&rmap_item->_mapcount);
	if (map > 0) {
		pksm_del_sharing_page_counter(page, map);
	}

	page->pksm = NULL;
	rmap_item->page = NULL;

	spin_lock_irq(&pksm_np_list_lock);
	if (rmap_item->address & (NEWLIST_FLAG | RESCAN_LIST_FLAG)) {
		/*rmap_item still at new list, have not added to pksm, directly free rmap_item*/
		list_del(&rmap_item->list);
		rmap_item->address = 0;
		pksm_free_rmap_item(rmap_item);
	} else {
		/*rmap_item have added to pksm*/
		rmap_item->address |= DELLIST_FLAG;
		list_add_tail(&rmap_item->del_list, &del_anon_page_list);
	}
	spin_unlock_irq(&pksm_np_list_lock);

	return 0;
}

struct page *ksm_does_need_to_copy(struct page *page,
			struct vm_area_struct *vma, unsigned long address)
{
	struct page *new_page;

	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
	if (new_page) {
		copy_user_highpage(new_page, page, address, vma);

		SetPageDirty(new_page);
		__SetPageUptodate(new_page);
		SetPageSwapBacked(new_page);
		__set_page_locked(new_page);

		if (page_evictable(new_page, vma))
			lru_cache_add_lru(new_page, LRU_ACTIVE_ANON);
		else
			add_page_to_unevictable_list(new_page);
	}

	return new_page;
}

int page_referenced_ksm(struct page *page, struct mem_cgroup *memcg,
			unsigned long *vm_flags)
{
	struct stable_node_anon *stable_anon;
	struct rmap_item *rmap_item;
	struct hlist_node *hlist;
	unsigned int mapcount = page_mapcount(page);
	int referenced = 0;
	int search_new_forks = 0;

	VM_BUG_ON(!PageKsm(page));
	VM_BUG_ON(!PageLocked(page));

	rmap_item =  page_stable_rmap_item(page);
	if (!rmap_item)
		return 0;
again:
	hlist_for_each_entry(stable_anon, hlist, &rmap_item->hlist, hlist) {
		struct anon_vma *anon_vma = stable_anon->anon_vma;
		struct anon_vma_chain *vmac;
		struct vm_area_struct *vma;

		anon_vma_lock(anon_vma);
		list_for_each_entry(vmac, &anon_vma->head, same_anon_vma) {
			vma = vmac->vma;

			if (memcg && !mm_match_cgroup(vma->vm_mm, memcg))
				continue;

			referenced += page_referenced_one(page, vma,
				rmap_item->address, &mapcount, vm_flags);
			if (!search_new_forks || !mapcount)
				break;
		}
		anon_vma_unlock(anon_vma);
		if (!mapcount)
			goto out;
	}
	if (!search_new_forks++)
		goto again;
out:
	return referenced;
}

int try_to_unmap_ksm(struct page *page, enum ttu_flags flags)
{
	struct stable_node_anon *stable_anon;
	struct hlist_node *hlist;
	struct rmap_item *rmap_item;
	int ret = SWAP_AGAIN;
	int search_new_forks = 0;

	VM_BUG_ON(!PageKsm(page));
	VM_BUG_ON(!PageLocked(page));

	rmap_item = page_stable_rmap_item(page);
	if (!rmap_item)
		return SWAP_FAIL;
again:
	hlist_for_each_entry(stable_anon, hlist, &rmap_item->hlist, hlist) {
		struct anon_vma *anon_vma = stable_anon->anon_vma;
		struct anon_vma_chain *vmac;
		struct vm_area_struct *vma;

		anon_vma_lock(anon_vma);
		list_for_each_entry(vmac, &anon_vma->head, same_anon_vma) {
			vma = vmac->vma;

			ret = try_to_unmap_one(page, vma,
					rmap_item->address, flags);
			if (ret != SWAP_AGAIN || !page_mapped(page)) {
				anon_vma_unlock(anon_vma);
				goto out;
			}
		}
		anon_vma_unlock(anon_vma);
	}
	if (!search_new_forks++)
		goto again;
out:
	return ret;
}

#ifdef CONFIG_MIGRATION
int rmap_walk_ksm(struct page *page, int (*rmap_one)(struct page *,
		  struct vm_area_struct *, unsigned long, void *), void *arg)
{
	struct stable_node_anon *stable_anon;
	struct hlist_node *hlist;
	struct rmap_item *rmap_item;
	int ret = SWAP_AGAIN;
	int search_new_forks = 0;

	VM_BUG_ON(!PageKsm(page));
	VM_BUG_ON(!PageLocked(page));

	rmap_item =  page_stable_rmap_item(page);
	if (!rmap_item)
		return ret;
again:
	hlist_for_each_entry(stable_anon, hlist, &rmap_item->hlist, hlist) {
		struct anon_vma *anon_vma = stable_anon->anon_vma;
		struct anon_vma_chain *vmac;
		struct vm_area_struct *vma;

		anon_vma_lock(anon_vma);
		list_for_each_entry(vmac, &anon_vma->head, same_anon_vma) {
			vma = vmac->vma;

			ret = rmap_one(page, vma, rmap_item->address, arg);
			if (ret != SWAP_AGAIN) {
				anon_vma_unlock(anon_vma);
				goto out;
			}
		}
		anon_vma_unlock(anon_vma);
	}
	if (!search_new_forks++)
		goto again;
out:
	return ret;
}

void ksm_migrate_page(struct page *newpage, struct page *oldpage)
{
	struct rmap_item *rmap_item;

	VM_BUG_ON(!PageLocked(oldpage));
	VM_BUG_ON(!PageLocked(newpage));
	VM_BUG_ON(newpage->mapping != oldpage->mapping);

	rmap_item = page_stable_rmap_item(newpage);
}
#endif /* CONFIG_MIGRATION */

#ifdef CONFIG_MEMORY_HOTREMOVE
static struct stable_node *ksm_check_stable_tree(unsigned long start_pfn,
						 unsigned long end_pfn)
{

}

static int ksm_memory_callback(struct notifier_block *self,
			       unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	struct stable_node *stable_node;

	switch (action) {
	case MEM_GOING_OFFLINE:
		/*
		 * Keep it very simple for now: just lock out ksmd and
		 * MADV_UNMERGEABLE while any memory is going offline.
		 * mutex_lock_nested() is necessary because lockdep was alarmed
		 * that here we take ksm_thread_mutex inside notifier chain
		 * mutex, and later take notifier chain mutex inside
		 * ksm_thread_mutex to unlock it.   But that's safe because both
		 * are inside mem_hotplug_mutex.
		 */
		mutex_lock_nested(&ksm_thread_mutex, SINGLE_DEPTH_NESTING);
		break;

	case MEM_OFFLINE:
		/*
		 * Most of the work is done by page migration; but there might
		 * be a few stable_nodes left over, still pointing to struct
		 * pages which have been offlined: prune those from the tree.
		 */
#if 0
		while ((stable_node = ksm_check_stable_tree(mn->start_pfn,
					mn->start_pfn + mn->nr_pages)) != NULL)
			remove_node_from_stable_tree(stable_node);
		/* fallthrough */
#endif

	case MEM_CANCEL_OFFLINE:
		mutex_unlock(&ksm_thread_mutex);
		break;
	}
	return NOTIFY_OK;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

#ifdef CONFIG_SYSFS
/*
 * This all compiles without CONFIG_SYSFS, but is a waste of space.
 */

#define KSM_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)
#define KSM_ATTR(_name) \
	static struct kobj_attribute _name##_attr = \
		__ATTR(_name, 0644, _name##_show, _name##_store)

static ssize_t sleep_millisecs_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_thread_sleep_millisecs);
}

static ssize_t sleep_millisecs_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long msecs;
	int err;

	err = strict_strtoul(buf, 10, &msecs);
	if (err || msecs > UINT_MAX)
		return -EINVAL;

	ksm_thread_sleep_millisecs = msecs;

	return count;
}
KSM_ATTR(sleep_millisecs);

static ssize_t period_seconds_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", pksm_unshared_page_update_period);
}

static ssize_t period_seconds_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long secs;
	int err;

	err = strict_strtoul(buf, 10, &secs);
	if (err || secs > UINT_MAX)
		return -EINVAL;

	pksm_unshared_page_update_period= secs;

	return count;
}
KSM_ATTR(period_seconds);

static ssize_t pages_to_scan_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_thread_pages_to_scan);
}

static ssize_t pages_to_scan_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long nr_pages;

	err = strict_strtoul(buf, 10, &nr_pages);
	if (err || nr_pages > UINT_MAX)
		return -EINVAL;

	ksm_thread_pages_to_scan = nr_pages;

	return count;
}
KSM_ATTR(pages_to_scan);

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", ksm_run);
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int err;
	unsigned long flags;

	err = strict_strtoul(buf, 10, &flags);
	if (err || flags > UINT_MAX)
		return -EINVAL;
	if (flags > KSM_RUN_UNMERGE)
		return -EINVAL;

	mutex_lock(&ksm_thread_mutex);
	if (ksm_run != flags) {
		ksm_run = flags;
	}
	mutex_unlock(&ksm_thread_mutex);

	if (flags & KSM_RUN_MERGE)
		wake_up_interruptible(&ksm_thread_wait);

	return count;
}
KSM_ATTR(run);

static ssize_t deferred_timer_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 8, "%d\n", use_deferred_timer);
}

static ssize_t deferred_timer_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long enable;
	int err;

	err = kstrtoul(buf, 10, &enable);
	use_deferred_timer = enable;

	return count;
}
KSM_ATTR(deferred_timer);

static ssize_t pages_shared_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_shared);
}
KSM_ATTR_RO(pages_shared);

static ssize_t pages_sharing_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_sharing+ksm_pages_zero_sharing);
}
KSM_ATTR_RO(pages_sharing);

static ssize_t pages_zero_sharing_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_zero_sharing);
}
KSM_ATTR_RO(pages_zero_sharing);

static ssize_t pages_unshared_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_unshared);
}
KSM_ATTR_RO(pages_unshared);

static ssize_t full_scans_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_scan.seqnr);
}
KSM_ATTR_RO(full_scans);

static ssize_t stable_nodes_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_stable_nodes);
}
KSM_ATTR_RO(stable_nodes);

static ssize_t rmap_items_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_rmap_items);
}
KSM_ATTR_RO(rmap_items);

static struct attribute *ksm_attrs[] = {
	&run_attr.attr,
	&pages_shared_attr.attr,
	&pages_sharing_attr.attr,
	&pages_unshared_attr.attr,
	&full_scans_attr.attr,
	&deferred_timer_attr.attr,
	NULL,
};

static struct attribute *pksm_attrs[] = {
	&sleep_millisecs_attr.attr,
	&pages_to_scan_attr.attr,
	&period_seconds_attr.attr,
	&pages_zero_sharing_attr.attr,
	&stable_nodes_attr.attr,
	&rmap_items_attr.attr,
	NULL,
};

static struct attribute_group ksm_attr_group = {
	.attrs = ksm_attrs,
	.name = "ksm",
};

static struct attribute_group pksm_attr_group = {
	.attrs = pksm_attrs,
	.name = "pksm",
};
#endif /* CONFIG_SYSFS */

static inline int init_random_sampling(void)
{
	unsigned long i;
	pksm_random_table = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!pksm_random_table)
		return -ENOMEM;

	for (i = 0; i < RSAD_STRENGTH_FULL; i++)
		pksm_random_table[i] = i;

	for (i = 0; i < RSAD_STRENGTH_FULL; i++) {
		/*init pksm_random_table*/
		unsigned long rand_range, swap_index, tmp;
		rand_range = RSAD_STRENGTH_FULL - i;
		swap_index = i + random32() % rand_range;
		tmp = pksm_random_table[i];
		pksm_random_table[i] =  pksm_random_table[swap_index];
		pksm_random_table[swap_index] = tmp;
	}

	pksm_zero_random_checksum = pksm_calc_zero_page_checksum(RSAD_STRENGTH_FULL >> 4);

	return 0;
}

static int __init ksm_init(void)
{
	struct task_struct *ksm_thread;
	int err;

	err = ksm_slab_init();
	if (err)
		goto out;

	err = init_random_sampling();
	if (err)
		goto out_random;

	ksm_thread = kthread_run(ksm_scan_thread, NULL, "pksmd");
	if (IS_ERR(ksm_thread)) {
		printk(KERN_ERR "pksm: creating kthread failed\n");
		err = PTR_ERR(ksm_thread);
		goto out_free;
	}

#ifdef CONFIG_SYSFS
	err = sysfs_create_group(mm_kobj, &ksm_attr_group);
	err = sysfs_create_group(mm_kobj, &pksm_attr_group);
	if (err) {
		printk(KERN_ERR "ksm: register sysfs failed\n");
		kthread_stop(ksm_thread);
		goto out_free;
	}
#else
	ksm_run = KSM_RUN_MERGE;	/* no way for user to start it */

#endif /* CONFIG_SYSFS */

#ifdef CONFIG_MEMORY_HOTREMOVE
	/*
	 * Choose a high priority since the callback takes ksm_thread_mutex:
	 * later callbacks could only be taking locks which nest within that.
	 */
	hotplug_memory_notifier(ksm_memory_callback, 100);
#endif
	return 0;

out_free:
	kfree(pksm_random_table);
out_random:
	ksm_slab_free();
out:
	return err;
}
module_init(ksm_init)
