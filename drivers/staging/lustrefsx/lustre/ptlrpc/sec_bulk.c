/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ptlrpc/sec_bulk.c
 *
 * Author: Eric Mei <ericm@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_SEC

#include <libcfs/libcfs.h>

#include <obd.h>
#include <obd_cksum.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lustre_dlm.h>
#include <lustre_sec.h>

#include "ptlrpc_internal.h"

static int mult = 20 - PAGE_SHIFT;
static int enc_pool_max_memory_mb;
module_param(enc_pool_max_memory_mb, int, 0644);
MODULE_PARM_DESC(enc_pool_max_memory_mb,
		 "Encoding pool max memory (MB), 1/8 of total physical memory by default");

/****************************************
 * bulk encryption page pools           *
 ****************************************/


#define PTRS_PER_PAGE   (PAGE_SIZE / sizeof(void *))
#define PAGES_PER_POOL  (PTRS_PER_PAGE)

#define IDLE_IDX_MAX            (100)
#define IDLE_IDX_WEIGHT         (3)

#define CACHE_QUIESCENT_PERIOD  (20)

static struct ptlrpc_enc_page_pool {
        /*
         * constants
         */
        unsigned long    epp_max_pages;   /* maximum pages can hold, const */
        unsigned int     epp_max_pools;   /* number of pools, const */

	/*
	 * wait queue in case of not enough free pages.
	 */
	wait_queue_head_t    epp_waitq;       /* waiting threads */
	unsigned int     epp_waitqlen;    /* wait queue length */
	unsigned long    epp_pages_short; /* # of pages wanted of in-q users */
	unsigned int     epp_growing:1;   /* during adding pages */

        /*
         * indicating how idle the pools are, from 0 to MAX_IDLE_IDX
         * this is counted based on each time when getting pages from
         * the pools, not based on time. which means in case that system
         * is idled for a while but the idle_idx might still be low if no
         * activities happened in the pools.
         */
        unsigned long    epp_idle_idx;

        /* last shrink time due to mem tight */
	time64_t	epp_last_shrink;
	time64_t	epp_last_access;

        /*
         * in-pool pages bookkeeping
         */
	spinlock_t	 epp_lock;	   /* protect following fields */
        unsigned long    epp_total_pages; /* total pages in pools */
        unsigned long    epp_free_pages;  /* current pages available */

        /*
         * statistics
         */
        unsigned long    epp_st_max_pages;      /* # of pages ever reached */
        unsigned int     epp_st_grows;          /* # of grows */
        unsigned int     epp_st_grow_fails;     /* # of add pages failures */
        unsigned int     epp_st_shrinks;        /* # of shrinks */
        unsigned long    epp_st_access;         /* # of access */
        unsigned long    epp_st_missings;       /* # of cache missing */
        unsigned long    epp_st_lowfree;        /* lowest free pages reached */
        unsigned int     epp_st_max_wqlen;      /* highest waitqueue length */
        cfs_time_t       epp_st_max_wait;       /* in jeffies */
	unsigned long	 epp_st_outofmem;	/* # of out of mem requests */
	/*
	 * pointers to pools, may be vmalloc'd
	 */
	struct page    ***epp_pools;
} page_pools;

/*
 * memory shrinker
 */
static const int pools_shrinker_seeks = DEFAULT_SEEKS;
static struct shrinker *pools_shrinker;


/*
 * /proc/fs/lustre/sptlrpc/encrypt_page_pools
 */
