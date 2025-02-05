// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Exposes device node interface(/dev/neuron0) for each device.
 *  see neuron_ioctl.h for all the operations that can be done this node.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/dma-buf.h>

#include "neuron_ioctl.h"
#include "neuron_device.h"
#include "neuron_core.h"
#include "neuron_mmap.h"
#include "neuron_crwl.h"
#include "neuron_dma.h"
#include "neuron_mempool.h"
#include "neuron_topsp.h"
#include "neuron_trace.h"
#include "neuron_arch.h"
#include "neuron_reset.h"
#include "neuron_sysfs_metrics.h"
#include "neuron_dmabuf.h"
#include "neuron_dhal.h"
#include "neuron_nq.h"
#include "neuron_pci.h"
#include "neuron_cdev.h"

#include "v1/address_map.h"
#include "v2/address_map.h"
#include "neuron_fw_io.h"
#include "v1/fw_io.h"

static dev_t neuron_dev;
static int major;
static struct class *neuron_dev_class;

/* one device node per device */
#define NEURON_MAX_DEV_NODES MAX_NEURON_DEVICE_COUNT
#define IS_NEURON_DEVICE_FREE_ACCESS(filep) ((filep->f_flags & O_WRONLY) == 1)
struct ncdev {
	int minor;
	int open_count; // number of times this node is opened.
	struct cdev cdev;
	struct mutex ncdev_lock;
	struct neuron_device *ndev; // neuron device associated with this device node.
	struct device *device;
};

/* char device nodes created for each device. */
static struct ncdev devnodes[NEURON_MAX_DEV_NODES];

static u64 ncdev_mem_chunk_to_mem_handle(struct mem_chunk *mc)
{
	return (u64)mc;
}

static struct mem_chunk *ncdev_mem_handle_to_mem_chunk(u64 mh)
{
	struct mem_chunk *mc = (struct mem_chunk *)mh;
	if (!mc || mc->magic != MEMCHUNK_MAGIC) {
		pr_err("invalid memory handle %llx\n", mh);
		return NULL;
	}
	return mc;
}


static unsigned long neuron_copy_from_user(const char *const fname, void * to, const void __user * from, unsigned long n) {
	const long ret = copy_from_user(to, from, n);
	if (ret) {
		pr_err("copy_from_user failed: %s\n", fname);
	}
	return ret;
}

static int ncdev_dma_engine_set_state(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_eng_set_state arg;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_eng_set_state *)param, sizeof(arg));
	if (ret)
		return ret;
	return ndmar_eng_set_state(nd, arg.eng_id, arg.state);
}

static int ncdev_dma_engine_get_state(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_eng_get_state arg;
	struct neuron_dma_eng_state state;
	int ret;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_eng_get_state *)param, sizeof(arg));
	if (ret)
		return ret;
	ret = ndmar_eng_get_state(nd, arg.eng_id, &state);
	if (ret)
		return ret;
	return copy_to_user(arg.state, &state, sizeof(state));
}

static int ncdev_dma_queue_init(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_queue_init arg;
	struct mem_chunk *rx_mc;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rxc_mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_queue_init *)param, sizeof(arg));
	if (ret) {
		return -EACCES;
	}
	if (arg.rx_handle)
		rx_mc = ncdev_mem_handle_to_mem_chunk(arg.rx_handle);
	else
		rx_mc = NULL;
	if (arg.tx_handle)
		tx_mc = ncdev_mem_handle_to_mem_chunk(arg.tx_handle);
	else
		tx_mc = NULL;
	if (arg.rxc_handle)
		rxc_mc = ncdev_mem_handle_to_mem_chunk(arg.rxc_handle);
	else
		rxc_mc = NULL;
	ret = ndmar_queue_init(nd, arg.eng_id, arg.qid, arg.tx_desc_count, arg.rx_desc_count, tx_mc,
			       rx_mc, rxc_mc, arg.axi_port);
	return ret;
}

static int ncdev_dma_queue_init_batch_entry(struct neuron_device *nd, struct neuron_ioctl_dma_queue_init * arg)
{
	struct mem_chunk *rx_mc;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rxc_mc;
	int ret;

	if (arg->rx_handle)
		rx_mc = ncdev_mem_handle_to_mem_chunk(arg->rx_handle);
	else
		rx_mc = NULL;
	if (arg->tx_handle)
		tx_mc = ncdev_mem_handle_to_mem_chunk(arg->tx_handle);
	else
		tx_mc = NULL;
	if (arg->rxc_handle)
		rxc_mc = ncdev_mem_handle_to_mem_chunk(arg->rxc_handle);
	else
		rxc_mc = NULL;
	ret = ndmar_queue_init(nd, arg->eng_id, arg->qid, arg->tx_desc_count, arg->rx_desc_count, tx_mc,
						   rx_mc, rxc_mc, arg->axi_port);
	return ret;
}

static int ncdev_dma_queue_init_batch(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_queue_init_batch *arg = kmalloc(sizeof(struct neuron_ioctl_dma_queue_init_batch), GFP_KERNEL);
	int ret;

	if (!arg) {
		return -ENOMEM;
	}

	ret = neuron_copy_from_user(__func__, arg, (struct neuron_ioctl_dma_queue_init_batch *)param, sizeof(struct neuron_ioctl_dma_queue_init_batch));
	if (ret) {
		return -EACCES;
	}

	if (arg->count >= MAX_DMA_QUEUE_INIT_BATCH) {
		ret = -E2BIG;
		goto done;
	}

	u32 i = 0;
	for (i = 0; i < arg->count; i++) {
		ret = ncdev_dma_queue_init_batch_entry(nd, &arg->entries[i]);
		if (ret) goto done;
	}

done:
	if (arg) kfree(arg);
	return ret;
}

static int ncdev_dma_copy_descriptors(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_copy_descriptors arg;
	struct mem_chunk *src_mc;
	u32 offset = 0, copy_size = 0;
	int remaining, ret;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_copy_descriptors *)param, sizeof(arg));
	if (ret)
		return ret;

	struct mem_chunk *mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	if (!mc)
		return -EINVAL;
	// check access is within the range.
	if (arg.offset + (arg.num_descs * sizeof(union udma_desc)) > mc->size) {
		ret = -EINVAL;
		goto out;
	}

	remaining = arg.num_descs * sizeof(union udma_desc);
	ret = mc_alloc(nd, MC_LIFESPAN_LOCAL, MAX_DMA_DESC_SIZE, MEM_LOC_HOST, 0, 0, mc->nc_id, &src_mc);
	if (ret) {
		ret = -ENOMEM;
		goto out;
	}
	while (remaining) {
		copy_size = remaining < MAX_DMA_DESC_SIZE ? remaining : MAX_DMA_DESC_SIZE;
		ret = neuron_copy_from_user(__func__, src_mc->va, arg.buffer + offset, copy_size);
		if (ret) {
			break;
		}
		ret = ndma_memcpy_dma_copy_descriptors(nd, src_mc->va, 0, mc, arg.offset + offset,
						       copy_size, arg.queue_type);
		if (ret) {
			break;
		}
		remaining -= copy_size;
		offset += copy_size;
	}
out:
	mc_free(&src_mc);
	return ret;
}

static int ncdev_dma_copy_start(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_queue_copy_start arg;
	int ret;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_queue_copy_start *)param, sizeof(arg));
	if (ret)
		return ret;

	ret = ndmar_queue_copy_start(nd, arg.eng_id, arg.qid, arg.tx_desc_count, arg.rx_desc_count);
	return ret;
}

static int ncdev_dma_ack_completed(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_ack_completed arg;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_ack_completed *)param, sizeof(arg));
	if (ret)
		return ret;

	return ndmar_ack_completed(nd, arg.eng_id, arg.qid, arg.count);
}

static int ncdev_dma_queue_get_state(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_queue_get_state arg;
	struct neuron_dma_queue_state tx, rx;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_queue_get_state *)param, sizeof(arg));
	if (ret)
		return ret;
	ret = ndmar_queue_get_state(nd, arg.eng_id, arg.qid, &tx, &rx);
	if (ret)
		return ret;
	ret = copy_to_user(arg.tx, &tx, sizeof(tx));
	if (ret)
		return ret;
	return copy_to_user(arg.rx, &rx, sizeof(rx));
}

static int ncdev_dma_queue_release(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_queue_release arg;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_queue_release *)param, sizeof(arg));
	if (ret)
		return ret;
	return ndmar_queue_release(nd, arg.eng_id, arg.qid);
}

static int ncdev_dma_quiesce_queues(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_dma_quiesce_queues arg;
	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_quiesce_queues*)param, sizeof(arg));
	if (ret)
		return ret;
	return ndhal->ndhal_ndmar.ndmar_quiesce_queues(nd, arg.nc_id, arg.engine_count, arg.queue_mask);
}

