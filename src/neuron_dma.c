// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "udma/udma.h"
#include "v1/address_map.h"

#include "v2/address_map.h"

#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_dma_wait);
#endif

struct neuron_device;

void ndma_ack_completed_desc(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, ring->qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);
}

#define DMA_COMPLETION_MARKER_SIZE sizeof(u32)
#define DMA_COMPLETION_MARKER 0xabcdef01

/**
 * Wait for completion by start transfer of a DMA between two host memory locations and polling
 * on the host memory for the data to be written.
 */
int ndma_memcpy_wait_for_completion(struct ndma_eng *eng, struct ndma_ring *ring, u32 count)
{
	struct udma_ring_ptr completion;
	int ret = 0;
	volatile u32 *dst;
	volatile u32 *src;
	u64 i;

	// One descriptor takes ~4 usec to transfer (64K at 16G/sec) -  wait 100x longer
	u64 est_wait_time = 4 * count;
	u64 first_wait_time = (8 * est_wait_time) / 10; // first wait will be for 80% of est wait time. This will reduce the number of times we need to poll for completion
	u64 wait = (est_wait_time * 100) - first_wait_time;

	if (narch_get_arch() == NEURON_ARCH_TRN) {
		// for some reason getting a timeout when staging some of
		// BERT training graphs.  Need to investigate: https://t.corp.amazon.com/P55240908
		// In the meantime make the timeout 100x the original
		wait *= 100;
	}
	if (narch_is_qemu())
		wait *= 10 * 1000;
	else if (narch_is_emu())
		wait *= 100 * 1000;

	unsigned long one_loop_sleep = 1; // poll every 1 usecs
	u64 loop = wait / one_loop_sleep + 1;

	// For h2t ring the memory for completion is allocated at init and so use that
	if (eng->used_for_h2t)
		completion.ptr = ring->h2t_completion.ptr;
	else
		completion.ptr = kmalloc(DMA_COMPLETION_MARKER_SIZE * 2, GFP_KERNEL);

	if (!completion.ptr) {
		pr_err("can't allocate memory for completion\n");
		return -1;
	}
	dst = (volatile u32 *)(completion.ptr + DMA_COMPLETION_MARKER_SIZE);
	src = (volatile u32 *)completion.ptr;

	// set the src value to the marker
	WRITE_ONCE(*src, DMA_COMPLETION_MARKER);
	WRITE_ONCE(*dst, 0);

	completion.addr = virt_to_phys(completion.ptr) | PCI_HOST_BASE(eng->nd);
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, completion.addr,
					completion.addr + DMA_COMPLETION_MARKER_SIZE,
					DMA_COMPLETION_MARKER_SIZE, false, false, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		ret = -1;
		goto error;
	}

	count++; // for host to host(completion) descriptor.

	ret = udma_m2m_copy_start(&eng->udma, ring->qid, 1, 1);
	if (ret) {
		pr_err("failed to start DMA copy for %s q%d\n", eng->udma.name, ring->qid);
		goto error;
	}

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
			WRITE_ONCE(*dst, 0);
			WRITE_ONCE(*src, DMA_COMPLETION_MARKER);
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
		pr_err("DMA completion timeout for %s q%d\n", eng->udma.name, ring->qid);
		ret = -1;
		goto error;
	}

error:
	if (completion.ptr && (completion.ptr != ring->h2t_completion.ptr))
		kfree(completion.ptr);

	return ret;
}

