// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/version.h>

#include "sdma.h"
#include "notific.h"
#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_topsp.h"
#include "../neuron_mmap.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "../neuron_fw_io.h"
#include "../neuron_pci.h"
#include "../neuron_trace.h"
#include "../neuron_cdev.h"
#include "../neuron_sysfs_metrics.h"
#include "../neuron_ring.h"
#include "../neuron_mempool.h"

#define NR_RESET_RETRY_SLEEP_MS 100

struct neuron_dm_special_mmap_ent dm_mmap_special_v2[] = {
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, V2_MMAP_TPB_OFFSET, V2_PCIE_BAR0_TPB_0_OFFSET,   V2_MMAP_TPB_SIZE, V2_MMAP_NC_EVENT_OFFSET, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, V2_MMAP_TPB_OFFSET, V2_PCIE_BAR0_TPB_0_OFFSET,   V2_MMAP_TPB_SIZE, V2_MMAP_NC_EVENT_OFFSET, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

struct ncdev_mem_region ncdev_mem_regions_v2[] = {
	{ V2_MMAP_TPB_OFFSET, V2_MMAP_TPB_SIZE * V2_MMAP_TPB_COUNT },
	{ V2_TOP_SP_0_BASE, V2_TOP_SP_0_SIZE * V2_TS_PER_DEVICE },
	{ NCDEV_MEM_REGION_INVALID, 0 },
};

u64 ncdev_bar0_write_blocked_addrs_v2[] = {
	V2_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET,
	V2_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET,
	V2_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET,
	V2_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET,
	V2_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET
	MMAP_BAR0_APB_MISC_RAM_INVALID,
};

// sysfs root node's attrs and its child nodes' attrs
static nsysfsmetric_attr_info_t root_info_node_attrs_info_tbl_v2[] = {
    ATTR_INFO("notify_delay", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY), OTHER),
    ATTR_INFO("serial_number", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_SERIAL_NUMBER), OTHER),
};
static int root_info_node_attrs_info_tbl_cnt_v2 = sizeof(root_info_node_attrs_info_tbl_v2) / sizeof(nsysfsmetric_attr_info_t);

static int ndhal_register_funcs_trn1(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for trn1.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v2";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v2";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Trn1";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Trainium1";
	return 0;
}

static int ndhal_register_funcs_inf2(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for inf2.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v3";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v2";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Inf2";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Inferentia2";
	return 0;
}

