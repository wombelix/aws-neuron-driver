// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>
#include <linux/mm.h>

#include "udma/udma.h"
#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"
#include "neuron_dhal.h"
#include "neuron_pci.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_dma_wait);
#endif


//#define NUNUSED	__attribute__ ((unused))

struct neuron_device;

static void ndma_ack_completed_desc(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);
}

static inline u32 ndma_mc_pair_to_nc( struct mem_chunk *src_mc, struct mem_chunk *dst_mc)
{
	if (src_mc->mem_location != MEM_LOC_HOST)
		return src_mc->nc_id;
	else
		return dst_mc->nc_id;

	// Note: In the case where this is a host-to-host transfer we end up using the dst_mc's nc_id
}

/**
 * ndma_dma_ctx_get_next_handle()
 *
 *    Return the next dma context handle based on the prev handle.
 *    The previous handle is the handle we will be waiting on when the next transfer is started.
 *    
 *    For an Async transfer the transition progression is NONE->ASYNC1->ASYNC2->ASYNC1.... until we finish the transfer.
 *    Basically starting out with NONE then toggling between ASYNC1 and ASYNC2.
 *
 *    In the case of a synchronous transfer, the prev transfer handle is the SYNC transfer handle
 *    since we will be waiting on the transfer we just started. So the progression is 
 *    SYNC->SYNC->SYNC.... until we finish the transfer.
 *
 */
static inline int ndma_dma_ctx_get_next_handle( int pdma_ctx_handle, int * dma_ctx_handle)
{
	if (pdma_ctx_handle < NEURON_DMA_H2T_CTX_HANDLE_NONE || pdma_ctx_handle > NEURON_DMA_H2T_CTX_HANDLE_ASYNC2) {
		return -EINVAL;
	}

	switch (pdma_ctx_handle) {
		case NEURON_DMA_H2T_CTX_HANDLE_NONE:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_ASYNC1;
		   break;
		case  NEURON_DMA_H2T_CTX_HANDLE_SYNC:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_SYNC;
		   break;
		case  NEURON_DMA_H2T_CTX_HANDLE_ASYNC1:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_ASYNC2;
		   break;
		case  NEURON_DMA_H2T_CTX_HANDLE_ASYNC2:
		   *dma_ctx_handle = NEURON_DMA_H2T_CTX_HANDLE_ASYNC1;
		   break;
	}
	return 0;
}

/**
 * memchunk to dma phy addr
 *
 */
static inline dma_addr_t ndma_mc_to_pa( struct mem_chunk *mc)
{
	if (mc->mem_location == MEM_LOC_HOST)
		return virt_to_phys(mc->va) | ndhal->ndhal_address_map.pci_host_base;   // why isn't this already set???
	else 
		return mc->pa;
}


/**
 * ndma_prefetch_user_pages()
 *
 *    Prefetch user buffer.
 *
 */
static int ndma_prefetch_user_pages( unsigned long start, int nr_pages)
{
	int nr_pinned;
	struct page **p = NULL;
	unsigned int gup_flags = FOLL_WRITE;

	// we technically check access here.

	p = kcalloc( nr_pages, sizeof(struct page *), GFP_KERNEL);

	if (!p) {
		pr_info("failed to allocate memory\n");
		return -ENOMEM;
	}

	nr_pinned = get_user_pages_fast( start, nr_pages, gup_flags, p);
	if (nr_pinned > 0) {
		int i;
		for (i = 0; i < nr_pinned; i++) {
			put_page(p[i]); // need to decide if we put page here or do it later.  If we do it later, need to grab context
		}
	} else {
		pr_info("prefetch failed\n");
	}

	kfree(p);

	return 0;
}


static inline int _ndma_prefetch_user_pages( unsigned long start, int len)
{
	const unsigned long offset = start & (PAGE_SIZE-1);
	int nr_pages = DIV_ROUND_UP(offset + len, PAGE_SIZE);

	return ndma_prefetch_user_pages( start & PAGE_MASK, nr_pages);
}


#define DMA_COMPLETION_MARKER_SIZE sizeof(u32)
#define DMA_COMPLETION_MARKER 0xabcdef01

/*
 * return descriptor for this dma_ctx_handle
 *
 *
 */
