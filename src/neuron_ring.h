// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_RING_H
#define NEURON_RING_H

#include "udma/udma.h"
#include "v1/address_map.h"
#include "v1/tdma.h"
#include "v2/address_map.h"
#include "v2/sdma.h"


#define DMA_H2T_DESC_COUNT 4096
#define NUM_DMA_ENG_PER_DEVICE 34 // for v2 2 nc with each 16,

#define NDMA_QUEUE_DUMMY_RING_DESC_COUNT 64
#define NDMA_QUEUE_DUMMY_RING_SIZE (NDMA_QUEUE_DUMMY_RING_DESC_COUNT * sizeof(union udma_desc))

struct neuron_device;
struct neuron_dma_eng_state;
struct neuron_dma_queue_state;

struct ndma_ring {
	u32 qid;
	u32 size; //total size - num desc * desc size
	bool has_compl;
	struct udma_ring_ptr tx;
	struct udma_ring_ptr rx;
	struct udma_ring_ptr rxc;
	struct udma_ring_ptr h2t_completion;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rx_mc;
	struct mem_chunk *rxc_mc;
	struct mem_chunk *h2t_completion_mc;
	u32 dram_channel;
};

struct ndma_queue {
	struct ndma_ring ring_info;
	u32 eng_id;
	u32 qid;
	pid_t owner; // process which initialized this queue.
};

struct ndma_eng {
	struct mutex lock;
	struct neuron_device *nd;
	u32 eng_id;
	struct ndma_queue queues[DMA_MAX_Q_MAX];
	struct udma udma;
	bool used_for_h2t;
	struct mutex h2t_ring_lock;
};

/**
 * ndmar_init() - Initialize DMA structures for given neuron device
 *
 * @nd: Neuron device to initialize
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int ndmar_init(struct neuron_device *nd);

/**
 * ndmar_close() - Close and cleanup DMA for given neuron device
 *
 * @nd: Neuron device to cleanup
 *
 * Return: 0 if cleanup succeeds, a negative error code otherwise.
 */
void ndmar_close(struct neuron_device *nd);

/**
 * ndmar_eng_init() - Initialize a DMA engine
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index to initialize
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int ndmar_eng_init(struct neuron_device *nd, int eng_id);

/**
 * ndmar_eng_set_state() - Change DMA engine's state
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index which state needs to be changed
 * @state: New state to set
 *
 * Return: 0 if state is successfully changed, a negative error code otherwise.
 */
int ndmar_eng_set_state(struct neuron_device *nd, int eng_id, u32 state);

/**
 * ndmar_queue_init() - Initialize a DMA queue.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index which needs to be initialized
 * @tx_desc_count: Total TX descriptors to allocate
 * @rx_desc_count: Total RX descriptors to allocate
 * @tx_mc: Memory chunk backing TX queue
 * @rx_mc: Memory chunk backing RX queue
 * @rxc_mc: Memory chunk backing RX completion queue
 * @port: AXI port.
 *
 * Return: 0 if queue init succeeds, a negative error code otherwise.
 */
int ndmar_queue_init(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
		     u32 rx_desc_count, struct mem_chunk *tx_mc, struct mem_chunk *rx_mc,
		     struct mem_chunk *rxc_mc, u32 port);

/**
 * ndmar_queue_release() - Release a DMA queue.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index which needs to be released
 *
 * Return: 0 if queue release succeeds, a negative error code otherwise.
 */
int ndmar_queue_release(struct neuron_device *nd, u32 eng_id, u32 qid);

/**
 * ndmar_handle_process_exit() - Stops all the queues used by the given process.
 *
 * This function should be called when a process exits(before the MCs are freed),
 * so that the DMA engines used can be reset and any ongoing DMA transaction can be
 * stopped.
 *
 * @nd: Neuron device
 * @pid: Process id.
 */
void ndmar_handle_process_exit(struct neuron_device *nd, pid_t pid);

/**
 * ndmar_queue_copy_start() - Start DMA transfer.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index which needs to be released
 * @tx_desc_count: Number of Tx descriptors to transfer
 * @rx_desc_count: Number of Rx descriptors to transfer
 *
 * Return: 0 if DMA copy succeeds, a negative error code otherwise.
 */
int ndmar_queue_copy_start(struct neuron_device *nd, u32 eng_id, u32 qid, u32 tx_desc_count,
			   u32 rx_desc_count);

/**
 * ndmar_ack_completed() - Ack completed descriptor.
 *
 * After doing DMA transfer, the number of descriptors completed needs to acknowledged so that
 * the descriptors can be reused.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index where the DMA transfers were done
 * @count: Number descriptors to ack.
 *
 * Return: 0 if queue release succeeds, a negative error code otherwise.
 */
int ndmar_ack_completed(struct neuron_device *nd, u32 eng_id, u32 qid, u32 count);

/**
 * ndmar_queue_get_descriptor_mc() - Get backing memory chunk info.
 *
 * @nd: Neuron device which contains the DMA engine
 * @nd: DMA engine index which contains the DMA queue
 * @qid: DMA queue index
 * @tx: Buffer to store TX mc
 * @rx: Buffer to store RX mc
 * @tx_size: Buffer to store tx descriptor count
 * @rx_size: Buffer to store rx descriptor count
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int ndmar_queue_get_descriptor_mc(struct neuron_device *nd, u8 eng_id, u8 qid,
				  struct mem_chunk **tx, struct mem_chunk **rx, u32 *tx_size,
				  u32 *rx_size);

/**
 * ndmar_eng_get_state() - Get DMA engine's current state.
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index
 * @state: Current hardware state will be updated here
 *
 * Return: 0 on success, a negative error code otherwise
 */
int ndmar_eng_get_state(struct neuron_device *nd, int eng_id, struct neuron_dma_eng_state *state);

/**
 * ndmar_queue_get_state() - Get current state of the Tx and Rx queue.
 *
 * @nd: Neuron device which contains the DMA engine
 * @eng_id: DMA engine index
 * @qid: DMA queue index
 * @tx: TxQueue state will be set here.
 * @rx: RxQueue state will be set here.
 *
 * Return: 0 on success, a negative error code otherwise
 */
int ndmar_queue_get_state(struct neuron_device *nd, int eng_id, int qid,
			  struct neuron_dma_queue_state *tx, struct neuron_dma_queue_state *rx);


/** ndmar_set_model_started_v1() -
 *
 * Checks to see if the pa belongs to PE IRAM FIFO offset. If so, then these
 * descs are used to load the iram. The mem chunk is going to have all the descriptors
 * to load the instructions in iram. So go through all the dma queues and check if this mem chunk is
 * in that queue. Once we have the queue we set that queue to have descs
 * for iram. The actual copy start of the queue would come when model is started and at that time
 * set the state of model start for this nc.
 *
 * @nd: Neuron device which contains the DMA engine
 * @pa: pa to check
 * @mc: mem chunk that has descs
 *
 * Return: None
 */
void ndmar_set_model_started_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc);

/** ndmar_get_h2t_qid()
 *
 * Return qid
 */
int ndmar_get_h2t_qid(void);

/** ndmar_get_h2t_eng_id()
 *
 *  @nd: Neuron device which contains the DMA engine
 *  @nc_id: Neuron core corresponding to H2T engine
 * Return engine id
 */
uint32_t ndmar_get_h2t_eng_id(struct neuron_device *nd, uint32_t nc_id);

#endif
