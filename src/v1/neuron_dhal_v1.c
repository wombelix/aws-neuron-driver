// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2023, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/
#include <linux/pci.h>
#include <linux/delay.h>

#include "../neuron_reset.h"
#include "../neuron_dhal.h"
#include "../neuron_topsp.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "../neuron_fw_io.h"
#include "../neuron_pci.h"
#include "../neuron_trace.h"
#include "../neuron_cdev.h"
#include "fw_io.h"
#include "putils.h"
#include "tdma.h"

struct neuron_dm_special_mmap_ent dm_mmap_special_v1[] = {
	{NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

struct ncdev_mem_region ncdev_mem_regions_v1[] = {
	{ V1_MMAP_TPB_OFFSET, V1_MMAP_NC_SIZE * V1_NC_PER_DEVICE },
	{ NCDEV_MEM_REGION_INVALID, 0 },
};

u64 ncdev_bar0_write_blocked_addrs_v1[] = {
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET,
	V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET,
	MMAP_BAR0_APB_MISC_RAM_INVALID,
};

// sysfs root node's attrs and its child nodes' attrs
static nsysfsmetric_attr_info_t root_info_node_attrs_info_tbl_v1[] = {
    ATTR_INFO("notify_delay", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY), OTHER),
};
static int root_info_node_attrs_info_tbl_cnt_v1 = sizeof(root_info_node_attrs_info_tbl_v1) / sizeof(nsysfsmetric_attr_info_t);

static int ndhal_register_funcs_inf1(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for inf1.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v1";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v1";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Inf1";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Inferentia";
	return 0;
}

int ndhal_register_funcs_v1(unsigned int pci_device_id) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for V1.");
		return -EINVAL;
	}

	ndhal->ndhal_address_map.pci_host_base = PCIEX8_0_BASE;
	ndhal->ndhal_address_map.mmap_p_offset = V1_MMAP_P_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_event_offset = V1_MMAP_NC_EVENT_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_read_offset = V1_MMAP_NC_SEMA_READ_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_set_offset = V1_MMAP_NC_SEMA_SET_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_incr_offset = V1_MMAP_NC_SEMA_INCR_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_sema_decr_offset = V1_MMAP_NC_SEMA_DECR_OFFSET;
	ndhal->ndhal_address_map.bar0_misc_ram_offset = V1_MMAP_BAR0_APB_MISC_RAM_OFFSET;
	ndhal->ndhal_address_map.mmap_nc_size = V1_MMAP_NC_SIZE;
	ndhal->ndhal_address_map.nc_per_device = V1_NC_PER_DEVICE;
	ndhal->ndhal_address_map.semaphore_count = V1_SEMAPHORE_COUNT;
	ndhal->ndhal_address_map.event_count = V1_EVENTS_COUNT;
	ndhal->ndhal_address_map.ts_per_device = 0;
	ndhal->ndhal_address_map.h2t_qid = V1_MAX_DMA_RINGS - 1;
	ndhal->ndhal_address_map.dma_eng_per_nd = V1_NUM_DMA_ENG_PER_DEVICE;
	ndhal->ndhal_address_map.dma_eng_per_nc = V1_DMA_ENG_PER_NC;
	ndhal->ndhal_reset.retry_count = NR_RESET_RETRY_COUNT;
	ndhal->ndhal_topsp.ts_nq_init = ts_nq_init_v1;
	ndhal->ndhal_topsp.ts_nq_destroy_one = ts_nq_destroy_one_v1;
	ndhal->ndhal_nc.nc_get_semaphore_base = nc_get_semaphore_base_v1;
	ndhal->ndhal_nc.nc_get_event_addr = nc_get_event_addr_v1;
	ndhal->ndhal_nq.nnq_get_nqid = nnq_get_nqid_v1;
	ndhal->ndhal_nq.nnq_set_hwaddr = nnq_set_hwaddr_v1;
	ndhal->ndhal_mpset.mp_min_alloc_size = mempool_min_alloc_size;
	ndhal->ndhal_mpset.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v1;
	ndhal->ndhal_mpset.mpset_block_carveout_regions = mpset_block_carveout_regions_v1;
	ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id = ndmar_get_h2t_eng_id_v1;
	ndhal->ndhal_ndmar.ndmar_is_nx_ring = ndmar_is_nx_ring_v1;
	ndhal->ndhal_ndmar.ndmar_quiesce_queues = ndmar_quiesce_queues_v1;
	ndhal->ndhal_ndmar.ndmar_set_model_started = ndmar_set_model_started_v1;
	ndhal->ndhal_fw_io.fw_io_topology = fw_io_topology_v1;
	ndhal->ndhal_fw_io.fw_io_register_readless_read_region = fw_io_register_readless_read_region_v1;
	ndhal->ndhal_fw_io.fw_io_read_csr_array = fw_io_read_csr_array_v1;
	ndhal->ndhal_reg_access.reg_read32_array = reg_read32_array_v1;
	ndhal->ndhal_mmap.dm_mmap_special = dm_mmap_special_v1;
	ndhal->ndhal_mmap.mmap_get_bar4_offset = mmap_get_bar4_offset_v1;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl_cnt = root_info_node_attrs_info_tbl_cnt_v1;
	ndhal->ndhal_sysfs_metrics.root_info_node_attrs_info_tbl = root_info_node_attrs_info_tbl_v1;
	ndhal->ndhal_sysfs_metrics.nsysfsmetric_add_ecc_nodes = nsysfsmetric_add_ecc_nodes_v1;
	ndhal->ndhal_pci.apb_bar = 0;
	ndhal->ndhal_pci.axi_bar = 2;
	ndhal->ndhal_pci.dram_bar = 4;
	ndhal->ndhal_pci.neuron_pci_release_bar = neuron_pci_release_bar_v1;
	ndhal->ndhal_pci.neuron_pci_reserve_bar = neuron_pci_reserve_bar_v1;
	ndhal->ndhal_pci.neuron_pci_set_npdev = neuron_pci_set_npdev_v1;
	ndhal->ndhal_pci.neuron_pci_get_device_id = neuron_pci_get_device_id_v1;
	ndhal->ndhal_cdev.ncdev_mem_regions = ncdev_mem_regions_v1;
	ndhal->ndhal_cdev.ncdev_bar0_write_blocked_addrs = ncdev_bar0_write_blocked_addrs_v1;
	ndhal->ndhal_cdev.ncdev_compatible_version = ncdev_compatible_version_v1;
	ndhal->ndhal_cdev.ncdev_quiesce_exec_on_proc_exit = ncdev_quiesce_exec_on_proc_exit_v1;
	ndhal->ndhal_cdev.ncdev_bar_write_data = ncdev_bar_write_data_v1;
	ndhal->ndhal_udma.udma_m2s_data_rd_cfg_boundaries_set = udma_m2s_data_rd_cfg_boundaries_set_v1;
	ndhal->ndhal_udma.udma_q_config = udma_q_config_v1;
	ndhal->ndhal_ndma.ndma_retry_memcpy = false;
	ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v1;
	ndhal->ndhal_ndma.ndma_validate_pa = ndma_validate_pa_v1;
	ndhal->ndhal_ndma.ndma_init = ndma_init_v1;
	ndhal->ndhal_ndma.ndma_is_bar0_write_blocked = ndma_is_bar0_write_blocked_v1;
	ndhal->ndhal_ndma.ndma_get_m2m_barrier_type = ndma_get_m2m_barrier_type_v1;

	if (narch_is_qemu()) {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v1_qemu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1_qemu;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v1_qemu;
	} else if (narch_is_emu()) {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v1_emu;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1_emu;
		ndhal->ndhal_ndma.ndma_get_wait_for_completion_time = ndma_get_wait_for_completion_time_v1_emu;
	} else {
		ndhal->ndhal_reset.nr_initiate_reset = nr_initiate_reset_v1;
		ndhal->ndhal_reset.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1;
	}

	switch (pci_device_id) {
		case INF1_DEVICE_ID0:
		case INF1_DEVICE_ID1:
		case INF1_DEVICE_ID2:
		case INF1_DEVICE_ID3:
			ret = ndhal_register_funcs_inf1();
			if (ret) {
				pr_err("failed to register ndhal funcs on inf1.\n");
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
	for (i = 0; i < ndhal->ndhal_reset.retry_count; i++) {
		if (fw_io_is_device_ready_v1(nd->npdev.bar0))
			break;
		if (nd->nr.stop)
			return -1;
	}
	if (i == ndhal->ndhal_reset.retry_count) {
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
	return ndhal->ndhal_address_map.mmap_p_offset + (nc_index * ndhal->ndhal_address_map.mmap_nc_size);
}

void *nc_get_semaphore_base_v1(struct neuron_device *nd, u8 nc_id)
{
	return nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id);
}

void *nc_get_event_addr_v1(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void *base = nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id) + ndhal->ndhal_address_map.mmap_nc_event_offset;
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
			if ((start_eng + eng_id) == ndhal->ndhal_ndmar.ndmar_get_h2t_eng_id(nd, nc_id) && qid == ndhal->ndhal_address_map.h2t_qid) {
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


/* FWIO Functions */
int fw_io_topology_v1(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count)
{
	int ret = 0, i;
	u64 addr = P_0_APB_MISC_RAM_BASE + V1_FW_IO_REG_FW_STATUS_OFFSET;
	u32 reg_val;
	bool is_ready;
	int found = 0;

	*count = 0;
	is_ready = fw_io_wait_for_device_ready_v1(ctx, &reg_val);
	if (!is_ready)
		return 1;

	// assume no device is connected.
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++)
		connected_device_ids[i] = -1;

	// if east link is up, read the link's device's address
	if (reg_val & V1_FW_IO_REG_FW_STATUS_EAST_LINK_MASK) {
		addr = PCIEX4_0_BASE | (P_0_APB_MISC_RAM_BASE + FW_IO_REG_DEVICE_ID_OFFSET);
		ret = fw_io_read(ctx, &addr, &connected_device_ids[found], 1);
		if (ret) {
			pr_err("failed to read east device id\n");
			return 1;
		}
		found++;
	}
	// if west link is up, read the link's device's address
	if (reg_val & V1_FW_IO_REG_FW_STATUS_WEST_LINK_MASK) {
		addr = PCIEX4_1_BASE | (P_0_APB_MISC_RAM_BASE + FW_IO_REG_DEVICE_ID_OFFSET);
		ret = fw_io_read(ctx, &addr, &connected_device_ids[found], 1);
		if (ret) {
			pr_err("failed to read west device id\n");
			return 1;
		}
		found++;
	}
	*count = found;

	return 0;
}

int fw_io_register_readless_read_region_v1(struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size)
{
	if (fw_io_register_read_region(ctx, bar0, bar0_size, P_0_APB_BASE)) {
		pr_err("failed to register readless read BAR0 region\n");
		return -1;
	}
	if (fw_io_register_read_region(ctx, bar2, bar2_size, V1_MMAP_TPB_OFFSET)) {
		pr_err("failed to register readless read BAR2 region\n");
		return -1;
	}
	return 0;
}

int fw_io_read_csr_array_v1(void **ptrs, u32 *values, u32 num_csrs, bool operational)
{
	if (num_csrs > FW_IO_MAX_READLESS_READ_REGISTER_COUNT)
		return -EINVAL;

	int ret = fw_io_read_csr_array_readless(ptrs, values, num_csrs);
	return ret;
}

/* Register Access (read and write) Functions */
inline int reg_read32_array_v1(void **addr, u32 *value, u32 num_values)
{
	int ret;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(addr, value, num_values, true);
	if (ret != 0) {
		pr_err("register read failure while reading %p\n", addr[0]);
		dump_stack();
	}

	return ret;
}

/* Memory Map Functions */
int mmap_get_bar4_offset_v1(u64 start_addr, u64 size, u64 *offset)
{
	// Note: 
	// 1) we mapped the address to get VA but R/W access to the BAR
	// from the instance might still be blocked.
	// 2) in the new future Neuron software will not request the mapping when running on INF
	if (start_addr >= P_0_DRAM_0_BASE && start_addr + size < P_0_DRAM_0_BASE + P_0_DRAM_0_SIZE)
		*offset = start_addr;
	else if (start_addr >= P_0_DRAM_1_BASE && start_addr + size < P_0_DRAM_1_BASE + P_0_DRAM_1_SIZE)
		// The BAR is squashed, 4GB+4GB are mapped consecutively but they are apart
		// in the actual address space
		*offset = start_addr - P_0_DRAM_1_BASE + P_0_DRAM_0_SIZE;
	else
		return -EINVAL;
	return 0;
}

/* Sysfs Metrics Functions */
int nsysfsmetric_add_ecc_nodes_v1(struct nsysfsmetric_metrics *metrics, 
                               struct nsysfsmetric_node *stats_node,
                               int ecc_attrs_info_tbl_cnt,
                               const nsysfsmetric_attr_info_t *attr_info_tbl)
{
	// ecc errors are only supported by sysfs for V2. V1 support will be added later.
	return 0;
}

/* PCI Functions */
int neuron_pci_release_bar_v1(struct pci_dev *dev, int bar)
{
	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}
	pci_release_region(dev, bar);
	return 0;
}