int sptlrpc_proc_enc_pool_seq_show(struct seq_file *m, void *v)
{
	spin_lock(&page_pools.epp_lock);

	seq_printf(m, "physical pages:          %lu\n"
		   "pages per pool:          %lu\n"
		   "max pages:               %lu\n"
		   "max pools:               %u\n"
		   "total pages:             %lu\n"
		   "total free:              %lu\n"
		   "idle index:              %lu/100\n"
		   "last shrink:             %lds\n"
		   "last access:             %lds\n"
		   "max pages reached:       %lu\n"
		   "grows:                   %u\n"
		   "grows failure:           %u\n"
		   "shrinks:                 %u\n"
		   "cache access:            %lu\n"
		   "cache missing:           %lu\n"
		   "low free mark:           %lu\n"
		   "max waitqueue depth:     %u\n"
		   "max wait time:           %ld/%lu\n"
		   "out of mem:              %lu\n",
		   TOTALRAM_PAGES, PAGES_PER_POOL,
		   page_pools.epp_max_pages,
		   page_pools.epp_max_pools,
		   page_pools.epp_total_pages,
		   page_pools.epp_free_pages,
		   page_pools.epp_idle_idx,
		   (long)(ktime_get_seconds() - page_pools.epp_last_shrink),
		   (long)(ktime_get_seconds() - page_pools.epp_last_access),
		   page_pools.epp_st_max_pages,
		   page_pools.epp_st_grows,
		   page_pools.epp_st_grow_fails,
		   page_pools.epp_st_shrinks,
		   page_pools.epp_st_access,
		   page_pools.epp_st_missings,
		   page_pools.epp_st_lowfree,
		   page_pools.epp_st_max_wqlen,
		   page_pools.epp_st_max_wait,
		   msecs_to_jiffies(MSEC_PER_SEC),
		   page_pools.epp_st_outofmem);

	spin_unlock(&page_pools.epp_lock);
	return 0;
}

static void enc_pools_release_free_pages(long npages)
{
        int     p_idx, g_idx;
        int     p_idx_max1, p_idx_max2;

        LASSERT(npages > 0);
        LASSERT(npages <= page_pools.epp_free_pages);
        LASSERT(page_pools.epp_free_pages <= page_pools.epp_total_pages);

        /* max pool index before the release */
        p_idx_max2 = (page_pools.epp_total_pages - 1) / PAGES_PER_POOL;

        page_pools.epp_free_pages -= npages;
        page_pools.epp_total_pages -= npages;

        /* max pool index after the release */
        p_idx_max1 = page_pools.epp_total_pages == 0 ? -1 :
                     ((page_pools.epp_total_pages - 1) / PAGES_PER_POOL);

        p_idx = page_pools.epp_free_pages / PAGES_PER_POOL;
        g_idx = page_pools.epp_free_pages % PAGES_PER_POOL;
        LASSERT(page_pools.epp_pools[p_idx]);

        while (npages--) {
                LASSERT(page_pools.epp_pools[p_idx]);
                LASSERT(page_pools.epp_pools[p_idx][g_idx] != NULL);

		__free_page(page_pools.epp_pools[p_idx][g_idx]);
                page_pools.epp_pools[p_idx][g_idx] = NULL;

                if (++g_idx == PAGES_PER_POOL) {
                        p_idx++;
                        g_idx = 0;
                }
	}

        /* free unused pools */
        while (p_idx_max1 < p_idx_max2) {
                LASSERT(page_pools.epp_pools[p_idx_max2]);
		OBD_FREE(page_pools.epp_pools[p_idx_max2], PAGE_SIZE);
                page_pools.epp_pools[p_idx_max2] = NULL;
                p_idx_max2--;
        }
}

/*
 * we try to keep at least PTLRPC_MAX_BRW_PAGES pages in the pool.
 */
static unsigned long enc_pools_shrink_count(struct shrinker *s,
					    struct shrink_control *sc)
{
	/*
	 * if no pool access for a long time, we consider it's fully idle.
	 * a little race here is fine.
	 */
	if (unlikely(ktime_get_real_seconds() - page_pools.epp_last_access >
		     CACHE_QUIESCENT_PERIOD)) {
		spin_lock(&page_pools.epp_lock);
		page_pools.epp_idle_idx = IDLE_IDX_MAX;
		spin_unlock(&page_pools.epp_lock);
	}

	LASSERT(page_pools.epp_idle_idx <= IDLE_IDX_MAX);
	return (page_pools.epp_free_pages <= PTLRPC_MAX_BRW_PAGES) ? 0 :
		(page_pools.epp_free_pages - PTLRPC_MAX_BRW_PAGES) *
		(IDLE_IDX_MAX - page_pools.epp_idle_idx) / IDLE_IDX_MAX;
}

/*
 * we try to keep at least PTLRPC_MAX_BRW_PAGES pages in the pool.
 */