int ndhal_register_funcs_v2(unsigned int pci_device_id) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for V2.");
		return -EINVAL;
	}

	ndhal->ndhal_address_map.pci_host_base = V2_PCIE_A0_BASE;
	ndhal->ndhal_address_map.mmap_p_offset = V2_MMAP_P_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_event_offset = V2_MMAP_NC_EVENT_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_read_offset = V2_MMAP_NC_SEMA_READ_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_set_offset = V2_MMAP_NC_SEMA_SET_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_incr_offset = V2_MMAP_NC_SEMA_INCR_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_decr_offset = V2_MMAP_NC_SEMA_DECR_OFFSET;
	ndhal->ndhal_address_map.bar0_misc_ram_offset = V2_MMAP_BAR0_APB_MISC_RAM_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_size = V2_MMAP_NC_SIZE;
	ndhal->ndhal_address_map.nc_per_device = V2_NC_PER_DEVICE;
	ndhal->ndhal_address_map.semaphore_count = V2_SEMAPHORE_COUNT;
	ndhal->ndhal_address_map.event_count = V2_EVENTS_COUNT;
	ndhal->ndhal_address_map.ts_per_device = V2_TS_PER_DEVICE;
	ndhal->ndhal_address_map.h2t_qid = 0;
	ndhal->ndhal_address_map.dma_eng_per_nc = V2_DMA_ENG_PER_NC;
	ndhal->ndhal_reset.retry_count = NR_RESET_RETRY_COUNT;
	ndhal->ndhal_topsp.ts_nq_init = ts_nq_init_v2;
	ndhal->ndhal_topsp.ts_nq_destroy_one = ts_nq_destroy_one_v2;
	ndhal->ndhal_nc.nc_get_semaphore_base = nc_get_semaphore_base_v2;
	ndhal->ndhal_nc.nc_get_event_addr = nc_get_event_addr_v2;
	ndhal->ndhal_nq.nnq_get_nqid = nnq_get_nqid_v2;
	ndhal->ndhal_nq.nnq_set_hwaddr = nnq_set_hwaddr_v2;
	ndhal->ndhal_mpset.mp_min_alloc_size = (mempool_min_alloc_size < 1024) ? 1024 : mempool_min_alloc_size;  // v2 has a bigger mem size and gen pool create fails if < 1024
	ndhal->ndhal_mpset.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v2;
	ndhal->ndhal_mpset.mpset_block_carveout_regions = mpset_block_carveout_regions_v2;
	ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id = ndmar_get_h2t_eng_id_v2;
	ndhal->ndhal_ndmar.ndmar_is_nx_ring = ndmar_is_nx_ring_v2;
	ndhal->ndhal_ndmar.ndmar_quiesce_queues = ndmar_quiesce_queues_v2;
	ndhal->ndhal_ndmar.ndmar_set_model_started = ndmar_set_model_started_v2;
	ndhal->ndhal_fw_io.fw_io_topology = fw_io_topology_v2;
	ndhal->ndhal_fw_io.fw_io_register_readless_read_region = fw_io_register_readless_read_region_v2;
	ndhal->ndhal_fw_io.fw_io_read_csr_array = fw_io_read_csr_array_v2;
	ndhal->ndhal_mmap.dm_mmap_special = dm_mmap_special_v2;
	ndhal->ndhal_mmap.mmap_get_bar4_offset = mmap_get_bar4_offset_v2;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl_cnt = root_info_node_attrs_info_tbl_cnt_v2;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl = root_info_node_attrs_info_tbl_v2;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_add_ecc_nodes = nsysfsmetric_add_ecc_nodes_v2;
	ndhal->ndhal_pci.axi_bar = BAR_UNUSED;
	ndhal->ndhal_pci.dram_bar = 4;
	ndhal->ndhal_pci.neuron_pci_release_bar = neuron_pci_release_bar_v2;
	ndhal->ndhal_pci.neuron_pci_reserve_bar = neuron_pci_reserve_bar_v2;
	ndhal->ndhal_pci.neuron_pci_set_npdev = neuron_pci_set_npdev_v2;
	ndhal->ndhal_pci.neuron_pci_get_device_id = neuron_pci_get_device_id_v2;
	ndhal->ndhal_cdev.ncdev_mem_regions = ncdev_mem_regions_v2;
	ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs = ncdev_bar0_write_blocked_addrs_v2;
	ndhal->ndhal_cdev.ncdev_compatible_version = ncdev_compatible_version_v2;
	ndhal->ndhal_cdev.ncdev_quiesce_exec_on_proc_exit = ncdev_quiesce_exec_on_proc_exit_v2;
	ndhal->ndhal_cdev.ncdev_bar_write_data = ncdev_bar_write_data_v2;
	ndhal->ndhal_udma.udma_m2s_data_rd_cfg_boundaries_set = udma_m2s_data_rd_cfg_boundaries_set_v2;
	ndhal->ndhal_udma.udma_q_config = udma_q_config_v2;
	ndhal->ndhal_ndma.ndma_retry_memcpy = true;
	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v2;
	ndhal->ndhal_ndma.ndma_validate_pa = ndma_validate_pa_v2;
	ndhal->ndhal_ndma.ndma_init = ndma_init_v2;
	ndhal->ndhal_ndma.ndma_is_bar0_write_blocked = ndma_is_bar0_write_blocked_v2;
	ndhal->ndhal_ndma.ndma_get_m2m_barrier_type = ndma_get_m2m_barrier_type_v2;

	if (narch_is_qemu()) {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v2_qemu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v2_qemu;
		ndhal->ndhal_address_map.dma_eng_per_nd = V2_NC_PER_DEVICE * V2_DMA_ENG_PER_NC;
		ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v2_qemu_emu;
		ndhal->ndhal_pci.apb_bar = 2;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v2_qemu;
	} else if (narch_is_emu()) {
		ndhal->ndhal_reset.retry_count *= 1000; // wait longer on the emulator
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v2_emu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v2_emu;
		ndhal->ndhal_address_map.dma_eng_per_nd = nc_per_dev_param * V2_DMA_ENG_PER_NC;
		ndhal->ndhal_address_map.nc_per_device = nc_per_dev_param;
		ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v2_qemu_emu;
		ndhal->ndhal_pci.apb_bar = 0;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v2_emu;
	} else {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v2;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v2;
		ndhal->ndhal_address_map.dma_eng_per_nd = V2_NC_PER_DEVICE * V2_DMA_ENG_PER_NC;
		ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v2;
		ndhal->ndhal_pci.apb_bar = 0;
	}

	switch (pci_device_id) {
		case TRN1_DEVICE_ID0:
			ret = ndhal_register_funcs_trn1();
			if (ret) {
				pr_err("failed to register ndhal funcs on trn1.\n");
				return ret;
			}
			break;
		case INF2_DEVICE_ID0:
			ret = ndhal_register_funcs_inf2();
			if (ret) {
				pr_err("failed to register ndhal funcs on inf2.\n");
				return ret;
			}
			break;
		default:
			pr_err("Unknown HW architecture. Can't init neuron_dhal.\n");
			return -EINVAL;
	}

	return ret;
}


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

	for (i = 0; i < ndhal->ndhal_reset.retry_count; i++) {
		bool reset_in_progress = true;
		u32 status;

		if (ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, &status, 1, false) == 0)
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

	for (i = 0; i < ndhal->ndhal_reset.retry_count; i++) {
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

	if (nd == NULL || ts_id >= ndhal->ndhal_address_map.ts_per_device)
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
			   (V2_PCIE_BAR0_TPB_0_SIZE * nc_id) + ndhal->ndhal_address_map.mmap_nc_event_offset;
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

void ndmar_set_model_started_v2(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc)
{
	return;
}

/* FWIO Functions */
const int trn1_32xl_neigbor_ids[16][4] = {
	{12, 3, 4, 1},   // neuron device 0
	{13, 0, 5, 2},   // neuron device 1
	{14, 1, 6, 3},   // neuron device 2
	{15, 2, 7, 0},   // neuron device 3
	{0, 7, 8, 5},    // neuron device 4
	{1, 4, 9, 6},    // neuron device 5
	{2, 5, 10, 7},   // neuron device 6
	{3, 6, 11, 4},   // neuron device 7
	{4, 11, 12, 9},  // neuron device 8
	{5, 8, 13, 10},  // neuron device 9
	{6, 9, 14, 11},  // neuron device 10
	{7, 10, 15, 8},  // neuron device 11
	{8, 15, 0, 13},  // neuron device 12
	{9, 12, 1, 14},  // neuron device 13
	{10, 13, 2, 15}, // neuron device 14
	{11, 14, 3, 12}  // neuron device 15
};

const int inf2_48xl_neighbor_ids[12][2] = {
	{11, 1}, // neuron device 0
	{0, 2},  // neuron device 1
	{1, 3},  // neuron device 2
	{2, 4},  // neuron device 3
	{3, 5},  // neuron device 4
	{4, 6},  // neuron device 5
	{5, 7},  // neuron device 6
	{6, 8},  // neuron device 7
	{7, 9},  // neuron device 8
	{8, 10}, // neuron device 9
	{9, 11}, // neuron device 10
	{10, 0}  // neuron device 11
};

const int *inf2_24xl_neighbor_ids[6] = {
	(int[]){1},     // neuron device 0
	(int[]){0, 2},  // neuron device 1
	(int[]){1, 3},  // neuron device 2
	(int[]){2, 4},  // neuron device 3
	(int[]){3, 5},  // neuron device 4
	(int[]){4}      // neuron device 5
};

int fw_io_topology_v2(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count)
{
	// V2 does not have the device support to detect east/west/south/north neighbors like V1,
	// so its topology is hardcoded based on instance type.
	*count = 0;

	if (total_neuron_devices == 0)
		return 0;

	switch (pdev_index) {
		case TRN1_DEVICE_ID0: // Trn1
			if (total_neuron_devices % 16 == 0) { // Trn1.32xl
				int i;
				*count = 4;
				memcpy(connected_device_ids, trn1_32xl_neigbor_ids[device_id % 16], (*count) * sizeof(int));
				for ( i=0; i < *count; i++) {
					connected_device_ids[i] |= (0x10 & device_id);
				}
			}
			break;
		case INF2_DEVICE_ID0: // Inf2
			if (total_neuron_devices == 12) { // Inf2.48xl
				*count = 2;
				memcpy(connected_device_ids, inf2_48xl_neighbor_ids[device_id], (*count) * sizeof(int));
			} else if (total_neuron_devices == 6) { // Inf2.24xl
				if (device_id == 0 || device_id == 5)
					*count = 1;
				else
					*count = 2;
				memcpy(connected_device_ids, inf2_24xl_neighbor_ids[device_id], (*count) * sizeof(int));
			}
			break;
		default:
			break;
	}
	return 0;
}

int fw_io_register_readless_read_region_v2(struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size)
{
	if (fw_io_register_read_region(ctx, bar0, bar0_size, V2_MMAP_TPB_OFFSET)) {
		pr_err("failed to register readless read BAR0 region\n");
		return -1;
	}
	return 0;
}

int fw_io_read_csr_array_v2(void **ptrs, u32 *values, u32 num_csrs, bool operational)
{
	if (num_csrs > FW_IO_MAX_READLESS_READ_REGISTER_COUNT)
		return -EINVAL;

	if (!use_rr) { // do not use readless read
		return fw_io_read_csr_array_direct(ptrs, values, num_csrs, operational);
	}

    int ret = fw_io_read_csr_array_readless(ptrs, values, num_csrs);
	return ret;
}

/* Register Access (read and write) Functions */
inline int reg_read32_array_v2(void **addr, u32 *value, u32 num_values)
{
	int ret;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(addr, value, num_values, true);
	if (ret != 0) {
		pr_err("register read failure while reading %p\n", addr[0]);
		dump_stack();
	}
	return ret;
}

inline int reg_read32_array_v2_qemu_emu(void **addr, u32 *value, u32 num_values)
{
	int i;
	for (i = 0; i < num_values; i++) {
		value[i] = readl(addr[i]);
	}
	return 0;
}

/* Memory Map Functions */
int mmap_get_bar4_offset_v2(u64 start_addr, u64 size, u64 *offset)
{
	if (start_addr >= V2_HBM_0_BASE && start_addr + size < V2_HBM_0_BASE + V2_HBM_0_SIZE)
		*offset = start_addr;
	else if (start_addr >= V2_HBM_1_BASE && start_addr + size < V2_HBM_1_BASE + V2_HBM_1_SIZE)
		// The 64GB - 80GB range is mapped to 16GB - 32GB on bar4
		*offset = start_addr - V2_HBM_1_BASE + V2_HBM_0_SIZE;
	else
		return -EINVAL;
	return 0;
}


/* Sysfs Metrics Functions */
int nsysfsmetric_add_ecc_nodes_v2(struct nsysfsmetric_metrics *metrics, 
                                  struct nsysfsmetric_node *stats_node,
                                  int ecc_attrs_info_tbl_cnt,
                                  const nsysfsmetric_attr_info_t *ecc_attrs_info_tbl)
{
	struct nsysfsmetric_node *hardware_node = nsysfsmetric_init_and_add_one_node(metrics, stats_node, "hardware", false, -1, ecc_attrs_info_tbl_cnt, ecc_attrs_info_tbl);
	if (!hardware_node) {
		pr_err("failed to add hardware node its attributes under stats\n");
		return -1;
	}

	return 0;
}


/* PCI Functions */
int neuron_pci_release_bar_v2(struct pci_dev *dev, int bar)
{
	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}
	if (bar == BAR_UNUSED) {
		return 0;
	}

	pci_release_region(dev, bar);
	return 0;
}

