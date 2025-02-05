/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DRIVER_SHARED_H
#define NEURON_DRIVER_SHARED_H

#include <linux/types.h>

#define NEURON_NC_MAP_DEVICE (0xffffffff)

enum neuron_dma_queue_type {
	NEURON_DMA_QUEUE_TYPE_TX = 0, // transmit queue
	NEURON_DMA_QUEUE_TYPE_RX, // receive queue
	NEURON_DMA_QUEUE_TYPE_COMPLETION, // completion queue
};

enum neuron_cinit_state {
	NEURON_CINIT_STATE_STARTED = 1, // Core Init is initiated
	NEURON_CINIT_STATE_COMPLETED, // Core Init is completed successfully
	NEURON_CINIT_STATE_INVALID // Core Init is not valid
};


struct neuron_dma_eng_state {
	__u32 revision_id; // revision id
	__u32 max_queues; // maximum queues supported
	__u32 num_queues; // number of queues configured
	__u32 tx_state; // Tx statue
	__u32 rx_state; // Rx state
};

struct neuron_dma_queue_state {
	__u32 hw_status; // hardware status
	__u32 sw_status; // software status
	__u64 base_addr; // base address of the queue
	__u32 length; // size of the queue
	__u32 head_pointer; // hardware pointer index
	__u32 tail_pointer; // software pointer index
	__u64 completion_base_addr; // completion queue base address
	__u32 completion_head; // completion head
};

/*
 * NOTE: In runtime version 5, this enum was passed in as a bool instead -
 * true if top_sp and false if NC. Match the enum values to the bool to
 * maintain compatibility with older runtime. Do not change these values
 * until the min compatibility version is updated to >=6.
 */
enum NQ_DEVICE_TYPE {
    NQ_DEVICE_TYPE_NEURON_CORE = 0,
    NQ_DEVICE_TYPE_TOPSP,
    NQ_DEVICE_TYPE_MAX
};

enum NQ_TYPE {
	NQ_TYPE_TRACE = 0, /**< Implicit notifications generated during execution. */
	NQ_TYPE_NOTIFY, /**< Explicit notifications generated by NOTIFY instruction */
	NQ_TYPE_EVENT, /**< Notifications triggered by event set/clear operations. */
	NQ_TYPE_ERROR, /**< Notifications triggered by an error condition. */
	NQ_TYPE_TRACE_DMA, /**< Implicit notifications generated by DMA transfers.*/
	NQ_TYPE_THROTTLE,   /**< Notifications triggered by HAM throttling activity. */
	NQ_TYPE_MAX
};

struct neuron_uuid {
	__u8 value[32];
};

#define NEURON_MAX_PROCESS_PER_DEVICE 8 // 2 per core (arbitrary but needs to small number for fast lookup)

#define APP_INFO_PID_NC_LOCK_INFO	(1)
#define APP_INFO_PID_MEM_USAGE		(1 << 1)
#define APP_INFO_ALL			(0xF)

#define APP_INFO_MAX_MODELS_PER_DEVICE	(4)
#define NDS_INVALID_ID (-1)

struct neuron_app_info {
	__s32 pid;							// PID of this app
	__u8 nc_lock_map;						// NCs which are locked by it (one bit set for each locked NC)
	struct neuron_uuid uuid_data[APP_INFO_MAX_MODELS_PER_DEVICE];	// UUIDs running for this app for each neuroncore
	size_t host_mem_size;						// Amount of host memory used by this PID
	size_t device_mem_size;						// Amount of device memory used by this PID
};

typedef union nmetric_version {
	struct {
		__u64 build_num : 32;
		__u64 minor_ver : 8;
		__u64 major_ver : 8;
		__u64 reserved : 16;
	};
	__u64 all;
} nmetric_version_t;

/*
 * NDS stats
 */
#define NDS_ND_COUNTER_RESERVED 20

// Device counter types
enum {
	NDS_ND_COUNTER_RUNTIME_VERSION,
	NDS_ND_COUNTER_FRAMEWORK_VERSION,
	NDS_ND_COUNTER_FAL_VERSION,
	NDS_ND_COUNTER_FEATURE_BITMAP,
	NDS_ND_COUNTER_MIN_NEFF_VERSION,
	NDS_ND_COUNTER_MAX_NEFF_VERSION,

	// memory usage counters
	NDS_ND_COUNTER_MEM_USAGE_CODE_HOST,
	NDS_ND_COUNTER_MEM_USAGE_TENSORS_HOST,
	NDS_ND_COUNTER_MEM_USAGE_CONSTANTS_HOST,
	NDS_ND_COUNTER_MEM_USAGE_SCRATCHPAD_HOST,
	NDS_ND_COUNTER_MEM_USAGE_MISC_HOST,

