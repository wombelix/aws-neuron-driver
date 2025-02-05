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
	if (narch_get_arch() == NEURON_ARCH_V1)
		nq_id = (nq_type * MAX_NQ_TYPE) + index;
	else
		nq_id = (nq_type * V2_MAX_NQ_QUEUES) + index; //for v2 nq is based on queue
	return nq_id;
}

static void nnq_set_hwaddr(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size,
			   u64 queue_pa)
{
	void *apb_base;
	u32 low, high;

	if (narch_get_arch() == NEURON_ARCH_V1) {
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

	if (narch_get_arch() == NEURON_ARCH_V1) {
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

static int nnq_halt(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type)
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
	
	return 0;
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

	mc_free(&nd->nq_mc[nc_id][nq_id]);

	return 0;
}


int nnq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size,
	     u32 on_host_memory, u32 dram_channel, u32 dram_region, bool force_alloc_mem,
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
	if (mc == NULL || force_alloc_mem) {
		struct mem_chunk *_mc = NULL;
		int ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, size, (on_host_memory) ? 0 : size, on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
			       dram_channel, dram_region, nc_id, &_mc);
		if (ret)
			return ret;
		nnq_set_hwaddr(nd, nc_id, eng_index, nq_type, size, _mc->pa);
		nd->nq_mc[nc_id][nq_id] = _mc;
		if (mc) {
			mc_free(&mc);
		}
		mc = _mc;
	}
	if (mc->mem_location == MEM_LOC_HOST)
		*mmap_offset = nmmap_offset(mc);
	else
		*mmap_offset = -1;
	*nq_mc = mc;

	return 0;
}

void nnq_destroy_nc(struct neuron_device *nd, u8 nc_id)
{
	u8 eng_index;
	u8 nq_type;
	u8 ts_id;

	for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			nnq_halt(nd, nc_id, eng_index, nq_type);
		}
	}

	// wait for halted notific queues to drain
	msleep(1);

	for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			nnq_destroy(nd, nc_id, eng_index, nq_type);
		}
	}

	if (narch_get_arch() == NEURON_ARCH_V2) {
		u8 ts_per_nc = V2_TS_PER_DEVICE / V2_NC_PER_DEVICE;
		for (ts_id = nc_id * ts_per_nc; ts_id < (nc_id + 1) * ts_per_nc; ts_id++) {
			ts_nq_destroy_one(nd, ts_id);
		}
	}
}

void nnq_destroy_all(struct neuron_device *nd)
{
	u8 nc_id;

	for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
		nnq_destroy_nc(nd, nc_id);
	}
}



