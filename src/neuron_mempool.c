// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <asm/io.h>
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/fault-inject.h>

#include "neuron_mempool.h"
#include "neuron_device.h"
#include "neuron_reg_access.h"

/*
 *  genalloc.h mempool we use doesn't support VA=0; since we set it up such that va==pa for device memory
 *  allocations, it doesn't work for PA=0. Therefore, we amend address in all interactions with device
 *  memory pools to set the upper most bit as a workaround.
 */
#define GENPOOL_DEVMEM_BASE (0x1ull << 63)


int mempool_min_alloc_size = PAGE_SIZE; // always allocate on mmap() boundary
int mempool_host_memory_size = 32 * 1024 * 1024;

module_param(mempool_min_alloc_size, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(mempool_min_alloc_size, "Minimum size for memory allocation");

module_param(mempool_host_memory_size, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(mempool_host_memory_size, "Host memory to reserve(in bytes)");

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_mc_alloc);
#endif

// Upper 16MB is used internally by the firmware, don't use it in
// the allocation pool
#define MEMPOOL_CARVEOUT_SIZE 0x1000000 // 16MB

/**
 * mc_insert_node() - Insert a mem chunk to the tree
 *
 * @root: binary tree root
 * @mc: memory chunk that needs to be inserted
 */
static void mc_insert_node(struct rb_root *root, struct mem_chunk *mc)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;
	phys_addr_t pa = mc->pa;

	/* Go to the bottom of the tree */
	while (*link) {
		parent = *link;
		struct mem_chunk *mc = rb_entry(parent, struct mem_chunk, node);

		if (mc->pa > pa) {
			link = &(*link)->rb_left;
		} else {
			link = &(*link)->rb_right;
		}
	}

	/* Put the new node there */
	rb_link_node(&mc->node, parent, link);
	rb_insert_color(&mc->node, root);
}

/**
 * mc_remove_node() - Remove a mem chunk from the tree
 *
 * @root: binary tree root
 * @mc: memory chunk that needs to be removed
 */
void mc_remove_node(struct rb_root *root, struct mem_chunk *mc)
{
	rb_erase(&mc->node, root);
}

/**
 * mp_init_device_mem() Initialize the mempool structure to manage device memory.
 *
 * @mp: pointer to mempool that needs to be initialized
 * @mpset: ponter to mpset to which the given mp blongs.
 * @start_addr: starting address of the pool
 * @pool_size: size of the pool.
 * @mem_location: location of the backing memory.
 * @dram_channel: device dram channel backing this pool.
 * @dram_region: device dram region backing this pool.
 *
 * Return: 0 if pool is created, a negative error code otherwise.
 */

static int mp_init_device_mem(struct mempool *mp, struct mempool_set *mpset,
			      u64 start_addr, size_t pool_size,	u32 dram_channel, u32 dram_region)
{
	int ret;
	int min_alloc_size = mempool_min_alloc_size;
	// refuse to load with invalid memory alloc configuration
	BUG_ON(mempool_min_alloc_size < PAGE_SIZE);
	BUG_ON((mempool_min_alloc_size % PAGE_SIZE) != 0);

	memset(mp, 0, sizeof(*mp));

	mp->mpset = mpset;
	mp->mem_location = MEM_LOC_DEVICE;
	mp->dram_channel = dram_channel;
	mp->dram_region = dram_region;

	// v2 has a bigger mem size and gen pool create fails if < 1024
	if (v2_chip && min_alloc_size < 1024) {
		min_alloc_size = 1024;
	}

	mp->gen_pool = gen_pool_create(ilog2(min_alloc_size), -1);
	if (mp->gen_pool == NULL)
		return -ENOMEM;

	ret = gen_pool_add_virt(mp->gen_pool, start_addr | GENPOOL_DEVMEM_BASE, start_addr, pool_size, -1);
	if (ret) {
		gen_pool_destroy(mp->gen_pool);
		return ret;
	}


	snprintf(mp->name, sizeof(mp->name), "device mempool [%d:%d]", dram_channel, dram_region);
	mp->region_size = pool_size;
	mp->initialized = 1;


	return 0;
}

/**
 * Frees all backing pages allocated for reserved host_mem pool.
 * Does opposite work of mp_init_hrm_pool
 */