static inline void * ndma_memcpy_get_completion_buf( struct ndma_eng *eng, struct ndma_ring *ring, int dma_ctx_handle)
{
	if (eng->used_for_h2t)
		return ring->h2t_completion.ptr + dma_ctx_handle * 2 * DMA_COMPLETION_MARKER_SIZE;
	else
		return kmalloc(DMA_COMPLETION_MARKER_SIZE * 2, GFP_KERNEL);
}

static inline struct ndma_h2t_dma_context * ndma_get_dma_ctx( struct ndma_eng *eng, struct ndma_ring *ring, int dma_ctx_handle)
{
	if (dma_ctx_handle == -1) return NULL;

	if (eng->used_for_h2t)
    	return &ring->h2t_dma_ctx[dma_ctx_handle];
	else  {
		pr_info("allocating descriptor for non-h2t\n");   // FIXME remove at some point
    	return kmalloc( sizeof(struct ndma_h2t_dma_context), GFP_KERNEL);
	}
}

static inline void ndma_release_dma_ctx( struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_h2t_dma_context * dma_ctx)
{
	if (dma_ctx == NULL)
		return;
	if (eng->used_for_h2t) {
		dma_ctx->inuse = false;
	} else {
		if (dma_ctx->completion_ptr != NULL)
			kfree( dma_ctx->completion_ptr);
		kfree( dma_ctx);
	}
}


/*
 * ndma_memcpy_add_completion_desc()
 *
 *    add a completion entry to the ring 
 *
 */
int ndma_memcpy_add_completion_desc( struct ndma_eng *eng, struct ndma_ring *ring, void * completion_buffer)
{
	int ret = 0;
	struct udma_ring_ptr completion;
	volatile u32 *dst;
	volatile u32 *src;

	completion.ptr = completion_buffer;

	dst = (volatile u32 *)(completion.ptr + DMA_COMPLETION_MARKER_SIZE);
	src = (volatile u32 *)completion.ptr;

	// set the src value to the marker
	WRITE_ONCE(*src, DMA_COMPLETION_MARKER);
	WRITE_ONCE(*dst, 0);

	completion.addr = virt_to_phys(completion.ptr) | ndhal->ndhal_address_map.pci_host_base;
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, completion.addr,
					completion.addr + DMA_COMPLETION_MARKER_SIZE,
					DMA_COMPLETION_MARKER_SIZE, UDMA_M2M_BARRIER_NONE, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor on nd%02d for %s q%d\n", eng->nd->device_index, eng->udma.name, ring->qid);
		ret = -1;
		goto error;
	}

error:
	return ret;
}

int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count, void * ptr, bool async, bool is_d2d)
{
	int ret = 0;
	volatile u32 *dst;
	volatile u32 *src;
	u64 i;
	u64 first_wait_time, wait;

	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time(count, async, &first_wait_time, &wait);
	if (is_d2d && !async) {
		first_wait_time = 10; // device-to-device DMA is much faster, just choose a small value independent of number of descriptors
		wait = wait/20; // can probably be set even lower if required
	}

	unsigned long one_loop_sleep = 1; // poll every 1 usecs
	u64 loop = wait / one_loop_sleep + 1;

	dst = (volatile u32 *)(ptr + DMA_COMPLETION_MARKER_SIZE);
	src = (volatile u32 *)ptr;


#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_dma_wait, 1)) {
		ret = -ETIMEDOUT;
		goto error;
	}
#endif

	udelay(first_wait_time);
	for (i = 0; i <= loop; i++) {
		u32 dst_val = READ_ONCE(*dst);
		// this descriptor is executed, meaning all other have completed
		if (dst_val == DMA_COMPLETION_MARKER) {
			// reset in case we are going to use this ring again
			WRITE_ONCE(*dst, 0);                                                        // this isn't strictly necessary but it will detect improper reuse issues
			WRITE_ONCE(*src, DMA_COMPLETION_MARKER);                                    // this isn't strictly necessary but it will detect improper reuse issues
			// while we don't have completion ring, udma uses completion counter
			// for keeping track of which descriptors are free and can be allocated
			// Call ack in order to advance the counter, otherwise we eventually
			// run out of the descriptors to allocate on this ring
			ndma_ack_completed_desc(eng, ring, count);
			break;
		}
		udelay(one_loop_sleep);
	}
	if (i > loop) {
		pr_err("DMA completion timeout on nd%02d for %s q%d desc count %u\n", eng->nd->device_index, eng->udma.name, ring->qid, count);
		ret = -1;
		goto error;
	}

