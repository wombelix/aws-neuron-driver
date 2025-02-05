// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Notification Queues:
 *
 * As the engines execute instructions they produce messages in notification queue.
 * These messages are used by applications for monitoring completion of program and
 * also for profiling the program.
 *
 * Notification queue is a circular buffer in host memory - hardware writes to the buffer and
 * applications consumes it by memory mapping the area.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>

#include "v1/address_map.h"
#include "v1/putils.h"

#include "v2/address_map.h"
#include "v2/notific.h"

#include "neuron_mempool.h"
#include "neuron_mmap.h"
#include "neuron_topsp.h"
#include "neuron_nq.h"

static u8 nnq_get_nqid(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type)
{
	u8 nq_id = 0;
	if (narch_get_arch() == NEURON_ARCH_INFERENTIA)
		nq_id = (nq_type * NQ_TYPE_PER_ENGINE) + index;
	else
		nq_id = (nq_type * V2_MAX_NQ_QUEUES) + index; //for v2 nq is based on queue
	return nq_id;
}

static void nnq_set_hwaddr(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size,
			   u64 queue_pa)
{
	void *apb_base;
	u32 low, high;

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		apb_base = nd->npdev.bar0 + pu_get_relative_offset(nc_id);
	} else {
		if (nq_type == NQ_TYPE_TRACE_DMA) {
			apb_base = nd->npdev.bar0 + notific_get_relative_offset_sdma(nc_id, index);
			index = 0; //in the block write it on queue 0 as the base is different
		} else {
			apb_base = nd->npdev.bar0 + notific_get_relative_offset(nc_id);
		}
	}

	low = (u32)(queue_pa & 0xffffffff);
	high = (u32)(queue_pa >> 32U);

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		switch (nq_type) {
		case NQ_TYPE_ERROR:
			pu_write_error_notification_cfg_0(apb_base, low);
			pu_write_error_notification_cfg_1(apb_base, high);
			pu_write_error_notification_cfg_2(apb_base, size);
			break;
		case NQ_TYPE_EVENT:
			pu_write_event_notification_cfg_0(apb_base, low);
			pu_write_event_notification_cfg_1(apb_base, high);
			pu_write_event_notification_cfg_2(apb_base, size);
			break;
		case NQ_TYPE_NOTIFY:
			pu_write_expl_notification_cfg_0(apb_base, index, 0, low);
			pu_write_expl_notification_cfg_1(apb_base, index, 0, high);
			pu_write_expl_notification_cfg_2(apb_base, index, 0, size);
			break;
		case NQ_TYPE_TRACE:
			pu_write_impl_notification_cfg_0(apb_base, index, 0, low);
			pu_write_impl_notification_cfg_1(apb_base, index, 0, high);
			pu_write_impl_notification_cfg_2(apb_base, index, 0, size);
			break;
		default:
			BUG();
		}
	} else {
		notific_write_nq_base_addr_hi(apb_base, index, high);
		notific_write_nq_base_addr_lo(apb_base, index, low);
		notific_write_nq_f_size(apb_base, index, size);
	}
}

static int nnq_destroy(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type)
{
	u8 nq_id;

	if (nd == NULL || nc_id >= NC_PER_DEVICE(nd))
		return -EINVAL;

	nq_id = nnq_get_nqid(nd, nc_id, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	if (nd->nq_mc[nc_id][nq_id] == NULL) {
		return 0;
	}

	nnq_set_hwaddr(nd, nc_id, eng_index, nq_type, 0, 0);

	// sleep 1msec so that hw can drain
	msleep(1);

	mc_free(&nd->nq_mc[nc_id][nq_id]);

	return 0;
}


int nnq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size,
	     u32 on_host_memory, u32 dram_channel, u32 dram_region,
	     struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	// Check that size is power of 2
	if (size & (size - 1)) {
		pr_err("notification ring size must be power of 2");
		return -EINVAL;
	}
	if (nd == NULL || nc_id >= NC_PER_DEVICE(nd))
		return -EINVAL;

	u8 nq_id = nnq_get_nqid(nd, nc_id, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	struct mem_chunk *mc = nd->nq_mc[nc_id][nq_id];
	if (mc == NULL) {
		int ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, size, (on_host_memory) ? 0 : size, on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
			       dram_channel, dram_region, nc_id, &mc);
		if (ret)
			return ret;
		nnq_set_hwaddr(nd, nc_id, eng_index, nq_type, size, mc->pa);
		nd->nq_mc[nc_id][nq_id] = mc;
	}
	if (mc->mem_location == MEM_LOC_HOST)
		*mmap_offset = nmmap_offset(mc);
	else
		*mmap_offset = -1;
	*nq_mc = mc;
	return 0;
}

