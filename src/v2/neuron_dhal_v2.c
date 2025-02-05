// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/
#include <linux/delay.h>

#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_topsp.h"
#include "../neuron_mmap.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "notific.h"

#define NR_RESET_RETRY_SLEEP_MS 100

/* Device Reset Functions */
static void nr_get_tpb_reset_map(uint32_t nc_map, uint32_t *tpb_reset_map)
{
	int i, j;

	// Build the tpb reset map if we are not performing a device reset
	if (nc_map != NEURON_NC_MAP_DEVICE) {
		for (i = 0; i < MAX_NC_PER_DEVICE; i++) {
			if ((1 << i) & nc_map) {
				// Add this tpb to the reset map
				*tpb_reset_map |= (1 << i);
				int ts_per_nc = V2_TS_PER_DEVICE / V2_NC_PER_DEVICE;
				// Add all top sps owned by this tpb to the reset map
				for (j = i * ts_per_nc; j < (i + 1) * ts_per_nc; j++) {
					*tpb_reset_map |= (1 << (8 + j));
				}
			}
		}
	}
}

int nr_initiate_reset_v2(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	uint32_t nc_map = nd->nr.req_pending_head->nc_map;
	uint32_t tpb_reset_map = 0;
	nr_get_tpb_reset_map(nc_map, &tpb_reset_map);

	int ret = nr_initiate_reset_via_fw(nd, nc_map, tpb_reset_map);
	if (ret) {
		return ret;
	}

	return 0;
}

int nr_initiate_reset_v2_qemu(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	volatile void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_0_RESERVED1_RELBASE + 0x10;
	writel(1, (volatile uint32_t *)addr);  

	uint32_t nc_map = nd->nr.req_pending_head->nc_map;
	uint32_t tpb_reset_map = 0;
	nr_get_tpb_reset_map(nc_map, &tpb_reset_map);

	int ret = nr_initiate_reset_via_fw(nd, nc_map, tpb_reset_map);
	if (ret) {
		return ret;
	}

	return 0; 
}

int nr_initiate_reset_v2_emu(struct neuron_device *nd)
{
	return nr_initiate_reset_v2(nd);
}

int nr_wait_for_reset_completion_v2(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	int i;
	void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET + V2_APB_IOFAB_RELBASE + V2_APB_IOFAB_MISC_RAM_RELBASE + V2_FW_IO_REG_FW_STATUS_OFFSET;

	for (i = 0; i < ndhal->reset_funcs.retry_count; i++) {
		bool reset_in_progress = true;
		u32 status;

		if (fw_io_read_csr_array(&addr, &status, 1, false) == 0)
			reset_in_progress = status & V2_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK;
		if (!reset_in_progress)
			return 0;
		if (nr_msleep_stoppable(nd, NR_RESET_RETRY_SLEEP_MS * i)) 
			return -1;
	}
	return -1;
}

int nr_wait_for_reset_completion_v2_qemu(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	int i;
	void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_0_RESERVED1_RELBASE + 0x10;

	for (i = 0; i < ndhal->reset_funcs.retry_count; i++) {
		bool reset_in_progress = true;

		reset_in_progress = readl((volatile uint32_t *)addr);
		msleep(2 * 1000);

		if (!reset_in_progress)
			return 0;
		if (nr_msleep_stoppable(nd, NR_RESET_RETRY_SLEEP_MS * i)) 
			return -1;
	}
	return -1;   
}

int nr_wait_for_reset_completion_v2_emu(struct neuron_device *nd)
{
	return nr_wait_for_reset_completion_v2(nd);
}