int neuron_pci_reserve_bar_v2(struct pci_dev *dev, int bar, const char *res_name)
{
	int ret;

	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}
	if (bar == BAR_UNUSED) {
		return 0;
	}

	ret = pci_request_region(dev, bar, res_name);
	if (ret) {
		pci_info(dev, "BAR %d: can't reserve %s\n", bar, res_name);
		return -ENODEV;
	}

	return 0;
}

int neuron_pci_set_npdev_v2(struct pci_dev *dev,
                            int bar,
                            const char *res_name,
                            phys_addr_t *bar_pa,
                            void __iomem **bar_ioaddr,
                            u64 *bar_size)
{
	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}
	if (bar == BAR_UNUSED) {
		return 0;
	}

	if (pci_resource_len(dev, bar) == 0) {
		pci_info(dev, "BAR%d len is 0\n", bar);
		return -ENODEV;
	}
	
	*bar_pa = pci_resource_start(dev, bar);
	if (!(*bar_pa)) {
		pci_info(dev, "Can't get start address of BAR%d %s\n", bar, res_name);
		return -ENODEV;
	}
	*bar_size = pci_resource_len(dev, bar);
	
	if (bar == ndhal->ndhal_pci.dram_bar && wc_enable)
		*bar_ioaddr = pci_iomap_wc(dev, bar, pci_resource_len(dev, bar));
	else
		*bar_ioaddr = pci_iomap(dev, bar, pci_resource_len(dev, bar));
	
	return 0;
}

