
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/capability.h>
#include "neuron_mmap.h"
#include "neuron_device.h"
#include "neuron_dhal.h"

/**
 * mapping table of resources that can be mapped into user space.
 * Used by EFA.
 * 
 */
struct neuron_dm_special_mmap_ent {
	enum neuron_dm_block_type block;
	int  block_id;
	enum neuron_dm_resource_type resource;
	u64  offset;
	u64  size;
	u64  bar0_offset;
};


#define DM_SPECIAL_MM_ENT(blk, blk_id, res, blk_mmoff, blk_baroff, blk_sz, res_off, res_sz)  \
						{blk, blk_id, res, (blk_mmoff) + (blk_sz)*(blk_id) + (res_off), res_sz, (blk_baroff) + (blk_sz)*(blk_id) + res_off}

// FIXME move this to ptr ref from dhal.
static struct neuron_dm_special_mmap_ent dm_mmap_special_v2[] = {
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, V2_MMAP_TPB_OFFSET, V2_PCIE_BAR0_TPB_0_OFFSET,   V2_MMAP_TPB_SIZE, V2_MMAP_NC_EVENT_OFFSET, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, V2_MMAP_TPB_OFFSET, V2_PCIE_BAR0_TPB_0_OFFSET,   V2_MMAP_TPB_SIZE, V2_MMAP_NC_EVENT_OFFSET, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 0, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 1, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 2, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 3, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 4, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	DM_SPECIAL_MM_ENT( NEURON_DM_BLOCK_TOPSP, 5, NEURON_DM_RESOURCE_SEMAPHORE, V2_TOP_SP_0_BASE,   V2_PCIE_BAR0_TOPSP_0_OFFSET, V2_TOP_SP_0_SIZE, 0, V2_MMAP_NC_SEMA_SIZE),
	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

static struct neuron_dm_special_mmap_ent dm_mmap_special_v1[] = {
	{NEURON_DM_BLOCK_TPB,   0, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   1, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   2, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_TPB,   3, NEURON_DM_RESOURCE_SEMAPHORE, 0, 0, 0},
	{NEURON_DM_BLOCK_INVALID, 0, 0, 0, 0, 0},
};

int nmap_dm_special_resource_get( enum neuron_dm_block_type block, u32 block_id,  enum neuron_dm_resource_type resource, u64 *offset, u64 *size)
{
	struct neuron_dm_special_mmap_ent * ent;

	if (narch_get_arch() == NEURON_ARCH_V2)
		ent = dm_mmap_special_v2;
	else if (narch_get_arch() == NEURON_ARCH_V1)
		ent = dm_mmap_special_v1;
	else 
		return -EINVAL;

	while (ent->block != NEURON_DM_BLOCK_INVALID) {
		if ((ent->block == block) &&(ent->block_id == block_id) && (ent->resource == resource)) { 
			*offset = ent->offset;
			*size = ent->size;
			return 0;
		}
		ent++;
	}
	return -EINVAL;
}

int nmap_dm_special_resource_addr_valid( u64 offset, u64 size, u64 *bar0_offset)
{
	struct neuron_dm_special_mmap_ent * ent;

	if (narch_get_arch() == NEURON_ARCH_V2)
		ent = dm_mmap_special_v2;
	else if (narch_get_arch() == NEURON_ARCH_V1)
		ent = dm_mmap_special_v1;
	else
		return -EINVAL;

	while (ent->block != NEURON_DM_BLOCK_INVALID) {
		if ((ent->offset == offset) && (ent->size == size)) { 
			if (bar0_offset != NULL)
				*bar0_offset = ent->bar0_offset;
			return 0;
		}
		ent++;
	}
	return -EINVAL;
}

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
			if (mmap->pid == task_tgid_nr(current)) {
				return mmap;
			}
			else {
				pr_err("found 0x%llx on dev: %d slot: %d from another pid: %u != %u\n", (u64)va, nd->device_index, slot, mmap->pid, task_tgid_nr(current));
				return NULL;
			}
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
	/* Before deleting the mmap node, warn user if there's a pending dmabuf operation.
	 * This scenario could happen when an application is terminated midway between
	 * memory registration and de-registration. */
	if (mmap->dmabuf_ref_cnt > 0) {
		pr_warn("mmap node is being removed while dmabuf is in progress, va:0x%llx nd:%d pid:%d dmabuf_ref_cnt:%d\n",
				(u64)mmap->va, mmap->device_index, task_tgid_nr(current), mmap->dmabuf_ref_cnt);
	}
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
	if (!mmap) {
		pr_err("nd%d: failed to alloc mmap\n", nd->device_index);
		return;
	}
	// On failure we won't be inserting into the tree. This would lead to a situtation
	// where mmap is fine but any register VA API etc that is used to look for VA in the
	// system will return error.
	mmap->size = size;
	mmap->pa = pa;
	mmap->va = va;
	mmap->pid = pid;
	mmap->device_index = nd->device_index;
	mmap->free_callback = NULL;
	mmap->dmabuf_ref_cnt = 0;
	write_lock(&nd->mpset.rbmmaplock);
	nmmap_insert_node_rbtree(&nd->mpset.mmap_root[slot], mmap);
	write_unlock(&nd->mpset.rbmmaplock);
}

