// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Provides generic data store for applications to share counters, stats and other metrics */

#ifndef NEURON_DS_H
#define NEURON_DS_H

#include "share/neuron_driver_shared.h"
#include "neuron_mempool.h"
#include <linux/types.h>
#include <linux/mutex.h>

// Size of datastore
#define NEURON_DATASTORE_SIZE (256 * 1024UL)
#define NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE (NEURON_MAX_PROCESS_PER_DEVICE * 2)

struct neuron_device;

struct neuron_datastore_entry {
	pid_t pid;
	bool in_use_by_creating_pid;
	int ref_count;
	struct mem_chunk *mc;
};

struct neuron_datastore {
	struct mutex lock;
	struct neuron_device *parent;
	struct neuron_datastore_entry entries[NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE];
};

/**
 * neuron_ds_init() - Initializes the nds datastore
 *
 * @nds: neuron datastore
 * @parent: owner neuron_device
 *
 * Return: 0 on success, < 0 error code on failure
 */
void neuron_ds_init(struct neuron_datastore *nds, struct neuron_device *parent);

/** 
 * neuron_ds_check_entry_in_use() - Returns whether datastore entry is in use by creating pid. 
 * @note Expects datastore lock to have been acquired before calling
 * 
 * @nds: neuron datastore
 * @index: datastore entry index
 * 
 * Return: true if in use, false if not in use or index is invalid
 */
bool neuron_ds_check_entry_in_use(struct neuron_datastore *nds, u32 index);

/**
 * neuron_ds_acquire_pid() - Acquires a ds entry, increases ref count, *mc is NULL if PID doesn't have
 * an associated datastore
 *
 * @nds: neuron datastore
 * @pid: pid associated with this DS
 * @mc[out]: will contain a pointer to the found datastore entry
 *
 * Return: 0 on success, < 0 error code on failure
 */
int neuron_ds_acquire_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc);

/**
 * neuron_ds_release_pid() - Releases a ds entry, decreases ref count, deallocates if 0
 *
 * @nds: neuron datastore
 * @pid: pid associated with this DS
 *
 */
void neuron_ds_release_pid(struct neuron_datastore *nds, pid_t pid);

/**
 * neuron_ds_destroy() - Removes existing entries regardless of refcount on driver shutdown
 *
 * @nds: neuron datastore
 *
 */
void neuron_ds_destroy(struct neuron_datastore *nds);

/**
 * neuron_ds_acquire_lock() - Acquires datastore lock
 * 
 * @nds: neuron datastore
 * 
 */
void neuron_ds_acquire_lock(struct neuron_datastore *nds);

/**
 * neuron_ds_release_lock() - Releases datastore lock
 * 
 * @nds: neuron datastore
 * 
 */
void neuron_ds_release_lock(struct neuron_datastore *nds);

#endif
