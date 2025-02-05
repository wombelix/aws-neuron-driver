// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Definitions of data structures shared between user space application and driver. */

#ifndef NEURON_IOCTL_H
#define NEURON_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#include "share/neuron_driver_shared.h"

struct neuron_ioctl_mem_alloc {
	__u64 size; // [in] Allocation size
	__u32 host_memory; // [in] If true allocates from host memory; else allocates from device memory
	__u32 dram_channel; // [in] DRAM channel in device memory
	__u32 dram_region; // [in] DRAM region in device memory
	__u32 nc_id; // [in] NeuronCore id(valid only if location is device)
	__u64 *mem_handle; // [out] Allocated memory handle would stored here.
};

struct neuron_ioctl_device_init {
	/* Splits DRAM in the device into smaller regions.
	 * This improves performance of DDR by allowing parallel DMA using different regions.
	 * However reduces amount memory available for each NeuronCore.
	 */
	__u32 mem_regions; // [in] How many regions to create in the device memory
};

struct neuron_ioctl_mem_get_info {
	__u64 mem_handle; // [in] Memory handle of the allocated memory.
	__u64 *mmap_offset; // [out] offset where this mem can be mmapped
	__u64 *pa; // [out] Physical address of the memory
};

struct neuron_ioctl_get_apps_info {
	__u16 apps_info_flags; // Requested entries
	__u32 capacity; // [in] Size of the array allocated by the caller for the data (in number of items)
	__u32 size; // [out] Number of entries the driver has wrote in app_data
	struct neuron_app_info app_data[]; // [out] Array containing app data
};

struct neuron_ioctl_mem_get_pa {
	__u64 mem_handle; // [in] Memory handle of the allocated memory.
	__u64 *pa; // [out] Physical address of the memory
};

struct neuron_ioctl_mem_get_extended_info {
	__u64 mem_handle; // [in] Memory handle of the allocated memory.
	__u32 version; // [in] version of this structure - (current version is 1).
	__u32 host_memory; // [out] true if allocation is from host memory
	__u64 mmap_offset; // [out] offset where this mem can be mmapped
	__u64 pa; // [out] Physical address of the memory
	__u64 pid; // [out] Process that allocated this memory
	__u64 size; // [out] Memory allocation size
};

struct neuron_ioctl_mem_free {
	__u64 mem_handle; // [in] Memory handle to be freed.
};

struct neuron_ioctl_mem_copy {
	__u64 src_mem_handle; // [in] Source memory handle from where data is copied.
	__u64 dst_mem_handle; // [in] Destination memory handle to data is to be copied.
	__u32 size; // [in] Size of the transfer.
	__u32 src_offset; // [in] Offset in the source memory handle.
	__u32 dst_offset; // [in] Offset in the destination memory handle.
};

struct neuron_ioctl_memset {
	__u64 mem_handle; // [in] Destination memory handle to data is to be copied.
	__u64 offset; // [in] Offset in the memory handle.
	__u32 value; // [in] value to set the memory with
	__u32 size; // [in] Size of the transfer.
};

struct neuron_ioctl_mem_buf_copy {
	__u64 mem_handle; // [in] Source or Destination memory handle from/to data needs to be copied.
	void *buffer; // [in] Buffer from/to where data to be copied.
	__u32 size; // [in] Size of the data to be copied.
	__u32 offset; // [in] Offset in the memory handle where the data to be written/read.
	__u32 copy_to_mem_handle; // [in] if set to True copies from buffer to memhandle else copies from memhandle to buffer.
};

struct neuron_ioctl_program_engine {
	__u64 dst; // [in] Destination engine address
	void *buffer; // [in] Buffer from/to where data to be copied.
	__u32 size; // [in] Size of the data to be copied.
	__u32 offset; // [in] Offset in the dst address where the data to be written/read.
};

struct neuron_ioctl_bar_rw {
	__u32 bar; // [in] BAR index
	__u64 *address; // [in] Array of register addresses.
	__u32 *data; // [in/out] Buffer from where to data is read or written.
	__u32 count; // [in] Number of registers to read or write.
};

struct neuron_ioctl_post_metric {
	__u32 *data; // [in] Buffer from where to data is read.
	__u32 data_size; // [in] Total data size
};

struct neuron_ioctl_dma_copy_descriptors {
	__u64 mem_handle; // [in] Source or Destination memory handle from/to data needs to be copied.
	void *buffer; // [in] Buffer from/to where data to be copied.
	__u32 num_descs; // [in] Number of descs to copy
	__u32 offset; // [in] Offset in the memory handle where the data to be written/read.
	enum neuron_dma_queue_type queue_type; // [in] specifies whether it is RX/TX queue
};

