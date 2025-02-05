// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_NQ_H
#define NEURON_NQ_H

#include <linux/kernel.h>
#include <linux/types.h>
#include "neuron_device.h"

/**
 * nnq_track_register_write() - This function would be called when register write is called.
 *
 * @nd: neuron device
 * @bar: BAR number where write is destined.
 * @offset: Offset in the BAR.
 * @value: Value being written.
 *
 * @Return 0 on the register write is within NQ space, -1 otherwise.
 */
int nnq_track_register_write(struct neuron_device *nd, int bar, u64 offset, u32 value);

#endif