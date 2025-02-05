
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Provides memory map related functions */

#ifndef NEURON_DMM_H
#define NEURON_DMM_H

#include <linux/mm.h>
#include <linux/sched.h>
#include "neuron_mempool.h"

struct nmmap_node {
	struct rb_node node; //rbtree node
	u64 pa; //physical address of the device memory
	void *va; //mapped virtual address of the device memory
	pid_t pid; //pid that map'd this device memory
	u32 device_index; //device index to which the memory belongs to
	u64 size; //size of the device memory

	//call back routine that will be called when this device memory is freed
	//this will be used by efa or other utilities that are using this memory
	//and if they have this function registered then it will be called so that
	//it can clean up their state
	void (*free_callback) (void *data);
	void *data;
};

/**
 * nmmap_create_node - Creates a memory map node that can be used by external drivers
 * like EFA
 *
 * @nd: neuron device
 * @va: mapped virtual address
 * @pid_t: pid that has this virtual address
 * @size: size of the mapped memory
 * @pa: physical address of the device memory
 */
void nmmap_create_node(struct neuron_device *nd, void *va, pid_t pid, u64 size, u64 pa);

/**
 * nmmap_delete_node - Deletes the mmap node. If external drivers have any free function regsistered
 * then this will call the routine
 *
 * @vma: virtual memory area
 */
void nmmap_delete_node(struct vm_area_struct *vma);

/**
 * nmmap_offset() - Return mmap offset for given memory chunk.
 *
 * @mc: memory chunk which needs to be mapped
 *
 * Return: offset to be used to mmap in the /dev/ndX file.
 */
u64 nmmap_offset(struct mem_chunk *mc);

/**
 * nc_mmap() - mmap a range into userspace
 * @nd: neuron device
 * @vma: mmap area.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int nmmap_mem(struct neuron_device *nd, struct vm_area_struct *vma);


/**
 * nmmap_search_va - Searches for va (mmap'd) in the rbtree
 *
 * @nd: neuron device
 * @va: mapped virtual address
 *
 * Return: mmap node
 */
struct nmmap_node * nmmap_search_va(struct neuron_device *nd, void *va);

#endif