static void nmmap_delete_node(struct vm_area_struct *vma)
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
	write_lock(&nd->mpset.rbmmaplock);
	mmap = nmmap_search_va(nd, (void *)vma->vm_start);
	if (mmap != NULL) {
		nmmap_remove_node_rbtree(&nd->mpset.mmap_root[slot], mmap);
		if (mmap->free_callback != NULL) {
			free_callback = mmap->free_callback;
			data = mmap->data;
		}
		kfree(mmap);
	} else {
		pr_err("FAILED to delete mmap 0x%llx, pid: %d, dev: %d, slot: %d\n", (u64)(void*)vma->vm_start, task_tgid_nr(current), nd->device_index, slot);
	}
	write_unlock(&nd->mpset.rbmmaplock);
	if (free_callback)
		free_callback(data);
}

/* Cleanup all mmaped entries when the process goes away
 * Iterate over the entries in the process' slot and delete them
 * I'm sure there is a more efficient way of traversing rbtree but
 * normally the entries are removed when an application calls unmap.
 * So this is only for the exceptions, does not have to be fast.
 */
void nmmap_delete_all_nodes(struct neuron_device *nd)
{
	int slot;
	struct rb_node *root_node = NULL;

	slot = npid_find_process_slot(nd);
	if (slot == -1) {
		return;
	}

	do {
		void (*free_callback)(void *data) = NULL;
		void *data = NULL;

		write_lock(&nd->mpset.rbmmaplock);
		root_node = nd->mpset.mmap_root[slot].rb_node; /* top of the tree */
		if (root_node) {
			struct nmmap_node *mmap = rb_entry(root_node, struct nmmap_node, node);
			BUG_ON(mmap == NULL);
			if (task_tgid_nr(current) == mmap->pid) {
				nmmap_remove_node_rbtree(&nd->mpset.mmap_root[slot], mmap);
				if (mmap->free_callback != NULL) {
					free_callback = mmap->free_callback;
					data = mmap->data;
				}
				kfree(mmap);
			} else {
				pr_err("found mmap entry from another process, bailing out %d != %d", task_tgid_nr(current), mmap->pid);
				root_node = NULL;
			}
		}
		write_unlock(&nd->mpset.rbmmaplock);
		if (free_callback)
			free_callback(data);
	} while (root_node != NULL);
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
		// if we couldn't find mc and it's not a special resource, kick out an error
		if (nmap_dm_special_resource_addr_valid( offset, size, NULL))
			pr_err("nd%d: mc not found for mmap()\n", nd->device_index);
		return NULL;
	}
	if (!IS_ALIGNED(mc->size, PAGE_SIZE)) {
		pr_err("nd%d: invalid size %llx for mmap()\n", nd->device_index, mc->size);
		return NULL;
	}
	if (!IS_ALIGNED(mc->pa, PAGE_SIZE)) {
		pr_err("nd%d: unaligned address %llx for mmap()\n", nd->device_index, mc->pa);
		return NULL;
	}
	if (mc->size != size) {
		pr_err("nd%d: partial mmap of mc not supported(%llx != %llx)\n", nd->device_index,
		       mc->size, size);
		return NULL;
	}
	return mc;
}

struct mem_chunk *nmmap_get_mc_from_pa(struct neuron_device *nd, phys_addr_t pa)
{
	struct mem_chunk *mc;
	read_lock(&nd->mpset.rblock);
	mc = mpset_search_mc(&nd->mpset, pa);
	read_unlock(&nd->mpset.rblock);

	return mc;			
}

static const struct vm_operations_struct nmmap_dm_vm_ops = {
	.close = nmmap_delete_node,
};

