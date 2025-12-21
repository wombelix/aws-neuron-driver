// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2025, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/delay.h>
#include <linux/pci.h>

#include "address_map.h"
#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_arch.h"
#include "../neuron_cdev.h"
#include "../neuron_pci.h"
#include "../v3/neuron_pelect.h"


// TOP SP addresses are sparse on chip adjust to accommodate the table macro
//
#define V4_TOP_SP_GRP1_BASE V4_TOP_SP_0_BASE
#define V4_TOP_SP_GRP2_BASE (V4_TOP_SP_10_BASE - 8 * V4_TOP_SP_DIST)

#define V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET V4_PCIE_BAR0_TOP_SP_0_OFFSET
#define V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET (V4_PCIE_BAR0_TOP_SP_10_OFFSET - 8 * V4_TOP_SP_SIZE)

struct neuron_dm_special_mmap_ent dm_mmap_special_v4[] = {
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   4, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   5, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   6, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   7, NEURON_DM_RESOURCE_SEMAPHORE, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, V4_MMAP_NC_EVENT_OFFSET, V4_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   4, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   5, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   6, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TPB,   7, NEURON_DM_RESOURCE_SBUF, V4_MMAP_TPB_0_BASE, V4_PCIE_BAR0_TPB_0_OFFSET, V4_PCIE_BAR0_TPB_DIST, V4_PCIE_BAR0_TPB_SIZE, 0, V4_PCIE_BAR0_TPB_SBUF_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 6, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 7, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  8, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  9, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 10, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 11, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 12, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 13, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 14, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 15, NEURON_DM_RESOURCE_SEMAPHORE, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST,  V4_TOP_SP_SIZE, 0, V4_MMAP_NC_SEMA_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 6, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 7, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP1_BASE, V4_PCIE_BAR0_TOP_SP_GRP1_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),

	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  8, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP,  9, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 10, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 11, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 12, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 13, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 14, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),
	DM_SPECIAL_MM_ENT_( NEURON_DM_BLOCK_TOPSP, 15, NEURON_DM_RESOURCE_ALL, V4_TOP_SP_GRP2_BASE, V4_PCIE_BAR0_TOP_SP_GRP2_OFFSET, V4_TOP_SP_DIST, V4_TOP_SP_SIZE, 0, V4_TOP_SP_SIZE, 0),

	{.block = NEURON_DM_BLOCK_HBM, .block_id = 0, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_0_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 0, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 1, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_1_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 1, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 2, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_2_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 2, .bar_num = 4},
	{.block = NEURON_DM_BLOCK_HBM, .block_id = 3, .resource = NEURON_DM_RESOURCE_DMEM, .offset = V4_HBM_3_BASE, .size = V4_HBM_ACTIVE_SIZE, .bar_offset = V4_HBM_SIZE * 3, .bar_num = 4},

	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

struct ncdev_mem_region ncdev_mem_regions_v4[] = {
	{ V4_MMAP_TPB_0_BASE, V4_MMAP_NC_SIZE },    // FIXME this is inefficient this may need a routine to slice and range check
	{ V4_MMAP_TPB_1_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_2_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_3_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_4_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_5_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_6_BASE, V4_MMAP_NC_SIZE },
	{ V4_MMAP_TPB_7_BASE, V4_MMAP_NC_SIZE },
	{ V4_TOP_SP_0_BASE, V4_TOP_SP_SIZE },       // could flatten TOP_SP
	{ V4_TOP_SP_1_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_2_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_3_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_4_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_5_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_6_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_7_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_8_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_9_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_10_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_11_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_12_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_13_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_14_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_15_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_16_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_17_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_18_BASE, V4_TOP_SP_SIZE },
	{ V4_TOP_SP_19_BASE, V4_TOP_SP_SIZE },
	{ V4_HBM_0_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_HBM_1_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_HBM_2_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_HBM_3_BASE, V4_HBM_ACTIVE_SIZE },
	{ V4_PREPROC_0_BASE, V4_PREPROC_SIZE},
	{ V4_PREPROC_1_BASE, V4_PREPROC_SIZE},
	{ V4_PREPROC_2_BASE, V4_PREPROC_SIZE},
	{ V4_PREPROC_3_BASE, V4_PREPROC_SIZE},
	{ NCDEV_MEM_REGION_INVALID, 0 },
};