static unsigned long enc_pools_shrink_scan(struct shrinker *s,
					   struct shrink_control *sc)
{
	spin_lock(&page_pools.epp_lock);
	if (page_pools.epp_free_pages <= PTLRPC_MAX_BRW_PAGES)
		sc->nr_to_scan = 0;
	else
		sc->nr_to_scan = min_t(unsigned long, sc->nr_to_scan,
			      page_pools.epp_free_pages - PTLRPC_MAX_BRW_PAGES);
	if (sc->nr_to_scan > 0) {
		enc_pools_release_free_pages(sc->nr_to_scan);
		CDEBUG(D_SEC, "released %ld pages, %ld left\n",
		       (long)sc->nr_to_scan, page_pools.epp_free_pages);

		page_pools.epp_st_shrinks++;
		page_pools.epp_last_shrink = ktime_get_real_seconds();
	}
	spin_unlock(&page_pools.epp_lock);

	/*
	 * if no pool access for a long time, we consider it's fully idle.
	 * a little race here is fine.
	 */
	if (unlikely(ktime_get_real_seconds() - page_pools.epp_last_access >
		     CACHE_QUIESCENT_PERIOD)) {
		spin_lock(&page_pools.epp_lock);
		page_pools.epp_idle_idx = IDLE_IDX_MAX;
		spin_unlock(&page_pools.epp_lock);
	}

	LASSERT(page_pools.epp_idle_idx <= IDLE_IDX_MAX);
	return sc->nr_to_scan;
}

#ifndef HAVE_SHRINKER_COUNT
/*
 * could be called frequently for query (@nr_to_scan == 0).
 * we try to keep at least PTLRPC_MAX_BRW_PAGES pages in the pool.
 */
static int enc_pools_shrink(SHRINKER_ARGS(sc, nr_to_scan, gfp_mask))
{
	struct shrink_control scv = {
		.nr_to_scan = shrink_param(sc, nr_to_scan),
		.gfp_mask   = shrink_param(sc, gfp_mask)
	};
#if !defined(HAVE_SHRINKER_WANT_SHRINK_PTR) && !defined(HAVE_SHRINK_CONTROL)
	struct shrinker* shrinker = NULL;
#endif

	enc_pools_shrink_scan(shrinker, &scv);

	return enc_pools_shrink_count(shrinker, &scv);
}

#endif /* HAVE_SHRINKER_COUNT */

static inline
int npages_to_npools(unsigned long npages)
{
        return (int) ((npages + PAGES_PER_POOL - 1) / PAGES_PER_POOL);
}

/*
 * return how many pages cleaned up.
 */
static unsigned long enc_pools_cleanup(struct page ***pools, int npools)
{
	unsigned long cleaned = 0;
	int           i, j;

	for (i = 0; i < npools; i++) {
		if (pools[i]) {
			for (j = 0; j < PAGES_PER_POOL; j++) {
				if (pools[i][j]) {
					__free_page(pools[i][j]);
					cleaned++;
				}
			}
			OBD_FREE(pools[i], PAGE_SIZE);
			pools[i] = NULL;
		}
	}

	return cleaned;
}

/*
 * merge @npools pointed by @pools which contains @npages new pages
 * into current pools.
 *
 * we have options to avoid most memory copy with some tricks. but we choose
 * the simplest way to avoid complexity. It's not frequently called.
 */
