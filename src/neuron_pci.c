// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Manages Neuron device's PCI configuration such as BAR access and MSI interrupt setup. */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/atomic.h>
#include <linux/version.h>

#include "neuron_device.h"
#include "neuron_ds.h"
#include "neuron_reg_access.h"
#include "neuron_metrics.h"
#include "v1/fw_io.h"
#include "v1/address_map.h"
#include "v2/address_map.h"

#include "neuron_dma.h"

/* Vendor / Device ID for all devices supported by the driver */
#define INF_VENDOR_ID 0x1D0F
#define INF_DEVICE_ID0 0x7064
#define INF_DEVICE_ID1 0x7065
#define INF_DEVICE_ID2 0x7066
#define INF_DEVICE_ID3 0x7067
#define TRN_DEVICE_ID0 0x7164
static struct pci_device_id neuron_pci_dev_ids[] = {
	{ PCI_DEVICE(INF_VENDOR_ID, INF_DEVICE_ID0) },
	{ PCI_DEVICE(INF_VENDOR_ID, INF_DEVICE_ID1) },
	{ PCI_DEVICE(INF_VENDOR_ID, INF_DEVICE_ID2) },
	{ PCI_DEVICE(INF_VENDOR_ID, INF_DEVICE_ID3) },
	{ PCI_DEVICE(INF_VENDOR_ID, TRN_DEVICE_ID0) },
	{
		0,
	},
};

/* Inferentia BAR mapping */
#define INF_APB_BAR 0
#define INF_AXI_BAR 2

/* Training BAR mapping */
#define TRN_APB_BAR_QEMU 2
#define TRN_APB_BAR_EMU 0
#define TRN_APB_BAR 0
#define TRN_BAR4 4

// some old kernels do not have pci_info defined.
#ifndef pci_info
#define pci_info(pdev, fmt, arg...) dev_info(&(pdev)->dev, fmt, ##arg)
#endif

// number of devices managed
static atomic_t device_count = ATOMIC_INIT(0);

extern int ncdev_create_device_node(struct neuron_device *ndev);
extern int ncdev_create_misc_node(void);
extern int ncdev_delete_device_node(struct neuron_device *ndev);
extern int ncdev_delete_misc_node(void);
extern void ndmar_preinit(struct neuron_device *nd);

static struct neuron_device *neuron_devices[MAX_NEURON_DEVICE_COUNT] = { 0 };

struct neuron_device *neuron_pci_get_device(u8 device_index)
{
	BUG_ON(device_index >= MAX_NEURON_DEVICE_COUNT);
	return neuron_devices[device_index];
}

static int neuron_pci_device_init(struct neuron_device *nd)
{
	int i, ret;

	if (nd == NULL)
		return -1;

	// neuron devices are 64bit, so set dma mask to 64bit so that kernel can allocate memory from Normal zone
	ret = dma_set_mask(&nd->pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask(&nd->pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&nd->pdev->dev, "No usable DMA configuration.\n");
			return ret;
		} else {
			dma_set_coherent_mask(&nd->pdev->dev, DMA_BIT_MASK(32));
			dev_err(&nd->pdev->dev, "Using 32bit DMA configuration.\n");
		}
	} else {
		dma_set_coherent_mask(&nd->pdev->dev, DMA_BIT_MASK(64));
	}

	if (narch_get_arch() == NEURON_ARCH_TRN) {
		nd->fw_io_ctx = NULL;
	} else {
		nd->fw_io_ctx = fw_io_setup(nd->device_index, nd->npdev.bar0, nd->npdev.bar0_size,
					    nd->npdev.bar2, nd->npdev.bar2_size);
		if (nd->fw_io_ctx == NULL)
			return -1;
	}

	ret = nr_create_thread(nd);
	if (ret)
		return ret;
	nr_start(nd);

	// Set the core init state to invalid
	nci_reset_state(nd);

	ndmar_preinit(nd);

	// Initialize the device mpset
	memset(&nd->mpset, 0, sizeof(struct mempool_set));

	// Initialize the host portion in mpset
	ret = mpset_constructor(&nd->mpset, &(nd->pdev->dev), nd);
	if (ret)
		goto fail_mpset;

	// Initialize CRWL struct
	for (i = 0; i < MAX_NC_PER_DEVICE; i++)
		mutex_init(&nd->crwl[i].lock);

	ret = ncdev_create_device_node(nd);
	if (ret) {
		pci_info(nd->pdev, "create device node failed\n");
		goto fail_chardev;
	}

	return 0;