static int ndma_memcpy64k(struct ndma_eng *eng, struct ndma_ring *ring, dma_addr_t src,
			  dma_addr_t dst, u32 size, bool set_dmb)
{
	int ret = -1;
	bool use_write_barrier = false;

	use_write_barrier = narch_get_arch() == NEURON_ARCH_TRN && set_dmb;
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, src, dst, size, set_dmb,
					use_write_barrier, false);
	if (ret) {
		pr_err("failed to prepare DMA descriptor for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}
	// Start the DMA
	ret = udma_m2m_copy_start(&eng->udma, ring->qid, 1, 1);
	if (ret) {
		pr_err("failed to start DMA copy for %s q%d\n", eng->udma.name, ring->qid);
		return ret;
	}

	return ret;
}

/*
 * Common function for dma content from src to dst
 * if smove is set then the source offset will keep changing after every max desc size is copied
 * if dmove is set then the dest offset will keep changing after every max desc size is copied
 */
static int ndma_memcpy_offset_move(struct neuron_device *nd, u32 nc_id, dma_addr_t src, dma_addr_t dst, u32 size, bool smove, bool dmove)
{
	u32 chunk_size, remaining, prev_remaining;
	int pending_transfers = 0;
	// max number of usable descriptors - we never allocate the last 16 (max_num_... ) and need to
	// keep one free for checking completion
	const u32 sync_threshold = DMA_H2T_DESC_COUNT - UDMA_MAX_NUM_CDESC_PER_CACHE_LINE - 1;
	u32 offset, prev_offset;
	int ret = 0;

	const int eng_id = ndmar_get_h2t_eng_id(nd, nc_id);
	// for v2 the last one is reserved for collectives
	const int qid = ndmar_get_h2t_qid();

	struct ndma_eng *eng = &nd->ndma_engine[eng_id];
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring = &queue->ring_info;

	chunk_size = size < MAX_DMA_DESC_SIZE ? size : MAX_DMA_DESC_SIZE;
	remaining = size;
	prev_remaining = remaining;
	mutex_lock(&eng->h2t_ring_lock); // TODO: why is this lock needed given the eng lock?
	uint64_t memcpy_start_time = get_jiffies_64();

	for (offset = 0, prev_offset = 0; remaining; offset += chunk_size, remaining -= chunk_size) {
		if (remaining < MAX_DMA_DESC_SIZE)
			chunk_size = remaining;

		dma_addr_t src_offset, dst_offset;
		src_offset = smove ? src + offset : src;
		dst_offset = dmove ? dst + offset : dst;
		if (++pending_transfers == sync_threshold || chunk_size == remaining) {
			// no more room, transfer what's been queued so far OR last chunk
			ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, chunk_size, true);
			if (ret)
				goto fail;
			ret = ndma_memcpy_wait_for_completion(eng, ring, pending_transfers);
			if (ret) {
				// if the memcpy possibly starts within a NeuronCore reset window, 
				// the timeout is possible due to DMA hanging caused by hardware issue.
				// if so, restart DMA and retry the memcpy
				if (narch_get_arch() != NEURON_ARCH_TRN) {
					goto fail;
				}
				if (!nr_op_in_reset_wnd(memcpy_start_time, nd)) {
					goto fail;
				}
				pr_info("Failed to copy memory during a NeuronCore reset: nd %d, src %#llx, dst %#llx, size %u. Retrying the copy.\n", nd->device_index, src, dst, size);
				ret = ndmar_h2t_ring_init(eng, qid);
				if (ret) {
					pr_err("H2T ring init failed on nd %d: ret %d\n", nd->device_index, ret);
					goto fail;
				}
				offset = prev_offset - chunk_size;
				remaining = prev_remaining + chunk_size;
			} else {
				prev_offset = offset;
				prev_remaining = remaining;
			}
			memcpy_start_time = get_jiffies_64();
			pending_transfers = 0;
		} else {
			ret = ndma_memcpy64k(eng, ring, src_offset, dst_offset, chunk_size, false);
			if (ret)
				goto fail;
		}
		trace_dma_memcpy(nd, nc_id, src_offset, dst_offset, chunk_size, pending_transfers);
	}

fail:
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
		ret = ndma_memcpy_offset_move(nd, mc->nc_id, mc->pa + offset, mc->pa + offset + transfer_size, remaining_size, false, true);
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
	return ndma_memcpy_offset_move(nd, nc_id, src, dst, size, true, true);
}

int ndma_memcpy_mc(struct neuron_device *nd, struct mem_chunk *src_mc, struct mem_chunk *dst_mc,
		   u32 src_offset, u32 dst_offset, u32 size)
{
	dma_addr_t src_pa, dst_pa;
	u32 nc_id = 0; //default use NC 0

	if (src_mc->mem_location == MEM_LOC_HOST)
		src_pa = virt_to_phys(src_mc->va) | PCI_HOST_BASE(nd);
	else {
		src_pa = src_mc->pa;
		nc_id = src_mc->nc_id;
	}
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | PCI_HOST_BASE(nd);
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

int ndma_memcpy_buf_to_mc(struct neuron_device *nd, void *buffer, u32 src_offset,
			  struct mem_chunk *dst_mc, u32 dst_offset, u32 size)
{
	dma_addr_t src_pa;
	dma_addr_t dst_pa;
	u32 nc_id = 0;

	src_pa = virt_to_phys(buffer) | PCI_HOST_BASE(nd);
	src_pa += src_offset;

	if (dst_mc->mem_location == MEM_LOC_HOST) {
		dst_pa = virt_to_phys(dst_mc->va) | PCI_HOST_BASE(nd);
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

	dst_pa = virt_to_phys(buffer) | PCI_HOST_BASE(nd);
	dst_pa += dst_offset;

	if (src_mc->mem_location == MEM_LOC_HOST) {
		src_pa = virt_to_phys(src_mc->va) | PCI_HOST_BASE(nd);
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

/** ndma_is_valid_host_mem() - Check whether given PA is valid host memory.
 *
 * A PA is valid only if it is allocated by the current process.
 *
 * Return: True if PA is valid, false otherwise.
 */
static bool ndma_is_valid_host_mem(struct neuron_device *nd, phys_addr_t pa)
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

		// TONGA:
		// west side: PCIEX4_1_BASE: 0x00c00000000000 host: PCIEX8_0_BASE: 0x00400000000000
		// If west side is set then even host bit is set. When mc_alloc is called we set only the host bit
		// and insert into tree.. If some one sets the west side on that PA, then there is no way to check that,
		// since there could be a tdram address that could have the west side set
		// (that will look as though host is also set)
		// SUNDA:
		// similar idea.  Just check for valid address allocated in host memory
		if (narch_get_arch() == NEURON_ARCH_TRN) { 
			if ((pa & V2_PCIE_ALL_RT_MASK) == PCI_HOST_BASE(nd)) {
				if (!ndma_is_valid_host_mem(nd, pa))
					return -EINVAL;
			}
		} else if (((pa & PCIEX8_0_BASE) == PCI_HOST_BASE(nd)) &&
		    ((pa & PCIEX4_1_BASE) != PCIEX4_1_BASE)) {
			if (!ndma_is_valid_host_mem(nd, pa))
				return -EINVAL;
		} else {
			// For V1 need to set the first model start state. If the desc has pa for PE instr fifo, then
			// whichever dma engine queue that has this mc is set to have the pe instr.
			if (narch_get_arch() == NEURON_ARCH_INFERENTIA &&
			    desc_type == NEURON_DMA_QUEUE_TYPE_RX)
				ndmar_set_model_started_v1(nd, pa, dst_mc);
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