error:
	return ret;
}

int ndma_memcpy64k(struct ndma_eng *eng, struct ndma_ring *ring, dma_addr_t src,
			  dma_addr_t dst, u32 size, int barrier_type)
{
	int ret = -1;

	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, src, dst, size, barrier_type, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return ret;
}

/**
 * ndma_memcpy_chunks()
 *
 *
 *   caveats/notes:
 *     need to figure out inuse & cleanup
 *
 */
static int ndma_memcpy_chunks( struct ndma_eng *eng, struct ndma_ring *ring, struct ndma_h2t_dma_context * dma_ctx)
{
	int        ret;
	dma_addr_t src;
   	dma_addr_t dst;
	u32        chunk_size;
	u32        remaining;
	u32        offset;
	int        pending_transfers;
	bool       done;
	const u32  sync_threshold = DMA_H2T_DESC_COUNT/2 - UDMA_MAX_NUM_CDESC_PER_CACHE_LINE - 1;

	src               = dma_ctx->src;
	dst               = dma_ctx->dst;         
	remaining         = dma_ctx->remaining;
	offset            = dma_ctx->offset;
	done              = false;
   	pending_transfers = 0; 
	chunk_size        = MAX_DMA_DESC_SIZE; 

	while (!done) {
		dma_addr_t src_offset;
		dma_addr_t dst_offset;

		if (remaining <= MAX_DMA_DESC_SIZE) {
			chunk_size = remaining;
		} 

		if ((chunk_size == remaining) || (pending_transfers == sync_threshold)) {
			done    = true;
		}

		src_offset = dma_ctx->smove ? src + offset : src;
		dst_offset = dma_ctx->dmove ? dst + offset : dst;

		ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, chunk_size, ndhal->ndhal_ndma.ndma_get_m2m_barrier_type(done));
		if (ret) { 
			return ret;
		}

		offset    += chunk_size; 
		remaining -= chunk_size;
		pending_transfers++;
		
		//FIXME trace_dma_memcpy(nd, nc_id, src_offset, dst_offset, chunk_size, pending_transfers);
	}

	// write completion descriptor, kick off DMAs, record pending xfers and data outstanding and prefetch if requested
	//
	ret = ndma_memcpy_add_completion_desc( eng, ring, dma_ctx->completion_ptr);
	if (ret) {
		return ret; 
	}

	pending_transfers++;
	dma_ctx->pending_transfers = pending_transfers;
	dma_ctx->outstanding       = dma_ctx->remaining - remaining;
			
	ret = udma_m2m_copy_start(&eng->udma, ring->qid, pending_transfers, pending_transfers);
	if (ret) {
		pr_err("failed to start DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return 0;
}

static int _ndma_memcpy_wait_for_completion( struct neuron_device *nd, u32 nc_id, int qid, struct ndma_eng *eng, struct ndma_ring *ring, 
									  struct ndma_h2t_dma_context * dma_ctx, struct ndma_h2t_dma_context * ndma_ctx)
{
	int ret;
	bool async = (dma_ctx != ndma_ctx);

	while(true) {

		ret = ndma_memcpy_wait_for_completion(eng, ring, dma_ctx->pending_transfers, dma_ctx->completion_ptr, async, false);

		if (ret == 0) 
			return ret;

		// if the memcpy starts within a NeuronCore reset window, 
		// the timeout is possible due to DMA hanging caused by V2 hardware issue.
		// if so, restart DMA and retry the memcpy
		if (!ndhal->ndhal_ndma.ndma_retry_memcpy) {
			break;
		}

		if (!nr_op_in_reset_wnd(dma_ctx->start_time, nd)) {
			break;
		}
		
		pr_info("Failed to copy memory during a NeuronCore reset: nd %d, src %#llx, dst %#llx, size %u. Retrying the copy.\n", 
				nd->device_index, dma_ctx->src, dma_ctx->dst, dma_ctx->size);

		dma_ctx->start_time = get_jiffies_64();

		ret = ndmar_h2t_ring_init(eng, qid);

		if (ret) {
			pr_err("H2T ring init failed on nd %d: ret %d\n", nd->device_index, ret);
			break;
		}
	
		// restart dmas
		// 
		ret = ndma_memcpy_chunks( eng, ring, dma_ctx);
		if (ret)
			break;
		
		if (dma_ctx != ndma_ctx) {
			ret = ndma_memcpy_chunks( eng, ring, ndma_ctx);
			if (ret)
				break;
		}

		async = false;
	}	
	return ret;
}

/** 
 *   
 * Common function for dma content from src to dst
 * if smove is set then the source offset will keep changing after every max desc size is copied
 * if dmove is set then the dest offset will keep changing after every max desc size is copied
 *
 */
static int ndma_memcpy_offset_move(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u32 size, bool smove, bool dmove, 
		                           u64 prefetch_addr, int pwait_handle, int wait_handle)
{
	int ret = 0;

	const int eng_id = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	// for v2 the last one is reserved for collectives
	const int qid = ndhal->ndhal_ndmar.ndmar_get_h2t_qid(nc_id);

	struct ndma_eng   *eng   = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring  *ring  = &queue->ring_info;

	struct ndma_h2t_dma_context * dma_ctx  = ndma_get_dma_ctx( eng, ring, wait_handle);
	struct ndma_h2t_dma_context * pdma_ctx = (eng->used_for_h2t) ? ndma_get_dma_ctx( eng, ring, pwait_handle) : dma_ctx;


	// The h2t_ring_lock two things
	//   1. access to the ring itself
	//   2. usage of the SYNC dma context (basically even though we specify we are using the SYNC ctxt handle outside this routine
	//      the SYNC dma context itself is only used within this routine.
	//
	mutex_lock(&eng->h2t_ring_lock);

    // initialize the DMA context
	dma_ctx->inuse             = true;
	dma_ctx->eng               = eng;
	dma_ctx->ring              = ring;
	dma_ctx->src               = src;
	dma_ctx->dst               = dst;
	dma_ctx->offset            = 0;
	dma_ctx->remaining         = size;
	dma_ctx->pending_transfers = 0;
	dma_ctx->size              = size;
	dma_ctx->smove             = smove;
	dma_ctx->dmove             = dmove;
    dma_ctx->completion_ptr    = ndma_memcpy_get_completion_buf( eng, ring, wait_handle);

	// Sanity check 
	if ((pdma_ctx != NULL) && (!pdma_ctx->inuse)) {
		pr_err("Async dma previous request on nd %d nc %d has invalid state. src %#llx, dst %#llx, size %u.\n", 
				nd->device_index, nc_id, pdma_ctx->src, pdma_ctx->dst, pdma_ctx->size);
		ret = -EINVAL;
		goto fail;
	}

	dma_ctx->start_time = get_jiffies_64();

	while (true) {

		ret = ndma_memcpy_chunks( eng, ring, dma_ctx);

		if (ret) {
			goto fail;
		}

		if (prefetch_addr  && dma_ctx->offset == 0) { 
			_ndma_prefetch_user_pages( prefetch_addr, dma_ctx->size); 
		}

		if (pdma_ctx != NULL) {

			ret = _ndma_memcpy_wait_for_completion( nd, nc_id, qid, eng, ring, pdma_ctx, dma_ctx);

			if (ret) {
				goto fail;
			} else {

				if (dma_ctx->outstanding == dma_ctx->remaining)  {
					break;
				}

				if (dma_ctx != pdma_ctx) {
					pr_err("Async dma request on nd %d nc %d is too large. src %#llx, dst %#llx, size %u.\n", 
							nd->device_index, nc_id, dma_ctx->src, dma_ctx->dst, dma_ctx->size);
					ret = -EINVAL;
					goto fail;
				}	

				dma_ctx->start_time         = get_jiffies_64();
				dma_ctx->remaining         -= dma_ctx->outstanding;
				dma_ctx->offset            += dma_ctx->outstanding;
			}
			
		} else {
			if (dma_ctx->outstanding == dma_ctx->remaining)
				break;
			pr_err("Async dma request on nd %d nc %d is too large\n", nd->device_index, nc_id);
			ret = -EINVAL;
			break;
		}
	}

fail:
	// release the dma_ctx in the event of a failure
	if (ret  && (dma_ctx != pdma_ctx))
		ndma_release_dma_ctx( eng, ring, dma_ctx);

	ndma_release_dma_ctx( eng, ring, pdma_ctx);
	
	mutex_unlock(&eng->h2t_ring_lock);
	return ret;
}

int ndma_memset(struct neuron_device *nd, struct mem_chunk *mc, u64 offset, u32 value, u32 size)
{
	u32 transfer_size, remaining_size;
	struct mem_chunk *memset_mc = nd->memset_mc;
	int ret = 0;

	mutex_lock(&nd->memset_lock);

	// memset the preallocated host memory with the value passed
	transfer_size = size > MEMSET_HOST_BUF_SIZE ? MEMSET_HOST_BUF_SIZE : size;
	memset(memset_mc->va, value, transfer_size);

	// transfer the contents to the memory
	ret = ndma_memcpy_mc(nd, memset_mc, mc, 0, offset, transfer_size);
	if (ret) {
		pr_err("memset memory failed for size:%d\n", transfer_size);
		goto error;
	}
	remaining_size = size - transfer_size;
	if (remaining_size) {
		// copy rest of memroy with zers from the src
		ret = ndma_memcpy_offset_move(nd, mc->nc_id, mc->pa + offset, mc->pa + offset + transfer_size, remaining_size, false, true, 0, 
										NEURON_DMA_H2T_CTX_HANDLE_SYNC, NEURON_DMA_H2T_CTX_HANDLE_SYNC);
		if (ret) {
			pr_err("memset device to device failed for size:%d\n", remaining_size);
			goto error;
		}
	}

error:
	mutex_unlock(&nd->memset_lock);
	return ret;
}

int ndma_memcpy(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u32 size)
{
	return ndma_memcpy_offset_move(nd, nc_id, src, dst, size, true, true, 0, NEURON_DMA_H2T_CTX_HANDLE_SYNC, NEURON_DMA_H2T_CTX_HANDLE_SYNC);
}

int ndma_memcpy_mc_async(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u32 src_offset, u32 dst_offset, u32 size, u64 prefetch_addr, int pdma_ctx_handle, int *dma_ctx_handle)
{
	dma_addr_t src_pa, dst_pa;
	u32 nc_id = 0;
	int ret;

	ret = ndma_dma_ctx_get_next_handle( pdma_ctx_handle, dma_ctx_handle);

	if (ret) {
		return ret;
	}

	nc_id  = ndma_mc_pair_to_nc( src_mc, dst_mc);
	src_pa = ndma_mc_to_pa( src_mc) + src_offset;
	dst_pa = ndma_mc_to_pa( dst_mc) + dst_offset;

	return ndma_memcpy_offset_move(nd, nc_id, src_pa, dst_pa, size, true, true, prefetch_addr, pdma_ctx_handle, *dma_ctx_handle);
}

int ndma_memcpy_mc(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u32 src_offset, u32 dst_offset, u32 size)
{
	dma_addr_t src_pa, dst_pa;
	u32 nc_id = 0; //default use NC 0

	if (src_mc->mem_location == MEM_LOC_HOST)
		src_pa = virt_to_phys(src_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	else {
		src_pa = src_mc->pa;
		nc_id = src_mc->nc_id;
	}
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	} else {
		dst_pa = dst_mc->pa;
		nc_id = dst_mc->nc_id;
	}
	dst_pa += dst_offset;

	// FIXME: H2H memcpy's src and dst mc should have dedicated nc_id such as -1
	if (src_mc->mem_location == MEM_LOC_HOST && dst_mc->mem_location == MEM_LOC_HOST) {
		nc_id = dst_mc->nc_id;
	}

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

/**
 * ndma_memcpy_mc_wait()
 *
 *    This is ugly, but gets the job done.  We have to get nc_id from the MCs, then from there we get engine id, queue id, ring id 
 *    in a bunch of separate calls.  Once we have the ring, we can extract the dma context to wait on...
 *
 *
 */
int ndma_memcpy_mc_wait( struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc, int dma_ctx_handle)
{
	int ret;
	const u32  nc_id         = ndma_mc_pair_to_nc( src_mc, dst_mc);
	const int eng_id         = ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id);
	const int qid            = ndhal->ndhal_ndmar.ndmar_get_h2t_qid(nc_id);
	struct ndma_eng *eng     = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring   = &queue->ring_info;
	struct ndma_h2t_dma_context * dma_ctx;

	// non-h2t we do sync under the covers
	if (!eng->used_for_h2t) {
		return 0;
	}

	dma_ctx  = ndma_get_dma_ctx( eng, ring, dma_ctx_handle);

	if (dma_ctx == NULL) {
		return -EINVAL;
	}

	if (!dma_ctx->inuse) {
		pr_err("trying to wait on async DMA context that is not in use on nd %d nc %d handle %d\n", nd->device_index, nc_id, dma_ctx_handle);
		return -EINVAL;
	}

    ret = _ndma_memcpy_wait_for_completion( nd, nc_id, qid, eng, ring, dma_ctx, dma_ctx);

	ndma_release_dma_ctx( eng, ring, dma_ctx);

	return ret;	
}


int ndma_memcpy_buf_to_mc(struct neuron_device *nd, void *buffer, u32 src_offset,
			  struct mem_chunk *dst_mc, u32 dst_offset, u32 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	src_pa = virt_to_phys(buffer) | ndhal->ndhal_address_map.pci_host_base;
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	} else {
		dst_pa = dst_mc->pa;
		nc_id = dst_mc->nc_id;
	}
	dst_pa += dst_offset;

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

int ndma_memcpy_buf_from_mc(struct neuron_device *nd, void *buffer, u32 dst_offset,
				struct mem_chunk *src_mc, u32 src_offset, u32 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	dst_pa = virt_to_phys(buffer) | ndhal->ndhal_address_map.pci_host_base;
	dst_pa += dst_offset;

	if (src_mc->mem_location == MEM_LOC_HOST) {
		src_pa = virt_to_phys(src_mc->va) | ndhal->ndhal_address_map.pci_host_base;
	} else {
		src_pa = src_mc->pa;
		nc_id = src_mc->nc_id;
	}
	src_pa += src_offset;

	return ndma_memcpy(nd, nc_id, src_pa, dst_pa, size);
}

/**
 * Check whether given address is allocated in host memory by given pid and in given ND.
 */
static bool ndma_is_valid_host_mem_from_nd(u8 nd_index, phys_addr_t pa)
{
	struct neuron_device *nd;
	bool found = false;

	if (nd_index >= MAX_NEURON_DEVICE_COUNT)
		return false;
	nd = neuron_pci_get_device(nd_index);
	if (nd == NULL)
		return false;
	if (!npid_is_attached(nd))
		return false;

	read_lock(&nd->mpset.rblock);
	found = mpset_search_mc(&nd->mpset, pa) != NULL;
	read_unlock(&nd->mpset.rblock);

	return found;
}

bool ndma_is_valid_host_mem(struct neuron_device *nd, phys_addr_t pa)
{
	bool found = false;
	int i;

	// common case - check whether the PA is allocated from the current ND
	found = ndma_is_valid_host_mem_from_nd(nd->device_index, pa);
	if (found)
		goto done;
	// chaining - check neighbor NDs
	found = ndma_is_valid_host_mem_from_nd(nd->device_index - 1, pa);
	if (found)
		goto done;
	found = ndma_is_valid_host_mem_from_nd(nd->device_index + 1, pa);
	if (found)
		goto done;
	// check all devices
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
		// skip already checked devices
		if (i >= nd->device_index - 1 && i <= nd->device_index + 1)
			continue;
		found = ndma_is_valid_host_mem_from_nd(i, pa);
		if (found)
			goto done;
	}

done:
	if (!found)
		pr_err("nd%d:invalid host memory(%#llx) in DMA descriptor\n", nd->device_index, pa);
	return found;
}