extern int dup_helper_enable;
static atomic_t dup_rid_cnt = ATOMIC_INIT(0); // count of duplicate routing IDs encountered
static int neuron_pci_handle_dup_routing_id(void)
{
	int  ret = -ENODEV;
	int  dup_cnt;
	char cmd[256];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	dup_cnt = atomic_fetch_add(1, &dup_rid_cnt);
#else
	dup_cnt = atomic_add_return(1, &dup_rid_cnt) - 1;
#endif 

	// If this is the first dup encounted, unload the driver
	if ((dup_cnt == 0) && dup_helper_enable) {
		pr_err("scheduling unload of %s due to duplicate routing id\n", module_name(THIS_MODULE));

		int n = snprintf(cmd, sizeof(cmd), "sleep 10;/sbin/modprobe -r %s", module_name(THIS_MODULE));
		if (n > sizeof(cmd)) {
			pr_err("unable to schedule driver unload cmd buffer len exceeded\n");
			return -EINVAL;
		}
		char *argv[] = 		  { "/bin/sh",
								"-c",
								cmd,
								NULL};
		static char *envp[] = { "HOME=/",
								"TERM=linux",
								"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
								NULL};

		ret = call_usermodehelper( argv[0], argv, envp, UMH_WAIT_EXEC);
		if (ret)
			pr_err("unable to schedule driver unload. Error: %d\n", ret);
	}

	return ret;
}

