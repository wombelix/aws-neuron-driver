// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/string.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "udma/udma.h"
#include "v1/address_map.h"
#include "v2/address_map.h"

#include "neuron_trace.h"
#include "neuron_device.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"

int nc_per_dev_param = 1;

module_param(nc_per_dev_param, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(nc_per_dev_param, "Number of neuron cores");

extern int v1_dma_init(void __iomem *bar0, struct udma *udma, int eng_id);
extern int v2_dma_init(void __iomem *bar0, struct udma *udma, int eng_id);

static struct ndma_eng *ndmar_acquire_engine(struct neuron_device *nd, u32 eng_id)
{
	if (eng_id >= NUM_DMA_ENG_PER_DEVICE)
		return NULL;
	mutex_lock(&nd->ndma_engine[eng_id].lock);
	return &nd->ndma_engine[eng_id];
}

static void ndmar_release_engine(struct ndma_eng *eng)
{
	mutex_unlock(&eng->nd->ndma_engine[eng->eng_id].lock);
}

static struct ndma_queue *ndmar_get_queue(struct ndma_eng *eng, u32 qid)
{
	return &eng->queues[qid];
}

static struct ndma_ring *ndmar_get_ring(struct ndma_queue *queue)
{
	return &queue->ring_info;
}

uint32_t ndmar_get_h2t_eng_id(struct neuron_device *nd, uint32_t nc_id) {
	return  (nc_id * V1_DMA_ENG_PER_NC) + (V1_DMA_ENG_PER_NC - 1);
}

int ndmar_get_h2t_qid(void)
{
       // for v2 the last one is reserved for collectives
       return (narch_get_arch() == NEURON_ARCH_TRN) ? 0 : V1_MAX_DMA_RINGS - 1;
}

u32 ndmar_ring_get_desc_count(u32 v)
{
	if (v < 32) {
		return 64;
	}
	v += UDMA_MAX_NUM_CDESC_PER_CACHE_LINE;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

void ndmar_set_model_started_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc)
{
	// For v1, the first model started state needs to be set. Determine the nc
	// that has the pr iram instr descriptor and when the copy start comes
	// for that queue it would imply that the model is started

	int nc_id;
	u64 tpb_addr = pa & ~P_1_BASE; //for v1 axi port is used

	for (nc_id = 0; nc_id < V1_NC_PER_DEVICE; nc_id++) {
		u64 iram_offset = V1_MMAP_TPB_OFFSET + (nc_id * V1_MMAP_NC_SIZE) +
				  V1_MMAP_PE_IRAM_FIFO_OFFSET;
		if ((tpb_addr >= iram_offset) &&
		    (tpb_addr < (iram_offset + V1_MMAP_PE_IRAM_SIZE))) {
			mc->model_start_tracker.has_pe_iram_inst = true;
			mc->model_start_tracker.nc_id = nc_id;
			break;
		}
	}
	return;
}

/**
 * ndmar_ring_set_mem_chunk() - Set memory chunk backing the queue's physical memory(descriptor ring buffer)
 * @eng: dma engine
 * @qid: dma queue id in the engine for which the mc is being set.
 * @mc: backing memory chunk
 * @port: which axi port(0 or 1) to access the DRAM(for performance)
 * @queue_type: type of the queue(rx, tx, or completion)
 */
static void ndmar_ring_set_mem_chunk(struct ndma_eng *eng, u32 qid, struct mem_chunk *mc, u32 port,
				     enum neuron_dma_queue_type queue_type)
{
	struct ndma_queue *queue = ndmar_get_queue(eng, qid);
	struct ndma_ring *ring = ndmar_get_ring(queue);

	switch (queue_type) {
	case NEURON_DMA_QUEUE_TYPE_TX:
		ring->tx_mc = mc;
		ring->tx.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->tx.addr = virt_to_phys(ring->tx.ptr) | PCI_HOST_BASE(eng->nd);
		} else {
			ring->tx.addr = mc->pa;
			if (port) {
				ring->tx.addr |= P_1_BASE;
			}
		}
		break;
	case NEURON_DMA_QUEUE_TYPE_RX:
		ring->rx_mc = mc;
		ring->rx.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->rx.addr = virt_to_phys(ring->rx.ptr) | PCI_HOST_BASE(eng->nd);
		} else {
			ring->rx.addr = mc->pa;
			if (port) {
				ring->rx.addr |= P_1_BASE;
			}
		}
		break;
	case NEURON_DMA_QUEUE_TYPE_COMPLETION:
		ring->has_compl = true;
		ring->rxc_mc = mc;
		ring->rxc.ptr = mc->va;
		if (mc->mem_location == MEM_LOC_HOST) {
			ring->rxc.addr = virt_to_phys(ring->rxc.ptr) | PCI_HOST_BASE(eng->nd);
		} else {
			ring->rxc.addr = mc->pa;
			if (port) {
				ring->rxc.addr |= P_1_BASE;
			}
		}
		break;
	default:
		break;
	}
}