int ndma_memcpy_dma_copy_descriptors(struct neuron_device *nd, void *buffer, u32 src_offset,
					 struct mem_chunk *dst_mc, u32 dst_offset, u32 size,
					 u32 desc_type)
{
	u32 curr_size = size;
	union udma_desc *desc = (union udma_desc *)buffer;
	phys_addr_t pa;

	// Check the validity of the desc physical addresses
	while (curr_size > 0) {
		if (desc_type == NEURON_DMA_QUEUE_TYPE_TX)
			pa = desc->tx.buf_ptr;
		else if (desc_type == NEURON_DMA_QUEUE_TYPE_RX)
			pa = desc->rx.buf1_ptr;
		else
			return -1;

		int ret = ndhal->ndhal_ndma.ndma_validate_pa(nd, pa, dst_mc, desc_type);
		if (ret) {
			return ret;
		}

		curr_size = curr_size - sizeof(union udma_desc);
		desc++;
	}

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		memcpy(dst_mc->va + dst_offset, buffer + src_offset, size);
		return 0;
	} else {
		return ndma_memcpy_buf_to_mc(nd, buffer, src_offset, dst_mc, dst_offset, size);
	}
}

static int ndmar_queue_read_state(struct udma_q *hw_q, struct neuron_dma_queue_state *result)
{
	u32 low, high;

	result->sw_status = hw_q->status;
	if (reg_read32(&hw_q->q_regs->rings.status, &result->hw_status))
		return -EIO;

	if (reg_read32(&hw_q->q_regs->rings.drl, &low))
		return -EIO;
	result->length = low & UDMA_M2S_Q_TDRL_OFFSET_MASK;

	if (reg_read32(&hw_q->q_regs->rings.drbp_high, (u32 *)&result->base_addr))
		return -EIO;
	result->base_addr <<= 32;
	if (reg_read32(&hw_q->q_regs->rings.drbp_low, &low))
		return -EIO;
	result->base_addr |= (low & UDMA_M2S_Q_TDRBP_LOW_ADDR_MASK);

	if (reg_read32(&hw_q->q_regs->rings.crbp_high, &high))
		return -EIO;
	if (reg_read32(&hw_q->q_regs->rings.crbp_low, &low))
		return -EIO;
	result->completion_base_addr = ((u64)high << 32) | low;

	if (reg_read32(&hw_q->q_regs->rings.drhp, &low))
		return -EIO;

	result->head_pointer = low & UDMA_M2S_Q_TDRHP_OFFSET_MASK;

	if (reg_read32(&hw_q->q_regs->rings.drtp, &low))
		return -EIO;
	result->tail_pointer = low & UDMA_M2S_Q_TDRTP_OFFSET_MASK;

	if (reg_read32(&hw_q->q_regs->rings.crhp, &low))
		return -EIO;
	result->completion_head = low & UDMA_M2S_Q_TDRHP_OFFSET_MASK;

	return 0;
}