static int ncdev_dma_descriptor_copyout(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_dma_descriptor_copyout arg;
	struct mem_chunk *tx = NULL, *rx = NULL, *mc = NULL;
	u32 tx_size = 0, rx_size = 0;
	void *addr = NULL;
	u32 desc_size = sizeof(union udma_desc), total_size, offset;
	int ret;
	struct ndma_eng *eng;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_dma_descriptor_copyout *)param,
			     sizeof(arg));
	if (ret)
		return ret;
	if (arg.count == 0)
		return -EINVAL;

	total_size = arg.count * desc_size;
	offset = arg.start_index * desc_size;

	ret = ndmar_queue_get_descriptor_mc(nd, arg.eng_id, arg.qid, &tx, &rx, &tx_size, &rx_size);
	if (ret) {
		pr_err("get DMA queue desc failed %d\n", ret);
		return -EINVAL;
	}

	eng = ndmar_acquire_engine(nd, arg.eng_id);
	if (arg.type == NEURON_DMA_QUEUE_TYPE_TX) {
		if (arg.count > tx_size) {
			pr_err("tx size is less than count %d tx %d\n", arg.count, tx_size);
			ret = -EFBIG;
			goto done;
		}
		mc = tx;
	} else if (arg.type == NEURON_DMA_QUEUE_TYPE_RX) {
		if (arg.count > rx_size) {
			pr_err("rx size is less than count %d rx %d\n", arg.count, rx_size);
			ret = -EFBIG;
			goto done;
		}
		mc = rx;
	}
	if (mc == NULL) {
		ret = -EINVAL;
		goto done;
	}
	if (mc->magic != MEMCHUNK_MAGIC) {
		pr_err("invalid mem_chunk %p\n", mc);
		ret = -EINVAL;
		goto done;
	}
	if (mc->mem_location == MEM_LOC_DEVICE) {
		addr = kmalloc(total_size, GFP_KERNEL);
		if (addr == NULL) {
			ret = -ENOMEM;
			goto done;
		}
		ret = ndma_memcpy_buf_from_mc(nd, addr, 0, mc, offset, total_size);
		if (ret) {
			kfree(addr);
			goto done;
		}
	} else {
		addr = mc->va + offset;
	}

	ret = copy_to_user(arg.buffer, addr, total_size);
	if (mc->mem_location == MEM_LOC_DEVICE)
		kfree(addr);

done:
	ndmar_release_engine(eng);
	return ret;
}

static int ncdev_mem_alloc(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_alloc mem_alloc_arg;
	enum mem_location location;
	u64 mh;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &mem_alloc_arg, (struct neuron_ioctl_mem_alloc *)param,
			     sizeof(mem_alloc_arg));
	if (ret)
		return -EACCES;
	if (mem_alloc_arg.host_memory)
		location = MEM_LOC_HOST;
	else
		location = MEM_LOC_DEVICE;
	ret = mc_alloc(nd, MC_LIFESPAN_CUR_PROCESS, mem_alloc_arg.size, location, mem_alloc_arg.dram_channel,
		       mem_alloc_arg.dram_region, mem_alloc_arg.nc_id, &mc);
	if (ret)
		return ret;

	trace_ioctl_mem_alloc(nd, mc);

	mh = ncdev_mem_chunk_to_mem_handle(mc);
	ret = copy_to_user(mem_alloc_arg.mem_handle, &mh, sizeof(mc));
	if (ret) {
		mc_free(&mc);
		return ret;
	}
	return 0;
}

static int ncdev_mem_alloc_libnrt(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_alloc_v2 mem_alloc_arg;
	enum mem_location location;
	u64 mh;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &mem_alloc_arg, (struct neuron_ioctl_mem_alloc_v2 *)param,
			     sizeof(mem_alloc_arg));
	if (ret)
		return ret	;
	if (mem_alloc_arg.host_memory)
		location = MEM_LOC_HOST;
	else
		location = MEM_LOC_DEVICE;
	ret = mc_alloc_align(nd, MC_LIFESPAN_CUR_PROCESS, mem_alloc_arg.size, mem_alloc_arg.align, location, mem_alloc_arg.dram_channel,
		       mem_alloc_arg.dram_region, mem_alloc_arg.nc_id, &mc);
	if (ret)
		return ret;

	trace_ioctl_mem_alloc(nd, mc);

	mh = ncdev_mem_chunk_to_mem_handle(mc);
	ret = copy_to_user(mem_alloc_arg.mem_handle, &mh, sizeof(mc));
	if (ret) {
		mc_free(&mc);
		return ret;
	}
	return 0;
}

static int ncdev_mem_get_pa_deprecated(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_get_pa mem_get_pa_arg;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &mem_get_pa_arg, (struct neuron_ioctl_mem_get_pa *)param,
			     sizeof(mem_get_pa_arg));
	if (ret)
		return ret;

	mc = ncdev_mem_handle_to_mem_chunk(mem_get_pa_arg.mem_handle);
	if (!mc)
		return -EINVAL;
	return copy_to_user(mem_get_pa_arg.pa, &mc->pa, sizeof(u64));
}

static int ncdev_mem_get_info_deprecated(void *param)
{
	struct neuron_ioctl_mem_get_info arg;
	struct mem_chunk *mc;
	u64 mmap_offset;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	if (!mc)
		return -EINVAL;
	ret = copy_to_user(arg.pa, &mc->pa, sizeof(u64));
	if (ret)
		return ret;

	if (arg.mmap_offset) {
		mmap_offset = nmmap_offset(mc);
		ret = copy_to_user(arg.mmap_offset, &mmap_offset, sizeof(mmap_offset));
	}

	return ret;
}

static int ncdev_mem_get_extended_info(void *param)
{
	struct neuron_ioctl_mem_get_extended_info arg, local;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	if (!mc)
		return EINVAL;

	if (mc->mem_location == MEM_LOC_HOST) {
		local.pa = mc->pa | PCIEX8_0_BASE;
		local.host_memory = true;
	} else {
		local.pa = mc->pa;
		local.host_memory = false;
	}
	local.size = mc->size;
	local.mmap_offset = nmmap_offset(mc);
	local.mem_handle = (u64)mc;
	local.pid = mc->pid;

	return copy_to_user(param, &local, sizeof(local));
}

/**
 * ncdev_mem_get_mc_mmap_info()
 *
 *   for a given address return the mc info required to mmap the address
 */
static int ncdev_mem_get_mc_mmap_info(struct neuron_device *nd, unsigned int cmd, void*param)
{
	int ret;
	struct neuron_ioctl_mem_get_mc_mmap_info arg;
	struct mem_chunk *mc;

	// compatibility check
	if (_IOC_SIZE(cmd) != sizeof(arg)) {
		return -EINVAL;
	}

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	mc = nmmap_get_mc_from_pa(nd, arg.pa);

	if (mc == NULL) {
		return -EINVAL;
	}

	arg.mmap_offset = mc->pa;
	arg.size = mc->size;

	return copy_to_user(param, &arg, sizeof(arg));
}

static int ncdev_get_dmabuf_fd(void *param)
{
	int ret;
	struct neuron_ioctl_dmabuf_fd arg;
    int dmabuf_fd;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	ret = ndmabuf_get_fd(arg.va, arg.size, &dmabuf_fd);
	if (ret)
		return ret;

	return copy_to_user(arg.fd, &dmabuf_fd, sizeof(dmabuf_fd));
}

static int ncdev_mem_free(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_free mem_free_arg;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &mem_free_arg, (struct neuron_ioctl_mem_free *)param,
			     sizeof(mem_free_arg));
	if (ret)
		return ret;
	mc = ncdev_mem_handle_to_mem_chunk(mem_free_arg.mem_handle);
	if (!mc)
		return -EINVAL;
	trace_ioctl_mem_alloc(nd, mc);
	mc_free(&mc);
	return 0;
}

static int ncdev_memset(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_memset arg;
	struct mem_chunk *mc;
	int ret = 0;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_memset *)param, sizeof(arg));
	if (ret)
		return ret;
	mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	if (!mc)
		return -EINVAL;
	// check access is within the range.
	if (arg.offset + arg.size > mc->size) {
		pr_err("offset+size is too large for mem handle\n");
		return -EINVAL;
	}

	ret = ndma_memset(nd, mc, arg.offset, arg.value, arg.size);
	if (ret) {
		pr_err("memset failed\n");
	}
	return ret;
}

static int ncdev_mem_copy(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_copy arg;
	struct mem_chunk *src_mc;
	struct mem_chunk *dst_mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_mem_copy *)param, sizeof(arg));
	if (ret)
		return ret;
	src_mc = ncdev_mem_handle_to_mem_chunk(arg.src_mem_handle);
	dst_mc = ncdev_mem_handle_to_mem_chunk(arg.dst_mem_handle);
	if (!src_mc || !dst_mc)
		return -EINVAL;

	// check access is within the range.
	if (arg.src_offset + arg.size > src_mc->size) {
		pr_err("src offset+size is too large for mem handle\n");
		return -EINVAL;
	}
	// check access is within the range.
	if (arg.dst_offset + arg.size > dst_mc->size) {
		pr_err("src offset+size is too large for mem handle\n");
		return -EINVAL;
	}
	ret = ndma_memcpy_mc(nd, src_mc, dst_mc, arg.src_offset, arg.dst_offset, arg.size);
	if (ret) {
		pr_err("dma memcpy failed\n");
		return ret;
	}
	trace_ioctl_mem_copy(nd, src_mc, dst_mc);
	return 0;
}