static void mp_destroy_hrm_pool(struct mempool *mp)
{
	int i = 0;
	if (mp->page_va_array == NULL)
		return;
	if (mp->page_pa_array == NULL)
		return;
	for(i = 0; i < mp->page_count; i++) {
		if (mp->page_va_array[i] == NULL)
			break;
		dma_free_coherent(mp->mpset->pdev, mp->page_size, mp->page_va_array[i],
				  mp->page_pa_array[i]);
		mp->page_va_array[i] = NULL;
		mp->page_pa_array[i] = 0;
	}
	kfree(mp->page_pa_array);
	kfree(mp->page_va_array);
	mp->page_pa_array = NULL;
	mp->page_va_array = NULL;
}

/**
 * mp_init_hrm_pool() Initialize the mempool structure to manage host memory.
 * Create a genpool with backing memory from host.
 * Any page allocation failure is ignored.
 *
 * @mp: pointer to mempool that needs to be initialized
 * @mpset: pointer to parent mempool_set
 * @page_size: backing host memory's page size
 * @page_count: Max number of pages to allocate
 *
 * Return: 0 if pool is created, a negative error code otherwise.
 */
static int mp_init_hrm_pool(struct mempool *mp, struct mempool_set *mpset,
			    u32 page_size, u32 page_count)
{
	int ret;
	int i;

	memset(mp, 0, sizeof(*mp));

	mp->mpset = mpset;
	mp->mem_location = MEM_LOC_HOST;
	mp->gen_pool = gen_pool_create(ilog2(mempool_min_alloc_size), -1);
	if (mp->gen_pool == NULL)
		return -ENOMEM;
	mp->page_va_array = kzalloc(sizeof(void *) * page_count, GFP_KERNEL);
	if (mp->page_va_array == NULL)
		goto fail;
	mp->page_pa_array = kzalloc(sizeof(dma_addr_t) * page_count, GFP_KERNEL);
	if (mp->page_pa_array == NULL)
		goto fail;
	mp->page_size = page_size;
	for(i = 0; i < page_count; i++) {
		mp->page_va_array[i] = dma_alloc_coherent(mp->mpset->pdev, page_size,
							  &mp->page_pa_array[i],
							  GFP_KERNEL | GFP_DMA32);
		if (mp->page_va_array[i] == NULL)
			break;
		ret = gen_pool_add_virt(mp->gen_pool, (unsigned long)mp->page_va_array[i],
					mp->page_pa_array[i], mp->page_size, -1);
		if (ret) {
			dma_free_coherent(mpset->pdev, mp->page_size,
					  mp->page_va_array[i],
					  mp->page_pa_array[i]);
			break;
		}
	}
	mp->page_requested_count = page_count;
	mp->page_count = i;

	snprintf(mp->name, sizeof(mp->name), "host mempool [%d]", page_size);
	mp->region_size = mp->page_size * mp->page_count;
	mp->initialized = 1;

	return 0;
fail:
	if (mp->page_pa_array)
		kfree(mp->page_pa_array);
	if (mp->page_va_array)
		kfree(mp->page_va_array);
	if (mp->gen_pool)
		gen_pool_destroy(mp->gen_pool);
	mp->page_pa_array = NULL;
	mp->page_va_array = NULL;
	mp->gen_pool = NULL;

	return -ENOMEM;
}

/**
 * Frees all the chunks associated with the mempool and releases the mempool.
 */
static void mp_destroy_gen_pool(struct mempool *mp)
{
	BUG_ON(mp == NULL);
	if (!mp->initialized)
		return;

	if (mp->gen_pool == NULL)
		return;

	gen_pool_destroy(mp->gen_pool);
	mp->gen_pool = NULL;
}

/**
 * mpset_init_device_pools() - Prepare device mp in given mpset.
 *
 * @mpset: Pointer to mpset which need to be initialized
 * @nd: Neuron device
 *
 * Return: 0 if initialization succeeds, a negative error code otherwise.
 */