int ndmar_queue_get_state(struct neuron_device *nd, int eng_id, int qid,
			  struct neuron_dma_queue_state *tx, struct neuron_dma_queue_state *rx)
{
	int ret;
	struct ndma_eng *eng;
	struct udma_q *m2s_queue, *s2m_queue;

	eng = &(nd->ndma_engine[eng_id]);
	m2s_queue = &eng->udma.udma_q_m2s[qid];
	s2m_queue = &eng->udma.udma_q_s2m[qid];
	ret = ndmar_queue_read_state(m2s_queue, tx);
	if (ret)
		return ret;
	ret = ndmar_queue_read_state(s2m_queue, rx);

	return ret;
}

const static u64 udma_blocked[] = { offsetof(struct udma_rings_regs, drbp_low), offsetof(struct udma_rings_regs, drbp_high),
									offsetof(struct udma_rings_regs, crbp_low), offsetof(struct udma_rings_regs, crbp_high),
									offsetof(struct udma_rings_regs, drtp_inc) };
int ndma_bar0_blocked_one_engine(u64 base, u64 off)
{
	int qid, dir;
	// check m2s and s2m
	for (dir = 0; dir < 2; dir++) {
		u64 q_start;
		u64 q_size = sizeof(union udma_q_regs);
		if (dir == 0) { // m2s
			q_start = base + offsetof(struct unit_regs_v4, m2s); // start of m2s block
			q_start += offsetof(struct udma_m2s_regs_v4, m2s_q); // start of q registers
		} else { // s2m
			q_start = base + offsetof(struct unit_regs_v4, s2m); // start of s2m block
			q_start += offsetof(struct udma_s2m_regs_v4, s2m_q); // start of q registers
		}
		for (qid = 0; qid < DMA_MAX_Q_V4; qid++) {
			u64 q_off = q_start + q_size * qid;
			int i;
			for (i = 0; i < sizeof(udma_blocked) / sizeof(udma_blocked[0]); i++) {
				if (off == q_off + udma_blocked[i]) {
					return -1;
				}
			}
		}
	}
	return 0;
}
