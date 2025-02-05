// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Creates a thread which resets the device.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/module.h>

#include "neuron_ioctl.h"
#include "neuron_device.h"

#include "v1/address_map.h"
#include "v2/address_map.h"
#include "v1/fw_io.h"

int no_reset = 0;
module_param(no_reset, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(no_reset, "Dont reset device");

#define NR_RESET_INIT_PRE_WAIT_TIME_MS 7000
#define NR_RESET_INIT_RETRY_COUNT 5
#define NR_RESET_INIT_RETRY_SLEEP_MS 100
#define NR_RESET_RETRY_COUNT 5
#define NR_RESET_RETRY_SLEEP_MS 100

static int nr_initiate_reset(struct neuron_device *nd)
{
	if (no_reset)
		return 0;
	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		int i, j;
		if (narch_is_qemu())
			return 0;
		for (i = 0; i < NR_RESET_INIT_RETRY_COUNT; i++) {
			fw_io_initiate_reset(nd->npdev.bar0);
			// once reset is initiated, FWIO wont respond until the device
			// comes out of reset, so sleep here for sometime
			msleep(NR_RESET_INIT_PRE_WAIT_TIME_MS * (i + 1));
			for (j = 0; j < NR_RESET_RETRY_COUNT; j++) {
				if (fw_io_is_reset_initiated(nd->npdev.bar0))
					return 0;
				msleep(NR_RESET_INIT_RETRY_SLEEP_MS * j);
			}
		}
		if (i == NR_RESET_INIT_RETRY_COUNT)
			return -1;
	} else if (narch_get_arch() == NEURON_ARCH_TRN) {
		volatile void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET;
		if (narch_is_qemu())
			addr += V2_APB_SENG_0_RESERVED1_RELBASE + 0x10;
		else
			addr += V2_APB_IOFAB_RELBASE + V2_APB_IOFAB_MISC_RAM_RELBASE + V2_FW_IO_REG_FW_TRIGGER_OFFSET;
		writel(1, (volatile uint32_t *)addr);
		// wait a bit to give emulator a chance to kick off reset
		if (narch_is_emu()) {
			ssleep(5);
		}
	} else {
		BUG();
	}
	return 0;
}

static int nr_wait_for_reset_completion(struct neuron_device *nd)
{
	if (no_reset)
		return 0;
	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		int i;
		for (i = 0; i < NR_RESET_RETRY_COUNT; i++) {
			if (fw_io_is_device_ready(nd->npdev.bar0))
				break;
			msleep(NR_RESET_RETRY_SLEEP_MS * i);
		}
		if (i == NR_RESET_RETRY_COUNT)
			return -1;
		return fw_io_topology(nd->fw_io_ctx, nd->connected_devices,
				      &nd->connected_device_count);
	} else if (narch_get_arch() == NEURON_ARCH_TRN) {
		int i;
		uint32_t retry_count = NR_RESET_RETRY_COUNT;
		volatile void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET;
		if (narch_is_qemu()) {
			addr += V2_APB_SENG_0_RESERVED1_RELBASE + 0x10;
		} else {
			addr += V2_APB_IOFAB_RELBASE + V2_APB_IOFAB_MISC_RAM_RELBASE + V2_FW_IO_REG_FW_STATUS_OFFSET;
			if (narch_is_emu()) {
				retry_count *= 1000; // wait longer on the emulator
			}
		}

		for (i = 0; i < retry_count; i++) {
			bool reset_in_progress;

			if (narch_is_qemu()) {
				reset_in_progress = readl((volatile uint32_t *)addr);
				msleep(2 * 1000);
			} else {
				reset_in_progress = readl((volatile uint32_t *)addr) &
						    V2_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK;
			}
			if (!reset_in_progress)
				return 0;
			msleep(NR_RESET_RETRY_SLEEP_MS * i);
		}
		return -1;
	} else {
		BUG();
	}
	return 0;
}

static int nr_reset_thread_fn(void *arg)
{
	int ret;
	struct neuron_device *nd = (struct neuron_device *)arg;

	while (!kthread_should_stop() && !nd->nr.stop) {
		// sleep until there is a request pending or asked to stop
		wait_event_interruptible(nd->nr.wait_queue, nd->nr.request_pending || nd->nr.stop);
		if (kthread_should_stop() || nd->nr.stop)
			break;
		pr_info("nd%d: initiating reset\n", nd->device_index);
		nd->nr.state = NEURON_RESET_STATE_STARTED;
		ret = nr_initiate_reset(nd);
		if (ret) {
			nd->nr.state = NEURON_RESET_STATE_FAILED;
			continue;
		}
		ret = nr_wait_for_reset_completion(nd);
		if (ret) {
			pr_info("nd%d: device didnt come out reset\n", nd->device_index);
			nd->nr.state = NEURON_RESET_STATE_FAILED;
			continue;
		} else {
			pr_info("nd%d: reset completed\n", nd->device_index);
			nd->nr.state = NEURON_RESET_STATE_COMPLETED;
			nd->nr.request_pending = false;
		}
	}
	return 0;
}

int nr_create_thread(struct neuron_device *nd)
{
	nd->nr.state = NEURON_RESET_STATE_COMPLETED; // after boot - we have clean slate
	init_waitqueue_head(&nd->nr.wait_queue);
	nd->nr.thread = kthread_run(nr_reset_thread_fn, nd, "nd%d reset", nd->device_index);
	if (IS_ERR_OR_NULL(nd->nr.thread)) {
		pr_err("nd%d reset thread creation failed\n", nd->device_index);
		return -1;
	}
	return 0;
}

void nr_stop_thread(struct neuron_device *nd)
{
	if (nd->nr.thread == NULL)
		return;
	nd->nr.stop = true;
	wake_up(&nd->nr.wait_queue);
	kthread_stop(nd->nr.thread); //blocks till the thread exits
	nd->nr.thread = NULL;
}

void nr_start(struct neuron_device *nd)
{
	if (no_reset)
		return;
	nd->nr.state = NEURON_RESET_STATE_STARTED;
	nd->nr.request_pending = true;

	// After reset we want core init to be done again
	nci_reset_state(nd);
	// Reset the model started counter
	memset((void *)nd->nc_model_started_count, 0, sizeof(u64) * MAX_NC_PER_DEVICE);
	nnq_reset(nd);
	nnq_destroy_all(nd);
	wake_up_interruptible(&nd->nr.wait_queue);
}

int nr_wait(struct neuron_device *nd)
{
	if (no_reset)
		return 1;

	while (nd->nr.state == NEURON_RESET_STATE_STARTED)
		schedule(); // yield to other processes

	return nd->nr.state == NEURON_RESET_STATE_COMPLETED;
}