static void enc_pools_insert(struct page ***pools, int npools, int npages)
{
        int     freeslot;
        int     op_idx, np_idx, og_idx, ng_idx;
        int     cur_npools, end_npools;

        LASSERT(npages > 0);
        LASSERT(page_pools.epp_total_pages+npages <= page_pools.epp_max_pages);
        LASSERT(npages_to_npools(npages) == npools);
        LASSERT(page_pools.epp_growing);

	spin_lock(&page_pools.epp_lock);

        /*
         * (1) fill all the free slots of current pools.
         */
        /* free slots are those left by rent pages, and the extra ones with
         * index >= total_pages, locate at the tail of last pool. */
        freeslot = page_pools.epp_total_pages % PAGES_PER_POOL;
        if (freeslot != 0)
                freeslot = PAGES_PER_POOL - freeslot;
        freeslot += page_pools.epp_total_pages - page_pools.epp_free_pages;

        op_idx = page_pools.epp_free_pages / PAGES_PER_POOL;
        og_idx = page_pools.epp_free_pages % PAGES_PER_POOL;
        np_idx = npools - 1;
        ng_idx = (npages - 1) % PAGES_PER_POOL;

        while (freeslot) {
                LASSERT(page_pools.epp_pools[op_idx][og_idx] == NULL);
                LASSERT(pools[np_idx][ng_idx] != NULL);

                page_pools.epp_pools[op_idx][og_idx] = pools[np_idx][ng_idx];
                pools[np_idx][ng_idx] = NULL;

                freeslot--;

                if (++og_idx == PAGES_PER_POOL) {
                        op_idx++;
                        og_idx = 0;
                }
                if (--ng_idx < 0) {
                        if (np_idx == 0)
                                break;
                        np_idx--;
                        ng_idx = PAGES_PER_POOL - 1;
                }
        }

        /*
         * (2) add pools if needed.
         */
        cur_npools = (page_pools.epp_total_pages + PAGES_PER_POOL - 1) /
                     PAGES_PER_POOL;
        end_npools = (page_pools.epp_total_pages + npages + PAGES_PER_POOL -1) /
                     PAGES_PER_POOL;
        LASSERT(end_npools <= page_pools.epp_max_pools);

        np_idx = 0;
        while (cur_npools < end_npools) {
                LASSERT(page_pools.epp_pools[cur_npools] == NULL);
                LASSERT(np_idx < npools);
                LASSERT(pools[np_idx] != NULL);

                page_pools.epp_pools[cur_npools++] = pools[np_idx];
                pools[np_idx++] = NULL;
        }

        page_pools.epp_total_pages += npages;
        page_pools.epp_free_pages += npages;
        page_pools.epp_st_lowfree = page_pools.epp_free_pages;

        if (page_pools.epp_total_pages > page_pools.epp_st_max_pages)
                page_pools.epp_st_max_pages = page_pools.epp_total_pages;

        CDEBUG(D_SEC, "add %d pages to total %lu\n", npages,
               page_pools.epp_total_pages);

	spin_unlock(&page_pools.epp_lock);
}

static int enc_pools_add_pages(int npages)
{
	static DEFINE_MUTEX(add_pages_mutex);
	struct page   ***pools;
	int             npools, alloced = 0;
	int             i, j, rc = -ENOMEM;

	if (npages < PTLRPC_MAX_BRW_PAGES)
		npages = PTLRPC_MAX_BRW_PAGES;

	mutex_lock(&add_pages_mutex);

        if (npages + page_pools.epp_total_pages > page_pools.epp_max_pages)
                npages = page_pools.epp_max_pages - page_pools.epp_total_pages;
        LASSERT(npages > 0);

        page_pools.epp_st_grows++;

        npools = npages_to_npools(npages);
        OBD_ALLOC(pools, npools * sizeof(*pools));
        if (pools == NULL)
                goto out;

	for (i = 0; i < npools; i++) {
		OBD_ALLOC(pools[i], PAGE_SIZE);
		if (pools[i] == NULL)
			goto out_pools;

		for (j = 0; j < PAGES_PER_POOL && alloced < npages; j++) {
			pools[i][j] = alloc_page(GFP_NOFS |
						 __GFP_HIGHMEM);
			if (pools[i][j] == NULL)
				goto out_pools;

			alloced++;
		}
	}
	LASSERT(alloced == npages);

        enc_pools_insert(pools, npools, npages);
        CDEBUG(D_SEC, "added %d pages into pools\n", npages);
        rc = 0;

out_pools:
        enc_pools_cleanup(pools, npools);
        OBD_FREE(pools, npools * sizeof(*pools));
out:
        if (rc) {
                page_pools.epp_st_grow_fails++;
                CERROR("Failed to allocate %d enc pages\n", npages);
        }

	mutex_unlock(&add_pages_mutex);
        return rc;
}

static inline void enc_pools_wakeup(void)
{
	assert_spin_locked(&page_pools.epp_lock);

	if (unlikely(page_pools.epp_waitqlen)) {
		LASSERT(waitqueue_active(&page_pools.epp_waitq));
		wake_up_all(&page_pools.epp_waitq);
	}
}