int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, u32 port)
{
	int ret = -1;
	struct ndma_eng *eng;
	struct ndma_queue *queue;
	struct ndma_ring *ring;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	queue = ndmar_get_queue(eng, qid);
	ring = ndmar_get_ring(queue);

	queue->eng_id = eng_id;
	queue->qid = qid;
	queue->owner = task_tgid_nr(current);
	ring->qid = qid;

	trace_dma_queue_init(nd, eng_id, qid, tx_desc_count, rx_desc_count, tx_mc, rx_mc, rxc_mc,
			     port);

	ndmar_ring_set_mem_chunk(eng, qid, tx_mc, port, NEURON_DMA_QUEUE_TYPE_TX);
	ndmar_ring_set_mem_chunk(eng, qid, rx_mc, port, NEURON_DMA_QUEUE_TYPE_RX);

	if (rxc_mc)
		ndmar_ring_set_mem_chunk(eng, qid, rxc_mc, port, NEURON_DMA_QUEUE_TYPE_COMPLETION);

	ret = udma_m2m_init_queue(&eng->udma, qid, tx_desc_count, rx_desc_count, false, &ring->tx,
				  &ring->rx, rxc_mc != NULL ? &ring->rxc : NULL);

	ndmar_release_engine(eng);
	return ret;
}

void ndmar_handle_process_exit(struct neuron_device *nd, pid_t pid)
{
	int ret, eng_id, qid, dma_eng_per_nd;
	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		dma_eng_per_nd = V1_NUM_DMA_ENG_PER_DEVICE;
	} else {
		int nc_per_dev;
		if (narch_is_emu())
			nc_per_dev = nc_per_dev_param;
		else
			nc_per_dev = V2_NC_PER_DEVICE;
		dma_eng_per_nd = nc_per_dev * V2_DMA_ENG_PER_NC;
	}
	struct mem_chunk *mc = nd->ndma_q_dummy_mc;
	const int desc_count = NDMA_QUEUE_DUMMY_RING_DESC_COUNT;
	for (eng_id = 0; eng_id < dma_eng_per_nd; eng_id++) {
		for (qid = 0; qid < DMA_MAX_Q_MAX; qid++) {
			if (nd->ndma_engine[eng_id].queues[qid].owner != pid) {
				continue;
			}

			// h2t rings are maintained by the driver so dont reset.
			// there cant be any outstanding DMA transaction in h2t since it is a
			// synchronous system call(which will block till finished when a process crashes).
			if (nd->ndma_engine[eng_id].used_for_h2t && qid == ndmar_get_h2t_qid())
				continue;

			ret = ndmar_queue_init(nd, eng_id, qid, desc_count, desc_count, mc, mc, NULL, 0);
			// ignore the error and continue to reset other queues.
			if (ret)
				pr_err("nd%d:dma%d:q%d failed to reset (%d)", nd->device_index, eng_id, qid, ret);
		}
	}
}

int ndmar_ack_completed(struct neuron_device *nd, u32 eng_id, u32 qid, u32 count)
{
	struct ndma_eng *eng;
	struct udma_q *rxq, *txq;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_ack_completed(nd, eng_id, qid, count);

	udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, qid, UDMA_RX, &rxq);

	udma_cdesc_ack(rxq, count);
	udma_cdesc_ack(txq, count);

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_queue_copy_start(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
			   u32 rx_desc_count)
{
	int ret = -1;
	struct ndma_eng *eng;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_queue_copy_start(nd, eng_id, qid, tx_desc_count, rx_desc_count);

	ret = udma_m2m_copy_start(&eng->udma, qid, tx_desc_count, rx_desc_count);
	if (ret)
		pr_err("M2M copy start failed - %d\n", ret);

	// For V1 we need to see if first model was start. The copy start is used to copy
	// pe iram instructions and if that is done, then the state is set to indicate that
	// model has been started
    struct mem_chunk *rx_mc = eng->queues[qid].ring_info.rx_mc;
	if (!ret && rx_mc->model_start_tracker.has_pe_iram_inst) {
		nd->nc_model_started_count[rx_mc->model_start_tracker.nc_id]++;
	}

	ndmar_release_engine(eng);

	return ret;
}