// for V2 rename Neuron devices for better customer experience.
// see internal documentation: TRN1-Discovery
// map routing id to user id:
// const u32 v2_routing_id_to_user_id[MAX_NEURON_DEVICE_COUNT] = { FIXME NEED NEW MAPPING
static const u32 v2_routing_id_to_user_id[] = {
	0,   4,  1,  5,
	3,   7,  2,  6,
	12,  8, 13,  9,
	15, 11, 14, 10 };

#define V2_ROUTING_ID_TBL_SZ  (sizeof(v2_routing_id_to_user_id) / sizeof(v2_routing_id_to_user_id[0]))

static u32 neuron_pci_routing_id_to_user_id(u32 routing_id)
{
	u32 user_id_base = v2_routing_id_to_user_id[ routing_id % V2_ROUTING_ID_TBL_SZ];
	return user_id_base + (routing_id / V2_ROUTING_ID_TBL_SZ) * V2_ROUTING_ID_TBL_SZ;
}

int neuron_pci_get_device_id_v2(struct neuron_device *nd, struct pci_dev *dev)
{
	int ret = 0;
	int i;
	u32 routing_id = (u32)-1;

	// Poll the device id until the device is ready
	for (i = 0; i < 20; i++) {
		ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
		if (!ret && routing_id != 0xdeadbeef) {
			break;
		}
		msleep(1000);
	}

	if (ret) {
		pr_err("Could not retrieve device index (read timeout)");
		return -ENODEV;
	}

	// TODO - this should be a "valid routing_id check for both TRN1 & INF2
	if (routing_id < 0 || routing_id >= MAX_NEURON_DEVICE_COUNT) {
		pr_err("Invalid device index %u", routing_id);
		return -ENODEV;
	}

	// TODO - TRN1 and INF2 mappings are different - likely all of this and the INF1 should be encapsulated.
	if (nd->pdev->device == TRN1_DEVICE_ID0)
		nd->device_index = neuron_pci_routing_id_to_user_id(routing_id);
	else
		nd->device_index = routing_id;

	pr_err("** BDF: %2.2x:%2.2x.%x => nd[%d] (routing id: %u)\n", dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn), nd->device_index, routing_id);

	// protection against duplicate IDs - doesn't provide 100% protection in multi-threaded device discovery
	if (neuron_devices[nd->device_index] != NULL) {
		pr_err("duplicate routing id %u found\n", routing_id);
		neuron_pci_handle_dup_routing_id();
		return -ENODEV;
	}

	return 0;
}