struct neuron_ioctl_dma_queue_init {
	__u32 eng_id; // [in] DMA engine index
	__u32 qid; // [in] Queue index in the DMA engine
	__u32 tx_desc_count; // [in] number of tx desc's need to be allocated
	__u32 rx_desc_count; // [in] number of rx desc's need to be allocated
	__u64 tx_handle; // [in] mem handle for the tx ring
	__u64 rx_handle; // [in] mem handle for the rx ring
	__u64 rxc_handle; // [in] mem handle for the rxc ring
	__u32 axi_port; // [in] axi port
};

struct neuron_ioctl_dma_queue_release {
	__u32 eng_id; // [in] DMA engine index
	__u32 qid; // [in] Queue index in the DMA engine
};

struct neuron_ioctl_dma_ack_completed {
	__u32 eng_id; // [in] DMA engine index
	__u32 qid; // [in] Queue index in the DMA engine
	__u32 count; // [in] number of desc's that needs to be ack'd
};

struct neuron_ioctl_dma_queue_copy_start {
	__u32 eng_id; // [in] DMA engine index
	__u32 qid; // [in] Queue index in the DMA engine
	__u32 tx_desc_count; // [in] number of tx desc's need to be allocated
	__u32 rx_desc_count; // [in] number of rx desc's need to be allocated
};

struct neuron_ioctl_dma_eng_init {
	__u32 eng_id; // [in] DMA engine index
};

struct neuron_ioctl_dma_eng_set_state {
	__u32 eng_id; // [in] DMA engine index
	__u32 state; // [in] state to set
};

struct neuron_ioctl_dma_eng_get_state {
	__u32 eng_id; // [in] DMA engine index
	struct neuron_dma_eng_state *state; // [out] engine state
};

struct neuron_ioctl_dma_queue_get_state {
	__u32 eng_id; // [in] DMA engine index
	__u32 qid; // [in] Queue index in the DMA engine
	struct neuron_dma_queue_state *tx; // [out] tx queue state
	struct neuron_dma_queue_state *rx; // [out] tx queue state
};

struct neuron_ioctl_dma_descriptor_copyout {
	__u32 eng_id; // [in] DMA engine index
	__u32 qid; // [in] Queue index in the DMA engine
	enum neuron_dma_queue_type type; //[in] Queue type
	__u32 start_index; // [in] Starting descriptor index.
	__u32 count; // [in] Number of desc's need to be copied out
	void *buffer; // [out] Buffer to store the descriptors
};

struct neuron_ioctl_semaphore {
	__u32 nc_id; // [in] Neuron Core Index
	__u32 semaphore_index; // [in] Semaphore Index
	__u32 value; //[in/out] Value to read/write
};

struct neuron_ioctl_event {
	__u32 nc_id; // [in] Neuron Core Index
	__u32 event_index; // [in] Semaphore Index
	__u32 value; //[in/out] Value to read/write
};

struct neuron_ioctl_notifications_init_v1 {
	__u32 nc_id; // [in] Neuron Core Index
	__u32 nq_type; // [in] Notification queue type
	__u32 engine_index; // [in] Engine Index.
	__u32 size; // [in] Notification queue size in bytes
	__u64 mmap_offset; // [out] mmap() offset for this NQ
};

struct neuron_ioctl_notifications_init_v2 {
	__u32 nq_dev_id; // [in] Notification device Index
	__u32 nq_topsp; // [in] If true, notification for TopSp else NeuronCore.
	__u32 nq_type; // [in] Notification queue type
	__u32 engine_index; // [in] Engine Index.
	__u32 size; // [in] Notification queue size in bytes
	__u32 on_host_memory; // [in] If true allocates NQ in host memory; else allocates in device memory
	__u32 dram_channel; // [in] DRAM channel in device memory
	__u32 dram_region; // [in] DRAM region in device memory
	__u64 mmap_offset; // [out] mmap() offset for this NQ
	__u64 mem_handle; // [out] mem_handle for this NQ
};

struct neuron_ioctl_neuron_ds_info {
	pid_t pid; // [in] PID for this request, 0 to use own requester PID
	__u64 mmap_offset; // [out] mmap() offset for this ds
	__u32 size; // [out] size of memory allocated for this ds
};

