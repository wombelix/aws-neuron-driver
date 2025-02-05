// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Memory pool for allocating device memory and host memory.
 *
 *  1. mem_chunk/mc         - Is a chunk of memory in device/host DRAM.
 *  2. mempool/mp           - Is a pool of memory backed either device DRAM or host DRAM.
 *                            For device memory it uses gen_pool allocator to allocate memory.
 *                            For host memory it directly uses kmalloc().
 *  3. mempool_set/mpset    - Is collection for mp for given neuron device.
 */

#ifndef NEURON_MEMPOOL_H
#define NEURON_MEMPOOL_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>

#include "v1/address_map.h"

enum mem_location {
	MEM_LOC_INVALID = 0, // Invalid type
	MEM_LOC_HOST = 1, // Memory chunk is from Host DRAM
	MEM_LOC_DEVICE = 2 // Memory chunk is from Device DRAM
};

/** Memory pool to manage Device memory.
 *
 * Device is memory is split in to chunks and allocated.
 * Uses genpool allocator in the backend.
 */
struct mempool {
	char name[32]; // friendly name
	bool initialized; // True if initialized.

	struct mempool_set *mpset; // parent mpset

	enum mem_location mem_location; // location of the memory
	u32 dram_channel; // DRAM channel valid only if location is device
	u32 dram_region; // DRAM region valid only if location is device

	struct gen_pool *gen_pool; // backing gen_pool allocator

	struct list_head mc_list_head; // list of allocated chunks

	size_t region_size; // size of the initial region
	size_t allocated_size; // total allocated memory size in bytes

	u32 page_size; // size of the host page backing this pool
	u32 page_requested_count; // number pages requested during pool creation
	u32 page_count; // number pages allocated successfully
	void **page_va_array; // array of allocated page's kva
	dma_addr_t *page_pa_array; // array of allocated page's pa
};

// DRAM region is split into multiple regions.
#define MAX_DDR_REGIONS 4

// start page size for host MP
#define MP_HOST_PAGE_SIZE_MIN (256UL * 1024)
// Number for MPs for host allocation
#define MP_HOST_POOL_COUNT 4

struct mempool_set {
	atomic_t freed; // if 1, the structure is already freed.
	struct mutex lock;
	u32 mp_device_num_regions; // number of regions in the device pool
	struct mempool mp_device[V1_MAX_DRAM_CHANNELS][MAX_DDR_REGIONS]; // device memory pools

	struct mempool mp_host[MP_HOST_POOL_COUNT]; // host memory pools

	struct list_head host_allocated_head; // list of allocated host memory

	// for stats and debugging
	u64 host_mem_size; // host memory used
	u64 device_mem_size; // device memory used

	void *pdev; // pci_dev->dev pointer
	struct rb_root root; //rbtree that has all host mem chunks allocated
	rwlock_t rblock; //protect the rbtree access
};

struct mem_chunk {
	struct rb_node node; // valid when this chunk is added to the rbtree
	phys_addr_t pa; // physical address of the chunk
	void *va; // virtual address of the chunk

	u32 size; // chunk size

	struct mempool *mp; // backpointer to mp
	struct mempool_set *mpset; // back pointer to mpset
	bool used_by_nq; // used by notification queue

	u32 dram_channel; // DRAM channel
	u32 dram_region; // TDRAM region
	u32 nc_id; //neuron core index

	enum mem_location mem_location; // location of memory - Host or Device

	struct list_head device_allocated_list; // link for the allocated list in mempool
	struct list_head host_allocated_list; // link for the allocated host list in mpset
};

// List of chunks
struct mc_list {
	int count;
	struct mem_chunk *head;
	struct mem_chunk *tail;
};

/**
 * mpset_constructor() - Construct mpset for given device.
 *
 * @mpset: Pointer to mpset which need to be initialized
 * @pdev: Pointer to device structure.
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int mpset_constructor(struct mempool_set *mpset, void *pdev);

/**
 * mpset_destructor() - Free all mp in the set.
 *
 * @mpset: Pointer to mpset which need to be destroyed.
 */
void mpset_destructor(struct mempool_set *mpset);

/**
 * mpset_init() - Prepare mpset for application use.
 *
 * @mpset: Pointer to mpset which need to be initialized
 * @num_channels: Number of DRAM channels in the device
 * @num_regions: Number of regions inside each DRAM channel
 * @device_dram_addr: Array of start addresses of DRAM channel
 * @device_dram_size: Array of size of each DRAM channel
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
int mpset_init(struct mempool_set *mpset, int num_channels, int num_regions,
	       const phys_addr_t *device_dram_addr, const u64 *device_dram_size);

/**
 * mpset_destructor() - Release mpset from application use.
 *
 * @mpset: Pointer to mpset
 */
void mpset_release(struct mempool_set *mpset);

/** mpset_search_mc() - Find memory chunk which maps given physical address
 *
 * @mpset: Pointer to mpset
 * @pa: physical address to search
 *
 * Return: mem chunk that has pa on success, NULL on failure
 */
struct mem_chunk *mpset_search_mc(struct mempool_set *mp, phys_addr_t pa);

/**
 * mc_alloc() - Allocate a memory chunk of size from given mpset.
 *
 * @mpset: mpset from which the mc should be allocated
 * @result: Buffer to store the allocated memory chunk pointer
 * @size: Allocation size
 * @location: Backing DRAM location(host/device)
 * @channel: Backing DRAM channel
 * @region: Region in the backing DRAM
 *
 * Return: 0 if allocation succeeds, a negative error code otherwise.
 */
int mc_alloc(struct mempool_set *mpset, struct mem_chunk **result, u32 size,
	     enum mem_location location, u32 channel, u32 region, u32 nc_id);

/**
 * mc_free() - Free memory chunk and associated backing memory.
 *
 * @mc: Pointer to memory chunk to be freed(this would be set to NULL on success)
 */
void mc_free(struct mem_chunk **mcp);

/**
 * mc_used_by_nq() - Sets/unsets if the mc is used by nq
 *
 * @mc: Pointer to memory chunk to be set
 * @used: used or not
 */
void mc_set_used_by_nq(struct mem_chunk *mc, bool used);

#endif