/* Char Device (cdev) Functions */
/* 
 * IMPORTANT:
 *           - These variables track the range of "compatible" versions of the RT
 *             i.e. the range of RT versions that is compatible with this version of the driver.
 *           - This value is independent from the "release" version, because the
 *             "release" number is controlled by PM, marketing, etc. considerations.
 * 
 *           - MAX should be incremented when the driver API/behavior
 *             changes in a way that is meaningful to the RT.  In that case
 *             both the MAX here and the version expected by the RT should be
 *             incremented to prevent the new RT from starting on an old driver.
 *           - MIN should be incremented when we make changes in the driver
 *             that are not compatible with old RT.  When MIN is incremented,
 *             it will prevent old RT from starting up.
 *
 *           - Version 3 of runtime requires 1) aligned memory allocation support  2) SPROT.
 *           - Version 4 of the runtime requires support for DMA queue init w/o already allocated rings (2.7).
 *           - Version 5 of the runtime requires V2 device renumbering (don't care for V1).
 *           - Version 6 of the runtime requires ham notification support,
 *              + new V2 reset api for single-tpb reset + new notification init API with force mem realloc/resize.
 *           - Version 7 of the runtime requires udma queue size support for non power of 2 rings + dmabuf support.
 */
#define V2_RT_MIN_COMPATIBLE_VERSION 5
#define V2_RT_MAX_COMPATIBLE_VERSION 7
void ncdev_compatible_version_v2(struct neuron_ioctl_compatible_version *arg)
{
	arg->min = V2_RT_MIN_COMPATIBLE_VERSION;
	arg->max = V2_RT_MAX_COMPATIBLE_VERSION;
}

void ncdev_quiesce_exec_on_proc_exit_v2()
{
	// for V2, the 1 second DMA queisce delay in flush was eliminated to improve nrt_init performance
	return;
}

int ncdev_bar_write_data_v2(struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count)
{
	if (bar == 0) {
		int i;
		for (i = 0; i < data_count; i++) {
			u64 off = reg_addresses[i] - (u64)nd->npdev.bar0;
			if (off > nd->npdev.bar0_size) {
				return -EINVAL;
			}
			if (ndhal->ndhal_ndma.ndma_is_bar0_write_blocked(off)) {
				return -EINVAL;
			}
			writel(data[i], nd->npdev.bar0 + off);
			trace_bar_write(nd, bar, off, data[i]);
		}
	} else if (bar == 4) {
		// TODO: we don't have any use case for r/w memory over the BAR right now.  Disabling.
		//
		// We'd like to use DMA for r/w of BAR4 because we might expect access to large amounts of data.
		// Access via DMA requires an application to own a TPB because it determines which of the h2t DMAs
		// are safe to use, otherwise a TPB along with its DMA could be reset while that DMA is used here.
		// Don't want/need to solve it now.
		return -EINVAL;

		/*
		dma_addr_t dst_addr = reg_addresses[0] - (u64)nd->npdev.bar0;

		ret = ndma_memcpy(nd, 0, virt_to_phys(data) | ndhal->ndhal_address_map.pci_host_base, dst_addr, data_size);
		if (ret)
			return ret;
		*/
	} else {
		pr_err("direct BAR%d write is not supported.\n", bar);
		return -EINVAL;
	}

	return 0;
}


/* UDMA Functions */
#define UDMA_AXI_M2S_DATA_RD_CFG_ALWAYS_BREAK_ON_MAX_BOUDRY (1 << 16)
void udma_m2s_data_rd_cfg_boundaries_set_v2(struct udma *udma)
{
	reg_write32(&udma->udma_regs_m2s->axi_m2s.data_rd_cfg,
	  UDMA_AXI_M2S_DATA_RD_CFG_ALWAYS_BREAK_ON_MAX_BOUDRY | 0x8);
}

#define UDMA_M2S_Q_RATE_LIMIT_MASK_INTERNAL_PAUSE_DMB (1 << 2)
void udma_q_config_v2(struct udma_q *udma_q)
{
	if (udma_q->type != UDMA_TX) {
		return;
	}

	uint32_t *reg_addr = &udma_q->q_regs->m2s_q.rlimit.mask;
	uint32_t val = udma_q->rlimit_mask;

	// enable DMB
	val &= ~UDMA_M2S_Q_RATE_LIMIT_MASK_INTERNAL_PAUSE_DMB;
	reg_write32(reg_addr, val);
}