/* TOPSP Functions */
int ts_nq_init_v2(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
				u32 on_host_memory, u32 dram_channel, u32 dram_region,
				bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset)
{
	// Check that size is power of 2
	if (size & (size - 1)) {
		pr_err("notification ring size must be power of 2");
		return -EINVAL;
	}

	if (nd == NULL || ts_id >= ndhal->topsp_funcs.ts_per_device)
		return -EINVAL;

	u8 nq_id = ts_nq_get_nqid(nd, eng_index, nq_type);
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	struct mem_chunk *mc = nd->ts_nq_mc[ts_id][nq_id];
	if (mc == NULL || force_alloc_mem) {
		struct mem_chunk *_mc = NULL;
		u32 nc_id = ts_id / (V2_TS_PER_DEVICE / V2_NC_PER_DEVICE);
		int ret = mc_alloc_align(nd, MC_LIFESPAN_DEVICE, size, (on_host_memory) ? 0 : size, on_host_memory ? MEM_LOC_HOST : MEM_LOC_DEVICE,
				   dram_channel, dram_region, nc_id, &_mc);
		if (ret)
			return ret;
		ts_nq_set_hwaddr(nd, ts_id, eng_index, nq_type, size, _mc->pa);
		nd->ts_nq_mc[ts_id][nq_id] = _mc;
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

void ts_nq_destroy_one_v2(struct neuron_device *nd, u8 ts_id)
{
	u8 eng_index;
	u8 nq_type;
	for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
		for (nq_type = 0; nq_type < MAX_NQ_TYPE; nq_type++) {
			ts_nq_destroy(nd, ts_id, eng_index, nq_type);
		}
	}
}


/* Neuron Core Functions */
void *nc_get_semaphore_base_v2(struct neuron_device *nd, u8 nc_id)
{
	return nd->npdev.bar0 + V2_PCIE_BAR0_TPB_0_OFFSET + (V2_PCIE_BAR0_TPB_0_SIZE * nc_id);
}

void *nc_get_event_addr_v2(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void *base = nd->npdev.bar0 + V2_PCIE_BAR0_TPB_0_OFFSET +
			   (V2_PCIE_BAR0_TPB_0_SIZE * nc_id) + ndhal->nc_funcs.mmap_nc_event_offset;
	return (base + (event_index * NC_EVENT_SIZE));
}

/* Notification Queue Functions */
u8 nnq_get_nqid_v2(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type)
{
    return (nq_type * V2_MAX_NQ_QUEUES) + index;
}

void nnq_set_hwaddr_v2(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa)
{
	void *apb_base;
	if (nq_type == NQ_TYPE_TRACE_DMA) {
		apb_base = nd->npdev.bar0 + notific_get_relative_offset_sdma(nc_id, index);
		index = 0; //in the block write it on queue 0 as the base is different
	} else {
		apb_base = nd->npdev.bar0 + notific_get_relative_offset(nc_id);
	}

	u32 low = (u32)(queue_pa & 0xffffffff);
	u32 high = (u32)(queue_pa >> 32U);

	notific_write_nq_base_addr_hi(apb_base, index, high);
	notific_write_nq_base_addr_lo(apb_base, index, low);
	notific_write_nq_f_size(apb_base, index, size);
}

/* Memory Pool Functions */
void mpset_set_dram_and_mpset_info_v2(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	mpset->num_channels = V2_MAX_DRAM_CHANNELS;
	mpset->mp_device_num_regions = 1;
	device_dram_addr[0] = V2_HBM_0_BASE;
	device_dram_addr[1] = V2_HBM_1_BASE;
	device_dram_size[0] = V2_HBM_0_SIZE;
	device_dram_size[1] = V2_HBM_1_SIZE;
}

// Upper 16MB is used internally by the firmware, don't use it in the allocation pool
#define MEMPOOL_CARVEOUT_SIZE 0x1000000 // 16MB
int mpset_block_carveout_regions_v2(struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	int ret;
	u64 region_sz;
	int channel = 0, region = 0;

	/*
	*  Block carve out regions: Upper 16 MB is used internally by firmware for trainuim
	*
	*  Ideally we would carve out by simply changing the start address of the chunk;
	*  however, that breaks aligned allocation in 4.x kernel versions (fixed in 5.x).
	*  Fix here:
	*     commit 52fbf1134d479234d7e64ba9dcbaea23405f229e
	*     Author: Alexey Skidanov <alexey.skidanov@intel.com>
	*     Date:   Thu Jan 3 15:26:44 2019 -0800
	*
	*     lib/genalloc.c: fix allocation of aligned buffer from non-aligned chunk
	*/
	for (channel = 0; channel < mpset->num_channels; channel++) {
		region_sz = device_dram_size[channel] / mpset->mp_device_num_regions;
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			const dma_addr_t start_addr = device_dram_addr[channel] + (region * region_sz);
			struct mem_chunk *mc = NULL;
			u32 nc_id = channel;
			ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, MEMPOOL_CARVEOUT_SIZE, MEM_LOC_DEVICE, channel, region, nc_id, &mc);
			if (ret) {
				pr_err("failed to allocate hbm carevout region: ret=%d\n", ret);
				return -ENOMEM;
			}
			if (mc->pa != start_addr) {
				pr_err("carve out mc not offset 0!");
				mc_free(&mc);
				return -EINVAL;
			}
		}
	}

	return 0;
}

/* DMA Ring Functions */
uint32_t ndmar_get_h2t_eng_id_v2(struct neuron_device *nd, uint32_t nc_id)
{
	return (nc_id == 0) ? V2_D2H_IDX : V2_H2D_IDX;
}

bool ndmar_is_nx_ring_v2(uint32_t eng_id, uint32_t q_id)
{
	// the last queue is reserved for collectives, 
	// and the second to last queue in DMA engines 0-10 are reserved for NX cores
	return (q_id == (V2_DMA_QUEUE_PER_ENG - 2)) && ((eng_id % V2_DMA_ENG_PER_NC) < (V2_TPB_ENG_PER_NC + V2_TS_PER_DEVICE));
}

int ndmar_quiesce_queues_v2(struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask)
{
	if (engine_count > DMA_QUIESCE_MAX_ENG)
		return -EINVAL;

	u32 start_eng = nc_id * V2_DMA_ENG_PER_NC;
	u32 eng_id;
	for (eng_id = 0; eng_id < V2_DMA_ENG_PER_NC; eng_id++) {
		int qid;
		struct ndma_eng *eng  = ndmar_acquire_engine(nd, start_eng + eng_id);
		if (eng == NULL)
			return -EINVAL;
		for (qid = 0; qid < V2_MAX_DMA_RINGS; qid++) {
			u32 mask = 0x1 << qid;
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
