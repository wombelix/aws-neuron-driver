
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "neuron_mmap.h"
#include "neuron_device.h"

struct nmmap_node * nmmap_search_va(struct neuron_device *nd, void *va)
{
	struct rb_node *node = nd->mpset.mmap_root.rb_node; /* top of the tree */

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
	struct nmmap_node *mmap = kzalloc(sizeof(struct nmmap_node), GFP_KERNEL);
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
	nmmap_insert_node_rbtree(&nd->mpset.mmap_root, mmap);
	write_unlock(&nd->mpset.rbmmaplock);
}

void nmmap_delete_node(struct vm_area_struct *vma)
{
	struct neuron_device *nd = (struct neuron_device *)vma->vm_private_data;
	struct nmmap_node *mmap = nmmap_search_va(nd, (void *)vma->vm_start);
	if (mmap != NULL) {
		write_lock(&nd->mpset.rbmmaplock);
		nmmap_remove_node_rbtree(&nd->mpset.mmap_root, mmap);
		if (mmap->free_callback != NULL)
			mmap->free_callback(mmap->data);
		kfree(mmap);
		write_unlock(&nd->mpset.rbmmaplock);
	}
}

u64 nmmap_offset(struct mem_chunk *mc)
{
	return mc->pa;
}

/**
 * nmmap_get_mc() - Return mem_chunk for given vma.
 */
static struct mem_chunk * nmmap_get_mc(struct neuron_device *nd, struct vm_area_struct *vma)
{
	struct mem_chunk *mc;
	u64 offset, size;
	phys_addr_t pa;

	size = vma->vm_end - vma->vm_start;
	offset = vma->vm_pgoff << PAGE_SHIFT;

	pa = offset;

	mc = mpset_search_mc(&nd->mpset, pa);
	if (mc == NULL) {
		pr_err("nd%d: mc not found for mmap()", nd->device_index);
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
		pr_err("nd%d: partial mmap of mc not supported(%x != %llx)", nd->device_index, mc->size, size);
		return NULL;
	}
	return mc;
}

static const struct vm_operations_struct nmmap_dm_vm_ops = {
	.close = nmmap_delete_node,
};

static int nmmap_dm(struct neuron_device *nd, struct vm_area_struct *vma, struct mem_chunk *mc)
{
	int ret;
	u64 offset;
	u64 size;
	phys_addr_t pa;

	pa = mc->pa;
	size = vma->vm_end - vma->vm_start;

	if (!nd->npdev.bar4_pa) {
		pr_err("BAR4 not mapped\n");
		return -EINVAL;
	}

	// Readonly access for other processes
	if (mc->pid != task_tgid_nr(current)) {
		vma->vm_flags &= ~VM_WRITE;
		pgprot_val(vma->vm_page_prot) &= ~VM_WRITE;
	}

	offset = (u64)pa >> PAGE_SHIFT;  // convert to offset
	vma->vm_pgoff = offset + ((nd->npdev.bar4_pa) >> PAGE_SHIFT);

	// set the vm ops to cleanup on unmap
	vma->vm_private_data = (void *)nd;
	vma->vm_ops = &nmmap_dm_vm_ops;

	ret = io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size, vma->vm_page_prot);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current), (u64)(vma->vm_end - vma->vm_start), (pa + nd->npdev.bar4_pa));

	return 0;
}


int nmmap_mem(struct neuron_device *nd, struct vm_area_struct *vma)
{
	int ret;
	u64 offset;
	struct mem_chunk *mc;

	offset = vma->vm_pgoff * PAGE_SIZE;
	mc = nmmap_get_mc(nd, vma);
	if (mc == NULL)
		return -EINVAL;

	if (mc->mem_location == MEM_LOC_DEVICE)
		return nmmap_dm(nd, vma, mc);

	// Readonly access for other processes
	if (mc->pid != task_tgid_nr(current)) {
		vma->vm_flags &= ~VM_WRITE;
		pgprot_val(vma->vm_page_prot) &= ~VM_WRITE;
	}

#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_nc_mmap, 1))
		return -ENOSPC;
#endif
	ret = remap_pfn_range(vma, vma->vm_start, PHYS_PFN(mc->pa & ~PCI_HOST_BASE(nd)), mc->size, vma->vm_page_prot);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current), (u64)(vma->vm_end - vma->vm_start), mc->pa);
	return 0;
}