/* NDMA Functions */
void ndma_get_wait_for_completion_time_v2(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	u64 est_wait_time = 4 * (count -1);
	*first_wait_time = async ? 1 : (est_wait_time - 3);
	*following_wait_time = (est_wait_time * 100) - *first_wait_time;

	// for some reason getting a timeout when staging some of BERT training graphs.
	// https://t.corp.amazon.com/P55240908
	// In the meantime make the timeout 100x the original
	*following_wait_time *= 100;
}

void ndma_get_wait_for_completion_time_v2_qemu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v2(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 10 * 1000;
}

void ndma_get_wait_for_completion_time_v2_emu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v2(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 100 * 1000;
}

int ndma_validate_pa_v2(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type)
{
	if ((pa & V2_PCIE_ALL_RT_MASK) == ndhal->ndhal_address_map.pci_host_base) {
		if (!ndma_is_valid_host_mem(nd, pa)) {
			return -EINVAL;
		}
	}
	return 0;
}

const static uint64_t seng_udma_base[V2_MMAP_TPB_COUNT][V2_NUM_DMA_ENGINES_PER_TPB] = {
	{ V2_APB_SENG_0_UDMA_0_BASE, V2_APB_SENG_0_UDMA_1_BASE, V2_APB_SENG_0_UDMA_2_BASE,
	  V2_APB_SENG_0_UDMA_3_BASE, V2_APB_SENG_0_UDMA_4_BASE, V2_APB_SENG_0_UDMA_5_BASE,
	  V2_APB_SENG_0_UDMA_6_BASE, V2_APB_SENG_0_UDMA_7_BASE, V2_APB_SENG_0_UDMA_8_BASE,
	  V2_APB_SENG_0_UDMA_9_BASE, V2_APB_SENG_0_UDMA_10_BASE, V2_APB_SENG_0_UDMA_11_BASE,
	  V2_APB_SENG_0_UDMA_12_BASE, V2_APB_SENG_0_UDMA_13_BASE,
	  V2_APB_SENG_0_UDMA_14_BASE, V2_APB_SENG_0_UDMA_15_BASE },
	{ V2_APB_SENG_1_UDMA_0_BASE, V2_APB_SENG_1_UDMA_1_BASE, V2_APB_SENG_1_UDMA_2_BASE,
	  V2_APB_SENG_1_UDMA_3_BASE, V2_APB_SENG_1_UDMA_4_BASE, V2_APB_SENG_1_UDMA_5_BASE,
	  V2_APB_SENG_1_UDMA_6_BASE, V2_APB_SENG_1_UDMA_7_BASE, V2_APB_SENG_1_UDMA_8_BASE,
	  V2_APB_SENG_1_UDMA_9_BASE, V2_APB_SENG_1_UDMA_10_BASE, V2_APB_SENG_1_UDMA_11_BASE,
	  V2_APB_SENG_1_UDMA_12_BASE, V2_APB_SENG_1_UDMA_13_BASE,
	  V2_APB_SENG_1_UDMA_14_BASE, V2_APB_SENG_1_UDMA_15_BASE }
};
const static uint64_t seng_sdma_base[V2_MMAP_TPB_COUNT][V2_NUM_DMA_ENGINES_PER_TPB] = {
	{ V2_APB_SENG_0_SDMA_0_BASE, V2_APB_SENG_0_SDMA_1_BASE, V2_APB_SENG_0_SDMA_2_BASE,
	  V2_APB_SENG_0_SDMA_3_BASE, V2_APB_SENG_0_SDMA_4_BASE, V2_APB_SENG_0_SDMA_5_BASE,
	  V2_APB_SENG_0_SDMA_6_BASE, V2_APB_SENG_0_SDMA_7_BASE, V2_APB_SENG_0_SDMA_8_BASE,
	  V2_APB_SENG_0_SDMA_9_BASE, V2_APB_SENG_0_SDMA_10_BASE, V2_APB_SENG_0_SDMA_11_BASE,
	  V2_APB_SENG_0_SDMA_12_BASE, V2_APB_SENG_0_SDMA_13_BASE,
	  V2_APB_SENG_0_SDMA_14_BASE, V2_APB_SENG_0_SDMA_15_BASE },
	{ V2_APB_SENG_1_SDMA_0_BASE, V2_APB_SENG_1_SDMA_1_BASE, V2_APB_SENG_1_SDMA_2_BASE,
	  V2_APB_SENG_1_SDMA_3_BASE, V2_APB_SENG_1_SDMA_4_BASE, V2_APB_SENG_1_SDMA_5_BASE,
	  V2_APB_SENG_1_SDMA_6_BASE, V2_APB_SENG_1_SDMA_7_BASE, V2_APB_SENG_1_SDMA_8_BASE,
	  V2_APB_SENG_1_SDMA_9_BASE, V2_APB_SENG_1_SDMA_10_BASE, V2_APB_SENG_1_SDMA_11_BASE,
	  V2_APB_SENG_1_SDMA_12_BASE, V2_APB_SENG_1_SDMA_13_BASE,
	  V2_APB_SENG_1_SDMA_14_BASE, V2_APB_SENG_1_SDMA_15_BASE }
};
int ndma_init_v2(void __iomem *bar0, struct udma *udma, int eng_id)
{
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	const bool d2h = (eng_id == V2_D2H_IDX);
	const bool h2d = (eng_id == V2_H2D_IDX);


	void __iomem *udma_base = NULL;
	void __iomem *sdma_base = NULL;

	if (h2d || d2h) {
		const uint64_t seng_udma_relbase = ( h2d ? V2_APB_H2D_UDMA_BASE : V2_APB_D2H_UDMA_BASE) - V2_APB_BASE;
		const uint64_t seng_sdma_relbase = ( h2d ? V2_APB_H2D_SDMA_BASE : V2_APB_D2H_SDMA_BASE) - V2_APB_BASE;

		udma_base = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_udma_relbase;
		sdma_base = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_sdma_relbase;
	} else {
		const int nc_id = eng_id / V2_DMA_ENG_PER_NC;
		const int eid = eng_id % V2_DMA_ENG_PER_NC;

		const uint64_t seng_udma_relbase = seng_udma_base[nc_id][eid] - V2_APB_BASE;
		const uint64_t seng_sdma_relbase = seng_sdma_base[nc_id][eid] - V2_APB_BASE;

		udma_base = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_udma_relbase;
		sdma_base  = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_sdma_relbase;

		ret = sdma_configure_broadcast(sdma_base, eid);
		if (ret) {
			pr_err("SDMA BCAST:%d init failed\n", eng_id);
			goto done;
		}
	}

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(udma, udma_base, DMA_MAX_Q_MAX, udma_name, 0,
				   V2_ALLOWED_DESC_PER_PACKET + 1, true); // we add one to allow for MD descriptor
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = sdma_init_engine(sdma_base);
	if (ret) {
		pr_err("SDMA ENG:%d init failed\n", eng_id);
		goto done;
	}

	udma_m2m_set_axi_error_abort(udma); // setup dma abort

done:
	return ret;
}

