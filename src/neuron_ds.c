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

void neuron_ds_init(struct neuron_datastore *nds, struct neuron_device *parent)
{
	nds->parent = parent;
	memset(nds->entries, 0, NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE * sizeof(struct neuron_datastore_entry));
	mutex_init(&nds->lock);
}

void neuron_ds_acquire_lock(struct neuron_datastore *nds)
{
	mutex_lock(&nds->lock);
}

void neuron_ds_release_lock(struct neuron_datastore *nds)
{
	mutex_unlock(&nds->lock);
}

bool neuron_ds_check_entry_in_use(struct neuron_datastore *nds, u32 index) {
	BUG_ON(!mutex_is_locked(&nds->lock));
	if (index >= NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE)
		return false;
	return nds->entries[index].in_use_by_creating_pid;
}

static struct neuron_datastore_entry *neuron_ds_find(struct neuron_datastore *nds, pid_t pid)
{
	int idx;
	struct neuron_datastore_entry *entry = NULL;
	for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		entry = &nds->entries[idx];
		BUG_ON(entry->ref_count != 0 && entry->mc == NULL);
		if (entry->ref_count != 0 && entry->pid == pid)
			return entry;
	}
	return NULL;
}

static int neuron_ds_find_empty_slot(struct neuron_datastore *nds)
{
	int idx;
	for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		if (nds->entries[idx].ref_count == 0)
			return idx;
	}
	return -1;
}

int neuron_ds_add_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc)
{
	int ret;
	int new_index;
	struct neuron_datastore_entry *entry = NULL;

	*mc = NULL;

	new_index = neuron_ds_find_empty_slot(nds);
	if (new_index < 0) {
		ret = -ENOMEM;
		goto end;
	}

	ret = mc_alloc(nds->parent, MC_LIFESPAN_ALL_PROCESS, NEURON_DATASTORE_SIZE, MEM_LOC_HOST, 0, 0, 0, mc);
	if (ret) {
		goto end;
	}
	entry = &nds->entries[new_index];
	entry->pid = pid;
	entry->in_use_by_creating_pid = true;
	entry->ref_count = 1;
	entry->mc = *mc;
end:
	return ret;
}

int neuron_ds_acquire_existing_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc) {
	bool is_increased_by_owner;
	struct neuron_datastore_entry *entry = neuron_ds_find(nds, pid);
	if (entry == NULL)
		return -ENOENT;
	*mc = entry->mc;

	// In the unlikely event the acquire IOCTL is called by the owner of this nds after previously closing it
	is_increased_by_owner = entry->pid == task_tgid_nr(current);
	if (is_increased_by_owner) {
		if (entry->in_use_by_creating_pid)
			return 0;
		entry->in_use_by_creating_pid = true;
	}
	entry->ref_count++;
	return 0;
}

int neuron_ds_create_and_acquire_pid(struct neuron_datastore *nds, pid_t pid, struct mem_chunk **mc) {
	struct neuron_datastore_entry *entry;
	entry = neuron_ds_find(nds, pid);
	if (entry == NULL)
		return neuron_ds_add_pid(nds, pid, mc);
	if (!entry->in_use_by_creating_pid) {
		entry->in_use_by_creating_pid = true;
		entry->ref_count++;
	}
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

void neuron_ds_decref(struct neuron_datastore *nds, struct neuron_datastore_entry *entry)
{
	bool is_decreased_by_owner = entry->pid == task_tgid_nr(current);
	// Guard to avoid double ref_count decrease if owner explicitly decreased it
	// and ncdev_close attempts to decrease it again
	if (is_decreased_by_owner) {
		if(!entry->in_use_by_creating_pid)
			return;
		entry->in_use_by_creating_pid = false;
  
 		// Record stored metrics before datastore is released
 		nmetric_partial_aggregate(nds->parent, entry);
	}
	entry->ref_count--;
	if (entry->ref_count > 0)
		return;
	BUG_ON(entry->ref_count < 0 || entry->mc == NULL);
	mc_free(&entry->mc);
	entry->pid = 0;
}

void neuron_ds_release_pid(struct neuron_datastore *nds, pid_t pid)
{
	struct neuron_datastore_entry *entry;
	neuron_ds_acquire_lock(nds);
	if (pid == 0)
		pid = task_tgid_nr(current);
	entry = neuron_ds_find(nds, pid);
	if (entry != NULL)
		neuron_ds_decref(nds, entry);
	neuron_ds_release_lock(nds);
}

void neuron_ds_destroy(struct neuron_datastore *nds)
{
	int idx;
	struct neuron_datastore_entry *entry = NULL;
	neuron_ds_acquire_lock(nds);
	for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
		entry = &nds->entries[idx];
		if (entry->ref_count == 0)
			continue;
		entry->ref_count = 0;
		mc_free(&entry->mc);
	}
	neuron_ds_release_lock(nds);
}
