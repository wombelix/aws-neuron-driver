// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef __NEURON_REG_ACCESS__H_

#define __NEURON_REG_ACCESS__H_

#include <linux/kernel.h>
#include <linux/slab.h>
#include <asm/bug.h>
#include <asm/barrier.h>
#include <asm/delay.h>
#include <asm/io.h>

#include "neuron_arch.h"
#include "neuron_fw_io.h"

extern bool v2_chip;

/**
 * reg_read32() - read a 32bit register.
 * @addr: register address.
 * @value: read value would be stored here.
 * @num_values: num values to read
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
static inline int reg_read32_array(void **addr, u32 *value, u32 num_values)
{
	if (v2_chip && narch_is_qemu()) {
		int i;
		for (i = 0; i < num_values; i++) {
			value[i] = readl(addr[i]);
		}
		return 0;
	} else {
		int ret;
		ret = fw_io_read_csr_array(addr, value, num_values, true);
		if (ret != 0) {
			pr_err("register read failure while reading %p\n", addr[0]);
			dump_stack();
		}
		return ret;
	}
}

/**
 * reg_read32_array() - read a 32bit register.
 * @addr: register address.
 * @value: read value would be stored here.
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
static inline int reg_read32(const u32 __iomem *addr, u32 *value)
{
	return reg_read32_array((void **)&addr, value, 1);
}

/**
 * reg_write32() - write to a 32bit register
 *
 * @addr: register address.
 * @value: value to write.
 */
static inline void reg_write32(u32 __iomem *addr, u32 value)
{
	writel(value, addr);
}

/* take bits selected by mask from one data, the rest from background */
#define MASK_VAL(mask, data, background) (((mask) & (data)) | ((~(mask)) & (background)))

/**
 * reg_write32_masked() - change selected bits in a register.
 *
 * @addr: register address
 * @mask: mask to apply. Bits not selected(1) by mask will be left unchanged
 * @data: value to write
 *
 * Return: 0 if modification succeeds, a negative error code otherwise.
 */
static inline int reg_write32_masked(u32 __iomem *addr, u32 mask, u32 data)
{
	u32 temp;
	int ret;

	ret = reg_read32(addr, &temp);
	if (ret)
		return ret;

	reg_write32(addr, MASK_VAL(mask, data, temp));
	return 0;
}

#endif