int ndma_is_bar0_write_blocked_v2(u64 off)
{
	int eid;
	// check NC 0
	u64 start_off = V2_APB_SENG_0_UDMA_0_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	u64 end_off = V2_APB_SENG_0_UDMA_15_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_UDMA_SIZE;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V2_DMA_ENG_PER_NC; eid++) {
			if (ndma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V2_APB_SENG_UDMA_SIZE;
		}
	}
	// check NC 1
	start_off = V2_APB_SENG_1_UDMA_0_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	end_off = V2_APB_SENG_1_UDMA_15_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_UDMA_SIZE;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V2_DMA_ENG_PER_NC; eid++) {
			if (ndma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V2_APB_SENG_UDMA_SIZE;
		}
	}
	// check D2H
	start_off = V2_APB_D2H_UDMA_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	end_off = start_off + V2_APB_SENG_UDMA_SIZE;
	if (ndma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}
	// check H2D
	start_off = V2_APB_H2D_UDMA_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	end_off = start_off + V2_APB_SENG_UDMA_SIZE;
	if (ndma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}

	int i = 0;
	while (ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs[i] != MMAP_BAR0_APB_MISC_RAM_INVALID) {
		if (off == ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs[i]) {
			pr_err("** blocking %llx\n", off);
			return -1;
		}
		i++;
	}
	return 0;
}

int ndma_get_m2m_barrier_type_v2(bool set_dmb)
{
	if (set_dmb)
		return UDMA_M2M_BARRIER_WRITE_BARRIER;
	else
		return UDMA_M2M_BARRIER_NONE;
}
