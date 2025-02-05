/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DRIVER_SHARED_H
#define NEURON_DRIVER_SHARED_H

#include <linux/types.h>

enum neuron_dma_queue_type {
	NEURON_DMA_QUEUE_TYPE_TX = 0, // transmit queue
	NEURON_DMA_QUEUE_TYPE_RX, // receive queue
	NEURON_DMA_QUEUE_TYPE_COMPLETION, // completion queue
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

#endif  // NEURON_DRIVER_SHARED_H
