// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_RESET_H
#define NEURON_RESET_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/kthread.h>

enum {
	V2_FW_IO_REG_FW_TRIGGER_OFFSET = 0x800,
	V2_FW_IO_REG_FW_STATUS_OFFSET = 0x808,
	V2_FW_IO_REG_FW_STATUS_DEVICE_READY_MASK = 0x8
};

enum neuron_reset_state {
	NEURON_RESET_STATE_STARTED = 1, // Reset is initiated
	NEURON_RESET_STATE_COMPLETED, // Reset is completed successfully
	NEURON_RESET_STATE_FAILED // Reset failed
};

struct neuron_reset {
	struct task_struct *thread; // reset thread
	wait_queue_head_t wait_queue;
	volatile enum neuron_reset_state state; // state of reset
	volatile bool stop; // if set, reset thread would exit the loop
	volatile bool request_pending; // if set, reset thread would start "reset" operation
};

/**
 * nr_create_thread() - Create a thread to reset a neuron device.
 *
 * @nd: Neuron device which will be reset by the thread.
 *
 * Return: 0 on success, -1 on failure
 */
int nr_create_thread(struct neuron_device *nd);

/**
 * nr_stop_thread() - Stop reset thread.
 *
 * @nd: Neuron device
 */
void nr_stop_thread(struct neuron_device *nd);

/**
 * nr_start() - Initiate reset operation on the given device
 *
 * @nd: Neuron device to reset
 */
void nr_start(struct neuron_device *nd);

/**
 * nr_wait() - Waits for reset to complete
 *
 * @nd: Neuron device
 *
 * Return: 0 if reset was successfully completed, 1 otherwise.
 */
int nr_wait(struct neuron_device *nd);

#endif