static int ncdev_mem_copy_async(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_copy_async arg;
	struct mem_chunk *src_mc;
	struct mem_chunk *dst_mc;
	int    ret;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_mem_copy_async *)param, sizeof(arg));
	if (ret)
		return ret;
	src_mc = ncdev_mem_handle_to_mem_chunk(arg.src_mem_handle);
	dst_mc = ncdev_mem_handle_to_mem_chunk(arg.dst_mem_handle);
	if (!src_mc || !dst_mc)
		return -EINVAL;

	// check access is within the range.
	if (arg.src_offset + arg.size > src_mc->size) {
		pr_err("src offset+size is too large for mem handle\n");
		return -EINVAL;
	}
	// check access is within the range.
	if (arg.dst_offset + arg.size > dst_mc->size) {
		pr_err("dst offset+size is too large for mem handle\n");
		return -EINVAL;
	}

	ret = ndma_memcpy_mc_async(nd, src_mc, dst_mc, arg.src_offset, arg.dst_offset, arg.size, arg.host_prefetch_addr, arg.pwait_handle, &arg.wait_handle);
	if (ret) {
		pr_err("dma memcpy failed: %d\n", ret);
		return ret;
	}

	// return the new wait handle
	ret = copy_to_user((struct neuron_ioctl_mem_copy_async *)param, &arg, sizeof(arg));

	trace_ioctl_mem_copy(nd, src_mc, dst_mc);
	return ret;
}

static int ncdev_mem_copy_async_wait(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_copy_async_wait arg;
	struct mem_chunk *src_mc;
	struct mem_chunk *dst_mc;
	int    ret;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_mem_copy_async_wait *)param, sizeof(arg));
	if (ret)
		return ret;

	src_mc = ncdev_mem_handle_to_mem_chunk(arg.src_mem_handle);
	dst_mc = ncdev_mem_handle_to_mem_chunk(arg.dst_mem_handle);
	if (!src_mc || !dst_mc) {
		pr_err("dma memcpy wait failed. invalid mem chunk handle\n");
		return -EINVAL;
	}

	if ((arg.pwait_handle < NEURON_DMA_H2T_CTX_HANDLE_ASYNC1) || (arg.pwait_handle > NEURON_DMA_H2T_CTX_HANDLE_ASYNC2))  {
		pr_err("dma memcpy wait failed. invalid wait handle: %d\n", arg.pwait_handle);
		return -EINVAL;
	}

	ret = ndma_memcpy_mc_wait( nd, src_mc, dst_mc, arg.pwait_handle);
	if (ret) {
		pr_err("dma memcpy wait failed: %d\n", ret);
		return ret;
	}
	return ret;
}

static int ncdev_verify_mem_region(u64 addr)
{
	int i = 0;
	while (ndhal->ndhal_cdev.ncdev_mem_regions[i].start != NCDEV_MEM_REGION_INVALID) {
		if ((addr >= ndhal->ndhal_cdev.ncdev_mem_regions[i].start) 
		  && (addr <= (ndhal->ndhal_cdev.ncdev_mem_regions[i].start + ndhal->ndhal_cdev.ncdev_mem_regions[i].size))) {
			return 0;
		}
		i++;
	}

	pr_err("Address out of range addr:0x%llx", (u64)addr);
	return -ENOMEM;
}

int ncdev_program_engine(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_program_engine arg;
	int ret;
	struct mem_chunk *src_mc;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_program_engine *)param, sizeof(arg));
	if (ret)
		return ret;

	if (ncdev_verify_mem_region(arg.dst))
		return -ENOMEM;

	ret = mc_alloc(nd, MC_LIFESPAN_LOCAL, arg.size, MEM_LOC_HOST, 0, 0, 0, &src_mc);
	if (ret) {
		ret = -ENOMEM;
		return ret;
	}

	ret = neuron_copy_from_user(__func__, src_mc->va, arg.buffer + arg.offset, arg.size);
	if (ret)
		goto error;

	ret = ndma_memcpy(nd, 0, virt_to_phys(src_mc->va) | ndhal->ndhal_address_map.pci_host_base,
			  arg.dst + arg.offset, arg.size);

    if (ret) {
		pr_err("engine programming dma failed. addr: %llu\n", arg.dst + arg.offset);
	}

error:
	mc_free(&src_mc);
	return ret;
}

int ncdev_program_engine_nc(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_program_engine_nc arg;
	int ret;
	struct mem_chunk *src_mc;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_program_engine_nc *)param, sizeof(arg));
	if (ret)
		return ret;

	if (ncdev_verify_mem_region(arg.dst))
		return -ENOMEM;

	ret = mc_alloc(nd, MC_LIFESPAN_LOCAL, arg.size, MEM_LOC_HOST, 0, 0, arg.nc_id, &src_mc);
	if (ret) {
		pr_err("engine programming dma mc_alloc failed. nc_id: %d addr: %llu size: %u err: %d\n",arg.nc_id,  arg.dst + arg.offset, arg.size, ret);
		ret = -ENOMEM;
		return ret;
	}

	ret = neuron_copy_from_user(__func__, src_mc->va, arg.buffer + arg.offset, arg.size);
	if (ret)
		goto error;

	ret = ndma_memcpy(nd, arg.nc_id, virt_to_phys(src_mc->va) | ndhal->ndhal_address_map.pci_host_base, arg.dst + arg.offset, arg.size);

	if (ret) {
		pr_err("engine programming dma failed. nc_id: %d addr: %llu\n",arg.nc_id,  arg.dst + arg.offset);
	}
error:
	mc_free(&src_mc);
	return ret;
}

int ncdev_mem_buf_copy(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_mem_buf_copy arg;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_mem_buf_copy *)param, sizeof(arg));
	if (ret)
		return ret;
	mc = ncdev_mem_handle_to_mem_chunk(arg.mem_handle);
	if (!mc)
		return -EINVAL;
	// check access is within the range.
	if (arg.offset + arg.size > mc->size) {
		pr_err("offset+size is too large for mem handle\n");
		return -EINVAL;
	}

	if (arg.copy_to_mem_handle)
		trace_ioctl_mem_copyin(nd, mc, arg.buffer, arg.offset, arg.size);
	else
		trace_ioctl_mem_copyout(nd, mc, arg.buffer, arg.offset, arg.size);

	if (mc->mem_location == MEM_LOC_HOST) {
		if (arg.copy_to_mem_handle) {
			ret = neuron_copy_from_user(__func__, mc->va + arg.offset, arg.buffer, arg.size);
		} else {
			ret = copy_to_user(arg.buffer, mc->va + arg.offset, arg.size);
		}
		return ret;
	} else {
		// TODO - this has to be converted to mmap
		struct mem_chunk *src_mc;
		u32 offset = 0;
		int remaining = arg.size;
		u32 copy_size = 0;
		ret = mc_alloc(nd, MC_LIFESPAN_LOCAL, MAX_DMA_DESC_SIZE, MEM_LOC_HOST, 0, 0,
			       mc->nc_id, &src_mc);
		if (ret) {
			ret = -ENOMEM;
			return ret;
		}
		while (remaining) {
			copy_size = remaining < MAX_DMA_DESC_SIZE ? remaining : MAX_DMA_DESC_SIZE;
			if (arg.copy_to_mem_handle) {
				ret = neuron_copy_from_user(__func__, src_mc->va, arg.buffer + offset, copy_size);
				if (ret) {
					break;
				}
				ret = ndma_memcpy_buf_to_mc(nd, src_mc->va, 0, mc,
							    arg.offset + offset, copy_size);
				if (ret) {
					break;
				}
			} else {
				ret = ndma_memcpy_buf_from_mc(nd, src_mc->va, 0, mc,
							      arg.offset + offset, copy_size);
				if (ret) {
					break;
				}
				ret = copy_to_user(arg.buffer + offset, src_mc->va, copy_size);
				if (ret) {
					break;
				}
			}
			remaining -= copy_size;
			offset += copy_size;
		}
		mc_free(&src_mc);

		if (ret) {
			pr_err(" mem buffer copy failed\n");
		}
		return ret;
	}
}

static long ncdev_semaphore_ioctl(struct neuron_device *nd, unsigned int cmd, void *param)
{
	int ret;
	struct neuron_ioctl_semaphore arg;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_semaphore *)param, sizeof(arg));
	if (ret)
		return ret;
	if (cmd == NEURON_IOCTL_SEMAPHORE_READ) {
		ret = nc_semaphore_read(nd, arg.nc_id, arg.semaphore_index, &arg.value);
		if (ret)
			return ret;
		return copy_to_user((struct neuron_ioctl_semaphore *)param, &arg, sizeof(arg));
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_WRITE) {
		return nc_semaphore_write(nd, arg.nc_id, arg.semaphore_index, arg.value);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_INCREMENT) {
		return nc_semaphore_increment(nd, arg.nc_id, arg.semaphore_index, arg.value);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_DECREMENT) {
		return nc_semaphore_decrement(nd, arg.nc_id, arg.semaphore_index, arg.value);
	}
	return -1;
}