static int enc_pools_should_grow(int page_needed, time64_t now)
{
	/* don't grow if someone else is growing the pools right now,
	 * or the pools has reached its full capacity
	 */
	if (page_pools.epp_growing ||
	    page_pools.epp_total_pages == page_pools.epp_max_pages)
		return 0;

	/* if total pages is not enough, we need to grow */
	if (page_pools.epp_total_pages < page_needed)
		return 1;

	/*
	 * we wanted to return 0 here if there was a shrink just
	 * happened a moment ago, but this may cause deadlock if both
	 * client and ost live on single node.
	 */

	/*
	 * here we perhaps need consider other factors like wait queue
	 * length, idle index, etc. ?
	 */

	/* grow the pools in any other cases */
	return 1;
}

/*
 * Export the number of free pages in the pool
 */
int get_free_pages_in_pool(void)
{
	return page_pools.epp_free_pages;
}
EXPORT_SYMBOL(get_free_pages_in_pool);

/*
 * Let outside world know if enc_pool full capacity is reached
 */
int pool_is_at_full_capacity(void)
{
	return (page_pools.epp_total_pages == page_pools.epp_max_pages);
}
EXPORT_SYMBOL(pool_is_at_full_capacity);

/*
 * we allocate the requested pages atomically.
 */
int sptlrpc_enc_pool_get_pages(struct ptlrpc_bulk_desc *desc)
{
	wait_queue_entry_t waitlink;
	unsigned long   this_idle = -1;
	cfs_time_t      tick = 0;
	long            now;
	int             p_idx, g_idx;
	int             i;

	LASSERT(ptlrpc_is_bulk_desc_kiov(desc->bd_type));
	LASSERT(desc->bd_iov_count > 0);
	LASSERT(desc->bd_iov_count <= page_pools.epp_max_pages);

	/* resent bulk, enc iov might have been allocated previously */
	if (GET_ENC_KIOV(desc) != NULL)
		return 0;

	OBD_ALLOC_LARGE(GET_ENC_KIOV(desc),
		  desc->bd_iov_count * sizeof(*GET_ENC_KIOV(desc)));
	if (GET_ENC_KIOV(desc) == NULL)
		return -ENOMEM;

	spin_lock(&page_pools.epp_lock);

	page_pools.epp_st_access++;
again:
	if (unlikely(page_pools.epp_free_pages < desc->bd_iov_count)) {
		if (tick == 0)
			tick = cfs_time_current();

		now = ktime_get_real_seconds();

		page_pools.epp_st_missings++;
		page_pools.epp_pages_short += desc->bd_iov_count;

		if (enc_pools_should_grow(desc->bd_iov_count, now)) {
			page_pools.epp_growing = 1;

			spin_unlock(&page_pools.epp_lock);
			enc_pools_add_pages(page_pools.epp_pages_short / 2);
			spin_lock(&page_pools.epp_lock);

			page_pools.epp_growing = 0;

			enc_pools_wakeup();
		} else {
			if (page_pools.epp_growing) {
				if (++page_pools.epp_waitqlen >
				    page_pools.epp_st_max_wqlen)
					page_pools.epp_st_max_wqlen =
							page_pools.epp_waitqlen;

				set_current_state(TASK_UNINTERRUPTIBLE);
				init_waitqueue_entry(&waitlink, current);
				add_wait_queue(&page_pools.epp_waitq,
					       &waitlink);

				spin_unlock(&page_pools.epp_lock);
				schedule();
				remove_wait_queue(&page_pools.epp_waitq,
						  &waitlink);
				LASSERT(page_pools.epp_waitqlen > 0);
				spin_lock(&page_pools.epp_lock);
				page_pools.epp_waitqlen--;
			} else {
				/* ptlrpcd thread should not sleep in that case,
				 * or deadlock may occur!
				 * Instead, return -ENOMEM so that upper layers
				 * will put request back in queue. */
				page_pools.epp_st_outofmem++;
				spin_unlock(&page_pools.epp_lock);
				OBD_FREE_LARGE(GET_ENC_KIOV(desc),
					       desc->bd_iov_count *
						sizeof(*GET_ENC_KIOV(desc)));
				GET_ENC_KIOV(desc) = NULL;
				return -ENOMEM;
			}
		}

		LASSERT(page_pools.epp_pages_short >= desc->bd_iov_count);
		page_pools.epp_pages_short -= desc->bd_iov_count;

		this_idle = 0;
		goto again;
	}

        /* record max wait time */
        if (unlikely(tick != 0)) {
                tick = cfs_time_current() - tick;
                if (tick > page_pools.epp_st_max_wait)
                        page_pools.epp_st_max_wait = tick;
        }

        /* proceed with rest of allocation */
        page_pools.epp_free_pages -= desc->bd_iov_count;

        p_idx = page_pools.epp_free_pages / PAGES_PER_POOL;
        g_idx = page_pools.epp_free_pages % PAGES_PER_POOL;

	for (i = 0; i < desc->bd_iov_count; i++) {
		LASSERT(page_pools.epp_pools[p_idx][g_idx] != NULL);
		BD_GET_ENC_KIOV(desc, i).kiov_page =
		       page_pools.epp_pools[p_idx][g_idx];
		page_pools.epp_pools[p_idx][g_idx] = NULL;

		if (++g_idx == PAGES_PER_POOL) {
			p_idx++;
			g_idx = 0;
		}
	}

        if (page_pools.epp_free_pages < page_pools.epp_st_lowfree)
                page_pools.epp_st_lowfree = page_pools.epp_free_pages;

        /*
         * new idle index = (old * weight + new) / (weight + 1)
         */
        if (this_idle == -1) {
                this_idle = page_pools.epp_free_pages * IDLE_IDX_MAX /
                            page_pools.epp_total_pages;
        }
        page_pools.epp_idle_idx = (page_pools.epp_idle_idx * IDLE_IDX_WEIGHT +
                                   this_idle) /
                                  (IDLE_IDX_WEIGHT + 1);

	page_pools.epp_last_access = ktime_get_real_seconds();

	spin_unlock(&page_pools.epp_lock);
	return 0;
}
EXPORT_SYMBOL(sptlrpc_enc_pool_get_pages);

