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

int mempool_min_alloc_size = 256;
int mempool_host_memory_size = 32 * 1024 * 1024;

module_param(mempool_min_alloc_size, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(mempool_min_alloc_size, "Minimum size for memory allocation");

module_param(mempool_host_memory_size, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(mempool_host_memory_size, "Host memory to reserve(in bytes)");

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_mc_alloc);
#endif

// Limit for using kmalloc
#define MEMPOOL_KMALLOC_MAX_SIZE (256 * 1024)

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
 * Creates a backing gen_pool if the mem_location is device DRAM.
 *
 * @mp: pointer to mempool that needs to be initialized
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

	memset(mp, 0, sizeof(*mp));

	mp->mpset = mpset;
	mp->mem_location = MEM_LOC_DEVICE;
	mp->dram_channel = dram_channel;
	mp->dram_region = dram_region;
	INIT_LIST_HEAD(&mp->mc_list_head);
	mp->gen_pool = gen_pool_create(ilog2(mempool_min_alloc_size), -1);
	if (mp->gen_pool == NULL)
		return -ENOMEM;

	// 0 is special since we cant differentiate failure(NULL) in gen_pool_alloc().
	// so avoid starting at 0 by sacrificing first chunk.
	if (start_addr == 0) {
		start_addr = mempool_min_alloc_size;
		pool_size -= mempool_min_alloc_size;
	}
	ret = gen_pool_add_virt(mp->gen_pool, start_addr, start_addr, pool_size, -1);
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
 * Frees all the chunks associated with the mempool.
 */
static void mp_free_device_mem(struct mempool *mp)
{
	BUG_ON(mp == NULL);
	if (!mp->initialized)
		return;

	if (mp->gen_pool != NULL) {
		// Free all entries
		struct list_head *this, *next;

		list_for_each_safe (this, next, &mp->mc_list_head) {
			struct mem_chunk *mc =
				list_entry(this, struct mem_chunk, device_allocated_list);
			if (mc->va) {
				gen_pool_free(mp->gen_pool, (unsigned long)mc->va, mc->size);
				mc->va = NULL;
			}
			list_del(&mc->device_allocated_list);
			kfree(mc);
		}
		mp->allocated_size = 0;
	}
}

/**
 * Frees all the chunks associated with the mempool.
 */
static void mp_free_host_mem(struct mempool *mp)
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
 * mp_init_host_mem() Initialize the mempool structure to manage host memory.
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
static int mp_init_host_mem(struct mempool *mp, struct mempool_set *mpset,
			    u32 page_size, u32 page_count)
{
	int ret;
	int i;

	memset(mp, 0, sizeof(*mp));

	mp->mpset = mpset;
	mp->mem_location = MEM_LOC_HOST;
	INIT_LIST_HEAD(&mp->mc_list_head);
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
static void mp_destroy(struct mempool *mp)
{
	BUG_ON(mp == NULL);
	if (!mp->initialized)
		return;

	if (mp->gen_pool == NULL)
		return;

	// Free all entries
	if (mp->mem_location == MEM_LOC_HOST)
		mp_free_host_mem(mp);
	else
		mp_free_device_mem(mp);
	gen_pool_destroy(mp->gen_pool);
	mp->gen_pool = NULL;
}

int mpset_constructor(struct mempool_set *mpset, void *pdev)
{
	int host_page_index;
	u64 host_allocated_size = 0;

	memset(mpset, 0, sizeof(*mpset));

	mutex_init(&mpset->lock);
	INIT_LIST_HEAD(&mpset->host_allocated_head);
	mpset->root = RB_ROOT;
	mpset->pdev = pdev;

	// reserve host memory
	for (host_page_index = MP_HOST_POOL_COUNT - 1; host_page_index >= 0; host_page_index--) {
		u32 page_size = MP_HOST_PAGE_SIZE_MIN << host_page_index;
		u32 page_count = mempool_host_memory_size / page_size;
		int ret = 0;
		ret = mp_init_host_mem(&mpset->mp_host[host_page_index], mpset,
				       page_size, page_count);
		if (ret) {
			pr_err("neuron: mpset host init failed %d\n", ret);
			goto fail;
		}
		host_allocated_size += mpset->mp_host[host_page_index].region_size;
	}
	pr_info("reserved %llu bytes of host memory\n", host_allocated_size);

	return 0;

fail:
	for (; host_page_index < MP_HOST_POOL_COUNT; host_page_index++)
		mp_destroy(&mpset->mp_host[host_page_index]);

	return -ENOMEM;
}

void mpset_destructor(struct mempool_set *mpset)
{
	int i;

	mpset_release(mpset);
	mutex_lock(&mpset->lock);
	for (i = 0; i < MP_HOST_POOL_COUNT; i++)
		mp_destroy(&mpset->mp_host[i]);
	mutex_unlock(&mpset->lock);
}

int mpset_init(struct mempool_set *mpset, int num_channels, int num_regions,
	       const phys_addr_t *device_dram_addr, const u64 *device_dram_size)
{
	int ret;
	int channel = 0, region = 0;
	u64 region_sz;

	if (num_regions <= 0 || num_regions > 4)
		num_regions = 1;
	mpset->mp_device_num_regions = num_regions;

	for (channel = 0; channel < num_channels; channel++) {
		region_sz = device_dram_size[channel] / mpset->mp_device_num_regions;
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			dma_addr_t addr = device_dram_addr[channel] + (region * region_sz);
			ret = mp_init_device_mem(&mpset->mp_device[channel][region], mpset, addr,
						 region_sz, channel, region);
			if (ret) {
				pr_err("neuron: mpset device init failed %d\n", ret);
				goto fail;
			}
		}
	}

	atomic_set(&mpset->freed, 0);

	return 0;

fail:
	for (; channel >= 0; channel--) {
		for (; region >= 0; region--) {
			mp_destroy(&mpset->mp_device[channel][region]);
		}
	}
	memset(mpset, 0, sizeof(struct mempool_set));

	return ret;
}

static void mpset_free_host_memory(struct mempool_set *mpset)
{
	struct list_head *this, *next;
	if (mpset->host_allocated_head.next == NULL)
		return;
	list_for_each_safe (this, next, &mpset->host_allocated_head) {
		struct mem_chunk *mc = list_entry(this, struct mem_chunk, host_allocated_list);
		if (mc->va) {
			write_lock(&mpset->rblock);
			mc_remove_node(&mpset->root, mc);
			write_unlock(&mpset->rblock);
			if (mc->size > MEMPOOL_KMALLOC_MAX_SIZE || IS_ALIGNED(mc->size, PAGE_SIZE)) {
				dma_free_coherent(mpset->pdev, mc->size, mc->va, mc->pa);
			} else {
				kfree(mc->va);
			}
			mc->va = NULL;
		}
		list_del(&mc->host_allocated_list);
		kfree(mc);
	}
	mpset->host_mem_size = 0;
}

void mpset_release(struct mempool_set *mpset)
{
	u32 channel, region;
	int already_freed;

	already_freed = atomic_xchg(&mpset->freed, 1);
	if (already_freed)
		return;

	mutex_lock(&mpset->lock);
	for (channel = 0; channel < V1_MAX_DRAM_CHANNELS; channel++) {
		for (region = 0; region < mpset->mp_device_num_regions; region++) {
			mp_destroy(&mpset->mp_device[channel][region]);
		}
	}
	mpset_free_host_memory(mpset);
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

int mc_alloc(struct mempool_set *mpset, struct mem_chunk **result, u32 size,
	     enum mem_location location, u32 channel, u32 region, u32 nc_id)
{
	struct mem_chunk *mc;
	struct mempool *mp = NULL;
	int ret = 0;

	*result = NULL;

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
		// kmalloc uses compound pages which cant be mmmaped(), to avoid applications knowing
		// about kmalloc vs dma_alloc, use dma_alloc() for any PAGE_SIZE allocations
		// since mmap() can make request only of multiple of PAGE_SIZE.
		// TODO - get rid of kmalloc() and always use dma_alloc()
		if (size > MEMPOOL_KMALLOC_MAX_SIZE || IS_ALIGNED(size, PAGE_SIZE)) {
			dma_addr_t addr;
			mc->va = dma_alloc_coherent(mpset->pdev, size, &addr,
						    GFP_KERNEL | GFP_DMA32);
			mc->pa = (phys_addr_t)addr;
		} else {
			mc->va = (void *)kmalloc(size, GFP_KERNEL);
			if (mc->va) {
				memset(mc->va, 0, size);
				mc->pa = virt_to_phys(mc->va);
			}
		}
		// try to fill from reserved host memory
		if (mc->va == NULL) {
			int i;
			for (i = 0; i < MP_HOST_POOL_COUNT; i++) {
				u32 page_size = MP_HOST_PAGE_SIZE_MIN << i;
				if (page_size < size)
					continue;
				mp = &mpset->mp_host[i];
				mc->va = gen_pool_dma_alloc(mp->gen_pool, size, &mc->pa);
				if (mc->va)
					break;
			}
		}
		if (mc->va) {
			INIT_LIST_HEAD(&mc->host_allocated_list);
			list_add(&mc->host_allocated_list, &mpset->host_allocated_head);
			write_lock(&mpset->rblock);
			mc_insert_node(&mpset->root, mc);
			write_unlock(&mpset->rblock);
		} else {
			pr_info("host mem occupied %lld\n", mpset->host_mem_size);
		}
	} else {
		mp = &mpset->mp_device[channel][region];
		if (!mp->gen_pool) {
			pr_err("neuron: mempool not initialized\n");
			ret = -ENOMEM;
			goto exit;
		}

		mc->va = gen_pool_dma_alloc(mp->gen_pool, size, &mc->pa);
		if (mc->va) {
			INIT_LIST_HEAD(&mc->device_allocated_list);
			list_add(&mc->device_allocated_list, &mp->mc_list_head);
		} else {
			pr_info("%s total %ld occupied %ld needed %d available %ld\n", mp->name,
				mp->region_size, mp->allocated_size, size,
				gen_pool_avail(mp->gen_pool));
			pr_info("device regions %d occupied %lld\n", mpset->mp_device_num_regions,
				mpset->device_mem_size);
		}
		mp->allocated_size += size;
	}
	if (mc->va == NULL) {
		ret = -ENOMEM;
		goto exit;
	}

	mc->mpset = mpset;
	mc->mp = mp;
	mc->size = size;
	mc->mem_location = location;
	mc->dram_channel = channel;
	mc->dram_region = region;
	mc->nc_id = nc_id;

	if (location == MEM_LOC_HOST)
		mpset->host_mem_size += size;
	else
		mpset->device_mem_size += size;

exit:
	mutex_unlock(&mpset->lock);
	if (ret) {
		kfree(mc);
		*result = NULL;
	}
	return ret;
}

void mc_free(struct mem_chunk **mcp)
{
	struct mempool_set *mpset;
	struct mem_chunk *mc = *mcp;

	if (mc == NULL)
		return;

	mpset = mc->mpset;
	mutex_lock(&mpset->lock);

	if (mc->used_by_nq) {
		pr_err("Trying to free memory that is still used by nq");
		mutex_unlock(&mpset->lock);
		return;
	}
	if (mc->mem_location == MEM_LOC_HOST) {
		list_del(&mc->host_allocated_list);
		write_lock(&mpset->rblock);
		mc_remove_node(&mpset->root, mc);
		write_unlock(&mpset->rblock);
		if (mc->mp) {
			gen_pool_free(mc->mp->gen_pool, (u64)mc->va, mc->size);
			mc->mp->allocated_size -= mc->size;
		} else if (mc->size > MEMPOOL_KMALLOC_MAX_SIZE || IS_ALIGNED(mc->size, PAGE_SIZE)) {
			dma_free_coherent(mpset->pdev, mc->size, mc->va, mc->pa);
		} else {
			kfree(mc->va);
			mc->va = NULL;
		}
		mpset->host_mem_size -= mc->size;
	} else if (mc->mem_location == MEM_LOC_DEVICE) {
		struct mempool *mp;
		mp = &mpset->mp_device[mc->dram_channel][mc->dram_region];
		list_del(&mc->device_allocated_list);
		gen_pool_free(mp->gen_pool, (u64)mc->va, mc->size);
		mp->allocated_size -= mc->size;
		mpset->device_mem_size -= mc->size;
	} else {
		BUG();
	}

	*mcp = NULL;
	mutex_unlock(&mpset->lock);

	kfree(mc);
}

void mc_set_used_by_nq(struct mem_chunk *mc, bool used)
{
	struct mempool_set *mpset;

	if (mc == NULL)
		return;

	mpset = mc->mpset;
	mutex_lock(&mpset->lock);
	mc->used_by_nq = used;
	mutex_unlock(&mpset->lock);
}