int ndmar_queue_release(struct neuron_device *nd, u32 eng_id, u32 qid)
{
	trace_dma_queue_release(nd, eng_id, qid);
	// inf1 does not need any special handling
	return 0;
}

int ndmar_h2t_ring_alloc(struct neuron_device *nd, int nc_id)
{
	int ret = 0;
	struct mem_chunk *rx_mc = NULL, *tx_mc = NULL, *h2t_completion_mc = NULL;

	const int eng_id = ndmar_get_h2t_eng_id(nd, nc_id);
	const int ndesc = DMA_H2T_DESC_COUNT;
	const u32 ring_size = ndmar_ring_get_desc_count(ndesc) * sizeof(union udma_desc);
	const int qid = ndmar_get_h2t_qid();

	struct ndma_eng *eng  = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	eng->used_for_h2t = true;
	struct ndma_queue *queue = &eng->queues[qid];
	queue->qid = qid;
	queue->eng_id = eng_id;
	struct ndma_ring *ring = &queue->ring_info;
	ring->qid = qid;
	ring->size = ring_size;
	ring->has_compl = false;

	ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, ring_size, MEM_LOC_HOST, 0, 0, nc_id, &rx_mc);
	if (ret) {
		pr_err("can't allocate rx queue for H2T - size %d\n", ring_size);
		goto error;
	}

	ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, ring_size, MEM_LOC_HOST, 0, 0, nc_id, &tx_mc);
	if (ret) {
		pr_err("can't allocate tx queue for H2T - size %d\n", ring_size);
		goto error;
	}

	ndmar_ring_set_mem_chunk(eng, qid, tx_mc, 0, NEURON_DMA_QUEUE_TYPE_TX);
	ndmar_ring_set_mem_chunk(eng, qid, rx_mc, 0, NEURON_DMA_QUEUE_TYPE_RX);

	ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, sizeof(u32) * 2, MEM_LOC_HOST, 0, 0, nc_id, &h2t_completion_mc);
	if (ret) {
		pr_err("can't allocate h2t_completion_mc memory for H2T\n");
		goto error;
	}

	ring->h2t_completion_mc = h2t_completion_mc;
	ring->h2t_completion.ptr = h2t_completion_mc->va;
	ring->h2t_completion.addr = virt_to_phys(ring->h2t_completion.ptr) | PCIEX8_0_BASE;

	mutex_init(&eng->h2t_ring_lock);

	ndmar_release_engine(eng);

	return 0;

error:
	ndmar_release_engine(eng);

	if (rx_mc)
		mc_free(&rx_mc);
	if (tx_mc)
		mc_free(&tx_mc);
	if (h2t_completion_mc)
		mc_free(&h2t_completion_mc);

	return ret;
}

static int ndmar_h2t_ring_init(struct ndma_eng *eng, int qid)
{
	int ret = -1;
	struct ndma_queue *queue;
	struct ndma_ring *ring;
	int ndesc = DMA_H2T_DESC_COUNT;
	u32 alloced_desc = ndmar_ring_get_desc_count(ndesc);

	queue = &eng->queues[qid];
	ring = &queue->ring_info;
	ret = udma_m2m_init_queue(&eng->udma, qid, alloced_desc, alloced_desc, true, &ring->tx,
				  &ring->rx, NULL);
	return ret;
}