int neuron_pci_reserve_bar_v1(struct pci_dev *dev, int bar, const char *res_name)
{
	int ret;

	if (bar != ndhal->ndhal_pci.apb_bar && bar != ndhal->ndhal_pci.axi_bar && bar != ndhal->ndhal_pci.dram_bar) {
		pci_info(dev, "invalid BAR%d\n", bar);
		return -ENODEV;
	}

	ret = pci_request_region(dev, bar, res_name);
	if (ret) {
		pci_info(dev, "BAR %d: can't reserve %s\n", bar, res_name);
		return -ENODEV;
	}

	return 0;
}

int neuron_pci_set_npdev_v1(struct pci_dev *dev,
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

int neuron_pci_get_device_id_v1(struct neuron_device *nd, struct pci_dev *dev)
{
	// dummy for V1
	return 0;
}


/* Char Device (cdev) Functions */
/* 
 * IMPORTANT:
 *           - These variables track the range of "compatible" versions of the RT
 *             i.e. the range of RT versions that is compatible with this version of the driver.
 *           - This value is independent from the "release" version because
 *             "release" number is controlled by PM, marketing, etc. considerations.
 * 
 *           - MAX should be incremented when the driver API/behavior
 *             changes in a way that is meaningful to the RT.  In that case
 *             both the MAX here and the version expected by the RT should be
 *             incremented to prevent the new RT from starting on an old driver
 *           - MIN should be incremented when we make changes in the driver
 *             that are not compatible with old RT.  When MIN is incremented
 *             it will prevent old RT from starting up.
 *
 *           - Version 3 of runtime requires 1) aligned memory allocation support  2) SPROT
 *           - Version 4 of the runtime requires support for DMA queue init w/o already allocated rings. (2.7)
 *           - Version 5 of the runtime requires V2 device renumbering (don't care for V1)
 *           - Version 6 of the runtime requires ham notification support
 *              + new V2 reset api for single-tpb reset + new notification init API with force mem realloc/resize
 *           - Version 7 of the runtime requires udma queue size support for non power of 2 rings + dmabuf support
 */
#define V1_RT_MIN_COMPATIBLE_VERSION 2
#define V1_RT_MAX_COMPATIBLE_VERSION 7
void ncdev_compatible_version_v1(struct neuron_ioctl_compatible_version *arg)
{
	arg->min = V1_RT_MIN_COMPATIBLE_VERSION;
	arg->max = V1_RT_MAX_COMPATIBLE_VERSION;
}

void ncdev_quiesce_exec_on_proc_exit_v1()
{
	msleep(1000);  // TODO: should directly clear semaphore and events instead of 1 sec sleep
}

int ncdev_bar_write_data_v1(struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count)
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
	} else if (bar == 2) {
		int i;
		u64 off = reg_addresses[0] - (u64)nd->npdev.bar2;
		for (i = 0; i < data_count; i++, off += sizeof(u32)) {
			if (off > nd->npdev.bar2_size) {
				return -EINVAL;
			}
			writel(data[i], nd->npdev.bar2 + off);
			trace_bar_write(nd, bar, off, data[i]);
		}
	} else {
		pr_err("direct BAR%d write is not supported.\n", bar);
		return -EINVAL;
	}

	return 0;
}