void sptlrpc_enc_pool_put_pages(struct ptlrpc_bulk_desc *desc)
{
	int     p_idx, g_idx;
	int     i;

	LASSERT(ptlrpc_is_bulk_desc_kiov(desc->bd_type));

	if (GET_ENC_KIOV(desc) == NULL)
		return;

	LASSERT(desc->bd_iov_count > 0);

	spin_lock(&page_pools.epp_lock);

	p_idx = page_pools.epp_free_pages / PAGES_PER_POOL;
	g_idx = page_pools.epp_free_pages % PAGES_PER_POOL;

	LASSERT(page_pools.epp_free_pages + desc->bd_iov_count <=
		page_pools.epp_total_pages);
	LASSERT(page_pools.epp_pools[p_idx]);

	for (i = 0; i < desc->bd_iov_count; i++) {
		LASSERT(BD_GET_ENC_KIOV(desc, i).kiov_page != NULL);
		LASSERT(g_idx != 0 || page_pools.epp_pools[p_idx]);
		LASSERT(page_pools.epp_pools[p_idx][g_idx] == NULL);

		page_pools.epp_pools[p_idx][g_idx] =
			BD_GET_ENC_KIOV(desc, i).kiov_page;

		if (++g_idx == PAGES_PER_POOL) {
			p_idx++;
			g_idx = 0;
		}
	}

	page_pools.epp_free_pages += desc->bd_iov_count;

	enc_pools_wakeup();

	spin_unlock(&page_pools.epp_lock);

	OBD_FREE_LARGE(GET_ENC_KIOV(desc),
		 desc->bd_iov_count * sizeof(*GET_ENC_KIOV(desc)));
	GET_ENC_KIOV(desc) = NULL;
}

/*
 * we don't do much stuff for add_user/del_user anymore, except adding some
 * initial pages in add_user() if current pools are empty, rest would be
 * handled by the pools's self-adaption.
 */
int sptlrpc_enc_pool_add_user(void)
{
	int     need_grow = 0;

	spin_lock(&page_pools.epp_lock);
	if (page_pools.epp_growing == 0 && page_pools.epp_total_pages == 0) {
		page_pools.epp_growing = 1;
		need_grow = 1;
	}
	spin_unlock(&page_pools.epp_lock);

	if (need_grow) {
		enc_pools_add_pages(PTLRPC_MAX_BRW_PAGES +
				    PTLRPC_MAX_BRW_PAGES);

		spin_lock(&page_pools.epp_lock);
		page_pools.epp_growing = 0;
		enc_pools_wakeup();
		spin_unlock(&page_pools.epp_lock);
	}
	return 0;
}
EXPORT_SYMBOL(sptlrpc_enc_pool_add_user);

