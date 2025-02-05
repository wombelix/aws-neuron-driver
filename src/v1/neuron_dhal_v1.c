// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#include "../neuron_reset.h"
#include "../neuron_dhal.h"
#include "../neuron_topsp.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "fw_io.h"
#include "putils.h"


/* Device Reset Functions */
int nr_initiate_reset_v1(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	uint32_t nc_map = nd->nr.req_pending_head->nc_map;
	int ret = nr_initiate_reset_via_fw(nd, nc_map, 0);
	if (ret) {
		return ret;
	}

	return 0;
}

int nr_initiate_reset_v1_qemu(struct neuron_device *nd)
{
	return 0;
}

int nr_initiate_reset_v1_emu(struct neuron_device *nd)
{
	return nr_initiate_reset_v1(nd);
}

int nr_wait_for_reset_completion_v1(struct neuron_device *nd)
{
	if (no_reset)
		return 0;
	
	int i;
	for (i = 0; i < ndhal->reset_funcs.retry_count; i++) {
		if (fw_io_is_device_ready_v1(nd->npdev.bar0))
			break;
		if (nd->nr.stop)
			return -1;
	}
	if (i == ndhal->reset_funcs.retry_count) {
		return -1;
	}

	// V1 reset seems to wipe out the device ID, so write the device ID at the end of reset
	fw_io_device_id_write(nd->npdev.bar0, nd->device_index);

	return 0;
}

int nr_wait_for_reset_completion_v1_qemu(struct neuron_device *nd)
{
	return nr_wait_for_reset_completion_v1(nd);
}

int nr_wait_for_reset_completion_v1_emu(struct neuron_device *nd)
{
	return nr_wait_for_reset_completion_v1(nd);
}


/* TOPSP Functions */
int ts_nq_init_v1(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
					u32 on_host_memory, u32 dram_channel, u32 dram_region, bool force_alloc_mem,
					struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	pr_err("topsp is not supported in v1\n");
	return -ENOSYS;
}

void ts_nq_destroy_one_v1(struct neuron_device *nd, u8 ts_id)
{
	pr_err("topsp is not supported in v1\n");
	BUG_ON(true);
}


/* Neuron Core Functions */
/**
 * nc_get_axi_offset() - Returns the axi offset
 * 
 * @param nd: neuron device
 * @param nc_index: neuron core index
 * @return u64: the axi offset
 */
static u64 nc_get_axi_offset(struct neuron_device *nd, int nc_index)
{
	return ndhal->nc_funcs.mmap_p_offset + (nc_index * ndhal->nc_funcs.mmap_nc_size);
}

void *nc_get_semaphore_base_v1(struct neuron_device *nd, u8 nc_id)
{
	return nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id);
}

void *nc_get_event_addr_v1(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void *base = nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id) + ndhal->nc_funcs.mmap_nc_event_offset;
	return (base + (event_index * NC_EVENT_SIZE));
}

/* Notification Queue Functions */
u8 nnq_get_nqid_v1(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type)
{
	return (nq_type * MAX_NQ_TYPE) + index;
}

void nnq_set_hwaddr_v1(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa)
{
	void *apb_base = nd->npdev.bar0 + pu_get_relative_offset(nc_id);
	u32 low = (u32)(queue_pa & 0xffffffff);
	u32 high = (u32)(queue_pa >> 32U);

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
}

/* Memory Pool Functions */
void mpset_set_dram_and_mpset_info_v1(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	mpset->num_channels = V1_MAX_DRAM_CHANNELS;
	mpset->mp_device_num_regions = 4;
	device_dram_addr[0] = P_0_DRAM_0_BASE;
	device_dram_addr[1] = P_0_DRAM_1_BASE;
	device_dram_size[0] = P_0_DRAM_0_SIZE;
	device_dram_size[1] = P_0_DRAM_1_SIZE;
}

int mpset_block_carveout_regions_v1(struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	// V1 doesn't have carve out regions
	return 0;
}

/* DMA Ring Functions */
uint32_t ndmar_get_h2t_eng_id_v1(struct neuron_device *nd, uint32_t nc_id)
{
	return (nc_id * V1_DMA_ENG_PER_NC) + (V1_DMA_ENG_PER_NC - 1);
}

bool ndmar_is_nx_ring_v1(uint32_t eng_id, uint32_t q_id)
{
	// V1 doesn't have NX sequencer
	return false;
}

int ndmar_quiesce_queues_v1(struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask)
{
	if (engine_count > DMA_QUIESCE_MAX_ENG)
		return -EINVAL;

	// For V1, reset queues on all 3 engines that belong to NC;
	// Skip h2t because it is shared between models, processes
	u32 start_eng = nc_id * V1_DMA_ENG_PER_NC;
	u32 eng_id;
	for (eng_id = 0; eng_id < V1_DMA_ENG_PER_NC; eng_id++) {
		int qid;
		struct ndma_eng *eng  = ndmar_acquire_engine(nd, start_eng + eng_id);
		if (eng == NULL)
			return -EINVAL;
		for (qid = 0; qid < V1_MAX_DMA_RINGS; qid++) {
			u32 mask = 0x1 << qid;
			// skip h2t because it is shared
			if ((start_eng + eng_id) == ndhal->ndmar_funcs.ndmar_get_h2t_eng_id(nd, nc_id) && qid == ndhal->ndmar_funcs.h2t_qid) {
				continue;
			}
			// check if only the specific queues were requested
			if (engine_count > 0) {
				// if was not specified for this engine or was not requested for this queue
				if (eng_id >= engine_count || (queue_mask[eng_id] & mask) == 0) {
					continue;
				}
			}
			udma_q_pause(&eng->udma.udma_q_m2s[qid]);
			udma_q_pause(&eng->udma.udma_q_s2m[qid]);
		}
		ndmar_release_engine(eng);
	}
	// sleep a bit, ideally we would wait for prefetch and scheduling
	// to get disabled but that requires reads that we don't want to do
	udelay(4);
	return 0;
}