static long ncdev_events_ioctl(struct neuron_device *nd, unsigned int cmd, void *param)
{
	int ret;
	struct neuron_ioctl_event arg;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_event *)param,
			     sizeof(struct neuron_ioctl_event));
	if (ret)
		return ret;

	if (cmd == NEURON_IOCTL_EVENT_GET) {
		ret = nc_event_get(nd, arg.nc_id, arg.event_index, &arg.value);
		if (ret)
		return ret;
		return copy_to_user((struct neuron_ioctl_event *)param, &arg, sizeof(arg));
	} else if (cmd == NEURON_IOCTL_EVENT_SET) {
		return nc_event_set(nd, arg.nc_id, arg.event_index, arg.value);
	}
	return -1;
}

static long ncdev_bar_read(struct neuron_device *nd, u8 bar, u64 *reg_addresses, void *user_va,
			   u32 data_count)
{
	int ret;
	u64 data_size = data_count * sizeof(u32);
	int i;
	if (bar == 0 || bar == 2) {
		u32 *data = NULL;
		data = kmalloc(data_size, GFP_KERNEL);
		if (data == NULL)
			return -ENOMEM;
		ret = ndhal->ndhal_reg_access.reg_read32_array((void **)reg_addresses, data, data_count);
		if (ret) {
			kfree(data);
			return ret;
		}
		for (i = 0; i < data_count; i++) {
			trace_bar_read(nd, bar, reg_addresses[i], data[i]);
		}
		ret = copy_to_user(user_va, data, data_size);
		kfree(data);
	} else {
		// TODO: we don't have any use case for r/w memory over the BAR right now.  Disabling.
		//
		// We'd like to use DMA for r/w of BAR4 because we might expect access to large amounts of data.
		// Access via DMA requires an application to own a TPB because it determines which of the h2t DMAs
		// are safe to use, otherwise a TPB along with its DMA could be reset while that DMA is used here.
		// Don't want/need to solve it now.
		ret = -EINVAL;
		/*
		struct mem_chunk *mc;
		u32 nc_id = 0;
		dma_addr_t src_addr = reg_addresses[0];

		ret = mc_alloc(nd, MC_LIFESPAN_LOCAL, data_size, MEM_LOC_HOST, 0, 0, nc_id, &mc);
		if (ret)
			return -ENOMEM;

		ret = ndma_memcpy(nd, mc->nc_id, src_addr, mc->pa, data_size);
		if (ret) {
			mc_free(&mc);
			return ret;
		}
		for (i = 0; i < data_count; i++) {
			uint32_t *data = mc->va;
			trace_bar_read(nd, bar, src_addr + (i * sizeof(uint32_t)), data[i]);
		}
		ret = copy_to_user(user_va, mc->va, data_size);
		mc_free(&mc);
		*/
	}
	return ret;
}

static long ncdev_bar_write(struct neuron_device *nd, u8 bar, u64 *reg_addresses, void *user_va,
			    u32 data_count)
{
	int ret = 0;
	u32 *data = NULL;
	u64 data_size = data_count * sizeof(u32);

	data = kmalloc(data_size, GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;
	ret = neuron_copy_from_user(__func__, data, user_va, data_size);
	if (ret)
		goto done;

	ret = ndhal->ndhal_cdev.ncdev_bar_write_data(nd, bar, reg_addresses, data, data_count);
	if (ret)
		goto done;
done:
	kfree(data);

	return ret;
}

static long ncdev_bar_rw(struct neuron_device *nd, void *param, bool read)
{
	int ret;
	struct neuron_ioctl_bar_rw arg;
	u64 *reg_addresses = NULL;
	u64 address_count;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_bar *)param, sizeof(arg));
	if (ret)
		return ret;

	/* BAR2 reads are always sequential and so addresses are autogenerated from base*/
	if (arg.bar == 0)
		address_count = arg.count;
	else
		address_count = 1;

	reg_addresses = kmalloc(address_count * sizeof(u64), GFP_KERNEL);
	if (reg_addresses == NULL)
		return -ENOMEM;

	ret = neuron_copy_from_user(__func__, reg_addresses, arg.address, address_count * sizeof(u64));
	if (ret != 0)
		goto done;

	if (read)
		ret = ncdev_bar_read(nd, arg.bar, reg_addresses, arg.data, arg.count);
	else
		ret = ncdev_bar_write(nd, arg.bar, reg_addresses, arg.data, arg.count);

done:
	kfree(reg_addresses);
	return ret;
}

static long ncdev_post_metric(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_post_metric arg;
	u32 *data = NULL;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_post_metric *)param, sizeof(arg));
	if (ret)
		return ret;

	data = kmalloc(arg.data_size, GFP_KERNEL);
	if (data == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	ret = neuron_copy_from_user(__func__, data, arg.data, arg.data_size);
	if (ret)
		goto done;
	ret = fw_io_post_metric(nd->fw_io_ctx, (u8 *)data, arg.data_size);
done:
	kfree(data);
	return ret;
}

static long ncdev_read_hw_counters(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_read_hw_counters arg;
	uint64_t *reg_addresses = NULL;
	uint32_t *data = NULL;

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_read_hw_counters *)param, sizeof(arg));
	if (ret)
		return ret;

	reg_addresses = kmalloc(arg.count * sizeof(uint64_t), GFP_KERNEL);
	if (reg_addresses == NULL)
		return -ENOMEM;
	ret = neuron_copy_from_user(__func__, reg_addresses, arg.address, arg.count * sizeof(uint64_t));
	if (ret != 0)
		goto done;

	data = kmalloc(arg.count * sizeof(uint32_t), GFP_KERNEL);
	if (data == NULL)
		goto done;

	ret = fw_io_read_counters(nd->fw_io_ctx, reg_addresses, data, arg.count);
	if (ret)
		goto done;
	ret = copy_to_user(arg.data, data, arg.count * sizeof(uint32_t));
done:
	kfree(reg_addresses);
	kfree(data);
	return ret;
}