/* UDMA Functions */
void udma_m2s_data_rd_cfg_boundaries_set_v1(struct udma *udma)
{
    return;
}

void udma_q_config_v1(struct udma_q *udma_q)
{
    return;
}


/* NDMA Functions */
void ndma_get_wait_for_completion_time_v1(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	u64 est_wait_time = 16 * (count - 1);
	*first_wait_time = async ? 1 : (est_wait_time - 12);
	*following_wait_time = (est_wait_time * 100) - *first_wait_time;

	// for some reason getting a timeout tools pipeline, so bumping wait by 10x 
	// https://t.corp.amazon.com/P99249422/communication
	*following_wait_time *= 10;
}

void ndma_get_wait_for_completion_time_v1_qemu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v1(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 10 * 1000;
}

void ndma_get_wait_for_completion_time_v1_emu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time)
{
	ndma_get_wait_for_completion_time_v1(count, async, first_wait_time, following_wait_time);
	*following_wait_time *= 100 * 1000;
}

int ndma_validate_pa_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type)
{
	if (((pa & PCIEX8_0_BASE) == ndhal->ndhal_address_map.pci_host_base) && ((pa & PCIEX4_1_BASE) != PCIEX4_1_BASE)) {
		if (!ndma_is_valid_host_mem(nd, pa)) {
			return -EINVAL;
		}
	} else if (desc_type == NEURON_DMA_QUEUE_TYPE_RX) {
		// For V1 need to set the first model start state. If the desc has pa for PE instr fifo, then
		// whichever dma engine queue that has this mc is set to have the pe instr.
		ndmar_set_model_started_v1(nd, pa, dst_mc);
	}
	return 0;
}

