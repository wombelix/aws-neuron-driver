// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_TOPSP_H
#define NEURON_TOPSP_H

/**
 * ts_nq_init() - Initialize notification queue for TopSp.
 *
 * @nd: neuron device
 * @ts_id: ToSp Id
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @size: size of queue in bytes
 * @on_host_memory: if true, NQ is created in host memory
 * @dram_channel: If NQ is created on device memory which DRAM channel to use.
 * @dram_region: If NQ is created on device memory which DRAM region to use.
 * @nq_mc[out]: memchunk used by the NQ will be written here
 * @mc_ptr[out]: Pointer to memchunk backing this NQ
 *
 * Return: 0 on if initialization succeeds, a negative error code otherwise.
 */
int ts_nq_init(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
	       u32 on_host_memory, u32 dram_channel, u32 dram_region,
	       struct mem_chunk **nq_mc, u64 *mmap_offset);

/**
 * ts_nq_destroy() - Cleanup and free notification queue.
 *
 * @nd: neuron device
 * @ts_id: core index in the device
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int ts_nq_destroy(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type);

/**
 * ts_nq_destroy_all() - Disable notification in the device
 *
 * @nd: neuron device
 *
 */
void ts_nq_destroy_all(struct neuron_device *nd);

/**
 * ts_nq_get_info() - Update NQ head and phase bit
 *
 * @nd: neuron device
 * @ts_id: TopSP index
 * @nq_type: type of the notification queue
 * @head: current head would be stored here
 * @phase_bit: current phase bit would be stored here
 */
int ts_nq_get_info(struct neuron_device *nd, u8 ts_id, u32 nq_type, u32 *head, u32 *phase_bit);

/**
 * ts_nq_update_head() - Update NQ head
 *
 * @nd: neuron device
 * @ts_id: TopSP index
 * @nq_type: type of the notification queue
 * @new_head: New index of the head
 */
void ts_nq_update_head(struct neuron_device *nd, u8 ts_id, u32 nq_type, u32 new_head);

#endif
