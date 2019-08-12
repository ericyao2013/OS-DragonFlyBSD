/*
 * Copyright (c) 2013-2018 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/mountctl.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

#include "hammer2.h"

/*
 * breadth-first search
 */
typedef struct hammer2_chain_save {
	TAILQ_ENTRY(hammer2_chain_save)	entry;
	hammer2_chain_t	*chain;
	int pri;
} hammer2_chain_save_t;

TAILQ_HEAD(hammer2_chain_save_list, hammer2_chain_save);
typedef struct hammer2_chain_save_list hammer2_chain_save_list_t;

typedef struct hammer2_bulkfree_info {
	hammer2_dev_t		*hmp;
	kmem_anon_desc_t	kp;
	hammer2_off_t		sbase;		/* sub-loop iteration */
	hammer2_off_t		sstop;
	hammer2_bmap_data_t	*bmap;
	int			depth;
	long			count_10_00;	/* staged->free	     */
	long			count_11_10;	/* allocated->staged */
	long			count_00_11;	/* (should not happen) */
	long			count_01_11;	/* (should not happen) */
	long			count_10_11;	/* staged->allocated */
	long			count_l0cleans;
	long			count_linadjusts;
	long			count_inodes_scanned;
	long			count_dirents_scanned;
	long			count_dedup_factor;
	long			count_bytes_scanned;
	long			count_chains_scanned;
	long			count_chains_reported;
	long			bulkfree_calls;
	int			bulkfree_ticks;
	hammer2_off_t		adj_free;
	hammer2_tid_t		mtid;
	hammer2_tid_t		saved_mirror_tid;
	time_t			save_time;
	hammer2_chain_save_list_t list;
	hammer2_dedup_t		*dedup;
	int			pri;
} hammer2_bulkfree_info_t;

static int h2_bulkfree_test(hammer2_bulkfree_info_t *info,
			hammer2_blockref_t *bref, int pri);

/*
 * General bulk scan function with callback.  Called with a referenced
 * but UNLOCKED parent.  The parent is returned in the same state.
 */
static
int
hammer2_bulk_scan(hammer2_chain_t *parent,
		  int (*func)(hammer2_bulkfree_info_t *info,
			      hammer2_blockref_t *bref),
		  hammer2_bulkfree_info_t *info)
{
	hammer2_blockref_t bref;
	hammer2_chain_t *chain;
	int first = 1;
	int rup_error;
	int error;

	++info->pri;

	hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS |
				   HAMMER2_RESOLVE_SHARED);
	chain = NULL;
	rup_error = 0;
	error = 0;

	/*
	 * Generally loop on the contents if we have not been flagged
	 * for abort.
	 *
	 * Remember that these chains are completely isolated from
	 * the frontend, so we can release locks temporarily without
	 * imploding.
	 */
	for (;;) {
		error |= hammer2_chain_scan(parent, &chain, &bref, &first,
					    HAMMER2_LOOKUP_NODATA |
					    HAMMER2_LOOKUP_SHARED);

		/*
		 * Handle EOF or other error at current level.  This stops
		 * the bulkfree scan.
		 */
		if (error)
			break;

		/*
		 * Account for dirents before thre data_off test, since most
		 * dirents do not need a data reference.
		 */
		if (bref.type == HAMMER2_BREF_TYPE_DIRENT)
			++info->count_dirents_scanned;

		/*
		 * Ignore brefs without data (typically dirents)
		 */
		if ((bref.data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
			continue;

		/*
		 * Process bref, chain is only non-NULL if the bref
		 * might be recursable (its possible that we sometimes get
		 * a non-NULL chain where the bref cannot be recursed).
		 */
		++info->pri;
		if (h2_bulkfree_test(info, &bref, 1))
			continue;

		if (bref.type == HAMMER2_BREF_TYPE_INODE)
			++info->count_inodes_scanned;

		error |= func(info, &bref);
		if (error)
			break;

		/*
		 * A non-null chain is always returned if it is
		 * recursive, otherwise a non-null chain might be
		 * returned but usually is not when not recursive.
		 */
		if (chain == NULL)
			continue;

		if (chain) {
			info->count_bytes_scanned += chain->bytes;
			++info->count_chains_scanned;

			if (info->count_chains_scanned >=
			    info->count_chains_reported + 1000000 ||
			    (info->count_chains_scanned < 1000000 &&
			     info->count_chains_scanned >=
			     info->count_chains_reported + 100000)) {
				kprintf(" chains %-7ld inodes %-7ld "
					"dirents %-7ld bytes %5ldMB\n",
					info->count_chains_scanned,
					info->count_inodes_scanned,
					info->count_dirents_scanned,
					info->count_bytes_scanned / 1000000);
				info->count_chains_reported =
					info->count_chains_scanned;
			}
		}


		/*
		 * Else check type and setup depth-first scan.
		 *
		 * Account for bytes actually read.
		 */
		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_VOLUME:
		case HAMMER2_BREF_TYPE_FREEMAP:
			++info->depth;
			if (info->depth > 16) {
				hammer2_chain_save_t *save;
				save = kmalloc(sizeof(*save), M_HAMMER2,
					       M_WAITOK | M_ZERO);
				save->chain = chain;
				hammer2_chain_ref(chain);
				TAILQ_INSERT_TAIL(&info->list, save, entry);

				/* guess */
				info->pri += 10;
			} else {
				int savepri = info->pri;

				hammer2_chain_unlock(chain);
				hammer2_chain_unlock(parent);
				info->pri = 0;
				rup_error |=
					hammer2_bulk_scan(chain, func, info);
				info->pri += savepri;
				hammer2_chain_lock(parent,
						   HAMMER2_RESOLVE_ALWAYS |
						   HAMMER2_RESOLVE_SHARED);
				hammer2_chain_lock(chain,
						   HAMMER2_RESOLVE_ALWAYS |
						   HAMMER2_RESOLVE_SHARED);
			}
			--info->depth;
			break;
		case HAMMER2_BREF_TYPE_DATA:
			break;
		default:
			/* does not recurse */
			break;
		}
		if (rup_error & HAMMER2_ERROR_ABORTED)
			break;
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}

	/*
	 * Save with higher pri now that we know what it is.
	 */
	h2_bulkfree_test(info, &parent->bref, info->pri + 1);

	hammer2_chain_unlock(parent);

	return ((error | rup_error) & ~HAMMER2_ERROR_EOF);
}