struct neuron_ioctl_notifications_destroy {
	__u64 mmap_offset; // [in] NQ's mmap offset
};

struct neuron_ioctl_notifications_destroy_nq {
	__u32 nq_dev_id; // [in] Notification device Index
	__u32 nq_type; // [in] Notification queue type
	__u32 engine_index; // [in] Engine Index.
};

struct neuron_ioctl_notifications_queue_info {
	__u8 nq_dev_id; // [in] Neuron Core Index or top sp index
	__u8 nq_top_sp; // [in] If set then get info for top sp NQ
	__u8 nq_type; // [in] Notification queue type
	__u8 engine_index; // [in] Engine Index.
	__u32 head; // [out] Notification queue head
	__u32 phase_bit; // [out] Notification queue's current phase_bit
};

struct neuron_ioctl_read_hw_counters {
	__u64 *address; // [in] Array of register addresses.
	__u32 *data; // [iout] Buffer from where to data written.
	__u32 count; // [in] Number of registers to read or write.
};

struct neuron_ioctl_crwl {
	__u32 nc_id; // [in] neuron core index
	struct neuron_uuid uuid; // [in] model identifier
};

struct neuron_ioctl_crwl_nc_map {
	__u32 nc_count; // [in] number of neuron cores needed/available.
	__u32 start_nc_index; // [in] starting neuron core index from which search should start.
	__u32 end_nc_index; // [in] ending neuron core index.
	__u32 max_nc_available; // [out] max free nc available.
	volatile long unsigned int bitmap; // [in/out] bitmap of neuron cores.
};

struct neuron_ioctl_cinit_set {
	__u32 nc_id; // [in] neuron code id whose init state that needs to be set
	__u32 state; // [in] state to set
	__u32 new_state; // [out] new state after the set is called
};

struct neuron_ioctl_nc_model_started_count {
	__u32 nc_id; // [in] neuron code id whose init state that needs to be set
	__u64 started_count; // [out] number of times model start is called
};

struct neuron_ioctl_compatible_version {
	__u32 max; // [out] the highest supported RT version
	__u32 min; // [out] the lowest supported RT version
};

#define NEURON_IOCTL_MAX_CONNECTED_DEVICES 8
#define NEURON_MAX_BARS 2
struct neuron_ioctl_device_info {
	__u32 architecture; // [out] Architecture of the device
	__u32 revision; // [out] Revision of the board
	__u32 connected_device_count; // [out] Number devices connected
	__u32 connected_devices[NEURON_IOCTL_MAX_CONNECTED_DEVICES]; // [out] List of connected device ids
	__u64 bar_address[NEURON_MAX_BARS]; // [out] BAR addresses
	__u64 bar_size[NEURON_MAX_BARS]; // [out] Size of the bar
};

#define NEURON_IOCTL_BASE 'N'

/* Deprecated reset related IOCTLs. Now it would always return success. */
#define NEURON_IOCTL_DEVICE_RESET _IO(NEURON_IOCTL_BASE, 1)
#define NEURON_IOCTL_DEVICE_RESET_STATUS _IOR(NEURON_IOCTL_BASE, 2, __u8)
#define NEURON_IOCTL_DEVICE_READY _IOR(NEURON_IOCTL_BASE, 2, __u8)

/** Returns devices information and connection topology. */
#define NEURON_IOCTL_DEVICE_INFO _IOR(NEURON_IOCTL_BASE, 3, struct neuron_ioctl_device_info *)

/* Deprecated reset related IOCTLs. Now it would always return success. */
#define NEURON_IOCTL_DEVICE_INIT _IOR(NEURON_IOCTL_BASE, 4, struct neuron_ioctl_device_init *)
#define NEURON_IOCTL_DEVICE_RELEASE _IO(NEURON_IOCTL_BASE, 5)

/** Returns current application pid using the device. */
#define NEURON_IOCTL_DEVICE_APP_PID _IOR(NEURON_IOCTL_BASE, 6, __s32)
#define NEURON_IOCTL_DEVICE_GET_ALL_APPS_INFO _IOR(NEURON_IOCTL_BASE, 7, struct neuron_ioctl_get_apps_info *)

/** Read from BAR */
#define NEURON_IOCTL_BAR_READ _IOR(NEURON_IOCTL_BASE, 11, struct neuron_ioctl_bar_rw *)
/** Write to BAR */
#define NEURON_IOCTL_BAR_WRITE _IOW(NEURON_IOCTL_BASE, 12, struct neuron_ioctl_bar_rw *)
/** Write to metric in misc ram */
#define NEURON_IOCTL_POST_METRIC _IOW(NEURON_IOCTL_BASE, 13, struct neuron_ioctl_post_metric *)