fail_chardev:
	mpset_destructor(&nd->mpset);
fail_mpset:
	if (nd->fw_io_ctx)
		fw_io_destroy((struct fw_io_ctx *)nd->fw_io_ctx);

	nd->fw_io_ctx = NULL;
	return ret;
}

int neuron_pci_device_close(struct neuron_device *nd)
{
	int ret;

	ret = ncdev_delete_device_node(nd);
	if (ret) {
		pci_info(nd->pdev, "delete device node failed\n");
		return ret;
	}
	// disable NQ after disabling PCI device so that the device cant DMA anything after this
	nnq_destroy_all(nd);
	ndmar_close(nd);
	neuron_ds_destroy(&nd->datastore);
	mpset_destructor(&nd->mpset);

	if (nd->fw_io_ctx)
		fw_io_destroy((struct fw_io_ctx *)nd->fw_io_ctx);

	nd->fw_io_ctx = NULL;
	return 0;
}

static int neuron_pci_get_apb_bar(struct neuron_device *nd)
{
	int apb_bar;
	if (narch_get_arch() == NEURON_ARCH_TRN) {
		if (narch_is_emu())
			apb_bar = TRN_APB_BAR_EMU;
		else if (narch_is_qemu())
			apb_bar = TRN_APB_BAR_QEMU;
		else
			apb_bar = TRN_APB_BAR;
	} else {
		apb_bar = INF_APB_BAR; //default
	}

	return apb_bar;
}

static void neuron_pci_set_device_architecture(struct neuron_device *nd)
{
	unsigned short device = nd->pdev->device;
	u8 revision;
	pci_read_config_byte(nd->pdev, PCI_REVISION_ID, &revision);
	narch_init(device == TRN_DEVICE_ID0 ? NEURON_ARCH_TRN : NEURON_ARCH_INFERENTIA, revision);
}