static int mpset_init_device_pools(struct mempool_set *mpset, struct neuron_device *nd)
{
	int ret;
	int channel = 0, region = 0;
	u64 region_sz;
	u64 device_dram_addr[MAX_DRAM_CHANNELS];
	u64 device_dram_size[MAX_DDR_REGIONS];

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		mpset->num_channels = V1_MAX_DRAM_CHANNELS;
		device_dram_addr[0] = P_0_DRAM_0_BASE;
		device_dram_addr[1] = P_0_DRAM_1_BASE;
		device_dram_size[0] = P_0_DRAM_0_SIZE;
		device_dram_size[1] = P_0_DRAM_1_SIZE;
		mpset->mp_device_num_regions = 4;
	} else {
		mpset->num_channels = V2_MAX_DRAM_CHANNELS;
		device_dram_addr[0] = V2_HBM_0_BASE;
		device_dram_addr[1] = V2_HBM_1_BASE;
		device_dram_size[0] = V2_HBM_0_SIZE;
		device_dram_size[1] = V2_HBM_1_SIZE;
		mpset->mp_device_num_regions = 1;
	}

	for (channel = 0; channel < mpset->num_channels; channel++) {
		region_sz = device_dram_size[channel] / mpset->mp_device_num_regions;
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			dma_addr_t addr = device_dram_addr[channel] + (region * region_sz);
			ret = mp_init_device_mem(&mpset->mp_device[channel][region], mpset, addr,
									 region_sz, channel, region);
			if (ret) {
				pr_err("mpset device init failed %d\n", ret);
				goto fail;
			}
		}
	}

	/*
	*  Block carve out regions: Upper 16 MB is used internally by firmware for trainuim
	*
	*  Ideally we would carve out by simply changing the start address of the chunk;
	*  however, that breaks aligned allocation in 4.x kernel versions (fixed in 5.x).
	*  Fix here:
	*     commit 52fbf1134d479234d7e64ba9dcbaea23405f229e
	*     Author: Alexey Skidanov <alexey.skidanov@intel.com>
	*     Date:   Thu Jan 3 15:26:44 2019 -0800
	*
	*     lib/genalloc.c: fix allocation of aligned buffer from non-aligned chunk
	*/
	for (channel = 0; channel < mpset->num_channels; channel++) {
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			const dma_addr_t start_addr = device_dram_addr[channel] + (region * region_sz);
			if (narch_get_arch() == NEURON_ARCH_INFERENTIA)
				continue;
			struct mem_chunk *mc = NULL;
			ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, MEMPOOL_CARVEOUT_SIZE, MEM_LOC_DEVICE,
				       channel, region, 0, &mc);
			if (ret) {
				pr_err("failed to allocate hbm carevout region: ret=%d\n", ret);
				goto fail;
			}
			if (mc->pa != start_addr) {
				pr_err("carve out mc not offset 0!");
				mc_free(&mc);
				goto fail;
			}
		}
	}
	return 0;

fail:
	for (; channel >= 0; channel--) {
		for (; region >= 0; region--) {
			mp_destroy_gen_pool(&mpset->mp_device[channel][region]);
		}
	}
	memset(mpset, 0, sizeof(struct mempool_set));

	return ret;
}

/** Prints all entries in given lifespan list and returns the number of MC.
 */
static int mpset_print_lifespan_list(const char *name, struct list_head *head)
{
	int count = 0;
	struct list_head *this, *next;
	if (list_empty(head->next))
		return 0;

	pr_err("%s has unfreed mcs:\n", name);
	list_for_each_safe (this, next, head) {
		struct mem_chunk *mc = list_entry(this, struct mem_chunk, lifespan_list);
		pr_err("mc:%px ref_count:%d pid:%d caller:%pf location:%d size:%llu\n", mc, mc->ref_count, mc->pid, mc->caller_pc, mc->mem_location, mc->size);
		count++;
	}
	return count;
}

/** Verifies all MC allocated from the mpset is freed.
 */
static void mpset_verify_all_mc_freed(struct mempool_set *mpset)
{
	int i, count;
	count = mpset_print_lifespan_list("LOCAL", &mpset->mc_lifespan_local_head);
	for (i = 0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		count += mpset_print_lifespan_list("PROCESS",
						   &mpset->mc_lifespan_cur_process_head[i]);
	}
	count += mpset_print_lifespan_list("ALL_PROCESS", &mpset->mc_lifespan_all_process_head);
	count += mpset_print_lifespan_list("DEVICE", &mpset->mc_lifespan_device_head);
	BUG_ON(count != 0);
}