u32 npe_neighbor_eng_ids_v4[2][2] =
{
    {40, 72},  // Left
    {8, 104}   // Right
};

static int ndhal_register_funcs_trn3(void) {
	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for trn3.");
		return -EINVAL;
	}
	ndhal->ndhal_sysfs_metrics.arch_nd_type_suffix = "v4";
	ndhal->ndhal_sysfs_metrics.arch_nc_type_suffix = "v4";
	ndhal->ndhal_sysfs_metrics.arch_instance_suffix = "Trn3";
	ndhal->ndhal_sysfs_metrics.arch_device_name_suffix = "Trainium3";
	return 0;
}

/* Instance names
 */
#define NEURON_TRN3PDS_INSTANCE_NAME "trn3s.48xlarge"
#define NEURON_TRN3PDS0_INSTANCE_NAME "trn3-dev0.48xlarge"

static enum neuron_platform_type ndhal_platform_type_v4(void)
{
	enum neuron_platform_type platform_type = NEURON_PLATFORM_TYPE_INVALID;
	char buf[128];

	if (narch_get_instance_type_name(buf, sizeof(buf))) goto done;
	if ((strncmp(buf, NEURON_TRN3PDS_INSTANCE_NAME, sizeof(NEURON_TRN3PDS_INSTANCE_NAME)-1) == 0)) {
		platform_type = NEURON_PLATFORM_TYPE_PDS;
	} else if ((strncmp(buf, NEURON_TRN3PDS0_INSTANCE_NAME, sizeof(NEURON_TRN3PDS_INSTANCE_NAME)-1) == 0)) {
		platform_type = NEURON_PLATFORM_TYPE_PDS;
	} else {
		platform_type = NEURON_PLATFORM_TYPE_STD;
	}

done:
	return platform_type;
}

static bool ndhal_instance_type_3xl(void)
{
	static bool instance_type_is_3xl = false;
#define NEURON_TRN3PD98_3XL_INSTANCE_NAME "trn3pd98.3xlarge"
	char buf[128];
	if (narch_get_instance_type_name(buf, sizeof(buf))) goto done;
	if (strncmp(buf, NEURON_TRN3PD98_3XL_INSTANCE_NAME, sizeof(NEURON_TRN3PD98_3XL_INSTANCE_NAME)-1) == 0) {
		instance_type_is_3xl = true;
	}

done:
	return instance_type_is_3xl;
}


/* Memory Pool Functions */
/**
 * mpset_set_dram_and_mpset_info()
 *              - set the address and size of device dram
 *              - set mpset's num_channels and number of regions in the device pool
 *
 * @param mpset: pointer to mpset
 * @param device_dram_addr: DRAM Channel 0 and 1's addresses
 * @param device_dram_size: DRAM Channel 0 and 1's sizes
 */
static void mpset_set_dram_and_mpset_info_v4(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size)
{
	mpset->num_channels = V4_MAX_DRAM_CHANNELS;
	mpset->mp_device_num_regions = 1;
	device_dram_addr[0] = V4_HBM_0_BASE;
	device_dram_addr[1] = V4_HBM_1_BASE;
	device_dram_addr[2] = V4_HBM_2_BASE;
	device_dram_addr[3] = V4_HBM_3_BASE;

	if (narch_is_qemu()) {
		// Allow qemu setups to dynamically allocate their HBM sizes
		const u64 msize = ndhal->ndhal_pci.dram_bar_size / 4;
		device_dram_size[0] = msize;
		device_dram_size[1] = msize;
		device_dram_size[2] = msize;
		device_dram_size[3] = msize;

		u32 mem_regions = sizeof(dm_mmap_special_v4) / sizeof(dm_mmap_special_v4[0]);
		int i = 0;
		for (; i < mem_regions; ++i) {
			if ((dm_mmap_special_v4[i].offset == V4_HBM_0_BASE) ||
				(dm_mmap_special_v4[i].offset == V4_HBM_1_BASE) ||
				(dm_mmap_special_v4[i].offset == V4_HBM_2_BASE) ||
				(dm_mmap_special_v4[i].offset == V4_HBM_3_BASE)) {
				dm_mmap_special_v4[i].size = msize;
			}
		}
		pr_info("overriding hbm size to %llu bytes", msize);
	} else {
		device_dram_size[0] = V4_HBM_ACTIVE_SIZE;
		device_dram_size[1] = V4_HBM_ACTIVE_SIZE;
		device_dram_size[2] = V4_HBM_ACTIVE_SIZE;
		device_dram_size[3] = V4_HBM_ACTIVE_SIZE;
	}
	int i;
	for (i = 0; i < mpset->num_channels; i++) {
		ndhal->ndhal_mpset.device_dram_end_addr[i] = device_dram_addr[i] + device_dram_size[i];
	}
}


