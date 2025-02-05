// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __V2_ADDR_MAP_H__

// Host memory access
#define V2_PCIE_A0_BASE                        0

// relative to nc
#define V2_MMAP_P_OFFSET 0
#define V2_MMAP_NC_EVENT_OFFSET 0
#define V2_MMAP_NC_SEMA_READ_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0
#define V2_MMAP_NC_SEMA_SET_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0
#define V2_MMAP_NC_SEMA_INCR_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0
#define V2_MMAP_NC_SEMA_DECR_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0

// relative to bar0
#define V2_MMAP_BAR0_APB_OFFSET                    0
#define V2_APB_IOFAB_RELBASE                       0
#define V2_APB_IOFAB_MISC_RAM_RELBASE              0
#define V2_MMAP_BAR0_APB_MISC_RAM_OFFSET           V2_MMAP_BAR0_APB_OFFSET + V2_APB_IOFAB_RELBASE + V2_APB_IOFAB_MISC_RAM_RELBASE

#define V2_MMAP_NC_SIZE 0

// Number of Neuron Core per device
#define V2_NC_PER_DEVICE 0
// Number of DMA engines per NC
#define V2_DMA_ENG_PER_NC 0

#define V2_MAX_DMA_RINGS 0

// Number of TOP_SP per device
#define V2_TS_PER_DEVICE 0

// max channels supported by v2 device
#define V2_MAX_DRAM_CHANNELS 0

#define V2_SEMAPHORE_COUNT 32
#define V2_EVENTS_COUNT 256


#define V2_MAX_NQ_QUEUES 16
#define V2_MAX_NQ_TYPE 5
#define V2_MAX_NQ_SUPPORTED (V2_MAX_NQ_TYPE * V2_MAX_NQ_QUEUES)

#define V2_DRAM_0_BASE 0
#define V2_DRAM_1_BASE 0
#define V2_DRAM_0_SIZE 0
#define V2_DRAM_1_SIZE 0
#define V2_PCIE_BAR0_TPB_0_OFFSET 0
#define V2_PCIE_BAR0_TPB_0_SIZE 0
#define V2_PCIE_BAR0_APB_OFFSET 0
#define V2_PCIE_A0_BASE 0

#define V2_TOP_SP_0_BASE 0
#define V2_TOP_SP_0_SIZE 0

#define V2_MMAP_TPB_OFFSET 0
#define V2_MMAP_TPB_SIZE 0
#define V2_MMAP_TPB_COUNT 0

#define V2_APB_SENG_0_SDMA_0_APP_RELBASE 0

#define V2_APB_IOFAB_RELBASE 0
#define V2_APB_IOFAB_MISC_RAM_RELBASE 0
#define V2_APB_SENG_0_RESERVED1_RELBASE 0
#endif