/*
 * Bulkfree algorithm
 *
 * Repeat {
 *	Chain flush (partial synchronization) XXX removed
 *	Scan the whole topology - build in-memory freemap (mark 11)
 *	Reconcile the in-memory freemap against the on-disk freemap.
 *		ondisk xx -> ondisk 11 (if allocated)
 *		ondisk 11 -> ondisk 10 (if free in-memory)
 *		ondisk 10 -> ondisk 00 (if free in-memory) - on next pass
 * }
 *
 * The topology scan may have to be performed multiple times to window
 * freemaps which are too large to fit in kernel memory.
 *
 * Races are handled using a double-transition (11->10, 10->00).  The bulkfree
 * scan snapshots the volume root's blockset and thus can run concurrent with
 * normal operations, as long as a full flush is made between each pass to
 * synchronize any modified chains (otherwise their blocks might be improperly
 * freed).
 *
 * Temporary memory in multiples of 64KB is required to reconstruct the leaf
 * hammer2_bmap_data blocks so they can later be compared against the live
 * freemap.  Each 64KB block represents 128 x 16KB x 1024 = ~2 GB of storage.
 * A 32MB save area thus represents around ~1 TB.  The temporary memory
 * allocated can be specified.  If it is not sufficient multiple topology
 * passes will be made.
 */

/*
 * Bulkfree callback info
 */
static void hammer2_bulkfree_thread(void *arg __unused);
static void cbinfo_bmap_init(hammer2_bulkfree_info_t *cbinfo, size_t size);
static int h2_bulkfree_callback(hammer2_bulkfree_info_t *cbinfo,
			hammer2_blockref_t *bref);
static int h2_bulkfree_sync(hammer2_bulkfree_info_t *cbinfo);
static void h2_bulkfree_sync_adjust(hammer2_bulkfree_info_t *cbinfo,
			hammer2_off_t data_off, hammer2_bmap_data_t *live,
			hammer2_bmap_data_t *bmap, hammer2_key_t alloc_base);

void
hammer2_bulkfree_init(hammer2_dev_t *hmp)
{
	hammer2_thr_create(&hmp->bfthr, NULL, hmp,
			   hmp->devrepname, -1, -1,
			   hammer2_bulkfree_thread);
}

void
hammer2_bulkfree_uninit(hammer2_dev_t *hmp)
{
	hammer2_thr_delete(&hmp->bfthr);
}

static void
hammer2_bulkfree_thread(void *arg)
{
	hammer2_thread_t *thr = arg;
	hammer2_ioc_bulkfree_t bfi;
	uint32_t flags;

	for (;;) {
		hammer2_thr_wait_any(thr,
				     HAMMER2_THREAD_STOP |
				     HAMMER2_THREAD_FREEZE |
				     HAMMER2_THREAD_UNFREEZE |
				     HAMMER2_THREAD_REMASTER,
				     hz * 60);

		flags = thr->flags;
		cpu_ccfence();
		if (flags & HAMMER2_THREAD_STOP)
			break;
		if (flags & HAMMER2_THREAD_FREEZE) {
			hammer2_thr_signal2(thr, HAMMER2_THREAD_FROZEN,
						 HAMMER2_THREAD_FREEZE);
			continue;
		}
		if (flags & HAMMER2_THREAD_UNFREEZE) {
			hammer2_thr_signal2(thr, 0,
						 HAMMER2_THREAD_FROZEN |
						 HAMMER2_THREAD_UNFREEZE);
			continue;
		}
		if (flags & HAMMER2_THREAD_FROZEN)
			continue;
		if (flags & HAMMER2_THREAD_REMASTER) {
			hammer2_thr_signal2(thr, 0, HAMMER2_THREAD_REMASTER);
			bzero(&bfi, sizeof(bfi));
			bfi.size = 8192 * 1024;
			/* hammer2_bulkfree_pass(thr->hmp, &bfi); */
		}
	}
	thr->td = NULL;
	hammer2_thr_signal(thr, HAMMER2_THREAD_STOPPED);
	/* structure can go invalid at this point */
}

