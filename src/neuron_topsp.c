// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Each neuron device has N number of TOP_SPs. (inf1 does not have it).
 *
 * Engine:
 * -------
 * TOP_SP has one engine which can execute instructions, mainly to orchestrate collective operations
 * (e.g. allreduce) on a neuron device. Each engine's instruction stream is fed through DMA.
 *
 * Notifications:
 * -------------
 * As the engines execute instructions they produce messages in notification queue.
 * These messages are used by applications for monitoring completion of program and
 * also for profiling the program.
 *
 * Notification queue is a circular buffer in device memory - hardware writes to the buffer and
 * applications consumes it by device DMA (NEURON_IOCTL_MEM_BUF_COPY).
 *
 * Semaphores and events:
 * ---------------------
 * For synchronization between hardware blocks and software, TOP_SP provides two type
 * synchronization hardware primitives, semaphores and events. Events can be considered simple
 * bitmap which hold either 1 or 0. Semaphores hold any value in signed 32 bit range. Engines can be
 * programmed with instructions which can wait for semaphore to reach a certain value or a
 * particular event is set. Applications can use this to manipulate execution of the program.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>

#include "v2/address_map.h"
#include "v2/notific.h"

#include "neuron_mempool.h"
#include "neuron_mmap.h"
#include "neuron_device.h"
#include "neuron_arch.h"
#include "neuron_dhal.h"

u8 ts_nq_get_nqid(struct neuron_device *nd, u8 index, u32 nq_type)
{
	u8 nq_id = 0;
	nq_id = (nq_type * V2_MAX_NQ_QUEUES) + index; // for v2 nq is based on queue
	return nq_id;
}

void ts_nq_set_hwaddr(struct neuron_device *nd, u8 ts_id, u8 index, u32 nq_type, u32 size,
			     u64 queue_pa)
{
	void *apb_base;
	u32 low, high;

	apb_base = nd->npdev.bar0 + notific_get_relative_offset_topsp(ts_id);

	low = (u32)(queue_pa & 0xffffffff);
	high = (u32)(queue_pa >> 32U);

	notific_write_nq_base_addr_hi(apb_base, index, high);
	notific_write_nq_base_addr_lo(apb_base, index, low);
	notific_write_nq_f_size(apb_base, index, size);
}

int ts_nq_destroy(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type)
{
	u8 nq_id;

	if (nd == NULL || ts_id >= ndhal->topsp_funcs.ts_per_device)
		return -EINVAL;

	nq_id = ts_nq_get_nqid(nd, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	if (nd->ts_nq_mc[ts_id][nq_id] == NULL)
		return 0;

	ts_nq_set_hwaddr(nd, ts_id, eng_index, nq_type, 0, 0);

	mc_free(&nd->ts_nq_mc[ts_id][nq_id]);
	return 0;
}

void ts_nq_destroy_all(struct neuron_device *nd)
{
	u8 ts_id;
	for (ts_id = 0; ts_id < ndhal->topsp_funcs.ts_per_device; ts_id++) {
		ndhal->topsp_funcs.ts_nq_destroy_one(nd, ts_id);
	}
}