	NDS_ND_COUNTER_COUNT = NDS_ND_COUNTER_MEM_USAGE_MISC_HOST + NDS_ND_COUNTER_RESERVED + 1
};

#define NDS_NC_COUNTER_RESERVED 4

// Neuroncore counter types
enum {
	NDS_NC_COUNTER_TIME_IN_USE = 0,

	NDS_NC_COUNTER_INFER_COMPLETED,
	NDS_NC_COUNTER_INFER_COMPLETED_WITH_ERR,
	NDS_NC_COUNTER_INFER_COMPLETED_WITH_NUM_ERR,
	NDS_NC_COUNTER_INFER_TIMED_OUT,
	NDS_NC_COUNTER_INFER_INCORRECT_INPUT,
	NDS_NC_COUNTER_INFER_FAILED_TO_QUEUE,

	// these must be in this specifc order
	// runtime assumes these are offset by
	// error code
	NDS_NC_COUNTER_ERR_GENERIC,
	NDS_NC_COUNTER_ERR_NUMERICAL,
	NDS_NC_COUNTER_ERR_MODEL,
	NDS_NC_COUNTER_ERR_TRANSIENT,
	NDS_NC_COUNTER_ERR_HW,
	NDS_NC_COUNTER_ERR_RT,

	NDS_NC_COUNTER_LATENCY_DEVICE,
	NDS_NC_COUNTER_LATENCY_TOTAL,
	NDS_NC_COUNTER_NC_TIME,

	// these are new counters
	// these shall be placed at the
	// end so there offsets are always
	// greater than old counters
	// This will ensure
	// new runtime + old driver will
	// write to reserved setions and not
	// break anything
	NDS_NC_COUNTER_GENERIC_FAIL,
	NDS_NC_COUNTER_ERR_RESOURCE,
	NDS_NC_COUNTER_ERR_RESOURCE_NC,
	NDS_NC_COUNTER_ERR_INVALID,
	NDS_NC_COUNTER_ERR_UNSUPPORTED_NEFF_VERSION,

	NDS_NC_COUNTER_CC_TIME,

	NDS_NC_COUNTER_MEM_USAGE_CODE_DEVICE,
	NDS_NC_COUNTER_MEM_USAGE_TENSORS_DEVICE,
	NDS_NC_COUNTER_MEM_USAGE_CONSTANTS_DEVICE,
	NDS_NC_COUNTER_MEM_USAGE_SCRATCHPAD_DEVICE,
	NDS_NC_COUNTER_MEM_USAGE_MISC_DEVICE,

	NDS_NC_COUNTER_COUNT = NDS_NC_COUNTER_MEM_USAGE_MISC_DEVICE + NDS_NC_COUNTER_RESERVED + 1
};

typedef struct nds_header {
	char signature[4];      // Fixed signature: 'n', 'd', 's', 0
	int  version;           // Version of the datastore's format
} nds_header_t;

/* --------------------------------------------
 * NDS shared data offsets
 * --------------------------------------------
 */

#define NDS_HEADER_START (0)
#define NDS_HEADER_SIZE (sizeof(nds_header_t))

#define NDS_ND_COUNTERS_START (NDS_HEADER_START + NDS_HEADER_SIZE)
#define NDS_ND_COUNTERS_SIZE (NDS_ND_COUNTER_COUNT * sizeof(uint64_t))
#define NDS_ND_COUNTERS(base_addr) ((uint64_t *)(base_addr + NDS_ND_COUNTERS_START))

#define MAX_NEURONCORE_COUNT (4)

#define NDS_NEURONCORE_COUNTERS_COUNT (NDS_NC_COUNTER_COUNT)
#define NDS_NEURONCORE_COUNTERS_START (NDS_ND_COUNTERS_START + NDS_ND_COUNTERS_SIZE)
#define NDS_NEURONCORE_COUNTERS_SIZE (NDS_NEURONCORE_COUNTERS_COUNT * MAX_NEURONCORE_COUNT * sizeof(uint64_t))
#define NDS_NEURONCORE_COUNTERS(base_addr, nc_index) ((uint64_t *)(base_addr + NDS_NEURONCORE_COUNTERS_START) + (nc_index * NDS_NEURONCORE_COUNTERS_COUNT))

#endif  // NEURON_DRIVER_SHARED_H