static long ncdev_nc_reset(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_device_reset arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	ndmar_close_ncs(nd, arg.nc_map);
	arg.request_id = task_tgid_nr(current);
	ret = nr_start_ncs(nd, arg.nc_map, arg.request_id);
	if (ret) {
		return ret;
	}
	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_device_reset_deprecated(struct neuron_device *nd)
{
	ndmar_close(nd);
	nr_start(nd);
	return 0;
}

static long ncdev_device_reset_status_deprecated(struct neuron_device *nd, void *param)
{
	u8 result = 1; // always return reset status as started.
	return copy_to_user(param, &result, 1);
}

static long ncdev_nc_reset_ready(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_device_ready arg;
	int ret;
	arg.result = 0;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	arg.result = nr_wait(nd, arg.request_id, false);

	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_device_ready_deprecated(struct neuron_device *nd, void *param)
{
	u8 result;
	// there is a bug with this deprecated ioctl (see caller for more details)
	// where this api can be called twice. set the flag to check if the request
	// exists and return success if not so the second pass doesn't fail
	//
	// deprecated api returns true (1) if success and false if failed
	result = (nr_wait(nd, task_tgid_nr(current), true) == 0);
	return copy_to_user(param, &result, 1);
}

static void narch_fill_device_basic_info(struct neuron_ioctl_device_basic_info *dest)
{
	dest->architecture = narch_get_arch();
	dest->revision = narch_get_revision();
}

static long ncdev_device_basic_info(void *param)
{
	struct neuron_ioctl_device_basic_info result;
	narch_fill_device_basic_info(&result);
	return copy_to_user(param, &result, sizeof(result));
}

static long ncdev_device_bdf(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_device_bdf result;
	result.bus_number = nd->pdev->bus->number;
	result.slot = PCI_SLOT(nd->pdev->devfn);
	result.func = PCI_FUNC(nd->pdev->devfn);
	return copy_to_user(param, &result, sizeof(result));
}

static long ncdev_device_bdf_ext(struct file *filep, void *param)
{
	struct ncdev *ncd;
	struct neuron_device *nd;
	struct neuron_ioctl_device_bdf_ext arg;
	u32    offset;

	// for containers the index passed will be  relative to the device ordering as
	// seen from the container so we have to offset it from the device opened by misc ioctl
	// from ndl which is always device 0 (from the callers view)
	//
	ncd = filep->private_data;
	if (ncd == NULL) {
		return -EINVAL;
	}
	nd = ncd->ndev;
	if (nd == NULL) {
		return -EINVAL;
	}
	offset = nd->device_index;

	int ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	nd = neuron_pci_get_device(arg.nd_index + offset);
	if (!nd) {
		pr_err("Invalid nd index %d, offset %d", arg.nd_index, offset);
		return -EINVAL;
	}
	arg.domain = (__u32)pci_domain_nr(nd->pdev->bus);
	arg.bus_number = nd->pdev->bus->number;
	arg.slot = PCI_SLOT(nd->pdev->devfn);
	arg.func = PCI_FUNC(nd->pdev->devfn);
	return copy_to_user(param, &arg, sizeof(arg));
}

/* only one process can do discovery at a time */
static DEFINE_MUTEX(ncdev_discovery_lock);
static long ncdev_device_info(struct neuron_device *nd, void *param)
{
	int i, ret;
	u32 connected_devices[MAX_NEURON_DEVICE_COUNT];
	int connected_device_count = 0;
	struct neuron_ioctl_device_info result;

	narch_fill_device_basic_info((struct neuron_ioctl_device_basic_info *)&result);

	mutex_lock(&ncdev_discovery_lock);
	// if topology discovery is not yet done, do it and cache the result
	// if it fails, don't fail completely, wait for a retry otherwise ndl_open_device would fail
	ret = ndhal->ndhal_fw_io.fw_io_topology(nd->fw_io_ctx, nd->pdev->device, nd->device_index, connected_devices, &connected_device_count);
	if (ret) {
		connected_device_count = 0;
		pr_err("Unable to get connected devices for device %d", nd->device_index);
	}
	mutex_unlock(&ncdev_discovery_lock);

	// sanity check
	//
	if (connected_device_count > NEURON_IOCTL_MAX_CONNECTED_DEVICES) {
		pr_err("Connected device count exceeded maximum for device %d", nd->device_index);
		return -EINVAL;
	}

	for (i = 0; i < connected_device_count; i++) {
		result.connected_devices[i] = connected_devices[i];
	}
	result.connected_device_count = connected_device_count;

	result.bar_address[0] = (u64)nd->npdev.bar0;
	result.bar_size[0] = nd->npdev.bar0_size;
	result.bar_address[1] = (u64)nd->npdev.bar2;
	result.bar_size[1] = nd->npdev.bar2_size;

	ret = copy_to_user(param, &result, sizeof(result));

	return ret;
}

// as we change versions...
//Static_assert( sizeof(struct neuron_ioctl_device_driver_info0) < sizeof(struct neuron_ioctl_device_driver_info), "driver info struct versions must be different in size");

/**
 * ncdev_driver_info()
 *
 *    get/set driver info.  currently only support get. Interface uses
 *    same ioctl and overloads size/direction, allowing the API to work for multiple versions.
 *
 */
static long ncdev_driver_info(unsigned int cmd, void *param)
{
	struct neuron_ioctl_device_driver_info driver_info;
	unsigned int dir  = _IOC_DIR(cmd);
	unsigned int size = _IOC_SIZE(cmd);

	if (dir == _IOC_WRITE) {
		return -ENOTSUPP;
	} else if (dir == _IOC_READ) {
		// for forward/backward compatibility, there's two options (a) copy lesser of passed in size or struct size.
		// (b) or any size > most recent version just gets version size.
		if (size >= _IOC_SIZE(NEURON_IOCTL_DRIVER_INFO_GET)) {
			driver_info.architecture = narch_get_arch();
			driver_info.revision = narch_get_revision();
			driver_info.version = NEURON_DEVICE_DRIVER_INFO_VERSION0;
			driver_info.size = sizeof(driver_info);
			driver_info.feature_flags1 = NEURON_DRIVER_FEATURE_DMABUF | NEURON_DRIVER_FEATURE_ASYNC_DMA |
										 NEURON_DRIVER_FEATURE_BATCH_DMAQ_INIT | NEURON_DRIVER_FEATURE_BIG_CORE_MAPS;

			return copy_to_user(param, &driver_info, sizeof(driver_info));
		}
	}


	return -EINVAL;
}

static long ncdev_device_app_pid_deprecated(struct neuron_device *nd, void *param)
{
	return copy_to_user(param, &nd->attached_processes[0].pid, sizeof(int));
}

static long ncdev_device_get_all_apps_info(struct neuron_device *nd, void *param)
{
	int ret;
	int added_items_count;
	int proc_index;
	int nc_index;
	__u8 nc_lock_map;
	struct neuron_attached_process *proc_entry;
	struct neuron_ioctl_get_apps_info *arg;
	struct neuron_ioctl_get_apps_info *user_arg = (struct neuron_ioctl_get_apps_info *) param;

	arg = kmalloc(sizeof(struct neuron_ioctl_get_apps_info) +
		      sizeof(struct neuron_app_info) * NEURON_MAX_PROCESS_PER_DEVICE, GFP_KERNEL);
	if (arg == NULL)
		return -ENOMEM;

	ret = neuron_copy_from_user(__func__, arg, user_arg, sizeof(struct neuron_ioctl_get_apps_info));
	if (ret) {
		kfree(arg);
		return ret;
	}

	added_items_count = 0;
	proc_index = 0;

	while(added_items_count < arg->capacity && proc_index < NEURON_MAX_PROCESS_PER_DEVICE) {
		proc_entry = &nd->attached_processes[proc_index++];
		if (proc_entry->pid == 0)
			continue;

		nc_lock_map = 0;
		if (arg->apps_info_flags & APP_INFO_PID_NC_LOCK_INFO)
			for (nc_index = 0; nc_index < MAX_NC_PER_DEVICE; nc_index++) {
				mutex_lock(&nd->crwl[nc_index].lock);
				if(nd->crwl[nc_index].writer_pid == proc_entry->pid && (
				   nd->crwl[nc_index].writer_acquired ||
				   nd->crwl[nc_index].reader_count > 0)) {
					nc_lock_map |= (__u8)(1 << nc_index);
					memcpy(&arg->app_data[added_items_count].uuid_data[nc_index], &nd->crwl[nc_index].uuid,
					       sizeof(nd->crwl[nc_index].uuid));
				}
				mutex_unlock(&nd->crwl[nc_index].lock);
			}

		arg->app_data[added_items_count].pid = proc_entry->pid;
		arg->app_data[added_items_count].nc_lock_map = nc_lock_map;

		arg->app_data[added_items_count].host_mem_size = 0;
		arg->app_data[added_items_count].device_mem_size = 0;
		if (arg->apps_info_flags & APP_INFO_PID_MEM_USAGE) {
			arg->app_data[added_items_count].host_mem_size = proc_entry->memory_used[MEM_LOC_HOST - 1];
			arg->app_data[added_items_count].device_mem_size = proc_entry->memory_used[MEM_LOC_DEVICE - 1];
		}

		added_items_count++;
	}
	arg->size = added_items_count;
	ret = copy_to_user(user_arg, arg, sizeof(struct neuron_ioctl_get_apps_info) +
			   sizeof(struct neuron_app_info) * arg->size);
	kfree(arg);

	return ret;
}

static long ncdev_nc_nq_init_deprecated(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_notifications_init_v1 arg;
	struct mem_chunk *mc;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	ret = nnq_init(nd, arg.nc_id, arg.engine_index, arg.nq_type, arg.size, true, 0, 0,
		       false, &mc, &arg.mmap_offset);
	if (ret)
		return ret;

	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_nc_nq_init_libnrt(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_notifications_init_v2 arg;
	int ret;
	struct mem_chunk *mc;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	if (arg.nq_dev_type == NQ_DEVICE_TYPE_NEURON_CORE) {
		ret = nnq_init(nd, arg.nq_dev_id, arg.engine_index, arg.nq_type, arg.size,
			       arg.on_host_memory, arg.dram_channel, arg.dram_region,
			       false, &mc, &arg.mmap_offset);
	} else if (arg.nq_dev_type == NQ_DEVICE_TYPE_TOPSP) {
		ret = ndhal->ndhal_topsp.ts_nq_init(nd, arg.nq_dev_id, arg.engine_index, arg.nq_type, arg.size,
				 arg.on_host_memory, arg.dram_channel, arg.dram_region,
				 false, &mc, &arg.mmap_offset);
	} else {
		return -ENOSYS;
	}
	if (ret)
		return ret;

	arg.mem_handle = ncdev_mem_chunk_to_mem_handle(mc);
	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_nc_nq_init_with_realloc_libnrt(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_notifications_init_with_realloc_v2 arg;
	int ret;
	struct mem_chunk *mc;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	if (arg.nq_dev_type == NQ_DEVICE_TYPE_NEURON_CORE) {
		ret = nnq_init(nd, arg.nq_dev_id, arg.engine_index, arg.nq_type, arg.size,
			       arg.on_host_memory, arg.dram_channel, arg.dram_region,
			       arg.force_alloc_mem, &mc, &arg.mmap_offset);
	} else if (arg.nq_dev_type == NQ_DEVICE_TYPE_TOPSP) {
		ret = ndhal->ndhal_topsp.ts_nq_init(nd, arg.nq_dev_id, arg.engine_index, arg.nq_type, arg.size,
				 arg.on_host_memory, arg.dram_channel, arg.dram_region,
				 arg.force_alloc_mem, &mc, &arg.mmap_offset);
	} else {
		return -ENOSYS;
	}
	if (ret)
		return ret;

	arg.mem_handle = ncdev_mem_chunk_to_mem_handle(mc);
	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_acquire_neuron_ds(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_neuron_ds_info arg;
	struct mem_chunk *mc;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	// If PID is 0, then a new datastore is acquired for the caller (an inference app), otherwise
	// the caller tries to acquire an existing datastore (in case of a monitoring app)
	ret = neuron_ds_acquire_pid(&nd->datastore, arg.pid, &mc);
	if (ret)
		return ret;

	arg.mmap_offset = nmmap_offset(mc);
	arg.size = mc->size;

	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_release_neuron_ds(struct neuron_device *nd, void *param)
{
	int ret;
	struct neuron_ioctl_neuron_ds_info arg;
	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;
	neuron_ds_release_pid(&nd->datastore, arg.pid);
	return 0;
}

static long ncdev_crwl_reader_enter(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_crwl arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	return ncrwl_reader_enter(nd, arg.nc_id, arg.uuid);
}

static long ncdev_crwl_reader_exit(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_crwl arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	return ncrwl_reader_exit(nd, arg.nc_id, arg.uuid);
}

static long ncdev_crwl_writer_enter(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_crwl arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	return ncrwl_writer_enter(nd, arg.nc_id, arg.uuid);
}

static long ncdev_crwl_writer_downgrade(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_crwl arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	return ncrwl_writer_downgrade(nd, arg.nc_id, arg.uuid);
}

/**
 * ncdev_crwl_nc_range_mark()
 *
 *   attempt to acquire a number of consecutive cores
 *
 */
static long ncdev_crwl_nc_range_mark(struct file *filep, unsigned int cmd, void *param)
{
	struct ncdev *ncd;
	struct neuron_device *nd;
	struct neuron_ioctl_crwl_nc_map_ext arg = {0};
	unsigned int size = _IOC_SIZE(cmd);
	int ret;
	u32 offset = 0;

	ncd = filep->private_data;
	if (ncd == NULL) {
		return -EINVAL;
	}
	nd = ncd->ndev;
	if (nd == NULL) {
		return -EINVAL;
	}

	// verify the bitmap is large enough to hold all cores (at compile time)
	static_assert(sizeof(arg.bitmap) >= ((MAX_NEURON_DEVICE_COUNT * MAX_NC_PER_DEVICE)/8), "mismatch between neuron_ioctl_crwl_nc_map and max core count");

	// vet/correct arg size.
	//
	if (size == sizeof(struct neuron_ioctl_crwl_nc_map *)) {
		size = sizeof(struct neuron_ioctl_crwl_nc_map);
	} else if (size != sizeof(struct neuron_ioctl_crwl_nc_map_ext)) {
		return -EINVAL;
	}

	ret = neuron_copy_from_user(__func__, &arg, param, size);
	if (ret)
		return ret;

	// clear bitmap before call
	memset(arg.bitmap, 0, sizeof(arg.bitmap));

	offset = ndhal->ndhal_address_map.nc_per_device * nd->device_index;
	ret = ncrwl_nc_range_mark(arg.nc_count, arg.start_nc_index + offset, arg.end_nc_index + offset,
				  &arg.max_nc_available, arg.bitmap);

	bitmap_shift_right(arg.bitmap, arg.bitmap, offset, sizeof(arg.bitmap)*8);
	if (ret) {
		int unused = copy_to_user(param, &arg, size);
		unused = unused;
		// note that ret indicates whether we marked the range
		// successfully
		return ret;
	}

	return copy_to_user(param, &arg, size);
}

/**
 * ncdev_crwl_nc_range_unmark()
 *
 *    free a set of cores acquired by this process
 */
static long ncdev_crwl_nc_range_unmark(struct file *filep, unsigned int cmd, void *param)
{
	struct ncdev *ncd;
	struct neuron_device *nd;
	struct neuron_ioctl_crwl_nc_map_ext arg = {0};
	unsigned int size = _IOC_SIZE(cmd);
	int ret;
	u32 offset = 0;

	ncd = filep->private_data;
	if (ncd == NULL) {
		return -EINVAL;
	}
	nd = ncd->ndev;
	if (nd == NULL) {
		return -EINVAL;
	}

	// vet/correct arg size.
	//
	if (size == sizeof(struct neuron_ioctl_crwl_nc_map *)) {
		size = sizeof(struct neuron_ioctl_crwl_nc_map);
	} else if (size != sizeof(struct neuron_ioctl_crwl_nc_map_ext)) {
		return -EINVAL;
	}

	ret = neuron_copy_from_user(__func__, &arg, param, size);
	if (ret)
		return ret;

	offset = ndhal->ndhal_address_map.nc_per_device * nd->device_index;
	bitmap_shift_left(arg.bitmap, arg.bitmap, offset, sizeof(arg.bitmap)*8);
	ncrwl_nc_range_unmark(arg.bitmap);
	return 0;
}

static long ncdev_cinit_set_state(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_cinit_set arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	nci_set_state(nd, arg.nc_id, arg.state, &arg.new_state);
	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_nc_model_started_count(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_nc_model_started_count arg;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	arg.started_count = nd->nc_model_started_count[arg.nc_id];
	return copy_to_user(param, &arg, sizeof(arg));
}

/**
 * ncdev_resource_mmap_info()
 *
 *   retrieve mmap offset and size for a resource.   Resources are
 *   specified by an abstraction of a block type, index (id) and
 *   resource type that the caller wants to map.
 *
 */
static long ncdev_resource_mmap_info(struct neuron_device *nd, void *param)
{
	struct neuron_ioctl_resource_mmap_info  arg;
	u64 offset;
	u64 size;
	int ret;

	ret = neuron_copy_from_user(__func__, &arg, param, sizeof(arg));
	if (ret)
		return ret;

	ret = nmap_dm_special_resource_get( arg.block, arg.block_id,  arg.resource, &offset, &size);

	if (ret)
		return ret;

	arg.offset = offset;
	arg.size = size;

	return copy_to_user(param, &arg, sizeof(arg));
}

static long ncdev_compatible_version(void *param)
{
	struct neuron_ioctl_compatible_version arg;
	ndhal->ndhal_cdev.ncdev_compatible_version(&arg);
	return copy_to_user(param, &arg, sizeof(arg));
}

/** Writes an error message to kernel logs/serial console
 * On success: 0
 * On userspace buffer size being too big: EFAULT
 * On string not null terminated: EBADMSG 
 */
static long ncdev_printk(void *param) {
	struct neuron_ioctl_printk arg;
	int ret;
	char str[512];

	ret = neuron_copy_from_user(__func__, &arg, (struct neuron_ioctl_printk *)param, sizeof(arg));
	if (ret)
		return ret;

	if(arg.size > sizeof(str))
		return -EFAULT;

	ret = neuron_copy_from_user(__func__, str, arg.buffer, arg.size);
	if (ret)
		return ret;

	if(str[arg.size - 1] != 0)
		return -EBADMSG;

	printk(KERN_ERR "%s\n", str);
	return 0;
}

inline static long ncdev_misc_ioctl(struct file *filep, unsigned int cmd, unsigned long param) {
	if ((cmd == NEURON_IOCTL_CRWL_NC_RANGE_MARK) || (cmd == NEURON_IOCTL_CRWL_NC_RANGE_MARK_EXT0)) {
		return ncdev_crwl_nc_range_mark(filep, cmd, (void *)param);
	} else if ((cmd == NEURON_IOCTL_CRWL_NC_RANGE_UNMARK) || (cmd == NEURON_IOCTL_CRWL_NC_RANGE_UNMARK_EXT0)) {
		return ncdev_crwl_nc_range_unmark(filep, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_COMPATIBLE_VERSION) {
		return ncdev_compatible_version((void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_BASIC_INFO) {
		return ncdev_device_basic_info((void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_BDF_EXT) {
		return ncdev_device_bdf_ext(filep, (void*)param);
	} else if (cmd == NEURON_IOCTL_DMABUF_FD) {
		/* Add dmabuf support under misc ioctl to avoid iterating
		 * over all devices in the user space
		 */
		return ncdev_get_dmabuf_fd((void *)param);
	} else if (_IOC_NR(cmd) == _IOC_NR(NEURON_IOCTL_DRIVER_INFO_GET)) {
		return ncdev_driver_info(cmd, (void*)param);
	} else if (cmd == NEURON_IOCTL_PRINTK) {
		return ncdev_printk((void*)param);
	}

	pr_err("invalid misc IOCTL %d\n", cmd);
	return -EINVAL;
}

long ncdev_ioctl(struct file *filep, unsigned int cmd, unsigned long param)
{
	struct ncdev *ncd;
	struct neuron_device *nd;

	if (IS_NEURON_DEVICE_FREE_ACCESS(filep))
		return ncdev_misc_ioctl(filep, cmd, param);

	ncd = filep->private_data;
	if (ncd == NULL) {
		return -EINVAL;
	}
	nd = ncd->ndev;
	if (nd == NULL) {
		return -EINVAL;
	}
	// the following IOCTL allowed only for the process which did DEVICE_INIT
	if (cmd == NEURON_IOCTL_DMA_ENG_INIT || cmd == NEURON_IOCTL_DMA_ENG_SET_STATE ||
	    cmd == NEURON_IOCTL_DMA_QUEUE_INIT || cmd == NEURON_IOCTL_DMA_ACK_COMPLETED ||
	    cmd == NEURON_IOCTL_DMA_QUEUE_RELEASE || cmd == NEURON_IOCTL_DMA_COPY_DESCRIPTORS ||
	    cmd == NEURON_IOCTL_MEM_ALLOC || cmd == NEURON_IOCTL_MEM_FREE ||
	    cmd == NEURON_IOCTL_MEM_COPY || cmd == NEURON_IOCTL_MEM_GET_PA ||
	    cmd == NEURON_IOCTL_MEM_COPY_ASYNC || cmd == NEURON_IOCTL_MEM_COPY_ASYNC_WAIT ||
	    cmd == NEURON_IOCTL_MEM_GET_INFO || cmd == NEURON_IOCTL_BAR_WRITE ||
	    cmd == NEURON_IOCTL_POST_METRIC || cmd == NEURON_IOCTL_NOTIFICATIONS_INIT_V1 ||
	    cmd == NEURON_IOCTL_NOTIFICATIONS_INIT_V2 || cmd == NEURON_IOCTL_DRIVER_INFO_SET ||
	    cmd == NEURON_IOCTL_NOTIFICATIONS_INIT_WITH_REALLOC_V2) {
		if (!npid_is_attached(nd)) {
			pr_err("Process not allowed to request cmd=%u, pid not attached\n", cmd);
			npid_print_usage(nd);
			return -EACCES;
		}
	}

	if (cmd == NEURON_IOCTL_DEVICE_RESET) {
		return ncdev_device_reset_deprecated(nd);
	} else if (cmd == NEURON_IOCTL_DEVICE_READY) {
		// WARNING - there is a bug in older driver versions where
		// NEURON_IOCTL_DEVICE_READY and NEURON_IOCTL_DEVICE_RESET_STATUS
		// are assigned to the same ioctl 2. device_ready is an important
		// api to call to wait for the device reset completion, while
		// reset_status api is a no-op. Make sure this ioctl is checked
		// before RESET_STATUS so we enter the right function.
		return ncdev_device_ready_deprecated(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NC_RESET_READY) {
		return ncdev_nc_reset_ready(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_RESET_STATUS) {
		return ncdev_device_reset_status_deprecated(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NC_RESET) {
		return ncdev_nc_reset(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_CINIT_SET_STATE) {
		return ncdev_cinit_set_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NC_MODEL_STARTED_COUNT) {
		return ncdev_nc_model_started_count(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_INFO) {
		return ncdev_device_info(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_INIT) {
		return 0;
	} else if (cmd == NEURON_IOCTL_DEVICE_RELEASE) {
		return 0;
	} else if (cmd == NEURON_IOCTL_DEVICE_APP_PID) {
		return ncdev_device_app_pid_deprecated(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_GET_ALL_APPS_INFO) {
		return ncdev_device_get_all_apps_info(nd, (void*)param);
	} else if (cmd == NEURON_IOCTL_DMA_ENG_INIT) {
		return 0;
	} else if (cmd == NEURON_IOCTL_DMA_ENG_SET_STATE) {
		return ncdev_dma_engine_set_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_INIT) {
		return ncdev_dma_queue_init(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_INIT_BATCH) {
		return ncdev_dma_queue_init_batch(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_COPY_START) {
		return ncdev_dma_copy_start(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_ACK_COMPLETED) {
		return ncdev_dma_ack_completed(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_RELEASE) {
		return ncdev_dma_queue_release(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_COPY_DESCRIPTORS) {
		return ncdev_dma_copy_descriptors(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_ENG_GET_STATE) {
		return ncdev_dma_engine_get_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUEUE_GET_STATE) {
		return ncdev_dma_queue_get_state(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_DESCRIPTOR_COPYOUT) {
		return ncdev_dma_descriptor_copyout(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DMA_QUIESCE_QUEUES) {
		return ncdev_dma_quiesce_queues(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_ALLOC) {
		return ncdev_mem_alloc(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_ALLOC_V2) {
		return ncdev_mem_alloc_libnrt(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_GET_EXTENDED_INFO) {
		return ncdev_mem_get_extended_info((void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_GET_INFO) {
		return ncdev_mem_get_info_deprecated((void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_GET_PA) {
		return ncdev_mem_get_pa_deprecated(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_FREE) {
		return ncdev_mem_free(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_COPY) {
		return ncdev_mem_copy(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_COPY_ASYNC) {
		return ncdev_mem_copy_async(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_COPY_ASYNC_WAIT) {
		return ncdev_mem_copy_async_wait(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEM_BUF_COPY) {
		return ncdev_mem_buf_copy(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_PROGRAM_ENGINE) {
		return ncdev_program_engine(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_PROGRAM_ENGINE_NC) {
		return ncdev_program_engine_nc(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_MEMSET) {
		return ncdev_memset(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_READ) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_WRITE) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_INCREMENT) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_SEMAPHORE_DECREMENT) {
		return ncdev_semaphore_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_EVENT_GET) {
		return ncdev_events_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_EVENT_SET) {
		return ncdev_events_ioctl(nd, cmd, (void *)param);
	} else if (cmd == NEURON_IOCTL_BAR_READ) {
		return ncdev_bar_rw(nd, (void *)param, true);
	} else if (cmd == NEURON_IOCTL_BAR_WRITE) {
		return ncdev_bar_rw(nd, (void *)param, false);
	} else if (cmd == NEURON_IOCTL_POST_METRIC) {
		return ncdev_post_metric(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_INIT_V1) {
		return ncdev_nc_nq_init_deprecated(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_INIT_V2) {
		return ncdev_nc_nq_init_libnrt(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_INIT_WITH_REALLOC_V2) {
		return ncdev_nc_nq_init_with_realloc_libnrt(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_DESTROY_V1) {
		return 0;
	} else if (cmd == NEURON_IOCTL_NOTIFICATIONS_QUEUE_INFO) {
		return -1;
	} else if (cmd == NEURON_IOCTL_READ_HW_COUNTERS) {
		return ncdev_read_hw_counters(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_ACQUIRE_NEURON_DS) {
		return ncdev_acquire_neuron_ds(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_RELEASE_NEURON_DS) {
		return ncdev_release_neuron_ds(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_CRWL_READER_ENTER) {
		return ncdev_crwl_reader_enter(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_CRWL_READER_EXIT) {
		return ncdev_crwl_reader_exit(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_CRWL_WRITER_ENTER) {
		return ncdev_crwl_writer_enter(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_CRWL_WRITER_DOWNGRADE) {
		return ncdev_crwl_writer_downgrade(nd, (void *)param);
	} else if (cmd == NEURON_IOCTL_DEVICE_BDF) {
		return ncdev_device_bdf(nd, (void*)param);
	} else if (_IOC_NR(cmd) == _IOC_NR(NEURON_IOCTL_MEM_MC_GET_INFO)) {
		return ncdev_mem_get_mc_mmap_info(nd, cmd, (void*)param);
	} else if (cmd == NEURON_IOCTL_RESOURCE_MMAP_INFO) {
		return ncdev_resource_mmap_info(nd, (void *)param);
	}
	// B/W compatibility
	return ncdev_misc_ioctl(filep, cmd, param);
}

static int ncdev_open(struct inode *inode, struct file *filep)
{
	struct ncdev *dev;
	struct neuron_device *nd;

	dev = &devnodes[iminor(inode)];
	nd = dev->ndev;

	if (IS_NEURON_DEVICE_FREE_ACCESS(filep)) {
		filep->private_data = dev;
		return 0;
	}

	mutex_lock(&dev->ncdev_lock);
	dev->open_count++;
	mutex_unlock(&dev->ncdev_lock);

	// wait for device init to complete.
	// TODO: implement some better wait system than schedule()
	while (nd->device_state == NEURON_DEVICE_STATE_RESET) {
		schedule();
	}
	if (nd->device_state == NEURON_DEVICE_STATE_INVALID) {
		mutex_lock(&dev->ncdev_lock);
		dev->open_count--;
		mutex_unlock(&dev->ncdev_lock);
		pr_err("nd%d is in an invalid state", nd->device_index);
		return -EINVAL;
	}

	mutex_lock(&dev->ncdev_lock);
	if (!npid_attach(nd)) {
		dev->open_count--;
		pr_err("nd%d: pid %d failed to open\n", nd->device_index, task_tgid_nr(current));
		npid_print_usage(nd);
		mutex_unlock(&dev->ncdev_lock);
		return -EBUSY;
	}
	mutex_unlock(&dev->ncdev_lock);
	filep->private_data = dev;
	return 0;
}

static inline int ncdev_misc_flush(struct file *filep)
{
	struct neuron_ioctl_crwl_nc_map_ext nc_map;

	// Clear all NCs used by the closing process
	memset( (void *)nc_map.bitmap, 0xff, sizeof(nc_map.bitmap));
	ncrwl_nc_range_unmark(nc_map.bitmap);
	return 0;
}

static int ncdev_flush(struct file *filep, fl_owner_t id)
{
	struct ncdev *dev;
	struct neuron_device *nd;

	if (IS_NEURON_DEVICE_FREE_ACCESS(filep))
		return ncdev_misc_flush(filep);

	dev = (struct ncdev *)filep->private_data;
	nd = dev->ndev;

	mutex_lock(&dev->ncdev_lock);

	// if the current process is going away then cleanup per process state
	if (npid_is_attached(nd) == 1) {
		// If this proc exited in the middle of a reset, wait for the reset to be processed.
		nr_wait(nd, task_tgid_nr(current), true);

		ndhal->ndhal_cdev.ncdev_quiesce_exec_on_proc_exit();

		ndmar_handle_process_exit(nd, task_tgid_nr(current));
		msleep(10); // TODO - confirm with HW dev, whether any delay needed after q reset.
		ncrwl_release_current_process(nd);
		neuron_ds_release_pid(&nd->datastore, task_tgid_nr(current));
		mpset_free_expired_mc(&nd->mpset, MC_LIFESPAN_CUR_PROCESS);
		nmmap_delete_all_nodes(nd);
	}
	npid_detach(nd);

	mutex_unlock(&dev->ncdev_lock);

	return 0;
}

static int ncdev_release(struct inode *inode, struct file *filep)
{
	struct ncdev *dev;
	struct neuron_device *nd;

	if (IS_NEURON_DEVICE_FREE_ACCESS(filep))
		return 0;

	dev = (struct ncdev *)filep->private_data;
	nd = dev->ndev;

	mutex_lock(&dev->ncdev_lock);
	dev->open_count--;
	if (dev->open_count == 0) {
		neuron_ds_clear(&nd->datastore);
		mpset_free_expired_mc(&nd->mpset, MC_LIFESPAN_ALL_PROCESS);
		nmmap_delete_all_nodes(nd);
	}
	mutex_unlock(&dev->ncdev_lock);

	return 0;
}

static int ncdev_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct ncdev *ncd;
	struct neuron_device *nd;

	ncd = filep->private_data;
	if (ncd == NULL)
		return -EINVAL;

	nd = ncd->ndev;
	if (nd == NULL)
		return -EINVAL;

	return nmmap_mem(nd, vma);
}

static struct file_operations ncdev_fops = {
	.owner = THIS_MODULE,
	.open = ncdev_open,
	.flush = ncdev_flush,
	.release = ncdev_release,
	.unlocked_ioctl = ncdev_ioctl,
	.mmap = ncdev_mmap,
};

static ssize_t device_reset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int minor = MINOR(dev->devt);
	return sprintf(buf, "%d\n", devnodes[minor].ndev->device_state);
}

static ssize_t driver_reset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int minor = MINOR(dev->devt);
	struct ncdev *devnode = &devnodes[minor];

	mutex_lock(&devnode->ncdev_lock);
	if (devnode->open_count == 0) { // only trigger sysfs reset if the device is not opened by app
		nr_start_ncs(devnode->ndev, NEURON_NC_MAP_DEVICE, NEURON_RESET_REQUEST_ALL);
	}
	mutex_unlock(&devnode->ncdev_lock);

	return count;
}

static DEVICE_ATTR(reset, S_IWUSR | S_IRUSR, device_reset_show, driver_reset_store);

static ssize_t neuron_core_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret = 0;
	ret = sprintf(buf, "%d", ndhal->ndhal_address_map.nc_per_device);
	return ret;
}

static DEVICE_ATTR(core_count, S_IRUSR, neuron_core_count_show, NULL);

#define CONNECTED_DEVICES_MAX_LEN 20
static ssize_t neuron_connected_devices_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i = 0;
	int offset = 0;
	int minor = MINOR(dev->devt); // neuron device id
	struct ncdev *devnode = &devnodes[minor];
	struct neuron_device *nd = devnode->ndev;
	u32 connected_devices[MAX_NEURON_DEVICE_COUNT];
	int connected_device_count = 0;
	int ret = ndhal->ndhal_fw_io.fw_io_topology(nd->fw_io_ctx, nd->pdev->device, minor, connected_devices, &connected_device_count);
	if (ret)
		return ret;

	for (i = 0; i < connected_device_count - 1; i++) {
		offset += snprintf(buf + offset, CONNECTED_DEVICES_MAX_LEN - offset, "%d, ", connected_devices[i]);
		if (offset >= CONNECTED_DEVICES_MAX_LEN) {
			pr_err("snprintf failed when showing connected devices\n");
			return 0;
		}
	}
	if (connected_device_count == 0)
		offset += snprintf(buf, CONNECTED_DEVICES_MAX_LEN, "\n");
	else
		offset += snprintf(buf + offset, CONNECTED_DEVICES_MAX_LEN - offset, "%d\n", connected_devices[connected_device_count - 1]);
	if (offset >= CONNECTED_DEVICES_MAX_LEN) {
		pr_err("snprintf failed when showing connected devices\n");
		return 0;
	}

	return offset;
}

static DEVICE_ATTR(connected_devices, S_IRUSR, neuron_connected_devices_show, NULL);

static struct attribute *attrs[] = {
	&dev_attr_reset.attr,
	&dev_attr_core_count.attr,
	&dev_attr_connected_devices.attr,
   	NULL,
};

static struct attribute_group attr_group = {
   	.attrs = attrs,
};

static inline int ncdev_init_device_node(struct ncdev *devnode, const char *dev_name, int minor,
				  struct file_operations *fops, struct neuron_device *ndev)
{
	int ret;
	dev_t devno;
	struct device *device = NULL;
	struct cdev *cdev = &devnode->cdev;

	devno = MKDEV(major, minor);
	cdev_init(cdev, fops);
	cdev->owner = THIS_MODULE;

	/* register cdev */
	ret = cdev_add(cdev, devno, 1);
	if (ret < 0) {
		pr_err("failed to register character device %s\n", dev_name);
		return ret;
	}

	device = device_create(neuron_dev_class, NULL, /* no parent device */
			       devno, NULL, /* no additional data */
			       "%s", dev_name);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		pr_err("error %d while trying to create %s\n", ret, dev_name);
		device_destroy(neuron_dev_class, devno);
		cdev_del(cdev);
		return ret;
	}
	devnode->device = device;

	ret = sysfs_create_group(&(device->kobj), &attr_group);
	if (ret) {
		pr_err("failed to create an attribute group for %s\n", dev_name);
		sysfs_remove_group(&(device->kobj), &attr_group);
		device_destroy(neuron_dev_class, devno);
		cdev_del(cdev);
		return ret;
	}

	ret = nsysfsmetric_register(ndev, &devnode->device->kobj);
	if (ret) {
		pr_err("failed to register sysfs metric for %s\n", dev_name);
		device_destroy(neuron_dev_class, devno);
		cdev_del(cdev);
		return -1;
	}

	devnode->minor = minor;
	devnode->ndev = ndev;

	return 0;
}

#define NEURON_MAX_DEV_NAME 32
int ncdev_create_device_node(struct neuron_device *ndev)
{
	int ret, minor = ndev->device_index;
	char dev_name[NEURON_MAX_DEV_NAME];
	snprintf(dev_name, sizeof(dev_name), "neuron%d", minor);

	ret = ncdev_init_device_node(&devnodes[minor], dev_name, minor, &ncdev_fops, ndev);
	if (ret)
		return ret;

	ndev->ncdev = &devnodes[minor];
	return 0;
}

static int ncdev_remove_device_node(struct ncdev *devnode)
{
	int minor;
	dev_t devno;

	sysfs_remove_group(&(devnode->device->kobj), &attr_group);
	nsysfsmetric_destroy(devnode->ndev);

	minor = devnode->minor;
	devno = MKDEV(major, minor);
	device_destroy(neuron_dev_class, devno);
	cdev_del(&devnode->cdev);
	memset(devnode, 0, sizeof(struct ncdev));

	return 0;
}

int ncdev_delete_device_node(struct neuron_device *ndev)
{
	return ncdev_remove_device_node(&devnodes[ndev->device_index]);
}

static void ncdev_cleanup(void)
{
	int i;
	for (i = 0; i < MAX_NEURON_DEVICE_COUNT; i++) {
		if (devnodes[i].ndev != NULL)
			pr_err("Error! ncdev is not NULL");
	}

	if (neuron_dev_class) {
		class_destroy(neuron_dev_class);
	}

	unregister_chrdev_region(MKDEV(major, 0), NEURON_MAX_DEV_NODES);
}

int ncdev_module_init(void)
{
	int i, ret;

	memset(devnodes, 0, sizeof(devnodes));
	for (i = 0; i < NEURON_MAX_DEV_NODES; i++)
		mutex_init(&devnodes[i].ncdev_lock);

	ret = alloc_chrdev_region(&neuron_dev, 0, NEURON_MAX_DEV_NODES, "neuron");
	if (ret < 0) {
		pr_err("can't get major\n");
		return ret;
	}

	major = MAJOR(neuron_dev);

	neuron_dev_class = class_create(THIS_MODULE, "neuron_device");
	if (IS_ERR(neuron_dev_class)) {
		ret = PTR_ERR(neuron_dev_class);
		goto fail;
	}

	return ret;

fail:
	ncdev_cleanup();
	return ret;
}

void ncdev_module_exit(void)
{
	ncdev_cleanup();
}