int ndmar_eng_set_state(struct neuron_device *nd, int eng_id, u32 state)
{
	struct ndma_eng *eng;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	udma_state_set(&eng->udma, state);

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_eng_get_state(struct neuron_device *nd, int eng_id, struct neuron_dma_eng_state *state)
{
	struct ndma_eng *eng;
	struct udma *udma;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	udma = &eng->udma;

	state->tx_state = udma_state_get(udma, UDMA_TX);
	state->rx_state = udma_state_get(udma, UDMA_RX);
	state->max_queues = udma->num_of_queues_max;
	state->num_queues = udma->num_of_queues;
	state->revision_id = udma->rev_id;

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_queue_get_descriptor_mc(struct neuron_device *nd, u8 eng_id, u8 qid,
				  struct mem_chunk **tx, struct mem_chunk **rx, u32 *tx_size,
				  u32 *rx_size)
{
	struct ndma_eng *eng;
	struct ndma_queue *q;
	struct udma *udma;

	eng = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	q = &eng->queues[qid];
	udma = &eng->udma;

	*tx_size = udma->udma_q_m2s[qid].size;
	*rx_size = udma->udma_q_s2m[qid].size;
	*rx = q->ring_info.rx_mc;
	*tx = q->ring_info.tx_mc;

	ndmar_release_engine(eng);
	return 0;
}

int ndmar_eng_init(struct neuron_device *nd, int eng_id)
{
	int ret = 0;
	struct ndma_eng *eng  = ndmar_acquire_engine(nd, eng_id);
	if (eng == NULL)
		return -EINVAL;

	trace_dma_engine_init(nd, eng_id);

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA)
		ret = v1_dma_init(nd->npdev.bar0, &eng->udma, eng_id);
	else
		ret = v2_dma_init(nd->npdev.bar0, &eng->udma, eng_id);

	if (ret)
		goto done;

done:
	ndmar_release_engine(eng);
	return ret;
}

void ndmar_preinit(struct neuron_device *nd)
{
	int i;
	struct ndma_eng *eng;

	for (i = 0; i < NUM_DMA_ENG_PER_DEVICE; i++) {
		eng = &nd->ndma_engine[i];
		memset((void *)eng, 0, sizeof(struct ndma_eng));
		eng->nd = nd;
		eng->eng_id = i;
		mutex_init(&eng->lock);
	}
}

int ndmar_init(struct neuron_device *nd)
{
	int ret = 0;
	int nc_per_dev;
	int dma_eng_per_nc, dma_eng_per_nd;
	enum neuron_arch arch = narch_get_arch();

	if (nd->dmar_init_done)
		return 0;


	if (arch == NEURON_ARCH_INFERENTIA) {
		nc_per_dev = V1_NC_PER_DEVICE;
		dma_eng_per_nc = V1_DMA_ENG_PER_NC;
		dma_eng_per_nd = V1_NUM_DMA_ENG_PER_DEVICE;
	} else {
		if (narch_is_emu())
			nc_per_dev = nc_per_dev_param;
		else
			nc_per_dev = V2_NC_PER_DEVICE;
		dma_eng_per_nc = V2_DMA_ENG_PER_NC;
		dma_eng_per_nd = nc_per_dev * dma_eng_per_nc;
	}

	// init all seng DMA engines in the ND
	int eng_id;
	for (eng_id = 0; eng_id < dma_eng_per_nd; eng_id++) {
		ret = ndmar_eng_init(nd, eng_id);
		if (ret) {
			pr_err("nd%d: DMA eng%d init failed - %d\n", nd->device_index, eng_id, ret);
			return ret;
		}
	}

	// allocate H2T engines and rings
	int nc_id;
	for (nc_id = 0; nc_id < nc_per_dev; nc_id++) {
		const int eng_id = ndmar_get_h2t_eng_id(nd, nc_id);
		ret = ndmar_eng_init(nd, eng_id);
                if (ret) {
                        pr_err("nd%d: DMA eng%d init failed - %d\n", nd->device_index, eng_id, ret);
                        return ret;
                }

		ret = ndmar_h2t_ring_alloc(nd, nc_id);
		if (ret) {
			pr_err("nd%d:nc%d H2T ring allocation failed - %d\n", nd->device_index, nc_id, ret);
			return ret;
		}

		struct ndma_eng *eng = ndmar_acquire_engine(nd, eng_id);
		if (eng == NULL)
			return -EINVAL;
		const int qid = ndmar_get_h2t_qid();
		ret = ndmar_h2t_ring_init(eng, qid);
		ndmar_release_engine(eng);
		if (ret) {
			pr_err("H2T ring init failed - %d\n", ret);
			return ret;
		}
	}


	nd->dmar_init_done = true;
	return ret;
}

static void ndmar_h2t_ring_free(struct neuron_device *nd, int eng_id)
{
	const int qid = ndmar_get_h2t_qid();
	struct ndma_eng *eng  = ndmar_acquire_engine(nd, eng_id);
	BUG_ON(eng == NULL);
	struct ndma_queue *queue = &eng->queues[qid];
	struct ndma_ring *ring = &queue->ring_info;

	if (ring->tx_mc)
		mc_free(&ring->tx_mc);

	if (ring->rx_mc)
		mc_free(&ring->rx_mc);

	if (ring->rxc_mc)
		mc_free(&ring->rxc_mc);

	ndmar_release_engine(eng);
}

void ndmar_close(struct neuron_device *nd)
{
	const int nc_per_dev  = (narch_get_arch() == NEURON_ARCH_INFERENTIA) ? V1_NC_PER_DEVICE : V2_NC_PER_DEVICE;
	if(!nd->dmar_init_done)
		return;
	int nc_id;
	for (nc_id = 0; nc_id < nc_per_dev; nc_id++) {
		const int eng_id = ndmar_get_h2t_eng_id(nd, nc_id);
		ndmar_h2t_ring_free(nd, eng_id);
	}
	nd->dmar_init_done = false;
}