int
hammer2_bulkfree_pass(hammer2_dev_t *hmp, hammer2_chain_t *vchain,
		      hammer2_ioc_bulkfree_t *bfi)
{
	hammer2_bulkfree_info_t cbinfo;
	hammer2_chain_save_t *save;
	hammer2_off_t incr;
	size_t size;
	int error;

	/*
	 * We have to clear the live dedup cache as it might have entries
	 * that are freeable as of now.  Any new entries in the dedup cache
	 * made after this point, even if they become freeable, will have
	 * previously been fully allocated and will be protected by the
	 * 2-stage bulkfree.
	 */
	hammer2_dedup_clear(hmp);

	/*
	 * Setup for free pass using the buffer size specified by the
	 * hammer2 utility, 32K-aligned.
	 */
	bzero(&cbinfo, sizeof(cbinfo));
	size = (bfi->size + HAMMER2_FREEMAP_LEVELN_PSIZE - 1) &
	       ~(size_t)(HAMMER2_FREEMAP_LEVELN_PSIZE - 1);

	/*
	 * Cap at 1/4 physical memory (hammer2 utility will not normally
	 * ever specify a buffer this big, but leave the option available).
	 */
	if (size > kmem_lim_size() * 1024 * 1024 / 4) {
		size = kmem_lim_size() * 1024 * 1024 / 4;
		kprintf("hammer2: Warning: capping bulkfree buffer at %jdM\n",
			(intmax_t)size / (1024 * 1024));
	}

#define HAMMER2_FREEMAP_SIZEDIV	\
	(HAMMER2_FREEMAP_LEVEL1_SIZE / HAMMER2_FREEMAP_LEVELN_PSIZE)
#define HAMMER2_FREEMAP_SIZEMASK	(HAMMER2_FREEMAP_SIZEDIV - 1)

	/*
	 * Cap at the size needed to cover the whole volume to avoid
	 * making an unnecessarily large allocation.
	 */
	if (size > hmp->voldata.volu_size / HAMMER2_FREEMAP_SIZEDIV) {
		size = (hmp->voldata.volu_size + HAMMER2_FREEMAP_SIZEMASK) /
			HAMMER2_FREEMAP_SIZEDIV;
	}

	/*
	 * Minimum bitmap buffer size, then align to a LEVELN_PSIZE (32K)
	 * boundary.
	 */
	if (size < 1024 * 1024)
		size = 1024 * 1024;
	size = (size + HAMMER2_FREEMAP_LEVELN_PSIZE - 1) &
	       ~(size_t)(HAMMER2_FREEMAP_LEVELN_PSIZE - 1);

	cbinfo.hmp = hmp;
	cbinfo.bmap = kmem_alloc_swapbacked(&cbinfo.kp, size, VM_SUBSYS_HAMMER);
	cbinfo.saved_mirror_tid = hmp->voldata.mirror_tid;

	cbinfo.dedup = kmalloc(sizeof(*cbinfo.dedup) * HAMMER2_DEDUP_HEUR_SIZE,
			       M_HAMMER2, M_WAITOK | M_ZERO);

	kprintf("hammer2: bulkfree buf=%jdM\n",
		(intmax_t)size / (1024 * 1024));

	/*
	 * Normalize start point to a 2GB boundary.  We operate on a
	 * 64KB leaf bitmap boundary which represents 2GB of storage.
	 */
	cbinfo.sbase = bfi->sbase;
	if (cbinfo.sbase > hmp->voldata.volu_size)
		cbinfo.sbase = hmp->voldata.volu_size;
	cbinfo.sbase &= ~HAMMER2_FREEMAP_LEVEL1_MASK;
	TAILQ_INIT(&cbinfo.list);

	cbinfo.bulkfree_ticks = ticks;

	/*
	 * Loop on a full meta-data scan as many times as required to
	 * get through all available storage.
	 */
	error = 0;
	while (cbinfo.sbase < hmp->voldata.volu_size) {
		/*
		 * We have enough ram to represent (incr) bytes of storage.
		 * Each 64KB of ram represents 2GB of storage.
		 *
		 * We must also clean out our de-duplication heuristic for
		 * each (incr) bytes of storage, otherwise we wind up not
		 * scanning meta-data for later areas of storage because
		 * they had already been scanned in earlier areas of storage.
		 * Since the ranging is different, we have to restart
		 * the dedup heuristic too.
		 */
		int allmedia;

		cbinfo_bmap_init(&cbinfo, size);
		bzero(cbinfo.dedup, sizeof(*cbinfo.dedup) *
				    HAMMER2_DEDUP_HEUR_SIZE);
		cbinfo.count_inodes_scanned = 0;
		cbinfo.count_dirents_scanned = 0;
		cbinfo.count_bytes_scanned = 0;
		cbinfo.count_chains_scanned = 0;
		cbinfo.count_chains_reported = 0;

		incr = size / HAMMER2_FREEMAP_LEVELN_PSIZE *
		       HAMMER2_FREEMAP_LEVEL1_SIZE;
		if (hmp->voldata.volu_size - cbinfo.sbase <= incr) {
			cbinfo.sstop = hmp->voldata.volu_size;
			allmedia = 1;
		} else {
			cbinfo.sstop = cbinfo.sbase + incr;
			allmedia = 0;
		}
		kprintf("hammer2: pass %016jx-%016jx ",
			(intmax_t)cbinfo.sbase,
			(intmax_t)cbinfo.sstop);
		if (allmedia && cbinfo.sbase == 0)
			kprintf("(all media)\n");
		else if (allmedia)
			kprintf("(remaining media)\n");
		else
			kprintf("(%jdGB of media)\n",
				(intmax_t)incr / (1024L*1024*1024));

		/*
		 * Scan topology for stuff inside this range.
		 *
		 * NOTE - By not using a transaction the operation can
		 *	  run concurrent with the frontend as well as
		 *	  with flushes.
		 *
		 *	  We cannot safely set a mtid without a transaction,
		 *	  and in fact we don't want to set one anyway.  We
		 *	  want the bulkfree to be passive and no interfere
		 *	  with crash recovery.
		 */
#undef HAMMER2_BULKFREE_TRANS	/* undef - don't use transaction */
#ifdef HAMMER2_BULKFREE_TRANS
		hammer2_trans_init(hmp->spmp, 0);
		cbinfo.mtid = hammer2_trans_sub(hmp->spmp);
#else
		cbinfo.mtid = 0;
#endif
		cbinfo.pri = 0;
		error |= hammer2_bulk_scan(vchain, h2_bulkfree_callback,
					   &cbinfo);

		while ((save = TAILQ_FIRST(&cbinfo.list)) != NULL &&
		       error == 0) {
			TAILQ_REMOVE(&cbinfo.list, save, entry);
			cbinfo.pri = 0;
			error |= hammer2_bulk_scan(save->chain,
						   h2_bulkfree_callback,
						   &cbinfo);
			hammer2_chain_drop(save->chain);
			kfree(save, M_HAMMER2);
		}
		while (save) {
			TAILQ_REMOVE(&cbinfo.list, save, entry);
			hammer2_chain_drop(save->chain);
			kfree(save, M_HAMMER2);
			save = TAILQ_FIRST(&cbinfo.list);
		}

		/*
		 * If the complete scan succeeded we can synchronize our
		 * in-memory freemap against live storage.  If an abort
		 * occured we cannot safely synchronize our partially
		 * filled-out in-memory freemap.
		 */
		if (error) {
			kprintf("bulkfree lastdrop %d %d error=0x%04x\n",
				vchain->refs, vchain->core.chain_count, error);
		} else {
			kprintf("bulkfree lastdrop %d %d\n",
				vchain->refs, vchain->core.chain_count);

			error = h2_bulkfree_sync(&cbinfo);

			hammer2_voldata_lock(hmp);
			hammer2_voldata_modify(hmp);
			hmp->voldata.allocator_free += cbinfo.adj_free;
			hammer2_voldata_unlock(hmp);
		}

		/*
		 * Cleanup for next loop.
		 */
#ifdef HAMMER2_BULKFREE_TRANS
		hammer2_trans_done(hmp->spmp, 0);
#endif
		if (error)
			break;
		cbinfo.sbase = cbinfo.sstop;
		cbinfo.adj_free = 0;
	}
	kmem_free_swapbacked(&cbinfo.kp);
	kfree(cbinfo.dedup, M_HAMMER2);
	cbinfo.dedup = NULL;

	bfi->sstop = cbinfo.sbase;

	incr = bfi->sstop / (hmp->voldata.volu_size / 10000);
	if (incr > 10000)
		incr = 10000;

	kprintf("bulkfree pass statistics (%d.%02d%% storage processed):\n",
		(int)incr / 100,
		(int)incr % 100);

	if (error) {
		kprintf("    bulkfree was aborted\n");
	} else {
		kprintf("    transition->free   %ld\n", cbinfo.count_10_00);
		kprintf("    transition->staged %ld\n", cbinfo.count_11_10);
		kprintf("    ERR(00)->allocated %ld\n", cbinfo.count_00_11);
		kprintf("    ERR(01)->allocated %ld\n", cbinfo.count_01_11);
		kprintf("    staged->allocated  %ld\n", cbinfo.count_10_11);
		kprintf("    ~2MB segs cleaned  %ld\n", cbinfo.count_l0cleans);
		kprintf("    linear adjusts     %ld\n",
			cbinfo.count_linadjusts);
		kprintf("    dedup factor       %ld\n",
			cbinfo.count_dedup_factor);
	}

	return error;
}