int sptlrpc_enc_pool_del_user(void)
{
        return 0;
}
EXPORT_SYMBOL(sptlrpc_enc_pool_del_user);

static inline void enc_pools_alloc(void)
{
	LASSERT(page_pools.epp_max_pools);
	OBD_ALLOC_LARGE(page_pools.epp_pools,
			page_pools.epp_max_pools *
			sizeof(*page_pools.epp_pools));
}

static inline void enc_pools_free(void)
{
	LASSERT(page_pools.epp_max_pools);
	LASSERT(page_pools.epp_pools);

	OBD_FREE_LARGE(page_pools.epp_pools,
		       page_pools.epp_max_pools *
		       sizeof(*page_pools.epp_pools));
}

int sptlrpc_enc_pool_init(void)
{
	DEF_SHRINKER_VAR(shvar, enc_pools_shrink,
			 enc_pools_shrink_count, enc_pools_shrink_scan);

	page_pools.epp_max_pages = TOTALRAM_PAGES / 8;
	if (enc_pool_max_memory_mb > 0 &&
	    enc_pool_max_memory_mb <= (TOTALRAM_PAGES >> mult))
		page_pools.epp_max_pages = enc_pool_max_memory_mb << mult;

	page_pools.epp_max_pools = npages_to_npools(page_pools.epp_max_pages);

	init_waitqueue_head(&page_pools.epp_waitq);
	page_pools.epp_waitqlen = 0;
	page_pools.epp_pages_short = 0;

        page_pools.epp_growing = 0;

        page_pools.epp_idle_idx = 0;
	page_pools.epp_last_shrink = ktime_get_real_seconds();
	page_pools.epp_last_access = ktime_get_real_seconds();

	spin_lock_init(&page_pools.epp_lock);
        page_pools.epp_total_pages = 0;
        page_pools.epp_free_pages = 0;

        page_pools.epp_st_max_pages = 0;
        page_pools.epp_st_grows = 0;
        page_pools.epp_st_grow_fails = 0;
        page_pools.epp_st_shrinks = 0;
        page_pools.epp_st_access = 0;
        page_pools.epp_st_missings = 0;
        page_pools.epp_st_lowfree = 0;
        page_pools.epp_st_max_wqlen = 0;
        page_pools.epp_st_max_wait = 0;
	page_pools.epp_st_outofmem = 0;

        enc_pools_alloc();
        if (page_pools.epp_pools == NULL)
                return -ENOMEM;

	pools_shrinker = set_shrinker(pools_shrinker_seeks, &shvar);
        if (pools_shrinker == NULL) {
                enc_pools_free();
                return -ENOMEM;
        }

        return 0;
}

void sptlrpc_enc_pool_fini(void)
{
        unsigned long cleaned, npools;

        LASSERT(pools_shrinker);
        LASSERT(page_pools.epp_pools);
        LASSERT(page_pools.epp_total_pages == page_pools.epp_free_pages);

	remove_shrinker(pools_shrinker);

        npools = npages_to_npools(page_pools.epp_total_pages);
        cleaned = enc_pools_cleanup(page_pools.epp_pools, npools);
        LASSERT(cleaned == page_pools.epp_total_pages);

        enc_pools_free();

	if (page_pools.epp_st_access > 0) {
		CDEBUG(D_SEC,
		       "max pages %lu, grows %u, grow fails %u, shrinks %u, access %lu, missing %lu, max qlen %u, max wait %ld/%lu, out of mem %lu\n",
		       page_pools.epp_st_max_pages, page_pools.epp_st_grows,
		       page_pools.epp_st_grow_fails,
		       page_pools.epp_st_shrinks, page_pools.epp_st_access,
		       page_pools.epp_st_missings, page_pools.epp_st_max_wqlen,
		       page_pools.epp_st_max_wait,
		       msecs_to_jiffies(MSEC_PER_SEC),
		       page_pools.epp_st_outofmem);
	}
}


