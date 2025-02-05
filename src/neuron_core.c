// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Each neuron device has N number of neuron cores. (inf1 has 4 neuron cores).
 *
 * Engines:
 * -------
 * Neuron Core has multiple engines(inf1 has 3) which can do different types of computations.
 * Each engine's instruction stream is feed through DMA.
 *
 * Semaphores and events:
 * ---------------------
 * For synchronization between hardware blocks and software, NC provides two type synchronization
 * hardware primitives, semaphores and events. Events can be considered simple bitmap which hold
 * either 1 or 0. Semaphores hold any value in signed 32 bit range. Engines can be programmed
 * with instructions which can wait for semaphore to reach a certain value or a particular event
 * is set. Applications can use this to manipulate execution of the program.
 *
 * mmap:
 * ----
 * The following can be mmap() into application's address space.
 *  1. Host DMAable memory
 *  2. Neuron Core's Notification queue
 * Host DMAable memory can be mmapped so that applications can do initialization/compute before
 * it is DMAed to device memory. This avoids a memcpy() from user space to kernel. A total of
 * 16GB per device is allowed to be mmaped.
 *
 * mmap() of notification queue allows application to do polling on the host memory instead of waiting
 * for an interrupt.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "v1/address_map.h"
#include "v2/address_map.h"

#include "neuron_mempool.h"
#include "neuron_device.h"
#include "neuron_nq.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_nc_mmap);
#endif

#define NC_SEMAPHORE_SIZE 4
#define NC_EVENT_SIZE 4

#define MMAP_P_OFFSET(nd)                                                                          		\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_P_OFFSET : V2_MMAP_P_OFFSET)
#define MMAP_NC_EVENT_OFFSET(nd)                                                                   		\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_NC_EVENT_OFFSET : V2_MMAP_NC_EVENT_OFFSET)
#define MMAP_NC_SEMA_READ_OFFSET(nd)                                                               		\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_NC_SEMA_READ_OFFSET : V2_MMAP_NC_SEMA_READ_OFFSET)
#define MMAP_NC_SEMA_SET_OFFSET(nd)                                                                	\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_NC_SEMA_SET_OFFSET : V2_MMAP_NC_SEMA_SET_OFFSET)
#define MMAP_NC_SEMA_INCR_OFFSET(nd)                                                               	\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_NC_SEMA_INCR_OFFSET : V2_MMAP_NC_SEMA_INCR_OFFSET)
#define MMAP_NC_SEMA_DECR_OFFSET(nd)                                                               	\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_NC_SEMA_DECR_OFFSET : V2_MMAP_NC_SEMA_DECR_OFFSET)

#define MMAP_NC_SIZE(nd)                                                                           	\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_MMAP_NC_SIZE : V2_MMAP_NC_SIZE)

#define SEMAPHORE_COUNT(nd)                                                                        	\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_SEMAPHORE_COUNT : V2_SEMAPHORE_COUNT)
#define EVENTS_COUNT(nd)                                                                           	\
	(narch_get_arch() == NEURON_ARCH_V1 ? V1_EVENTS_COUNT : V2_EVENTS_COUNT)

static u64 nc_get_axi_offset(struct neuron_device *nd, int nc_index)
{
	return MMAP_P_OFFSET(nd) + (nc_index * MMAP_NC_SIZE(nd));
}

static void *nc_get_semaphore_base(struct neuron_device *nd, u8 nc_id)
{
	if (narch_get_arch() == NEURON_ARCH_V1)
		return nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id);
	else if (narch_get_arch() == NEURON_ARCH_V2)
		return nd->npdev.bar0 + V2_PCIE_BAR0_TPB_0_OFFSET +
		       (V2_PCIE_BAR0_TPB_0_SIZE * nc_id);
	else
		return NULL;
}

int nc_semaphore_read(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 *result)
{
	void *addr;

	if (semaphore_index > SEMAPHORE_COUNT(nd))
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_READ_OFFSET(nd) + (semaphore_index * NC_SEMAPHORE_SIZE);
	return reg_read32_array((void **)&addr, result, 1);
}

int nc_semaphore_write(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	void *addr;

	if (semaphore_index > SEMAPHORE_COUNT(nd))
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_SET_OFFSET(nd) + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_semaphore_increment(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	void *addr;

	if (semaphore_index > SEMAPHORE_COUNT(nd))
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_INCR_OFFSET(nd) + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_semaphore_decrement(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	void *addr;

	if (semaphore_index > SEMAPHORE_COUNT(nd))
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_DECR_OFFSET(nd) + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

static void *nc_get_event_addr(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void *base;
	if (narch_get_arch() == NEURON_ARCH_V1)
		base = nd->npdev.bar2 + nc_get_axi_offset(nd, nc_id) + MMAP_NC_EVENT_OFFSET(nd);
	else if (narch_get_arch() == NEURON_ARCH_V2)
		base = nd->npdev.bar0 + V2_PCIE_BAR0_TPB_0_OFFSET +
		       (V2_PCIE_BAR0_TPB_0_SIZE * nc_id) + MMAP_NC_EVENT_OFFSET(nd);
	else
		return NULL;
	return (base + (event_index * NC_EVENT_SIZE));
}

int nc_event_get(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 *result)
{
	void *addr;

	if (event_index > EVENTS_COUNT(nd))
		return -EINVAL;

	addr = nc_get_event_addr(nd, nc_id, event_index);
	return reg_read32_array(&addr, result, 1);
}

int nc_event_set(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 value)
{
	u32 *addr;

	if (event_index > EVENTS_COUNT(nd))
		return -EINVAL;

	addr = nc_get_event_addr(nd, nc_id, event_index);
	writel(value, addr);
	return 0;
}