static int neuron_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int ret = 0;
	struct neuron_device *nd;
	int apb_bar;

	nd = kzalloc(sizeof(struct neuron_device), GFP_KERNEL);
	if (nd == NULL) {
		pci_info(dev, "Can't allocate memory\n");
		goto fail_alloc_mem;
	}

	nd->pdev = dev;
	pci_set_drvdata(dev, nd);

	ret = pci_enable_device(dev);
	if (ret) {
		pci_info(dev, "Can't enable the device\n");
		goto fail_enable;
	}

	pci_set_master(dev);

	// set the architecture
	neuron_pci_set_device_architecture(nd);

	// set the bars
	apb_bar = neuron_pci_get_apb_bar(nd);

	ret = pci_request_region(dev, apb_bar, "APB");
	if (ret) {
		pci_info(dev, "Can't map BAR%d\n", apb_bar);
		goto fail_bar0_map;
	}

	if (pci_resource_len(dev, apb_bar) == 0) {
		ret = -ENODEV;
		pci_info(dev, "BAR%d len is 0\n", apb_bar);
		goto fail_bar0_resource;
	}

	nd->npdev.bar0_pa = pci_resource_start(dev, apb_bar);
	if (!nd->npdev.bar0_pa) {
		ret = -ENODEV;
		pci_info(dev, "Can't get start address of BAR0\n");
		goto fail_bar0_resource;
	}

	nd->npdev.bar0 = pci_iomap(dev, apb_bar, pci_resource_len(dev, apb_bar));
	nd->npdev.bar0_size = pci_resource_len(dev, apb_bar);

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA) {
		ret = pci_request_region(dev, INF_AXI_BAR, "AXI");
		if (ret != 0) {
			pci_info(dev, "Can't map BAR2\n");
			goto fail_bar2_map;
		}

		nd->npdev.bar2_pa = pci_resource_start(dev, INF_AXI_BAR);
		if (!nd->npdev.bar2_pa) {
			ret = -ENODEV;
			pci_info(dev, "Can't get start address of BAR2\n");
			goto fail_bar2_resource;
		}
		nd->npdev.bar2 = pci_iomap(dev, INF_AXI_BAR, pci_resource_len(dev, INF_AXI_BAR));
		nd->npdev.bar2_size = pci_resource_len(dev, INF_AXI_BAR);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	nd->device_index = atomic_fetch_add(1, &device_count);
#else
	nd->device_index = atomic_add_return(1, &device_count) - 1;
#endif
	if (narch_get_arch() != NEURON_ARCH_INFERENTIA && !narch_is_qemu()) {
		ret = fw_io_device_id_read(nd->npdev.bar0, &nd->device_index);
		BUG_ON(ret != 0);
	}

	ret = neuron_pci_device_init(nd);
	if (ret)
		goto fail_bar2_resource;

	// initialize datastore
	neuron_ds_init(&nd->datastore, nd);

	// allocate memset mc (if datastore succeeded)
	ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, MEMSET_HOST_BUF_SIZE, MEM_LOC_HOST, 0, 0, 0,
		       &nd->memset_mc);
	if (ret)
		goto fail_bar2_resource;

	// initialize metric aggregation and posting
 	ret = nmetric_init(nd);
 	if (ret)
 		goto fail_nmetric_resource;

	mutex_init(&nd->memset_lock);

	BUG_ON(neuron_devices[nd->device_index] != NULL);
	neuron_devices[nd->device_index] = nd;

	return 0;

fail_nmetric_resource:
	nmetric_stop_thread(nd);
fail_bar2_resource:
	if (narch_get_arch() == NEURON_ARCH_INFERENTIA)
		pci_release_region(dev, INF_AXI_BAR);
fail_bar2_map:
fail_bar0_resource:
	pci_release_region(dev, INF_APB_BAR);
fail_bar0_map:
	pci_disable_device(dev);
fail_enable:
	kfree(nd);
fail_alloc_mem:
	return ret;
}

static void neuron_pci_remove(struct pci_dev *dev)
{
	struct neuron_device *nd;
	int apb_bar;

	nd = pci_get_drvdata(dev);
	if (nd == NULL)
		return;

	nr_stop_thread(nd);

	nmetric_stop_thread(nd);

	apb_bar = neuron_pci_get_apb_bar(nd);
	pci_release_region(dev, apb_bar);

	if (narch_get_arch() == NEURON_ARCH_INFERENTIA)
		pci_release_region(dev, INF_AXI_BAR);

	if (narch_get_arch() == NEURON_ARCH_TRN)
		pci_release_region(dev, TRN_BAR4);

	pci_disable_device(dev);

	neuron_pci_device_close(nd);

	kfree(nd);
}

static struct pci_driver neuron_pci_driver = {
	.name = "neuron-driver",
	.id_table = neuron_pci_dev_ids,
	.probe = neuron_pci_probe,
	.remove = neuron_pci_remove,
};

int neuron_pci_module_init(void)
{
	int ret;

	ret = pci_register_driver(&neuron_pci_driver);
	if (ret != 0) {
		pr_err("Failed to register neuron inf driver %d\n", ret);
		return ret;
	}
	return ncdev_create_misc_node();
}

void neuron_pci_module_exit(void)
{
	pci_unregister_driver(&neuron_pci_driver);
}