void nnq_destroy_all(struct neuron_device *nd)
{
	u8 nc_id;
	u8 eng_index;
	u8 nq_type;

	for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
		for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
			for (nq_type = 0; nq_type < NQ_TYPE_PER_ENGINE; nq_type++) {
				nnq_destroy(nd, nc_id, eng_index, nq_type);
			}
		}
	}
}

int nnq_get_nq_info(struct neuron_device *nd, u8 nq_dev_id, u8 use_top_sp, u8 eng_index, u32 nq_type,
		    u32 *head, u32 *phase_bit)
{
	u8 nq_id;
	struct neuron_nq *nq;

	if (narch_get_arch() == NEURON_ARCH_TRN && use_top_sp)
		return ts_nq_get_info(nd, nq_dev_id, nq_type, head, phase_bit);

	nq_id = nnq_get_nqid(nd, nq_dev_id, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;
	nq = &nd->nq[nq_dev_id][nq_id];
	*head = nq->head;
	*phase_bit = nq->phase_bit;
	return 0;
}

void nnq_reset(struct neuron_device *nd)
{
	int i, j;
	for (i = 0; i < MAX_NC_PER_DEVICE; i++)
		for(j = 0; j < MAX_NQ_SUPPORTED; j++) {
			struct neuron_nq *nq = &nd->nq[i][j];
			nq->head = 0;
			if (narch_get_arch() == NEURON_ARCH_INFERENTIA)
				nq->phase_bit = 1; // see nnq_v1_head_update() for why this is 1.
			else
				nq->phase_bit = 0;
		}
}

static void nnq_v1_head_update(struct neuron_device *nd, u8 nc_id, u8 instance, u8 nq_type,
			       u32 new_head)
{
	u8 nq_id = nnq_get_nqid(nd, nc_id, instance, nq_type);
	struct neuron_nq *nq;
	u64 max_size;

	if (nq_id >= MAX_NQ_SUPPORTED)
		return;
	nq = &nd->nq[nc_id][nq_id];

#define MAX_NQ_HEAD_TAIL_DISTANCE 256
	/* flip phase bit if head wraps around max size or if increment size is > expected size.
	 * Application(runtime) keeps distance between head and tail pointers for NQ to avoid
	 * a hardware bug. Hence detecting phase bit solely based on register written value
	 * would lead to error (since hardware and software reported states are different).
	 * For example, when hardware head/tail are 0(init), app would write head register
	 * as 3840(for NQ of size 4096) but keeps its internal pointers as 0.
	 * So the driver also mimics this behaviour to flip phase bit.
	 */
	max_size = nd->nq_mc[nc_id][nq_id]->size - MAX_NQ_HEAD_TAIL_DISTANCE;
	if (new_head >= max_size && nq->head < max_size)
		nq->phase_bit = !nq->phase_bit;
	nq->head = new_head;
}

static void nnq_v2_head_update(struct neuron_device *nd, u8 nc_id, u8 instance, u8 nq_type,
			       bool is_top_sp, u32 new_head)
{
	if (is_top_sp) {
		ts_nq_update_head(nd, nc_id, nq_type, new_head);
	} else {
		u8 nq_id = nnq_get_nqid(nd, nc_id, instance, nq_type);
		if (nq_id >= MAX_NQ_SUPPORTED)
			return;
		struct neuron_nq *nq = &nd->nq[nc_id][nq_id];
		if (new_head < nq->head)
			nq->phase_bit = !nq->phase_bit;
		nq->head = new_head;
	};
}

int nnq_track_register_write(struct neuron_device *nd, int bar, u64 offset, u32 value)
{
	int ret;
	u8 nc_id, instance;
	u32 nq_type;

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		ret = pu_decode_nq_head_reg_access(offset, &nc_id, &nq_type, &instance);
		// if not head register access, then nothing needs to be done
		if (ret)
			return -1;
		nnq_v1_head_update(nd, nc_id, instance, nq_type, value);
	} else {
		bool is_top_sp = false;
		ret = notific_decode_nq_head_reg_access(offset, &nc_id, &nq_type, &instance, &is_top_sp);
		// if not head register access, then nothing needs to be done
		if (ret)
			return -1;
		nnq_v2_head_update(nd, nc_id, instance, nq_type, is_top_sp, value);
	}
	return 0;
}