/** Allocated memory and return a memory_handle. */
#define NEURON_IOCTL_MEM_ALLOC _IOR(NEURON_IOCTL_BASE, 21, struct neuron_ioctl_mem_alloc *)
/** Free given memory_handle. */
#define NEURON_IOCTL_MEM_FREE _IOR(NEURON_IOCTL_BASE, 22, struct neuron_ioctl_mem_free *)
/** Copy data between two memory handles. (using DMA) */
#define NEURON_IOCTL_MEM_COPY _IOR(NEURON_IOCTL_BASE, 23, struct neuron_ioctl_mem_copy *)
/** Copy data from/to given host buffer to/from memory_handle. (using DMA)*/
#define NEURON_IOCTL_MEM_BUF_COPY _IOWR(NEURON_IOCTL_BASE, 24, struct neuron_ioctl_mem_buf_copy *)
/** DONT USE THIS IOCTL INSTEAD USE NEURON_IOCTL_MEM_GET_EXTENDED_INFO */
#define NEURON_IOCTL_MEM_GET_PA _IOR(NEURON_IOCTL_BASE, 25, struct neuron_ioctl_mem_get_pa *)
#define NEURON_IOCTL_PROGRAM_ENGINE _IOWR(NEURON_IOCTL_BASE, 26, struct neuron_ioctl_program_engine *)
/** DONT USE THIS IOCTL INSTEAD USE NEURON_IOCTL_MEM_GET_EXTENDED_INFO */
#define NEURON_IOCTL_MEM_GET_INFO _IOR(NEURON_IOCTL_BASE, 26, struct neuron_ioctl_mem_get_info *)
/** Meset zeros on the hanlde */
#define NEURON_IOCTL_MEMSET _IOR(NEURON_IOCTL_BASE, 27, struct neuron_ioctl_memset *)
/** Returns information of given memory_handle such as PA and mmap offset and size.
 *  Application can use this info to generate DMA descriptors or mmap memory.
 */
#define NEURON_IOCTL_MEM_GET_EXTENDED_INFO _IOR(NEURON_IOCTL_BASE, 28, struct neuron_ioctl_mem_get_extended_info *)


/** Deprecated - Initialize DMA engine. */
#define NEURON_IOCTL_DMA_ENG_INIT _IOR(NEURON_IOCTL_BASE, 30, struct neuron_ioctl_dma_eng_init *)

/** Change DMA engine state to - Start or Disable */
#define NEURON_IOCTL_DMA_ENG_SET_STATE _IOR(NEURON_IOCTL_BASE, 31, struct neuron_ioctl_dma_eng_set_state *)
/** Returns current state of the DMA engine*/
#define NEURON_IOCTL_DMA_ENG_GET_STATE _IOWR(NEURON_IOCTL_BASE, 32, struct neuron_ioctl_dma_eng_get_state *)
/** Initializes given DMA queue */
#define NEURON_IOCTL_DMA_QUEUE_INIT _IOR(NEURON_IOCTL_BASE, 33, struct neuron_ioctl_dma_queue_init *)
/** Releases given DMA queue */
#define NEURON_IOCTL_DMA_QUEUE_RELEASE _IOR(NEURON_IOCTL_BASE, 34, struct neuron_ioctl_dma_queue_release *)
/** Starts DMA transfer of given number of descriptors */
#define NEURON_IOCTL_DMA_QUEUE_COPY_START _IOR(NEURON_IOCTL_BASE, 35, struct neuron_ioctl_dma_queue_copy_start *)
/** Acks the HW, the transfer completion of given number of descriptors*/
#define NEURON_IOCTL_DMA_ACK_COMPLETED _IOR(NEURON_IOCTL_BASE, 36, struct neuron_ioctl_dma_ack_completed *)
/** Returns currents state of the DMA Queue*/
#define NEURON_IOCTL_DMA_QUEUE_GET_STATE _IOWR(NEURON_IOCTL_BASE, 37, struct neuron_ioctl_dma_queue_get_state *)
/** Copy applications created descriptors to DMA queue */
#define NEURON_IOCTL_DMA_COPY_DESCRIPTORS _IOR(NEURON_IOCTL_BASE, 38, struct neuron_ioctl_dma_copy_descriptors *)
/** Copy descriptors in the Queue to host memory */
#define NEURON_IOCTL_DMA_DESCRIPTOR_COPYOUT _IOWR(NEURON_IOCTL_BASE, 39, struct neuron_ioctl_dma_descriptor_copyout *)