/* Memory Map Functions */
/**
 * mmap_get_bar4_offset() - calculate the offset of BAR4
 *
 * @param start_addr: start address
 * @param size: size of memory
 * @param offset: offset of BAR4
 * @return int: 0 on success; negative on failure
 */
static int mmap_get_bar4_offset_v4(u64 start_addr, u64 size, u64 *offset)
{
	u64 hbm_dist = narch_is_qemu() ? (ndhal->ndhal_pci.dram_bar_size / 4) : V4_HBM_SIZE;

	if (start_addr >= V4_HBM_0_BASE && start_addr + size < V4_HBM_0_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr;
	else if (start_addr >= V4_HBM_1_BASE && start_addr + size < V4_HBM_1_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr - V4_HBM_1_BASE + hbm_dist;
	else if (start_addr >= V4_HBM_2_BASE && start_addr + size < V4_HBM_2_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr - V4_HBM_2_BASE + hbm_dist * 2;
	else if (start_addr >= V4_HBM_3_BASE && start_addr + size < V4_HBM_3_BASE + V4_HBM_ACTIVE_SIZE)
		*offset = start_addr - V4_HBM_3_BASE + hbm_dist * 3;
	else
		return -EINVAL;
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

// for V4 rename Neuron devices for better customer experience.
// see internal documentation: TRN2-Discovery
// map routing id to user id:
static const u32 v4_torus_routing_id_to_user_id[] = {
	0,	3,	4,	7,
	12,	15,	8,	11,
	1,	2,	5,	6,
	13,	14,	9,	10 };

// map routing id to user id for trn2pds instance type.
// the only hard rule this map needs to follow is
// rid (i*2) and rid (i*2)+1 map to did (n*2) and did (n*2)+1
// since rid (i*2) and rid (i*2)+1 are on the same JBOG.
static const u32 v4_pds_routing_id_to_user_id[] = {
	0, 1,
	2, 3,
	4, 5,
	6, 7,
	8, 9,
	10, 11,
	12, 13,
	14, 15 };

#define V4_ROUTING_ID_TBL_SZ  (sizeof(v4_torus_routing_id_to_user_id) / sizeof(v4_torus_routing_id_to_user_id[0]))

static u32 neuron_pci_routing_id_to_user_id(u32 routing_id)
{
	if (ndhal->ndhal_arch.platform_type == NEURON_PLATFORM_TYPE_PDS) {
		return v4_pds_routing_id_to_user_id[ routing_id % V4_ROUTING_ID_TBL_SZ];
	}
	return v4_torus_routing_id_to_user_id[routing_id % V4_ROUTING_ID_TBL_SZ];
}

/**
 * neuron_pci_get_device_id() - get device id from pacific and set nd->device_index
 *
 * @param dev: PCI device
 * @param nd: neuron device
 * @return int: 0 on success, otherwise on failure
 */
static int neuron_pci_get_device_id_v4(struct neuron_device *nd, struct pci_dev *dev)
{
	int ret = 0;
	int i;
	u32 routing_id = (u32)-1;
	u32 routing_id_max = MAX_NEURON_DEVICE_COUNT;

	if (ndhal_instance_type_3xl()) {
		// Temporarily auto-assign routing_id to 0 for 3xl instances, since they
		// only have 1 device anyways
		routing_id = 0;
	} else {
		// Poll the device id until the device is ready
		for (i = 0; i < 20; i++) {
			ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
			if (!ret && routing_id != 0xdeadbeef) {
				break;
			}
			msleep(1000);
		}
	}

	if (ret) {
		pr_err("Could not retrieve device index (read timeout)");
		return -ENODEV;
	}

	if (ndhal->ndhal_arch.platform_type == NEURON_PLATFORM_TYPE_PDS) {
		u32 server_info = 0;
		bool server_id_valid = 0;
		u32 server_id = 0;
		ret = fw_io_server_info_read(nd->npdev.bar0, &server_info);
		if (ret) {
			return -ENODEV;
		}

		server_id_valid = (server_info >> 15) & 0x1; // TODO we probably need const shift value or macro
		if (server_id_valid) {
			server_id = server_info & 0x7fff; // TODO we probably need constant mask for this
		} else {
			pr_err("Could not retrieve valid server id, ret = %d\n", ret);
			return -ENODEV;
		}
		ndhal->ndhal_arch.server_id = server_id;
		routing_id_max = MAX_NEURON_DEVICE_COUNT * 2;
	}

	// TODO - this should be a "valid routing_id check for TRN3
	if (routing_id < 0 || routing_id >= routing_id_max) {
		pr_err("Invalid device index %u", routing_id);
		return -ENODEV;
	}

	nd->device_index = neuron_pci_routing_id_to_user_id(routing_id);

	pr_err("** BDF: %2.2x:%2.2x.%x => nd[%d] (routing id: %u)\n", dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn), nd->device_index, routing_id);

	// protection against duplicate IDs - doesn't provide 100% protection in multi-threaded device discovery
	if (neuron_devices[nd->device_index] != NULL) {
		pr_err("duplicate routing id %u found\n", routing_id);
		neuron_pci_handle_dup_routing_id();
		return -ENODEV;
	}

	return 0;
}

/**
 * ndhal_register_funcs_v4() - initialize the dhal for v4 chips
 *
 * This function only overrides the functions and
 * constants that are different from v3 in v4.
 */
int ndhal_register_funcs_v4(void) {
	int ret = 0;

	if (!ndhal) {
		pr_err("ndhal is null. Can't register functions for V4.");
		return -EINVAL;
	}

	ndhal->ndhal_arch.platform_type = ndhal_platform_type_v4();
	ndhal->ndhal_pci.neuron_pci_get_device_id = neuron_pci_get_device_id_v4;
	ndhal->ndhal_npe.npe_neighbor_eng_ids = npe_neighbor_eng_ids_v4;
	ndhal->ndhal_mpset.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v4;
	ndhal->ndhal_mmap.dm_mmap_special = dm_mmap_special_v4;
	ndhal->ndhal_mmap.mmap_get_bar4_offset = mmap_get_bar4_offset_v4;
	ndhal->ndhal_cdev.ncdev_mem_regions = ncdev_mem_regions_v4;

	if (narch_is_emu()) {
		// Temporarily disable resets on mariana emulation until pacific is ready
		extern int no_reset;
		no_reset = 1;
	}

	// TODO initialization needs refactoring because V4 is piggybacking on V3
	// which risks double calling any hal init functions
	//
	if (ndhal->ndhal_arch.platform_type == NEURON_PLATFORM_TYPE_ULTRASERVER) {
		ret = npe_init();
		if (ret) {
			pr_err("failed to initialize pod election on V4\n");
			return ret;
		}
	} else if (ndhal->ndhal_arch.platform_type == NEURON_PLATFORM_TYPE_PDS) {
		//TODO PDS
	}	

	switch (ndhal->pci_device_id) {
		case TRN3_DEVICE_ID0:
		case TRN3_DEVICE_ID1:
			ret = ndhal_register_funcs_trn3();
			if (ret) {
				pr_err("failed to register ndhal funcs for trn3.\n");
				return ret;
			}
			break;
		default:
			pr_err("Unknown HW architecture. Can't init neuron_dhal.\n");
			return -EINVAL;
	}

	return ret;
}
