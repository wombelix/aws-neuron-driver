// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
/** Exposes API to be used for p2p
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>

#include "neuron_device.h"
#include "neuron_mmap.h"
#include "neuron_p2p.h"

/*
 * Registers the VA with the callback and also returns the PA
 */
static u64 neuron_p2p_register_and_get_pa(void *va, u64 size, void (*free_callback)(void *data), void *data, int *device_index)
{
	int i;
	struct neuron_device *nd;

	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
		nd = neuron_pci_get_device(i);
		if (!nd)
			continue;
		read_lock(&nd->mpset.rbmmaplock);
		struct nmmap_node *mmap = nmmap_search_va(nd, va);
		if (mmap != NULL) {
			mmap->free_callback = free_callback;
			mmap->data = data;
			read_unlock(&nd->mpset.rbmmaplock);
			*device_index = i;
			return mmap->pa + (va - mmap->va);
		} else {
			read_unlock(&nd->mpset.rbmmaplock);
		}
	}
	return 0;
}

int neuron_p2p_register_va(u64 virtual_address, u64 length, struct neuron_p2p_va_info **va_info, void (*free_callback)(void *data), void *data)
{
	int device_index;
	struct neuron_p2p_va_info *vainfo;
	u64 pa = 0;
	u32 entries;
	int i;
	int pg_size = PAGE_SIZE;

	pa = neuron_p2p_register_and_get_pa((void *)virtual_address, length, free_callback, data,
				    &device_index);
	if (!pa) {
		pr_err("Could not find the physical address va:0x%llx, pid:%d", virtual_address, task_tgid_nr(current));
		return -EINVAL;
	}

	entries = DIV_ROUND_UP(length + (virtual_address & (pg_size - 1)), pg_size);
	vainfo = kzalloc(sizeof(struct neuron_p2p_va_info) + (entries * sizeof(u64)), GFP_KERNEL);
	if (!vainfo) {
        pr_err("Could not allocate memory for va info for va:0x%llx, pid:%d", virtual_address, task_tgid_nr(current));
		return -ENOMEM;
    }

	vainfo->size = NEURON_P2P_PAGE_SIZE_4KB;
	for (i = 0; i < entries; i++) {
		vainfo->physical_address[i] = pa + (i * pg_size);
	}
	vainfo->device_index = device_index;
	vainfo->entries = entries; 
	vainfo->virtual_address = (void *)virtual_address;
	*va_info = vainfo;

	return 0;
}
EXPORT_SYMBOL_GPL(neuron_p2p_register_va);

int neuron_p2p_unregister_va(struct neuron_p2p_va_info *vainfo)
{
	struct neuron_device *nd;

	if (!vainfo)
		return -1;

	nd = neuron_pci_get_device(vainfo->device_index);
	read_lock(&nd->mpset.rbmmaplock);
	struct nmmap_node *mmap = nmmap_search_va(nd, vainfo->virtual_address);
	if (mmap != NULL) {
		mmap->free_callback = NULL;
		mmap->data = NULL;
	}
	read_unlock(&nd->mpset.rbmmaplock);
	kfree(vainfo);
	return 0;
}
EXPORT_SYMBOL_GPL(neuron_p2p_unregister_va);