static int nmmap_dm(struct neuron_device *nd, struct vm_area_struct *vma, u64 *bar4_offset)
{
	u64 start, size, offset;

	if (!nd->npdev.bar4_pa) {
		pr_err("BAR4 not mapped\n");
		return -EINVAL;
	}

	start = vma->vm_pgoff << PAGE_SHIFT;
	size = vma->vm_end - vma->vm_start;

	if (narch_get_arch() == NEURON_ARCH_V2) {
		if (start >= V2_HBM_0_BASE && start + size < V2_HBM_0_BASE + V2_HBM_0_SIZE)
			offset = start;
		else if (start >= V2_HBM_1_BASE && start + size < V2_HBM_1_BASE + V2_HBM_1_SIZE)
			// The 64GB - 80GB range is mapped to 16GB - 32GB on bar4
			offset = start - V2_HBM_1_BASE + V2_HBM_0_SIZE;
		else
			return -EINVAL;
	} else if (narch_get_arch() == NEURON_ARCH_V1) {
		// Note: 1) we mapped the address to get VA but R/W access to the BAR
		// from the instance might still be blocked.
		// 2) in the new future Neuron software will not request the mapping when running on INF
		if (start >= P_0_DRAM_0_BASE && start + size < P_0_DRAM_0_BASE + P_0_DRAM_0_SIZE)
			offset = start;
		else if (start >= P_0_DRAM_1_BASE && start + size < P_0_DRAM_1_BASE + P_0_DRAM_1_SIZE)
			// The BAR is squashed, 4GB+4GB are mapped consecutively but they are apart
			// in the actual address space
			offset = start - P_0_DRAM_1_BASE + P_0_DRAM_0_SIZE;
		else
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	if (bar4_offset)
		*bar4_offset = offset;

	return io_remap_pfn_range(vma, vma->vm_start, (offset + nd->npdev.bar4_pa) >> PAGE_SHIFT,
				  size, vma->vm_page_prot);
}

static int nmmap_dm_mc(struct neuron_device *nd, struct vm_area_struct *vma, struct mem_chunk *mc)
{
	int ret;
	phys_addr_t pa;
	u64 bar4_offset; // BAR4 layout is not the same as the memory (the gaps are squashed)

	// Readonly access for other processes for memory whose lifespan is not per device
	if (mc->pid != task_tgid_nr(current) && mc->lifespan != MC_LIFESPAN_DEVICE) {
		vma->vm_flags &= ~VM_WRITE;
		pgprot_val(vma->vm_page_prot) &= ~VM_WRITE;
	}

	pa = mc->pa;
	vma->vm_pgoff = (u64)pa >> PAGE_SHIFT; // convert to offset

	ret = nmmap_dm(nd, vma, &bar4_offset);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current),
			  (u64)(vma->vm_end - vma->vm_start), (bar4_offset + nd->npdev.bar4_pa));

	// set the vm ops to cleanup on unmap
	vma->vm_private_data = (void *)nd;
	vma->vm_ops = &nmmap_dm_vm_ops;
	return 0;
}

static int nmmap_dm_root(struct neuron_device *nd, struct vm_area_struct *vma)
{
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	return nmmap_dm(nd, vma, NULL);
}

static int nmap_dm_special(struct neuron_device *nd, struct vm_area_struct *vma)
{
	u64 start, size, offset;
	int ret;

	start = vma->vm_pgoff << PAGE_SHIFT;
	size = vma->vm_end - vma->vm_start;

	// if its not in the special resource table, try mapping as root
	//
	if (nmap_dm_special_resource_addr_valid( start, size, &offset)) 
		return nmmap_dm_root(nd, vma);

	ret = io_remap_pfn_range(vma, vma->vm_start, (offset + nd->npdev.bar0_pa) >> PAGE_SHIFT,
				  size, vma->vm_page_prot);  
	if (ret != 0) {
		pr_err("io remap failed on nd: %d\n", nd->device_index);
		return ret;
	}
	
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current),
			  		  size, (offset + nd->npdev.bar0_pa));

	// set the vm ops to cleanup on unmap
	vma->vm_private_data = (void *)nd;
	vma->vm_ops = &nmmap_dm_vm_ops;

	return ret;
}

/**
 * nmmap_mem()
 *
 *    map host physical or device memory into user space and insert it into 
 *    a rb tree for tracking.  Generally only mem chunks (allocated by the
 *    driver) can be mapped.  Non memchunk tracked memory can be mapped
 *    on a limited basis for use by collectives.
 *
 *    Caveat:
 *      root is allowed to map hbm w/o inserting 
 */
int nmmap_mem(struct neuron_device *nd, struct vm_area_struct *vma)
{
	int ret;
	struct mem_chunk *mc;

	mc = nmmap_get_mc(nd, vma);
	if (mc == NULL) {
		// no memchunk found. Only allow special regions to be mapped or allow root to map arbitrary device mem
		return nmap_dm_special(nd, vma);
	}

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
	ret = remap_pfn_range(vma, vma->vm_start, PHYS_PFN(mc->pa & ~ndhal->address_map.pci_host_base), mc->size,
			      vma->vm_page_prot);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	// Insert the virtual address into tree so that we can do search using VA
	nmmap_create_node(nd, (void *)vma->vm_start, task_tgid_nr(current),
			  (u64)(vma->vm_end - vma->vm_start), mc->pa);
	// set the vm ops to cleanup on unmap
	vma->vm_private_data = (void *)nd;
	vma->vm_ops = &nmmap_dm_vm_ops;
	return 0;
}
