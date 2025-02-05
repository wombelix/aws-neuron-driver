// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/sched.h>
#include "neuron_ds.h"
#include "neuron_mempool.h"
#include "neuron_device.h"
#include "neuron_metrics.h"

int neuron_ds_init(struct neuron_datastore *nds, struct neuron_device *parent)
{
	int idx;
	int ret = 0;
	nds->parent = parent;
	memset(nds->entries, 0, NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE * sizeof(struct neuron_datastore_entry));
	mutex_init(&nds->lock);
	for (idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		ret = mc_alloc(parent, MC_LIFESPAN_DEVICE, NEURON_DATASTORE_SIZE, MEM_LOC_HOST,
			       0, 0, 0, &nds->entries[idx].mc);
		if (ret) {
			pr_err("nds allocation failure for nd[%d]", parent->device_index);
			return ret;
		}
		memset(nds->entries[idx].mc->va, 0, NEURON_DATASTORE_SIZE);
	}
	return 0;
}

void neuron_ds_acquire_lock(struct neuron_datastore *nds)
{
	mutex_lock(&nds->lock);
}

void neuron_ds_release_lock(struct neuron_datastore *nds)
{
	mutex_unlock(&nds->lock);
}

static inline bool neuron_ds_entry_used(struct neuron_datastore_entry *entry)
{
	return entry->pid != 0;
}

bool neuron_ds_check_entry_in_use(struct neuron_datastore *nds, u32 index)
{
	BUG_ON(!mutex_is_locked(&nds->lock));
	BUG_ON(index >= NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE);
	return neuron_ds_entry_used(&nds->entries[index]);
}

static struct neuron_datastore_entry *neuron_ds_find(struct neuron_datastore *nds, pid_t pid)
{
	int idx;
	struct neuron_datastore_entry *entry = NULL;
	for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		entry = &nds->entries[idx];
		BUG_ON(neuron_ds_entry_used(entry) && entry->mc == NULL);
		if (entry->pid == pid)
			return entry;
	}
	return NULL;
}

static int neuron_ds_find_empty_slot(struct neuron_datastore *nds)
{
	int idx;
	int found_idx = -1;
	u64 min_clear_tick = ~0ull;
	struct neuron_datastore_entry *entry = NULL;
	for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		entry = &nds->entries[idx];
		if (neuron_ds_entry_used(&nds->entries[idx]))
			continue;
		if (entry->clear_tick == 0)
			return idx;
		if (entry->clear_tick < min_clear_tick) {
			min_clear_tick = entry->clear_tick;
			found_idx = idx;
		}
	}
	return found_idx;
}

int neuron_ds_add_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc)
{
	int new_index;
	struct neuron_datastore_entry *entry = NULL;
	*mc = NULL;

	new_index = neuron_ds_find_empty_slot(nds);
	if (new_index < 0)
		return -ENOMEM;

	entry = &nds->entries[new_index];
	entry->pid = pid;
	*mc = entry->mc;

	return 0;
}

int neuron_ds_acquire_existing_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc)
{
	struct neuron_datastore_entry *entry = neuron_ds_find(nds, pid);
	if (entry == NULL)
		return -ENOENT;
	*mc = entry->mc;
	return 0;
}

int neuron_ds_create_and_acquire_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc)
{
	struct neuron_datastore_entry *entry;
	entry = neuron_ds_find(nds, pid);
	if (entry == NULL)
		return neuron_ds_add_pid(nds, pid, mc);
	*mc = entry->mc;
	return 0;
}

int neuron_ds_acquire_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc)
{
	int ret;
	*mc = NULL;
	neuron_ds_acquire_lock(nds);
	if (pid != 0)
		ret = neuron_ds_acquire_existing_pid(nds, pid, mc);
	else
		ret = neuron_ds_create_and_acquire_pid(nds, task_tgid_nr(current), mc);
	neuron_ds_release_lock(nds);
	return ret;
}

static void neuron_ds_free_entry(struct neuron_datastore_entry *entry)
{
	if (entry->mc != NULL)
		mc_free(&entry->mc);
	memset(entry, 0, sizeof(struct neuron_datastore_entry));
}

static u64 ds_entry_clear_counter = 1;

static void neuron_ds_clear_entry(struct neuron_datastore_entry *entry)
{
	entry->pid = 0;
	entry->clear_tick = __sync_fetch_and_add(&ds_entry_clear_counter, 1);
	memset(entry->mc->va, 0, NEURON_DATASTORE_SIZE);
}

static void neuron_ds_release_entry(struct neuron_datastore *nds, struct neuron_datastore_entry *entry)
{
	bool current_pid_is_owner = entry->pid == task_tgid_nr(current);
	if (!current_pid_is_owner)
		return;
	nmetric_partial_aggregate(nds->parent, entry);
	neuron_ds_clear_entry(entry);
}

void neuron_ds_release_pid(struct neuron_datastore *nds, pid_t pid)
{
	struct neuron_datastore_entry *entry;
	neuron_ds_acquire_lock(nds);
	if (pid == 0)
		pid = task_tgid_nr(current);
	entry = neuron_ds_find(nds, pid);
	if (entry != NULL)
		neuron_ds_release_entry(nds, entry);
	neuron_ds_release_lock(nds);
}

static void neuron_ds_for_each_entry(struct neuron_datastore *nds,
				     void (*f)(struct neuron_datastore_entry*)) {
	int idx;
	neuron_ds_acquire_lock(nds);
	for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		(*f)(&nds->entries[idx]);
	}
	neuron_ds_release_lock(nds);
}

void neuron_ds_destroy(struct neuron_datastore *nds)
{
	neuron_ds_for_each_entry(nds, neuron_ds_free_entry);
}

void neuron_ds_clear(struct neuron_datastore *nds)
{
	neuron_ds_for_each_entry(nds, neuron_ds_clear_entry);
}