/** Increment, decrement, get and set operations on NeuronCore's sempahore and events
 *  Applications can use semaphore and event to synchronize with host software.
 */
#define NEURON_IOCTL_SEMAPHORE_INCREMENT _IOR(NEURON_IOCTL_BASE, 41, struct neuron_ioctl_semaphore *)
#define NEURON_IOCTL_SEMAPHORE_DECREMENT _IOR(NEURON_IOCTL_BASE, 42, struct neuron_ioctl_semaphore *)
#define NEURON_IOCTL_SEMAPHORE_READ _IOWR(NEURON_IOCTL_BASE, 43, struct neuron_ioctl_semaphore *)
#define NEURON_IOCTL_SEMAPHORE_WRITE _IOR(NEURON_IOCTL_BASE, 44, struct neuron_ioctl_semaphore *)
#define NEURON_IOCTL_EVENT_SET _IOR(NEURON_IOCTL_BASE, 45, struct neuron_ioctl_semaphore *)
#define NEURON_IOCTL_EVENT_GET _IOWR(NEURON_IOCTL_BASE, 46, struct neuron_ioctl_semaphore *)

/** Initializes notification queues in the neuron core. */
#define NEURON_IOCTL_NOTIFICATIONS_INIT_V1 _IOR(NEURON_IOCTL_BASE, 51, struct neuron_ioctl_notifications_init_v1 *)
#define NEURON_IOCTL_NOTIFICATIONS_DESTROY_V1 _IOR(NEURON_IOCTL_BASE, 52, struct neuron_ioctl_notifications_destroy *)
#define NEURON_IOCTL_NOTIFICATIONS_INIT_V2 _IOR(NEURON_IOCTL_BASE, 53, struct neuron_ioctl_notifications_init_v2 *)

#define NEURON_IOCTL_NOTIFICATIONS_QUEUE_INFO _IOR(NEURON_IOCTL_BASE, 58, struct neuron_ioctl_notifications_queue_info *)

/** Gets the HW counters */
#define NEURON_IOCTL_READ_HW_COUNTERS _IOR(NEURON_IOCTL_BASE, 61, struct neuron_ioctl_read_hw_counters *)

/** Neuron DS functionality */
#define NEURON_IOCTL_ACQUIRE_NEURON_DS _IOR(NEURON_IOCTL_BASE, 71, struct neuron_ioctl_neuron_ds_info *)
#define NEURON_IOCTL_RELEASE_NEURON_DS _IOR(NEURON_IOCTL_BASE, 72, struct neuron_ioctl_neuron_ds_info *)

/** Increment/decrement neuron core use count */
#define NEURON_IOCTL_CRWL_READER_ENTER _IOW(NEURON_IOCTL_BASE, 81, struct neuron_ioctl_crwl *)
#define NEURON_IOCTL_CRWL_READER_EXIT  _IOW(NEURON_IOCTL_BASE, 82, struct neuron_ioctl_crwl *)
#define NEURON_IOCTL_CRWL_WRITER_ENTER _IOW(NEURON_IOCTL_BASE, 83, struct neuron_ioctl_crwl *)
#define NEURON_IOCTL_CRWL_WRITER_DOWNGRADE  _IOW(NEURON_IOCTL_BASE, 84, struct neuron_ioctl_crwl *)
#define NEURON_IOCTL_CRWL_NC_RANGE_MARK _IOW(NEURON_IOCTL_BASE, 85, struct neuron_ioctl_crwl_nc_map *)
#define NEURON_IOCTL_CRWL_NC_RANGE_UNMARK _IOW(NEURON_IOCTL_BASE, 86, struct neuron_ioctl_crwl_nc_map *)

/** Neuron Core Init State */
#define NEURON_IOCTL_CINIT_SET_STATE _IOW(NEURON_IOCTL_BASE, 91, struct  neuron_ioctl_cinit_set *)
#define NEURON_IOCTL_NC_MODEL_STARTED_COUNT _IOW(NEURON_IOCTL_BASE, 92, struct  neuron_ioctl_nc_model_started_count *)

/** Compatibility check */
#define NEURON_IOCTL_COMPATIBLE_VERSION _IOW(NEURON_IOCTL_BASE, 93, struct  neuron_ioctl_compatible_version *)
#endif