static int cfs_hash_alg_id[] = {
	[BULK_HASH_ALG_NULL]	= CFS_HASH_ALG_NULL,
	[BULK_HASH_ALG_ADLER32]	= CFS_HASH_ALG_ADLER32,
	[BULK_HASH_ALG_CRC32]	= CFS_HASH_ALG_CRC32,
	[BULK_HASH_ALG_MD5]	= CFS_HASH_ALG_MD5,
	[BULK_HASH_ALG_SHA1]	= CFS_HASH_ALG_SHA1,
	[BULK_HASH_ALG_SHA256]	= CFS_HASH_ALG_SHA256,
	[BULK_HASH_ALG_SHA384]	= CFS_HASH_ALG_SHA384,
	[BULK_HASH_ALG_SHA512]	= CFS_HASH_ALG_SHA512,
};
const char * sptlrpc_get_hash_name(__u8 hash_alg)
{
	return cfs_crypto_hash_name(cfs_hash_alg_id[hash_alg]);
}

__u8 sptlrpc_get_hash_alg(const char *algname)
{
	return cfs_crypto_hash_alg(algname);
}

int bulk_sec_desc_unpack(struct lustre_msg *msg, int offset, int swabbed)
{
        struct ptlrpc_bulk_sec_desc *bsd;
        int                          size = msg->lm_buflens[offset];

        bsd = lustre_msg_buf(msg, offset, sizeof(*bsd));
        if (bsd == NULL) {
                CERROR("Invalid bulk sec desc: size %d\n", size);
                return -EINVAL;
        }

        if (swabbed) {
                __swab32s(&bsd->bsd_nob);
        }

        if (unlikely(bsd->bsd_version != 0)) {
                CERROR("Unexpected version %u\n", bsd->bsd_version);
                return -EPROTO;
        }

        if (unlikely(bsd->bsd_type >= SPTLRPC_BULK_MAX)) {
                CERROR("Invalid type %u\n", bsd->bsd_type);
                return -EPROTO;
        }

        /* FIXME more sanity check here */

        if (unlikely(bsd->bsd_svc != SPTLRPC_BULK_SVC_NULL &&
                     bsd->bsd_svc != SPTLRPC_BULK_SVC_INTG &&
                     bsd->bsd_svc != SPTLRPC_BULK_SVC_PRIV)) {
                CERROR("Invalid svc %u\n", bsd->bsd_svc);
                return -EPROTO;
        }

        return 0;
}
EXPORT_SYMBOL(bulk_sec_desc_unpack);

/*
 * Compute the checksum of an RPC buffer payload.  If the return \a buflen
 * is not large enough, truncate the result to fit so that it is possible
 * to use a hash function with a large hash space, but only use a part of
 * the resulting hash.
 */
int sptlrpc_get_bulk_checksum(struct ptlrpc_bulk_desc *desc, __u8 alg,
			      void *buf, int buflen)
{
	struct cfs_crypto_hash_desc	*hdesc;
	int				hashsize;
	unsigned int			bufsize;
	int				i, err;

	LASSERT(ptlrpc_is_bulk_desc_kiov(desc->bd_type));
	LASSERT(alg > BULK_HASH_ALG_NULL && alg < BULK_HASH_ALG_MAX);
	LASSERT(buflen >= 4);

	hdesc = cfs_crypto_hash_init(cfs_hash_alg_id[alg], NULL, 0);
	if (IS_ERR(hdesc)) {
		CERROR("Unable to initialize checksum hash %s\n",
		       cfs_crypto_hash_name(cfs_hash_alg_id[alg]));
		return PTR_ERR(hdesc);
	}

	hashsize = cfs_crypto_hash_digestsize(cfs_hash_alg_id[alg]);

	for (i = 0; i < desc->bd_iov_count; i++) {
		cfs_crypto_hash_update_page(hdesc,
				  BD_GET_KIOV(desc, i).kiov_page,
				  BD_GET_KIOV(desc, i).kiov_offset &
					      ~PAGE_MASK,
				  BD_GET_KIOV(desc, i).kiov_len);
	}

	if (hashsize > buflen) {
		unsigned char hashbuf[CFS_CRYPTO_HASH_DIGESTSIZE_MAX];

		bufsize = sizeof(hashbuf);
		LASSERTF(bufsize >= hashsize, "bufsize = %u < hashsize %u\n",
			 bufsize, hashsize);
		err = cfs_crypto_hash_final(hdesc, hashbuf, &bufsize);
		memcpy(buf, hashbuf, buflen);
	} else {
		bufsize = buflen;
		err = cfs_crypto_hash_final(hdesc, buf, &bufsize);
	}

	return err;
}
