// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019-2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef FW_IO_H
#define FW_IO_H

#include "../neuron_fw_io.h"
/**
 * fw_io_is_device_ready_v1() - Checks if the device is ready.
 *
 * @bar0 - Device's BAR0 base address
 *
 * Return: true if device is ready, false if device is still coming out of reset.
 */
bool fw_io_is_device_ready_v1(void __iomem *bar0);


/**
 * fw_io_topology_v1() - Discovers devices connected to the given device.
 *
 * @ctx: FWIO context of the device for which topology
 * @device_ids:  Connected device IDs are stored here.
 * @count: Number of devices connected to the given device.
 *
 * Return: 0 on success.
 *
 */
int fw_io_topology_v1(struct fw_io_ctx *ctx, u32 *device_ids, int *count);

#endif