int mpset_constructor(struct mempool_set *mpset, void *pdev, struct neuron_device *nd)
{
	int host_page_index;
	u64 host_allocated_size = 0;
	int i;

	memset(mpset, 0, sizeof(*mpset));

	mutex_init(&mpset->lock);

	INIT_LIST_HEAD(&mpset->mc_lifespan_local_head);
	for(i = 0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		INIT_LIST_HEAD(&mpset->mc_lifespan_cur_process_head[i]);
		mpset->mmap_root[i] = RB_ROOT;
	}
	INIT_LIST_HEAD(&mpset->mc_lifespan_all_process_head);
	INIT_LIST_HEAD(&mpset->mc_lifespan_device_head);

	mpset->root = RB_ROOT;
	mpset->pdev = pdev;
	mpset->nd = nd;

	// reserve host memory
	for (host_page_index = MP_HOST_RESERVE_MEMORY_POOL_COUNT - 1; host_page_index >= 0; host_page_index--) {
		u32 page_size = MP_HOST_PAGE_SIZE_MIN << host_page_index;
		u32 page_count = mempool_host_memory_size / page_size;
		int ret = 0;
		ret = mp_init_hrm_pool(&mpset->mp_hrm[host_page_index], mpset, page_size,
				       page_count);
		if (ret) {
			pr_err("mpset host init failed %d\n", ret);
			goto fail;
		}
		host_allocated_size += mpset->mp_hrm[host_page_index].region_size;
	}
	pr_info("reserved %llu bytes of host memory\n", host_allocated_size);

	return mpset_init_device_pools(&nd->mpset, nd);
fail:
	for (; host_page_index < MP_HOST_RESERVE_MEMORY_POOL_COUNT; host_page_index++)
		mp_destroy_gen_pool(&mpset->mp_hrm[host_page_index]);

	return -ENOMEM;
}

static void mpset_free_lifespan_list(struct list_head *head, struct list_head *new_head);
static struct list_head * mpset_get_lifespan_head(struct mempool_set *mpset, enum mc_lifespan lifespan);

void mpset_destructor(struct mempool_set *mpset)
{
	int i, channel, region;
	struct list_head *head;
	struct neuron_device *nd;

	for(i = 0; i < NEURON_MAX_PROCESS_PER_DEVICE; i++) {
		nd = mpset->nd;
		if (nd->attached_processes[i].pid != 0) {
			pr_err("Found a still attached process: %d open_count: %d", nd->attached_processes[i].pid,
			       nd->attached_processes[i].open_count);
			head = &mpset->mc_lifespan_cur_process_head[i];
			mpset_free_lifespan_list(head, mpset_get_lifespan_head(mpset, MC_LIFESPAN_DEVICE));
		}
	}
	mpset_free_expired_mc(mpset, MC_LIFESPAN_ALL_PROCESS);
	mpset_free_expired_mc(mpset, MC_LIFESPAN_DEVICE);
	mpset_verify_all_mc_freed(mpset);

	mutex_lock(&mpset->lock);
	for (i = 0; i < MP_HOST_RESERVE_MEMORY_POOL_COUNT; i++) {
		mp_destroy_hrm_pool(&mpset->mp_hrm[i]);
		mp_destroy_gen_pool(&mpset->mp_hrm[i]);
	}

	for (channel = 0; channel < mpset->num_channels; channel++) {
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			mp_destroy_gen_pool(&mpset->mp_device[channel][region]);
		}
	}
	mutex_unlock(&mpset->lock);
}

struct mem_chunk *mpset_search_mc(struct mempool_set *mp, phys_addr_t pa)
{
	struct rb_node *node = mp->root.rb_node; /* top of the tree */

