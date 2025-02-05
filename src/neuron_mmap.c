
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/capability.h>
#include "neuron_mmap.h"
#include "neuron_device.h"

struct nmmap_node *nmmap_search_va(struct neuron_device *nd, void *va)
{
	int slot;
	struct rb_node *node;

	slot = npid_find_process_slot(nd);
	if (slot == -1)
		return NULL;
	node = nd->mpset.mmap_root[slot].rb_node; /* top of the tree */

	while (node) {
		struct nmmap_node *mmap = rb_entry(node, struct nmmap_node, node);

		if (va >= mmap->va && va < (mmap->va + mmap->size)) {
			if (mmap->pid == task_tgid_nr(current))
				return mmap;
			else
				return NULL;
		} else if (va < mmap->va) {
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return NULL;
}

/**
 * nmmap_insert_node_rbtree() - Insert a va node to the tree
 *
 * @root: binary tree root
 * @mmap: va node that needs to be inserted in tree
 */
static void nmmap_insert_node_rbtree(struct rb_root *root, struct nmmap_node *mmap)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;
	void *va = mmap->va;

	/* Go to the bottom of the tree */
	while (*link) {
		parent = *link;
		struct nmmap_node *mmap = rb_entry(parent, struct nmmap_node, node);

		if (mmap->va > va) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
		}
	}

	/* Put the new node there */
	rb_link_node(&mmap->node, parent, link);
	rb_insert_color(&mmap->node, root);
}

/**
 * nmmap_remove_node_rbtree() - Remove a va node from the tree
 *
 * @root: binary tree root
 * @mmap: va node that needs to be removed
 */
static void nmmap_remove_node_rbtree(struct rb_root *root, struct nmmap_node *mmap)
{
	rb_erase(&mmap->node, root);
}

void nmmap_create_node(struct neuron_device *nd, void *va, pid_t pid, u64 size, u64 pa)
{
	// Now insert the va in rb tree
	int slot;
	struct nmmap_node *mmap;

	slot = npid_find_process_slot(nd);
	if (slot == -1)
		return;
	mmap = kzalloc(sizeof(struct nmmap_node), GFP_KERNEL);
	if (!mmap)
		return;

	// On failure we won't be inserting into the tree. This would lead to a situtation
	// where mmap is fine but any register VA API etc that is used to look for VA in the
	// system will return error.
	mmap->size = size;
	mmap->pa = pa;
	mmap->va = va;
	mmap->pid = pid;
	mmap->device_index = nd->device_index;
	mmap->free_callback = NULL;
	write_lock(&nd->mpset.rbmmaplock);
	nmmap_insert_node_rbtree(&nd->mpset.mmap_root[slot], mmap);
	write_unlock(&nd->mpset.rbmmaplock);
}

void nmmap_delete_node(struct vm_area_struct *vma)
{
	struct neuron_device *nd = (struct neuron_device *)vma->vm_private_data;
	void (*free_callback)(void *data) = NULL;
	void *data = NULL;
	int slot;
	struct nmmap_node *mmap;
	slot = npid_find_process_slot(nd);
	if (slot == -1) {
		return;
	}
	mmap = nmmap_search_va(nd, (void *)vma->vm_start);
	if (mmap != NULL) {
		write_lock(&nd->mpset.rbmmaplock);
		nmmap_remove_node_rbtree(&nd->mpset.mmap_root[slot], mmap);
		if (mmap->free_callback != NULL) {
			free_callback = mmap->free_callback;
			data = mmap->data;
		}
		kfree(mmap);
		write_unlock(&nd->mpset.rbmmaplock);
		if (free_callback)
			free_callback(data);
	}
}

u64 nmmap_offset(struct mem_chunk *mc)
{
	return mc->pa;
}

/**
 * nmmap_get_mc() - Return mem_chunk for given vma.
 */
static struct mem_chunk *nmmap_get_mc(struct neuron_device *nd, struct vm_area_struct *vma)
{
	struct mem_chunk *mc;
	u64 offset, size;
	phys_addr_t pa;

	size = vma->vm_end - vma->vm_start;
	offset = vma->vm_pgoff << PAGE_SHIFT;

	pa = offset;

	read_lock(&nd->mpset.rblock);
	mc = mpset_search_mc(&nd->mpset, pa);
	read_unlock(&nd->mpset.rblock);
	if (mc == NULL) {
		pr_err("nd%d: mc not found for mmap()\n", nd->device_index);
		return NULL;
	}
	if (!IS_ALIGNED(mc->size, PAGE_SIZE)) {
		pr_err("nd%d: invalid size %x for mmap()\n", nd->device_index, mc->size);
		return NULL;
	}
	if (!IS_ALIGNED(mc->pa, PAGE_SIZE)) {
		pr_err("nd%d: unaligned address %llx for mmap()\n", nd->device_index, mc->pa);
		return NULL;
	}
	if (mc->size != size) {
		pr_err("nd%d: partial mmap of mc not supported(%x != %llx)\n", nd->device_index,
		       mc->size, size);
		return NULL;
	}
	return mc;
}

static const struct vm_operations_struct nmmap_dm_vm_ops = {
	.close = nmmap_delete_node,
};

static int nmmap_dm(struct neuron_device *nd, struct vm_area_struct *vma)
{
	return -EINVAL;

}

static int nmmap_dm_mc(struct neuron_device *nd, struct vm_area_struct *vma, struct mem_chunk *mc)
{
	int ret;
	phys_addr_t pa;

	// Readonly access for other processes for memory whose lifespan is not per device
	if (mc->pid != task_tgid_nr(current) && mc->lifespan != MC_LIFESPAN_DEVICE) {
		vma->vm_flags &= ~VM_WRITE;
		pgprot_val(vma->vm_page_prot) &= ~VM_WRITE;
	}

	pa = mc->pa;
	vma->vm_pgoff = (u64)pa >> PAGE_SHIFT; // convert to offset

	ret = nmmap_dm(nd, vma);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current),
			  (u64)(vma->vm_end - vma->vm_start), (pa + nd->npdev.bar4_pa));

	return 0;
}

static int nmmap_dm_root(struct neuron_device *nd, struct vm_area_struct *vma)
{
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	return nmmap_dm(nd, vma);
}

int nmmap_mem(struct neuron_device *nd, struct vm_area_struct *vma)
{
	int ret;
	struct mem_chunk *mc;

	mc = nmmap_get_mc(nd, vma);
	if (mc == NULL)
		// no memchunk found. only allow root to map arbitrary device mem
		return nmmap_dm_root(nd, vma);

	if (mc->mem_location == MEM_LOC_DEVICE)
		return nmmap_dm_mc(nd, vma, mc);

	// Readonly access for other processes for memory whose lifespan is not per device
	if (mc->pid != task_tgid_nr(current) && mc->lifespan != MC_LIFESPAN_DEVICE) {
		vma->vm_flags &= ~VM_WRITE;
		pgprot_val(vma->vm_page_prot) &= ~VM_WRITE;
	}

#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_nc_mmap, 1))
		return -ENOSPC;
#endif
	ret = remap_pfn_range(vma, vma->vm_start, PHYS_PFN(mc->pa & ~PCI_HOST_BASE(nd)), mc->size,
			      vma->vm_page_prot);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current),
			  (u64)(vma->vm_end - vma->vm_start), mc->pa);
	return 0;
}
