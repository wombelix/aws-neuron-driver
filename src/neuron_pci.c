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
#include <linux/delay.h>
#include <linux/umh.h>

#include "neuron_device.h"
#include "neuron_ds.h"
#include "neuron_reg_access.h"
#include "neuron_metrics.h"
#include "v1/fw_io.h"
#include "v1/address_map.h"
#include "v2/address_map.h"

#include "neuron_dma.h"


static struct pci_device_id neuron_pci_dev_ids[] = {
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID0) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID1) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID2) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF1_DEVICE_ID3) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, TRN1_DEVICE_ID0) },
	{ PCI_DEVICE(AMZN_VENDOR_ID, INF2_DEVICE_ID0) },
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

/* device memory is BAR4 */
#define BAR4 4

// some old kernels do not have pci_info defined.
#ifndef pci_info
#define pci_info(pdev, fmt, arg...) dev_info(&(pdev)->dev, fmt, ##arg)
#endif

int dup_helper_enable = 1;
module_param(dup_helper_enable, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dup_helper_enable, "enable duplicate routing id unload helper");

int wc_enable = 1;
module_param(wc_enable, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(wc_enable, "enable write combining");

// count of duplicate routing IDs encountered
//
static atomic_t dup_rid_cnt = ATOMIC_INIT(0);

// number of devices managed
static atomic_t device_count = ATOMIC_INIT(0);

extern int ncdev_create_device_node(struct neuron_device *ndev);
extern int ncdev_delete_device_node(struct neuron_device *ndev);
extern void ndmar_preinit(struct neuron_device *nd);

static struct neuron_device *neuron_devices[MAX_NEURON_DEVICE_COUNT] = { 0 };

struct neuron_device *neuron_pci_get_device(u8 device_index)
{
	BUG_ON(device_index >= MAX_NEURON_DEVICE_COUNT);
	return neuron_devices[device_index];
}


/**
 * neuron_pci_handle_dup_routing_id()
 *
 *
 */
static int neuron_pci_handle_dup_routing_id(void)
{
	int  ret = -ENODEV;
	int  dup_cnt;
	char cmd[256];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	dup_cnt = atomic_fetch_add(1, &dup_rid_cnt);
#else
	dup_cnt = atomic_add_return(1, &dup_rid_cnt) - 1;
#endif 

	// If this is the first dup encounted, unload the driver
	//
	if ((dup_cnt == 0) && dup_helper_enable) {
		pr_err("scheduling unload of %s due to duplicate routing id\n", module_name(THIS_MODULE));

		int n = snprintf(cmd, sizeof(cmd), "sleep 10;/sbin/modprobe -r %s", module_name(THIS_MODULE));
		if (n > sizeof(cmd)) {
			pr_err("unable to schedule driver unload cmd buffer len exceeded\n");
			return -EINVAL;
		}
		char *argv[] = 		  { "/bin/sh",
								"-c",
								cmd,
								NULL};
		static char *envp[] = { "HOME=/",
								"TERM=linux",
								"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
								NULL};

		ret = call_usermodehelper( argv[0], argv, envp, UMH_WAIT_EXEC);
		if (ret)
			pr_err("unable to schedule driver unload. Error: %d\n", ret);
	}


	return ret;
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

	ret = nr_create_thread(nd);
	if (ret)
		return ret;

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

	ret = nr_start_ncs(nd, NEURON_NC_MAP_DEVICE, NEURON_RESET_REQUEST_ALL);
	if (ret)
		return ret;

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
	// TODO - this is temp 
	narch_init(device == TRN1_DEVICE_ID0 || (device == INF2_DEVICE_ID0)  ? NEURON_ARCH_TRN : NEURON_ARCH_INFERENTIA, revision);
}

// for V2 rename Neuron devices for better customer experience.
// https://quip-amazon.com/rRRZAGmIdAaW/TRN1-Discovery
// map routing id to user id:
const u32 v2_routing_id_to_user_id[MAX_NEURON_DEVICE_COUNT] = {
	0,   4,  1,  5,
	3,   7,  2,  6,
	12,  8, 13,  9,
	15, 11, 14, 10 };


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

	// map bar4 for device memory.
	ret = pci_request_region(dev, BAR4, "BAR4");
	if (ret == 0) {
		nd->npdev.bar4_pa = pci_resource_start(dev, BAR4);
		if (nd->npdev.bar4_pa) {
			if (wc_enable)
				nd->npdev.bar4 = pci_iomap_wc(dev, BAR4, pci_resource_len(dev, BAR4));
			else
				nd->npdev.bar4 = pci_iomap(dev, BAR4, pci_resource_len(dev, BAR4));
			nd->npdev.bar4_size = pci_resource_len(dev, BAR4);
		}
	}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	nd->device_index = atomic_fetch_add(1, &device_count);
#else
	nd->device_index = atomic_add_return(1, &device_count) - 1;
#endif 
	nd->fw_io_ctx = fw_io_setup(nd->npdev.bar0, nd->npdev.bar0_size,
				    nd->npdev.bar2, nd->npdev.bar2_size);
	if (nd->fw_io_ctx == NULL) {
		pr_err("readless read initialization failed");
		ret = -ENODEV;
		goto fail_bar2_resource;
	}

	if (narch_get_arch() != NEURON_ARCH_INFERENTIA) {
		u32 routing_id = (u32)-1;
		// Poll the device id until the device is ready
		int i;
		for (i = 0; i < 20; i++) {
			ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
			if (!ret && routing_id != 0xdeadbeef) {
				break;
			}
			msleep(1000);
		}

		if (ret) {
			pr_err("Could not retrieve device index (read timeout)");
			ret = -ENODEV;
			goto fail_bar2_resource;
		}

		// TODO - this should be a "valid routing_id check for both TRN1 & INF2
		if (routing_id < 0 || routing_id >= MAX_NEURON_DEVICE_COUNT) {
			pr_err("Invalid device index %u", routing_id);
			ret = -ENODEV;
			goto fail_bar2_resource;
		}

		// TODO - TRN1 and INF2 mappings are different - likely all of this and the INF1 should be encapsulated.
		if (nd->pdev->device == TRN1_DEVICE_ID0)
			nd->device_index = v2_routing_id_to_user_id[routing_id];
		else
			nd->device_index = routing_id;

		// TODO temporary for the bringup, remove
		printk("** BDF: %2.2x:%2.2x.%x => nd[%d] (routing id: %u)\n", dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn), nd->device_index, routing_id);
		
		// protection against duplicate IDs - doesn't provide 100% protection in multi-threaded device discovery
		if (neuron_devices[nd->device_index] != NULL) {
			pr_err("duplicate routing id %u found\n", routing_id);
			neuron_pci_handle_dup_routing_id();
			ret = -ENODEV;
			goto fail_bar2_resource;
		}

	}

	ret = neuron_pci_device_init(nd);
	if (ret)
		goto fail_bar2_resource;

	// initialize datastore
	ret = neuron_ds_init(&nd->datastore, nd);
	if (ret)
		goto fail_nds_resource;

	ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, NDMA_QUEUE_DUMMY_RING_SIZE, MEM_LOC_HOST, 0, 0, 0,
		       &nd->ndma_q_dummy_mc);
	if (ret)
		goto fail_nds_resource;

	// allocate memset mc (if datastore succeeded)
	ret = mc_alloc(nd, MC_LIFESPAN_DEVICE, MEMSET_HOST_BUF_SIZE, MEM_LOC_HOST, 0, 0, 0,
		       &nd->memset_mc);
	if (ret)
		goto fail_memset_mc;

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
	mc_free(&nd->memset_mc);
fail_memset_mc:
	mc_free(&nd->ndma_q_dummy_mc);
fail_nds_resource:
	neuron_ds_destroy(&nd->datastore);
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

	pci_release_region(dev, BAR4);

	pci_disable_device(dev);

	neuron_pci_device_close(nd);

	if (nd->npdev.bar0) {
		pci_iounmap(dev, nd->npdev.bar0);
	}
	if (nd->npdev.bar2) {
		pci_iounmap(dev, nd->npdev.bar2);
	}
	if (nd->npdev.bar4) {
		pci_iounmap(dev, nd->npdev.bar4);
	}

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
	return 0;
}

void neuron_pci_module_exit(void)
{
	pci_unregister_driver(&neuron_pci_driver);
}
