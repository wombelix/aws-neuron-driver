// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "address_map.h"

#include "../neuron_reg_access.h"
#include "../neuron_device.h"
#include "../neuron_arch.h"

#include "fw_io.h"


// offsets in MISC RAM for FWIO
enum {
	V1_FW_IO_REG_FW_STATUS_OFFSET = 0x1c,
	V1_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK = 0x80000000,
	V1_FW_IO_REG_FW_STATUS_EAST_LINK_MASK = 0x1,
	V1_FW_IO_REG_FW_STATUS_WEST_LINK_MASK = 0x2,
};


// Max wait time seconds for the device to be ready
#define DEVICE_MAX_READY_WAIT 60

/** Wait for device to become ready.
 *
 * @param ctx		- FWIO context of the device.
 * @return true		- if device is ready.
 * 	   false	- if device is not ready even after waiting.
 */
static bool fw_io_wait_for_device_ready_v1(struct fw_io_ctx *ctx, u32 *reg_val)
{
	int i, ret;
	u64 addr = P_0_APB_MISC_RAM_BASE + V1_FW_IO_REG_FW_STATUS_OFFSET;
	for (i = 0; i < DEVICE_MAX_READY_WAIT; i++) {
		ret = fw_io_read(ctx, &addr, reg_val, 1);
		if (ret) {
			pr_err("failed to read device ready state\n");
			return false;
		}
		if (*reg_val & V1_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK)
			return true;
		msleep(1000);
	}
	return false;
}

int fw_io_topology_v1(struct fw_io_ctx *ctx, u32 *device_ids, int *count)
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
		device_ids[i] = -1;

	// if east link is up, read the link's device's address
	if (reg_val & V1_FW_IO_REG_FW_STATUS_EAST_LINK_MASK) {
		addr = PCIEX4_0_BASE | (P_0_APB_MISC_RAM_BASE + FW_IO_REG_DEVICE_ID_OFFSET);
		ret = fw_io_read(ctx, &addr, &device_ids[found], 1);
		if (ret) {
			pr_err("failed to read east device id\n");
			return 1;
		}
		found++;
	}
	// if west link is up, read the link's device's address
	if (reg_val & V1_FW_IO_REG_FW_STATUS_WEST_LINK_MASK) {
		addr = PCIEX4_1_BASE | (P_0_APB_MISC_RAM_BASE + FW_IO_REG_DEVICE_ID_OFFSET);
		ret = fw_io_read(ctx, &addr, &device_ids[found], 1);
		if (ret) {
			pr_err("failed to read west device id\n");
			return 1;
		}
		found++;
	}
	*count = found;

	return 0;
}

bool fw_io_is_device_ready_v1(void __iomem *bar0)
{
	void *address;
	int ret;
	u32 val;

	address = bar0 + V1_MMAP_BAR0_APB_MISC_RAM_OFFSET + V1_FW_IO_REG_FW_STATUS_OFFSET;
	ret = fw_io_read_csr_array((void **)&address, &val, 1, false);
	if (ret)
		return false;
	if (val & V1_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK)
		return true;
	return false;
}