static void
cbinfo_bmap_init(hammer2_bulkfree_info_t *cbinfo, size_t size)
{
	hammer2_bmap_data_t *bmap = cbinfo->bmap;
	hammer2_key_t key = cbinfo->sbase;
	hammer2_key_t lokey;
	hammer2_key_t hikey;

	lokey = (cbinfo->hmp->voldata.allocator_beg + HAMMER2_SEGMASK64) &
		~HAMMER2_SEGMASK64;
	hikey = cbinfo->hmp->voldata.volu_size & ~HAMMER2_SEGMASK64;

	bzero(bmap, size);
	while (size) {
		bzero(bmap, sizeof(*bmap));
		if (lokey < H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX))
			lokey = H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX);
		if (lokey < H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64)
			lokey = H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64;
		if (key < lokey || key >= hikey) {
			memset(bmap->bitmapq, -1,
			       sizeof(bmap->bitmapq));
			bmap->avail = 0;
			bmap->linear = HAMMER2_SEGSIZE;
		} else {
			bmap->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
		}
		size -= sizeof(*bmap);
		key += HAMMER2_FREEMAP_LEVEL0_SIZE;
		++bmap;
	}
}

static int
h2_bulkfree_callback(hammer2_bulkfree_info_t *cbinfo, hammer2_blockref_t *bref)
{
	hammer2_bmap_data_t *bmap;
	hammer2_off_t data_off;
	uint16_t class;
	size_t bytes;
	int radix;

	/*
	 * Check for signal and allow yield to userland during scan.
	 */
	if (hammer2_signal_check(&cbinfo->save_time))
		return HAMMER2_ERROR_ABORTED;

	/*
	 * Deal with kernel thread cpu or I/O hogging by limiting the
	 * number of chains scanned per second to hammer2_bulkfree_tps.
	 * Ignore leaf records (DIRENT and DATA), no per-record I/O is
	 * involved for those since we don't load their data.
	 */
	if (bref->type != HAMMER2_BREF_TYPE_DATA &&
	    bref->type != HAMMER2_BREF_TYPE_DIRENT) {
		++cbinfo->bulkfree_calls;
		if (cbinfo->bulkfree_calls > hammer2_bulkfree_tps) {
			int dticks = ticks - cbinfo->bulkfree_ticks;
			if (dticks < 0)
				dticks = 0;
			if (dticks < hz) {
				tsleep(&cbinfo->bulkfree_ticks, 0,
				       "h2bw", hz - dticks);
			}
			cbinfo->bulkfree_calls = 0;
			cbinfo->bulkfree_ticks = ticks;
		}
	}

	/*
	 * Calculate the data offset and determine if it is within
	 * the current freemap range being gathered.
	 */
	data_off = bref->data_off & ~HAMMER2_OFF_MASK_RADIX;
	if (data_off < cbinfo->sbase || data_off >= cbinfo->sstop)
		return 0;
	if (data_off < cbinfo->hmp->voldata.allocator_beg)
		return 0;
	if (data_off >= cbinfo->hmp->voldata.volu_size)
		return 0;

	/*
	 * Calculate the information needed to generate the in-memory
	 * freemap record.
	 *
	 * Hammer2 does not allow allocations to cross the L1 (2GB) boundary,
	 * it's a problem if it does.  (Or L0 (2MB) for that matter).
	 */
	radix = (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	KKASSERT(radix != 0);
	bytes = (size_t)1 << radix;
	class = (bref->type << 8) | hammer2_devblkradix(radix);

	if (data_off + bytes > cbinfo->sstop) {
		kprintf("hammer2_bulkfree_scan: illegal 2GB boundary "
			"%016jx %016jx/%d\n",
			(intmax_t)bref->data_off,
			(intmax_t)bref->key,
			bref->keybits);
		bytes = cbinfo->sstop - data_off;	/* XXX */
	}

	/*
	 * Convert to a storage offset relative to the beginning of the
	 * storage range we are collecting.  Then lookup the level0 bmap entry.
	 */
	data_off -= cbinfo->sbase;
	bmap = cbinfo->bmap + (data_off >> HAMMER2_FREEMAP_LEVEL0_RADIX);

	/*
	 * Convert data_off to a bmap-relative value (~4MB storage range).
	 * Adjust linear, class, and avail.
	 *
	 * Hammer2 does not allow allocations to cross the L0 (4MB) boundary,
	 */
	data_off &= HAMMER2_FREEMAP_LEVEL0_MASK;
	if (data_off + bytes > HAMMER2_FREEMAP_LEVEL0_SIZE) {
		kprintf("hammer2_bulkfree_scan: illegal 4MB boundary "
			"%016jx %016jx/%d\n",
			(intmax_t)bref->data_off,
			(intmax_t)bref->key,
			bref->keybits);
		bytes = HAMMER2_FREEMAP_LEVEL0_SIZE - data_off;
	}

	if (bmap->class == 0) {
		bmap->class = class;
		bmap->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
	}

	/*
	 * NOTE: bmap->class does not have to match class.  Classification
	 *	 is relaxed when free space is low, so some mixing can occur.
	 */
#if 0
	/*
	 * XXX removed
	 */
	if (bmap->class != class) {
		kprintf("hammer2_bulkfree_scan: illegal mixed class "
			"%016jx %016jx/%d (%04x vs %04x)\n",
			(intmax_t)bref->data_off,
			(intmax_t)bref->key,
			bref->keybits,
			class, bmap->class);
	}
#endif

	/*
	 * Just record the highest byte-granular offset for now.  Do not
	 * match against allocations which are in multiples of whole blocks.
	 *
	 * Make sure that any in-block linear offset at least covers the
	 * data range.  This can cause bmap->linear to become block-aligned.
	 */
	if (bytes & HAMMER2_FREEMAP_BLOCK_MASK) {
		if (bmap->linear < (int32_t)data_off + (int32_t)bytes)
			bmap->linear = (int32_t)data_off + (int32_t)bytes;
	} else if (bmap->linear >= (int32_t)data_off &&
		   bmap->linear < (int32_t)data_off + (int32_t)bytes) {
		bmap->linear = (int32_t)data_off + (int32_t)bytes;
	}

	/*
	 * Adjust the hammer2_bitmap_t bitmap[HAMMER2_BMAP_ELEMENTS].
	 * 64-bit entries, 2 bits per entry, to code 11.
	 *
	 * NOTE: data_off mask to 524288, shift right by 14 (radix for 16384),
	 *	 and multiply shift amount by 2 for sets of 2 bits.
	 *
	 * NOTE: The allocation can be smaller than HAMMER2_FREEMAP_BLOCK_SIZE.
	 *	 also, data_off may not be FREEMAP_BLOCK_SIZE aligned.
	 */
	while (bytes > 0) {
		hammer2_bitmap_t bmask;
		int bindex;

		bindex = (int)data_off >> (HAMMER2_FREEMAP_BLOCK_RADIX +
					   HAMMER2_BMAP_INDEX_RADIX);
		bmask = (hammer2_bitmap_t)3 <<
			((((int)data_off & HAMMER2_BMAP_INDEX_MASK) >>
			 HAMMER2_FREEMAP_BLOCK_RADIX) << 1);

		/*
		 * NOTE! The (avail) calculation is bitmap-granular.  Multiple
		 *	 sub-granular records can wind up at the same bitmap
		 *	 position.
		 */
		if ((bmap->bitmapq[bindex] & bmask) == 0) {
			if (bytes < HAMMER2_FREEMAP_BLOCK_SIZE) {
				bmap->avail -= HAMMER2_FREEMAP_BLOCK_SIZE;
			} else {
				bmap->avail -= bytes;
			}
			bmap->bitmapq[bindex] |= bmask;
		}
		data_off += HAMMER2_FREEMAP_BLOCK_SIZE;
		if (bytes < HAMMER2_FREEMAP_BLOCK_SIZE)
			bytes = 0;
		else
			bytes -= HAMMER2_FREEMAP_BLOCK_SIZE;
	}
	return 0;
}

/*
 * Synchronize the in-memory bitmap with the live freemap.  This is not a
 * direct copy.  Instead the bitmaps must be compared:
 *
 *	In-memory	Live-freemap
 *	   00		  11 -> 10	(do nothing if live modified)
 *			  10 -> 00	(do nothing if live modified)
 *	   11		  10 -> 11	handles race against live
 *			  ** -> 11	nominally warn of corruption
 * 
 * We must also fixup the hints in HAMMER2_BREF_TYPE_FREEMAP_LEAF.
 */
static int
h2_bulkfree_sync(hammer2_bulkfree_info_t *cbinfo)
{
	hammer2_off_t data_off;
	hammer2_key_t key;
	hammer2_key_t key_dummy;
	hammer2_bmap_data_t *bmap;
	hammer2_bmap_data_t *live;
	hammer2_chain_t *live_parent;
	hammer2_chain_t *live_chain;
	int bmapindex;
	int error;

	kprintf("hammer2_bulkfree - range ");

	if (cbinfo->sbase < cbinfo->hmp->voldata.allocator_beg)
		kprintf("%016jx-",
			(intmax_t)cbinfo->hmp->voldata.allocator_beg);
	else
		kprintf("%016jx-",
			(intmax_t)cbinfo->sbase);

	if (cbinfo->sstop > cbinfo->hmp->voldata.volu_size)
		kprintf("%016jx\n",
			(intmax_t)cbinfo->hmp->voldata.volu_size);
	else
		kprintf("%016jx\n",
			(intmax_t)cbinfo->sstop);
		
	data_off = cbinfo->sbase;
	bmap = cbinfo->bmap;

	live_parent = &cbinfo->hmp->fchain;
	hammer2_chain_ref(live_parent);
	hammer2_chain_lock(live_parent, HAMMER2_RESOLVE_ALWAYS);
	live_chain = NULL;
	error = 0;

	/*
	 * Iterate each hammer2_bmap_data_t line (128 bytes) managing
	 * 4MB of storage.
	 */
	while (data_off < cbinfo->sstop) {
		/*
		 * The freemap is not used below allocator_beg or beyond
		 * volu_size.
		 */

		if (data_off < cbinfo->hmp->voldata.allocator_beg)
			goto next;
		if (data_off >= cbinfo->hmp->voldata.volu_size)
			goto next;

		/*
		 * Locate the freemap leaf on the live filesystem
		 */
		key = (data_off & ~HAMMER2_FREEMAP_LEVEL1_MASK);

		if (live_chain == NULL || live_chain->bref.key != key) {
			if (live_chain) {
				hammer2_chain_unlock(live_chain);
				hammer2_chain_drop(live_chain);
			}
			live_chain = hammer2_chain_lookup(
					    &live_parent,
					    &key_dummy,
					    key,
					    key + HAMMER2_FREEMAP_LEVEL1_MASK,
					    &error,
					    HAMMER2_LOOKUP_ALWAYS);
			if (error) {
				kprintf("hammer2_bulkfree: freemap lookup "
					"error near %016jx, error %s\n",
					(intmax_t)data_off,
					hammer2_error_str(live_chain->error));
				break;
			}
		}
		if (live_chain == NULL) {
			/*
			 * XXX if we implement a full recovery mode we need
			 * to create/recreate missing freemap chains if our
			 * bmap has any allocated blocks.
			 */
			if (bmap->class &&
			    bmap->avail != HAMMER2_FREEMAP_LEVEL0_SIZE) {
				kprintf("hammer2_bulkfree: cannot locate "
					"live leaf for allocated data "
					"near %016jx\n",
					(intmax_t)data_off);
			}
			goto next;
		}
		if (live_chain->error) {
			kprintf("hammer2_bulkfree: unable to access freemap "
				"near %016jx, error %s\n",
				(intmax_t)data_off,
				hammer2_error_str(live_chain->error));
			hammer2_chain_unlock(live_chain);
			hammer2_chain_drop(live_chain);
			live_chain = NULL;
			goto next;
		}

		bmapindex = (data_off & HAMMER2_FREEMAP_LEVEL1_MASK) >>
			    HAMMER2_FREEMAP_LEVEL0_RADIX;
		live = &live_chain->data->bmdata[bmapindex];

		/*
		 * Shortcut if the bitmaps match and the live linear
		 * indicator is sane.  We can't do a perfect check of
		 * live->linear because the only real requirement is that
		 * if it is not block-aligned, that it not cover the space
		 * within its current block which overlaps one of the data
		 * ranges we scan.  We don't retain enough fine-grained
		 * data in our scan to be able to set it exactly.
		 *
		 * TODO - we could shortcut this by testing that both
		 * live->class and bmap->class are 0, and both avails are
		 * set to HAMMER2_FREEMAP_LEVEL0_SIZE (4MB).
		 */
		if (bcmp(live->bitmapq, bmap->bitmapq,
			 sizeof(bmap->bitmapq)) == 0 &&
		    live->linear >= bmap->linear) {
			goto next;
		}
		if (hammer2_debug & 1) {
			kprintf("live %016jx %04d.%04x (avail=%d)\n",
				data_off, bmapindex, live->class, live->avail);
		}

		hammer2_chain_modify(live_chain, cbinfo->mtid, 0, 0);
		live_chain->bref.check.freemap.bigmask = -1;
		cbinfo->hmp->freemap_relaxed = 0;	/* reset heuristic */
		live = &live_chain->data->bmdata[bmapindex];

		h2_bulkfree_sync_adjust(cbinfo, data_off, live, bmap,
					live_chain->bref.key +
					bmapindex *
					HAMMER2_FREEMAP_LEVEL0_SIZE);
next:
		data_off += HAMMER2_FREEMAP_LEVEL0_SIZE;
		++bmap;
	}
	if (live_chain) {
		hammer2_chain_unlock(live_chain);
		hammer2_chain_drop(live_chain);
	}
	if (live_parent) {
		hammer2_chain_unlock(live_parent);
		hammer2_chain_drop(live_parent);
	}
	return error;
}

/*
 * Merge the bulkfree bitmap against the existing bitmap.
 */
static
void
h2_bulkfree_sync_adjust(hammer2_bulkfree_info_t *cbinfo,
			hammer2_off_t data_off, hammer2_bmap_data_t *live,
			hammer2_bmap_data_t *bmap, hammer2_key_t alloc_base)
{
	int bindex;
	int scount;
	hammer2_off_t tmp_off;
	hammer2_bitmap_t lmask;
	hammer2_bitmap_t mmask;

	tmp_off = data_off;

	for (bindex = 0; bindex < HAMMER2_BMAP_ELEMENTS; ++bindex) {
		lmask = live->bitmapq[bindex];	/* live */
		mmask = bmap->bitmapq[bindex];	/* snapshotted bulkfree */
		if (lmask == mmask) {
			tmp_off += HAMMER2_BMAP_INDEX_SIZE;
			continue;
		}

		for (scount = 0;
		     scount < HAMMER2_BMAP_BITS_PER_ELEMENT;
		     scount += 2) {
			if ((mmask & 3) == 0) {
				/*
				 * in-memory 00		live 11 -> 10
				 *			live 10 -> 00
				 *
				 * Storage might be marked allocated or
				 * staged and must be remarked staged or
				 * free.
				 */
				switch (lmask & 3) {
				case 0:	/* 00 */
					break;
				case 1:	/* 01 */
					kprintf("hammer2_bulkfree: cannot "
						"transition m=00/l=01\n");
					break;
				case 2:	/* 10 -> 00 */
					live->bitmapq[bindex] &=
					    ~((hammer2_bitmap_t)2 << scount);
					live->avail +=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					if (live->avail >
					    HAMMER2_FREEMAP_LEVEL0_SIZE) {
						live->avail =
						    HAMMER2_FREEMAP_LEVEL0_SIZE;
					}
					cbinfo->adj_free +=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					++cbinfo->count_10_00;
					hammer2_io_dedup_assert(
						cbinfo->hmp,
						tmp_off |
						HAMMER2_FREEMAP_BLOCK_RADIX,
						HAMMER2_FREEMAP_BLOCK_SIZE);
					break;
				case 3:	/* 11 -> 10 */
					live->bitmapq[bindex] &=
					    ~((hammer2_bitmap_t)1 << scount);
					++cbinfo->count_11_10;
					hammer2_io_dedup_delete(
						cbinfo->hmp,
						HAMMER2_BREF_TYPE_DATA,
						tmp_off |
						HAMMER2_FREEMAP_BLOCK_RADIX,
						HAMMER2_FREEMAP_BLOCK_SIZE);
					break;
				}
			} else if ((mmask & 3) == 3) {
				/*
				 * in-memory 11		live 10 -> 11
				 *			live ** -> 11
				 *
				 * Storage might be incorrectly marked free
				 * or staged and must be remarked fully
				 * allocated.
				 */
				switch (lmask & 3) {
				case 0:	/* 00 */
					++cbinfo->count_00_11;
					cbinfo->adj_free -=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					live->avail -=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					if ((int32_t)live->avail < 0)
						live->avail = 0;
					break;
				case 1:	/* 01 */
					++cbinfo->count_01_11;
					break;
				case 2:	/* 10 -> 11 */
					++cbinfo->count_10_11;
					break;
				case 3:	/* 11 */
					break;
				}
				live->bitmapq[bindex] |=
					((hammer2_bitmap_t)3 << scount);
			}
			mmask >>= 2;
			lmask >>= 2;
			tmp_off += HAMMER2_FREEMAP_BLOCK_SIZE;
		}
	}

	/*
	 * Determine if the live bitmap is completely free and reset its
	 * fields if so.  Otherwise check to see if we can reduce the linear
	 * offset.
	 */
	for (bindex = HAMMER2_BMAP_ELEMENTS - 1; bindex >= 0; --bindex) {
		if (live->bitmapq[bindex] != 0)
			break;
	}
	if (bindex < 0) {
		/*
		 * Completely empty, reset entire segment
		 */
#if 0
		kprintf("hammer2: cleanseg %016jx.%04x (%d)\n",
			alloc_base, live->class, live->avail);
#endif
		live->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
		live->class = 0;
		live->linear = 0;
		++cbinfo->count_l0cleans;
	} else if (bindex < 7) {
		/*
		 * Partially full, bitmapq[bindex] != 0.  Our bulkfree pass
		 * does not record enough information to set live->linear
		 * exactly.
		 *
		 * NOTE: Setting live->linear to a sub-block (16K) boundary
		 *	 forces the live code to iterate to the next fully
		 *	 free block.  It does NOT mean that all blocks above
		 *	 live->linear are available.
		 *
		 *	 Setting live->linear to a fragmentary (less than
		 *	 16K) boundary allows allocations to iterate within
		 *	 that sub-block.
		 */
		if (live->linear < bmap->linear &&
		    ((live->linear ^ bmap->linear) &
		     ~HAMMER2_FREEMAP_BLOCK_MASK) == 0) {
			/*
			 * If greater than but still within the same
			 * sub-block as live we can adjust linear upward.
			 */
			live->linear = bmap->linear;
			++cbinfo->count_linadjusts;
		} else {
			/*
			 * Otherwise adjust to the nearest higher or same
			 * sub-block boundary.  The live system may have
			 * bounced live->linear around so we cannot make any
			 * assumptions with regards to available fragmentary
			 * allocations.
			 */
			live->linear =
				(bmap->linear + HAMMER2_FREEMAP_BLOCK_MASK) &
				~HAMMER2_FREEMAP_BLOCK_MASK;
			++cbinfo->count_linadjusts;
		}
	} else {
		/*
		 * Completely full, effectively disable the linear iterator
		 */
		live->linear = HAMMER2_SEGSIZE;
	}

#if 0
	if (bmap->class) {
		kprintf("%016jx %04d.%04x (avail=%7d) "
			"%08x %08x %08x %08x %08x %08x %08x %08x\n",
			(intmax_t)data_off,
			(int)((data_off &
			       HAMMER2_FREEMAP_LEVEL1_MASK) >>
			      HAMMER2_FREEMAP_LEVEL0_RADIX),
			bmap->class,
			bmap->avail,
			bmap->bitmap[0], bmap->bitmap[1],
			bmap->bitmap[2], bmap->bitmap[3],
			bmap->bitmap[4], bmap->bitmap[5],
			bmap->bitmap[6], bmap->bitmap[7]);
	}
#endif
}

/*
 * BULKFREE DEDUP HEURISTIC
 *
 * WARNING! This code is SMP safe but the heuristic allows SMP collisions.
 *	    All fields must be loaded into locals and validated.
 */
static
int
h2_bulkfree_test(hammer2_bulkfree_info_t *cbinfo, hammer2_blockref_t *bref,
		 int pri)
{
	hammer2_dedup_t *dedup;
	int best;
	int n;
	int i;

	n = hammer2_icrc32(&bref->data_off, sizeof(bref->data_off));
	dedup = cbinfo->dedup + (n & (HAMMER2_DEDUP_HEUR_MASK & ~7));

	for (i = best = 0; i < 8; ++i) {
		if (dedup[i].data_off == bref->data_off) {
			if (dedup[i].ticks < pri)
				dedup[i].ticks = pri;
			if (pri == 1)
				cbinfo->count_dedup_factor += dedup[i].ticks;
			return 1;
		}
		if (dedup[i].ticks < dedup[best].ticks)
			best = i;
	}
	dedup[best].data_off = bref->data_off;
	dedup[best].ticks = pri;

	return 0;
}