const static u64 teng_udma_base[] = {
	P_0_APB_TENG_0_UDMA_0_RELBASE,
	P_0_APB_TENG_1_UDMA_0_RELBASE,
	P_0_APB_TENG_2_UDMA_0_RELBASE,
	P_0_APB_TENG_3_UDMA_0_RELBASE };
const static u64 teng_tdma_base[] = {
	P_0_APB_TENG_0_TDMA_0_RELBASE,
	P_0_APB_TENG_1_TDMA_0_RELBASE,
	P_0_APB_TENG_2_TDMA_0_RELBASE,
	P_0_APB_TENG_3_TDMA_0_RELBASE };
int ndma_init_v1(void __iomem *bar0, struct udma *udma, int eng_id)
{
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	void __iomem *udma_base;
	void __iomem *tdma_base;
	int nc_id = eng_id / V1_DMA_ENG_PER_NC;
	int eid = eng_id % V1_DMA_ENG_PER_NC;
	udma_base = (void __iomem *)bar0 + teng_udma_base[nc_id] + (eid * P_0_APB_TENG_0_UDMA_0_SIZE);
	tdma_base = (void __iomem *)bar0 + teng_tdma_base[nc_id] + (eid * P_0_APB_TENG_0_TDMA_0_SIZE);

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(udma, udma_base, DMA_MAX_Q_MAX, udma_name, 0, V1_ALLOWED_DESC_PER_PACKET, false);
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = tdma_init_engine(tdma_base);
	if (ret) {
		pr_err("TDMA ENG:%d init failed\n", eng_id);
		goto done;
	}

done:
	return ret;
}

int ndma_is_bar0_write_blocked_v1(u64 off)
{
	int nc_id, eid;
	// if not writing to udma space - quick exit; note this also ignores writes to some tdma
	// space but we don't care since we will not be checking it later anyway
	if (off < P_0_APB_TENG_0_UDMA_0_RELBASE || off >= (P_0_APB_TENG_3_UDMA_0_RELBASE + V1_DMA_ENG_PER_NC * P_0_APB_TENG_0_UDMA_0_SIZE)) {
		return 0;
	}
	for (nc_id = 0; nc_id < sizeof(teng_udma_base) / sizeof(teng_udma_base[0]); nc_id++) {
		for (eid = 0; eid < V1_DMA_ENG_PER_NC; eid++) {
			u64 udma_off = teng_udma_base[nc_id] + (eid * P_0_APB_TENG_0_UDMA_0_SIZE);
			if (ndma_bar0_blocked_one_engine(udma_off, off)) {
				return -1;
			}
		} 
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

int ndma_get_m2m_barrier_type_v1(bool set_dmb)
{
	if (set_dmb)
		return UDMA_M2M_BARRIER_DMB;
	else
		return UDMA_M2M_BARRIER_NONE;
}

