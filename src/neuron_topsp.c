// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>

#include "v2/address_map.h"
#include "v2/notific.h"

#include "neuron_mempool.h"
#include "neuron_mmap.h"
#include "neuron_device.h"
#include "neuron_arch.h"


int ts_nq_get_info(struct neuron_device *nd, u8 ts_id, u32 nq_type, u32 *head, u32 *phase_bit)
{
	return 0;
}

void ts_nq_update_head(struct neuron_device *nd, u8 ts_id, u32 nq_type, u32 new_head)
{
}

int ts_nq_init(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
	       u32 on_host_memory, u32 dram_channel, u32 dram_region,
	       struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	return 0;
}

void ts_nq_destroy_all(struct neuron_device *nd)
{
}