	while (node) {
		struct mem_chunk *mc = rb_entry(node, struct mem_chunk, node);

		if (pa >= mc->pa && pa < (mc->pa + mc->size)) {
			return mc;
		} else if (pa < mc->pa) {
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return NULL;
}

static inline struct list_head * mpset_get_lifespan_head(struct mempool_set *mpset, enum mc_lifespan lifespan)
{
	struct list_head *head = NULL;
	if (lifespan == MC_LIFESPAN_LOCAL) {
		head = &mpset->mc_lifespan_local_head;
	} else if (lifespan == MC_LIFESPAN_CUR_PROCESS) {
		int slot = npid_find_process_slot(mpset->nd);
		BUG_ON(slot == -1);
		head = &mpset->mc_lifespan_cur_process_head[slot];
	} else if (lifespan == MC_LIFESPAN_ALL_PROCESS) {
		head = &mpset->mc_lifespan_all_process_head;
	} else if (lifespan == MC_LIFESPAN_DEVICE) {
		head = &mpset->mc_lifespan_device_head;
	}
	return head;
}

static void mc_add_to_lifespan_list(struct mem_chunk *mc)
{
	struct mempool_set *mpset = mc->mpset;
	struct list_head *head;
	head = mpset_get_lifespan_head(mpset, mc->lifespan);
	list_add(&mc->lifespan_list, head);
}

static void mc_remove_from_lifespan_list(struct mem_chunk *mc)
{
	list_del(&mc->lifespan_list);
}

/** Free MCs in given lifespan list if their refcount == 1 else move the MC to new_head
 */
static void mpset_free_lifespan_list(struct list_head *head, struct list_head *new_head)
{
	struct list_head *this, *next;

	if (list_empty(head->next))
		return;

	list_for_each_safe (this, next, head) {
		struct mem_chunk *mc = list_entry(this, struct mem_chunk, lifespan_list);
		mc_free(&mc); // freeing would remove from this list
		// if not freed, it means there is still some refcount pending so add it to new head
		if (mc) {
			if (new_head) {
				pr_info("mc lifespan extended mc:%p refcount:%d lifespan:%d caller:%pf\n", mc,
				       mc->ref_count, mc->lifespan, mc->caller_pc);
				list_del(&mc->lifespan_list);
				list_add(&mc->lifespan_list, new_head);
			} else { // no new head, so force free the mc
				pr_err("mc leaked mc:%p refcount:%d lifespan:%d caller:%pf\n", mc,
				       mc->ref_count, mc->lifespan, mc->caller_pc);
				mc->ref_count = 1;
				mc_free(&mc);
			}
		}
	}
}

void mpset_free_expired_mc(struct mempool_set *mpset, enum mc_lifespan lifespan)
{
	struct list_head *head, *next_head;
	head = mpset_get_lifespan_head(mpset, lifespan);
	next_head = mpset_get_lifespan_head(mpset, lifespan+1);
	mpset_free_lifespan_list(head, next_head);
}


static int mc_alloc_internal(struct neuron_device *nd, enum mc_lifespan lifespan, u64 size, u64 align,
	     enum mem_location location, u32 channel, u32 region, u32 nc_id,
	     struct mem_chunk **result)
{
	struct mem_chunk *mc;
	struct mempool *mp = NULL;
	struct mempool_set *mpset = &nd->mpset;
	int ret = 0;

	*result = NULL;

	// Round the size up to a full page or multiple pages.
	// Make mmap() happy with any memory allocated via this function.
	size = roundup(size, PAGE_SIZE);
	if (channel >= V1_MAX_DRAM_CHANNELS)
		return -EINVAL;
#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_mc_alloc, 1))
		return -ENOMEM;
#endif
	if (mpset->mp_device_num_regions == 1) // shared DRAM mode, always use region 0
		region = 0;

	mc = (struct mem_chunk *)kmalloc(sizeof(struct mem_chunk), GFP_KERNEL);
	if (mc == NULL)
		return -ENOMEM;

	*result = mc;
	memset(mc, 0, sizeof(struct mem_chunk));

	mutex_lock(&mpset->lock);
	if (location == MEM_LOC_HOST) {
		if (align) {
			pr_err("Allocating aligned host memory not supported");
			return -EINVAL;
		}
		dma_addr_t addr;
		mc->va = dma_alloc_coherent(mpset->pdev, size, &addr,
				GFP_KERNEL | GFP_DMA32);
		mc->pa = (phys_addr_t)addr;
		// try to fill from reserved host memory
		if (mc->va == NULL) {
			int i;
			for (i = 0; i < MP_HOST_RESERVE_MEMORY_POOL_COUNT; i++) {
				u32 page_size = MP_HOST_PAGE_SIZE_MIN << i;
				if (page_size < size)
					continue;
				mp = &mpset->mp_hrm[i];
				mc->va = gen_pool_dma_alloc(mp->gen_pool, size, &mc->pa);
				if (mc->va)
					break;
			}
		}
		if (mc->va)
			mc->pa |= PCI_HOST_BASE(nd);
		else
			pr_info("host mem occupied %lld\n", mpset->host_mem_size);
	} else {
		mp = &mpset->mp_device[channel][region];
		if (!mp->gen_pool) {
			pr_err("mempool not initialized\n");
			ret = -ENOMEM;
			goto exit;
		}

		if (align) {

			if (align > INT_MAX) {
				pr_err("alignment value not supported %llu\n", align);
				ret = -EINVAL;
				goto exit;
			}
			struct genpool_data_align align_data = { .align = align};
			mc->va = (void *)gen_pool_alloc_algo(mp->gen_pool, size,
							     gen_pool_first_fit_align, &align_data);
			mc->pa = gen_pool_virt_to_phys(mp->gen_pool, (unsigned long) mc->va);
			if ((((align-1) & mc->pa) != 0) || mc->va == NULL){
				pr_err("Aligned memory allocation failed! size: 0x%llx, alignmnet: 0x%llx\n", size, align);
				if (mc->va != NULL) {
					gen_pool_free(mp->gen_pool, (unsigned long)mc->va, size);
				}
				ret = -ENOMEM;
				goto exit;
			}
		} else {
			mc->va = gen_pool_dma_alloc(mp->gen_pool, size, &mc->pa);
		}
		if (mc->va) {
			mp->allocated_size += size;
		} else {
			pr_info("%s total %ld occupied %ld needed %lld available %ld\n", mp->name,
				mp->region_size, mp->allocated_size, size,
				gen_pool_avail(mp->gen_pool));
			pr_info("device regions %d occupied %lld\n", mpset->mp_device_num_regions,
				mpset->device_mem_size);
		}
	}
	if (mc->va == NULL) {
		ret = -ENOMEM;
		goto exit;
	}

	mc->magic = MEMCHUNK_MAGIC;
	mc->mpset = mpset;
	mc->mp = mp;
	mc->size = size;
	mc->mem_location = location;
	mc->dram_channel = channel;
	mc->dram_region = region;
	mc->nc_id = nc_id;
	mc->pid = task_tgid_nr(current);
	mc->ref_count = 1;
	mc->lifespan = lifespan;
	mc->caller_pc = __builtin_return_address(0);
	mc_add_to_lifespan_list(mc);

	write_lock(&mpset->rblock);
	mc_insert_node(&mpset->root, mc);
	write_unlock(&mpset->rblock);

	if (location == MEM_LOC_HOST)
		mpset->host_mem_size += size;
	else
		mpset->device_mem_size += size;

	npid_add_allocated_memory(nd, location, size);
exit:
	mutex_unlock(&mpset->lock);
	if (ret) {
		kfree(mc);
		*result = NULL;
	}
	return ret;
}

int mc_alloc(struct neuron_device *nd, enum mc_lifespan lifespan, u64 size,
	     enum mem_location location, u32 channel, u32 region, u32 nc_id,
	     struct mem_chunk **result) {
	return mc_alloc_internal(nd, lifespan, size, 0, location, channel, region, nc_id, result);
}

int mc_alloc_align(struct neuron_device *nd, enum mc_lifespan lifespan, u64 size, u64 align,
		   enum mem_location location, u32 channel, u32 region, u32 nc_id,
		   struct mem_chunk **result) {
	return mc_alloc_internal(nd, lifespan, size, align, location, channel, region, nc_id, result);
};

void mc_inc_refcount(struct mem_chunk *mc)
{
	struct mempool_set *mpset = mc->mpset;
	mutex_lock(&mpset->lock);
	mc->ref_count++;
	mutex_unlock(&mpset->lock);
}

void mc_free(struct mem_chunk **mcp)
{
	struct mempool_set *mpset;
	struct mem_chunk *mc = *mcp;

	BUG_ON(mc == NULL);
	BUG_ON(mc->magic != MEMCHUNK_MAGIC);
	mpset = mc->mpset;
	BUG_ON(mpset == NULL);
	mutex_lock(&mpset->lock);
	mc->ref_count--;
	if (mc->ref_count > 0) {
		mutex_unlock(&mpset->lock);
		return;
	}

	write_lock(&mpset->rblock);
	mc_remove_node(&mpset->root, mc);
	write_unlock(&mpset->rblock);
	if (mc->mem_location == MEM_LOC_HOST) {
		if (mc->mp) {
			gen_pool_free(mc->mp->gen_pool, (u64)mc->va, mc->size);
			mc->mp->allocated_size -= mc->size;
		} else {
			dma_free_coherent(mpset->pdev, mc->size, mc->va, mc->pa & ~PCI_HOST_BASE(nd));
		}
		mpset->host_mem_size -= mc->size;
	} else if (mc->mem_location == MEM_LOC_DEVICE) {
		struct mempool *mp;
		mp = &mpset->mp_device[mc->dram_channel][mc->dram_region];
		gen_pool_free(mp->gen_pool, (u64)mc->va, mc->size);
		mp->allocated_size -= mc->size;
		mpset->device_mem_size -= mc->size;
	} else {
		BUG();
	}

	npid_dec_allocated_memory(mpset->nd, mc->mem_location, mc->size);
	*mcp = NULL;
	mc_remove_from_lifespan_list(mc);
	mc->magic = 0xDEAD;
	mutex_unlock(&mpset->lock);

	kfree(mc);
}
