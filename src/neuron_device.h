// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef NEURON_DEVICE_H
#define NEURON_DEVICE_H

#include "neuron_arch.h"
#include "neuron_mempool.h"
#include "neuron_ring.h"
#include "neuron_core.h"
#include "neuron_reset.h"
#include "neuron_cinit.h"
#include "neuron_crwl.h"
#include "neuron_pid.h"
#include "neuron_ioctl.h"
#include "neuron_ds.h"
#include "neuron_metrics.h"
#include "v1/address_map.h"
#include "v2/address_map.h"

// Maximum neuron devices supported on a system.
#define MAX_NEURON_DEVICE_COUNT 16
#define MAX_NC_PER_DEVICE 4

#define NC_PER_DEVICE(nd)                                                                          \
	(narch_get_arch() == NEURON_ARCH_INFERENTIA ? V1_NC_PER_DEVICE : V2_NC_PER_DEVICE)

// Global host memory buf size used for memset the device memory
#define MEMSET_HOST_BUF_SIZE MAX_DMA_DESC_SIZE // guessed optimal DMA transfer and PCIe TLP size.

struct neuron_pci_device {
	phys_addr_t bar0_pa;
	void __iomem *bar0;
	u64 bar0_size;
	phys_addr_t bar2_pa;
	void __iomem *bar2;
	u64 bar2_size;
	phys_addr_t bar4_pa;
	void __iomem *bar4;
	u64 bar4_size;
};

struct neuron_nq {
	int head;
	int phase_bit;
};

struct neuron_device {
	struct pci_dev *pdev;
	int device_index;

	// all the processes that are opened this device
	struct neuron_attached_process attached_processes[NEURON_MAX_PROCESS_PER_DEVICE];

	void *ncdev; // chardev created for this devices

	struct neuron_pci_device npdev;

	bool dmar_init_done;
	struct ndma_eng ndma_engine[NUM_DMA_ENG_PER_DEVICE];

	void *fw_io_ctx;

	struct mempool_set mpset;

	// notification queue in each neuron core.
	struct neuron_nq nq[MAX_NC_PER_DEVICE][MAX_NQ_SUPPORTED];

	// memory chunk allocated for notification queue in each neuron core.
	struct mem_chunk *nq_mc[MAX_NC_PER_DEVICE][MAX_NQ_SUPPORTED];
	// memory chunk allocated for notification queue in each TOP_SP.
	struct mem_chunk *ts_nq_mc[V2_TS_PER_DEVICE][MAX_NQ_SUPPORTED];

	int connected_device_count; // number of devices connected to this device
	u32 connected_devices[MAX_NEURON_DEVICE_COUNT]; // device ids of the connected devices

	// memory chunk for setting device mem
	struct mem_chunk *memset_mc;
	struct mutex memset_lock;

	struct neuron_datastore datastore; // neuron datastore

	struct neuron_metrics metrics; // aggregated metrics

	struct neuron_crwl crwl[MAX_NC_PER_DEVICE]; // cooperative rw lock per NC.

	struct neuron_reset nr; // neuron reset thread's data

	struct neuron_cinit nci[MAX_NC_PER_DEVICE]; // neuron

	u64 nc_model_started_count[MAX_NC_PER_DEVICE]; // number of times the NCs has started model
};

#define PCI_HOST_BASE(nd) (narch_get_arch() == NEURON_ARCH_TRN ? V2_PCIE_A0_BASE : PCIEX8_0_BASE)

/**
 * neuron_pci_get_device() - Returns devices associated with given index.
 *
 * @device_index: device index
 *
 * Return: NULL if device does not exists, neuron_device otherwise.
 */
struct neuron_device *neuron_pci_get_device(u8 device_index);

#endif
