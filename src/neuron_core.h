// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_CORE_H
#define NEURON_CORE_H

/**
 * nc_semaphore_read() - Read current semaphore value
 *
 * @nd: neuron device from which semaphore needs to be read
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @result: location to store the result
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
int nc_semaphore_read(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 *result);

/**
 * nc_semaphore_write() - Write given value on a semaphore
 *
 * @nd: neuron device on which semaphore operation needs to be performed
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @value: value to set
 *
 * Return: 0 if write succeeds, a negative error code otherwise.
 */
int nc_semaphore_write(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value);

/**
 * nc_semaphore_increment() - Increment a semaphore by given value
 *
 * @nd: neuron device on which semaphore operation needs to be performed
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @value: value to increment
 *
 * Return: 0 if increment succeeds, a negative error code otherwise.
 */
int nc_semaphore_increment(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value);

/**
 * nc_semaphore_decrement() - Decrement a semaphore by given value
 *
 * @nd: neuron device on which semaphore operation needs to be performed
 * @nc_id: core which has the semaphore
 * @semaphore_index: index of the semaphore
 * @value: value to decrement
 *
 * Return: 0 if decrement succeeds, a negative error code otherwise.
 */
int nc_semaphore_decrement(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value);

/**
 * nc_event_get() - Get current value of given event
 *
 * @nd: neuron device on which event operation needs to be performed
 * @nc_id: core which has the event
 * @event_index: index of the event
 * @value: result is stored here(0 or 1)
 *
 * Return: 0 if event read succeeds, a negative error code otherwise.
 */
int nc_event_get(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 *result);

/**
 * nc_event_set() - Set or clear given event
 *
 * @nd: neuron device on which event operation needs to be performed
 * @nc_id: core which has the event
 * @event_index: index of the event
 * @value: value to set(0 or 1)
 *
 * Return: 0 if event set succeeds, a negative error code otherwise.
 */
int nc_event_set(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 value);

// followin defines have the max between versions of chip
// please check the chip's address_map.h to find the values
#define MAX_NQ_TYPE 4
#define MAX_NQ_ENGINE 4
#define NQ_TYPE_PER_ENGINE 4

#define MAX_NQ_SUPPORTED (MAX_NQ_TYPE * MAX_NQ_ENGINE)

/**
 * nc_get_nq_mem_handle() - Get notification queue's mem handle for given neuron core.
 *
 * @nd: neuron device
 * @nc_id: neuron core index.
 * @engine_index: engine index in the neuron core.
 * @nq_type: notification type.
 * @handle: handle for the notification queue is stored here.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_get_nq_mem_handle(struct neuron_device *nd, int nc_id, int engine_index, int nq_type, u64 *handle);

/**
 * nc_get_nq_mem_handle() - Get notification queue's mem handle for given neuron core.
 *
 * @nd: neuron device
 * @nc_id: neuron core index.
 * @engine_index: engine index in the neuron core.
 * @nq_type: notification type.
 * @handle: handle for the notification queue is stored here.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nc_get_nq_mem_handle(struct neuron_device *nd, int nc_id, int engine_index, int nq_type, u64 *handle);

/**
 * nnq_init() - Initialize notification queue for NeuronCore
 *
 * @nd: neuron device
 * @nc_id: core index in the device
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
int nnq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size,
	       u32 on_host_memory, u32 dram_channel, u32 dram_region,
	       struct mem_chunk **nq_mc, u64 *mmap_offset);


/**
 * nnq_destroy_all() - Disable notification in the device
 *
 * @nd: neuron device
 *
 */
void nnq_destroy_all(struct neuron_device *nd);

/**
 * nc_nq_device_init() - Initialize the mc's in device
 *
 * @nd: neuron device
 *
 */
void nc_nq_device_init(struct neuron_device *nd);

/**
 * nc_ds_mmap_offset() - Return mmap offset for the datastore for the given nc.
 *
 * @nc_index: neuroncore index
 * @nd: neuron device
 *
 * Return: offset to be used to mmap in the /dev/ndX file or (u64)(-1) in case of invalid nc index
 */
u64 nc_ds_mmap_offset(struct neuron_device *nd, u8 nc_index);

/**
 * nnq_get_nq_info() - Get notification queue information.
 *
 * @nd: neuron device
 * @nq_dev_id: neuron core index or top_sp index
 * @use_top_sp: if 1 then use top_sp else use neuron core
 * @eng_index: engine index
 * @nq_type: NQ type
 * @head: Current head pointer would be stored here.
 * @phase_bit: Current phase bit of the queue would be stored here.
 *
 * @return 0 on success
 */
int nnq_get_nq_info(struct neuron_device *nd, u8 nq_dev_id, u8 use_top_sp, u8 eng_index, u32 nq_type, u32 *head, u32 *phase_bit);

/**
 * nc_track_register_write() - This function would be called when register write is called.
 *
 * @nd: neuron device
 * @bar: BAR number where write is destined.
 * @offset: Offset in the BAR.
 * @value: Value being written.
 */
void nc_track_register_write(struct neuron_device *nd, int bar, u64 offset, u32 value);

/**
 * nnq_reset() - Reset software state for all NQ in given device.
 *
 * @nd: neuron device
 */
void nnq_reset(struct neuron_device *nd);

#endif
