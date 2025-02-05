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
#include <linux/mutex.h>

#include "neuron_ioctl.h"
#include "neuron_device.h"

#include "v1/address_map.h"
#include "v2/address_map.h"
#include "v1/fw_io.h"
#include "neuron_fw_io.h"

int no_reset = 0;
module_param(no_reset, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(no_reset, "Dont reset device");

#define NR_RESET_INIT_PRE_WAIT_TIME_MS 7000
#define NR_TPB_RESET_INIT_PRE_WAIT_TIME_MS 3000
#define NR_RESET_INIT_PRE_WAIT_TIME_INC_MS 2000
#define NR_RESET_INIT_RETRY_COUNT 5 
#define NR_RESET_RETRY_COUNT 5
#define NR_RESET_RETRY_SLEEP_MS 100

static int nr_msleep_stoppable(struct neuron_device *nd, uint32_t msec) 
{
	unsigned long timeout = msecs_to_jiffies(msec);

	while (timeout && !nd->nr.stop)
		timeout = schedule_timeout_interruptible(timeout);

	return jiffies_to_msecs(timeout);
}

static int nr_initiate_reset(struct neuron_device *nd)
{
	if (no_reset)
		return 0;

	if (narch_is_qemu()) {
		if (narch_get_arch() == NEURON_ARCH_V2) {
			volatile void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_0_RESERVED1_RELBASE + 0x10;
			writel(1, (volatile uint32_t *)addr);
		}
	} else {
		int i, j;
		uint32_t nc_map = nd->nr.req_pending_head->nc_map;
		uint32_t tpb_reset_map = 0;
		// Build the tpb reset map if we are not performing a device reset
		if (narch_get_arch() == NEURON_ARCH_V2 && nc_map != NEURON_NC_MAP_DEVICE) {
			for (i = 0; i < MAX_NC_PER_DEVICE; i++) {
				if ((1 << i) & nc_map) {
					// Add this tpb to the reset map
					tpb_reset_map |= (1 << i);
					int ts_per_nc = V2_TS_PER_DEVICE / V2_NC_PER_DEVICE;
					// Add all top sps owned by this tpb to the reset map
					for (j = i * ts_per_nc; j < (i + 1) * ts_per_nc; j++) {
						tpb_reset_map |= (1 << (8 + j));
					}
				}
			}
		}

        /* Wait times are different for device reset vs nc reset (aka tpb reset) */
		uint32_t reset_init_retry_wait_time =
				nc_map == NEURON_NC_MAP_DEVICE ?
						NR_RESET_INIT_PRE_WAIT_TIME_MS :
						NR_TPB_RESET_INIT_PRE_WAIT_TIME_MS;

		for (i = 0; i < NR_RESET_INIT_RETRY_COUNT; i++) {
			fw_io_initiate_reset(nd->npdev.bar0, nc_map == NEURON_NC_MAP_DEVICE, tpb_reset_map);
			// once reset is initiated, FWIO wont respond until the device
			// comes out of reset, so sleep here for sometime
			if (nr_msleep_stoppable(nd, reset_init_retry_wait_time))
				return -1;
			// Emulator doesn't have readless read support, just return
			if (narch_is_emu()) {
				return 0;
			}
			for (j = 0; j < NR_RESET_RETRY_COUNT; j++) {
				if (fw_io_is_reset_initiated(nd->npdev.bar0))
					return 0;
				if (nd->nr.stop)
					return -1;
			}
			reset_init_retry_wait_time += NR_RESET_INIT_PRE_WAIT_TIME_INC_MS;
		}
		if (i == NR_RESET_INIT_RETRY_COUNT)
			return -1;
	}
	return 0;
}

static int nr_wait_for_reset_completion(struct neuron_device *nd)
{
	if (no_reset)
		return 0;
	if (narch_get_arch() == NEURON_ARCH_V1) {
		int i;
		for (i = 0; i < NR_RESET_RETRY_COUNT; i++) {
			if (fw_io_is_device_ready_v1(nd->npdev.bar0))
				break;
			if (nd->nr.stop)
				return -1;
		}
		if (i == NR_RESET_RETRY_COUNT)
			return -1;
		return 0;
	} else if (narch_get_arch() == NEURON_ARCH_V2) {
		int i;
		uint32_t retry_count = NR_RESET_RETRY_COUNT;
		void *addr = nd->npdev.bar0 + V2_PCIE_BAR0_APB_OFFSET;
		if (narch_is_qemu()) {
			addr += V2_APB_SENG_0_RESERVED1_RELBASE + 0x10;
		} else {
			addr += V2_APB_IOFAB_RELBASE + V2_APB_IOFAB_MISC_RAM_RELBASE + V2_FW_IO_REG_FW_STATUS_OFFSET;
			if (narch_is_emu()) {
				retry_count *= 1000; // wait longer on the emulator
			}
		}

		for (i = 0; i < retry_count; i++) {
			bool reset_in_progress = true;
			u32 status;

			if (narch_is_qemu()) {
				reset_in_progress = readl((volatile uint32_t *)addr);
				msleep(2 * 1000);
			} else {
				if (fw_io_read_csr_array(&addr, &status, 1, false) == 0)
					reset_in_progress = status & V2_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK;
			}
			if (!reset_in_progress)
				return 0;
			if (nr_msleep_stoppable(nd, NR_RESET_RETRY_SLEEP_MS * i)) 
				return -1;
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
		wait_event_interruptible(nd->nr.wait_queue, nd->nr.req_pending_head || nd->nr.stop);
		if (kthread_should_stop() || nd->nr.stop)
			break;
		volatile struct neuron_reset_request *req = nd->nr.req_pending_head;
		enum neuron_reset_state state = NEURON_RESET_STATE_STARTED;
		nd->nr.reset_start_time = get_jiffies_64();
		pr_info("nd%d: initiating reset request %u\n", nd->device_index, req->request_id);
		ret = nr_initiate_reset(nd);
		if (ret) {
			state = NEURON_RESET_STATE_FAILED;
			nsysfsmetric_inc_reset_fail_count(nd);
		} else {
			ret = nr_wait_for_reset_completion(nd);
			if (ret) {
				pr_info("nd%d: device didnt come out reset\n", nd->device_index);
				state = NEURON_RESET_STATE_FAILED;
				nsysfsmetric_inc_reset_fail_count(nd);
			} else {
				if (narch_get_arch() == NEURON_ARCH_V1) {
					fw_io_device_id_write(nd->npdev.bar0, nd->device_index);
				}
				ret = ndmar_init_ncs(nd, req->nc_map);
				if (ret) {
					pr_info("nd%d: failed to initialize dma after reset\n", nd->device_index);
					state = NEURON_RESET_STATE_FAILED;
					nsysfsmetric_inc_reset_fail_count(nd);
				} else {
					pr_info("nd%d: reset request %u completed\n", nd->device_index, req->request_id);
					state = NEURON_RESET_STATE_COMPLETED;
				}
			}
		}
		nd->nr.reset_end_time = get_jiffies_64();
		mutex_lock(&nd->nr.nr_lock);
		// delete from pending list
		nd->nr.req_pending_head = req->next;
		if (!nd->nr.req_pending_head) {
			nd->nr.req_pending_tail = NULL;
		}

		if (req->request_id == NEURON_RESET_REQUEST_ALL) {
			// Update the device state based on reset state, then move on
			// This path is taken by internal driver reset logic, there is no need to move
			// the request to the completion queue, since waiters will be polling device state instead
			if (state == NEURON_RESET_STATE_COMPLETED) {
				nd->device_state = NEURON_DEVICE_STATE_READY;
			} else {
				nd->device_state = NEURON_DEVICE_STATE_INVALID;
			}
			kfree((void *)req);
		} else {
			// add to completed list
			req->next = NULL;
			req->prev = nd->nr.req_cmpl_tail;
			if (nd->nr.req_cmpl_tail) {
				nd->nr.req_cmpl_tail->next = req;
			}
			nd->nr.req_cmpl_tail = req;
			if (!nd->nr.req_cmpl_head) {
				nd->nr.req_cmpl_head = req;
			}
			req->ret = state;
		}
		mutex_unlock(&nd->nr.nr_lock);
	}
	return 0;
}

int nr_create_thread(struct neuron_device *nd)
{
	mutex_init(&nd->nr.nr_lock);
	init_waitqueue_head(&nd->nr.wait_queue);
	nd->nr.thread = kthread_run(nr_reset_thread_fn, nd, "nd%d reset", nd->device_index);
	if (IS_ERR_OR_NULL(nd->nr.thread)) {
		pr_err("nd%d reset thread creation failed\n", nd->device_index);
		return -1;
	}
	return 0;
}

static void nr_free_req_queue(volatile struct neuron_reset_request *req) {
	volatile struct neuron_reset_request *next = NULL;
	while (req) {
		next = req->next;
		kfree((void *)req);
		req = next;
	}
	return;
}

void nr_stop_thread(struct neuron_device *nd)
{
	if (nd->nr.thread == NULL)
		return;
	nd->device_state = NEURON_DEVICE_STATE_INVALID;
	nd->nr.stop = true;
	wake_up(&nd->nr.wait_queue);
	kthread_stop(nd->nr.thread); //blocks till the thread exits
	nd->nr.thread = NULL;
	nr_free_req_queue(nd->nr.req_pending_head);
	nr_free_req_queue(nd->nr.req_cmpl_head);
	mutex_destroy(&nd->nr.nr_lock);
}

// Expects nr_lock to be held
static volatile struct neuron_reset_request *nr_find_req(struct neuron_device *nd, uint32_t request_id) {
	volatile struct neuron_reset_request *curr = nd->nr.req_pending_head;
	while (curr) {
		if (curr->request_id == request_id) {
			return curr;
		}
		curr = curr->next;
	}
	curr = nd->nr.req_cmpl_head;
	while (curr) {
		if (curr->request_id == request_id) {
			return curr;
		}
		curr = curr->next;
	}
	return curr;
}

int nr_start_ncs(struct neuron_device *nd, uint32_t nc_map, uint32_t request_id)
{
	int nc_idx;
	if (no_reset) {
		// if we are in no-reset mode, just mark the device as ready, perform init, and return
		nd->device_state = NEURON_DEVICE_STATE_READY;
		ndmar_init_ncs(nd, NEURON_NC_MAP_DEVICE);
		return 0;
	}

	mutex_lock(&nd->nr.nr_lock);
	if (nr_find_req(nd, request_id)) {
		pr_err("Pending reset request for pid %u on device %u already exists!", request_id, nd->device_index);
		mutex_unlock(&nd->nr.nr_lock);
		return 1;
	}
	if (request_id == NEURON_RESET_REQUEST_ALL) {
		nd->device_state = NEURON_DEVICE_STATE_RESET;
	}
	for (nc_idx = 0; nc_idx < MAX_NC_PER_DEVICE; nc_idx++) {
		if (nc_map == NEURON_NC_MAP_DEVICE || ((1 << nc_idx) & nc_map)) {
			// After reset we want core init to be done again
			nci_reset_state_nc(nd, nc_idx);
			// Reset the model started counter
			nd->nc_model_started_count[nc_idx] = 0;
			nnq_destroy_nc(nd, nc_idx);

			nsysfsmetric_inc_reset_req_count(nd, nc_idx);
		}
	}
	struct neuron_reset_request *req = (struct neuron_reset_request *)kmalloc(sizeof(struct neuron_reset_request), GFP_KERNEL);
	if (!req) {
		pr_err("Failed to allocate memory for reset request %u for nd %u", request_id, nd->device_index);
		mutex_unlock(&nd->nr.nr_lock);
		return 1;
	}
	req->request_id = request_id;
	req->nc_map = nc_map;
	req->ret = NEURON_RESET_STATE_STARTED;
	req->next = NULL;
	if (nd->nr.req_pending_tail) {
		nd->nr.req_pending_tail->next = req;
	}
	nd->nr.req_pending_tail = req;
	if (!nd->nr.req_pending_head) {
		nd->nr.req_pending_head = req;
	}
	mutex_unlock(&nd->nr.nr_lock);
	wake_up_interruptible(&nd->nr.wait_queue);

	return 0;
}

void nr_start(struct neuron_device *nd)
{
	uint32_t request_id = task_tgid_nr(current);
	nr_start_ncs(nd, NEURON_NC_MAP_DEVICE, request_id);
}

int nr_wait(struct neuron_device *nd, uint32_t request_id, bool check)
{
	volatile struct neuron_reset_request *req = NULL;
	if (no_reset) {
		return 0;
	}

	mutex_lock(&nd->nr.nr_lock);
	req = nr_find_req(nd, request_id);
	mutex_unlock(&nd->nr.nr_lock);
	if (req == NULL) {
		if (check) {
			return 0;
		} else {
			pr_err("Invalid reset request id %u", request_id);
			return 1;
		}
	}

	// TODO: be smarter about polling for completion
	// improve this to a per-request semaphore or a wait_queue
	while (req->ret == NEURON_RESET_STATE_STARTED) {
		schedule(); // yield to other processes
	}
	mutex_lock(&nd->nr.nr_lock);
	// Remove from list
	if (req->prev) {
		req->prev->next = req->next;
	} else {
		// first in list
		nd->nr.req_cmpl_head = req->next;
	}
	if (req->next) {
		req->next->prev = req->prev;
	} else {
		// last in list
		nd->nr.req_cmpl_tail = req->prev;
	}
	mutex_unlock(&nd->nr.nr_lock);
	enum neuron_reset_state ret = req->ret;
	kfree((void *)req);
	return ((ret == NEURON_RESET_STATE_COMPLETED) ? 0 : 1);
}

bool nr_op_in_reset_wnd(uint64_t op_start_time, struct neuron_device *nd)
{
	if (time_before_eq64(nd->nr.reset_end_time, nd->nr.reset_start_time)) {
		return true;
	} else if (time_before_eq64(op_start_time, nd->nr.reset_end_time)) {
		return true;
	}

	return false;
}
