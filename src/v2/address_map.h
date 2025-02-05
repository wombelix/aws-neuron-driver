// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __V2_ADDR_MAP_H__

// Host memory access
#define V2_PCIE_A0_BASE                        0x00100000000000ull

// relative to nc
#define V2_MMAP_P_OFFSET 0x00000000000000ull
#define V2_MMAP_NC_EVENT_OFFSET 0x00000002700000ull
#define V2_MMAP_NC_SEMA_READ_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0x00000000001000ull
#define V2_MMAP_NC_SEMA_SET_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0x00000000001400ull
#define V2_MMAP_NC_SEMA_INCR_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0x00000000001800ull
#define V2_MMAP_NC_SEMA_DECR_OFFSET V2_MMAP_NC_EVENT_OFFSET + 0x00000000001c00ull

// relative to bar0
#define V2_MMAP_BAR0_APB_OFFSET                    0x00000030000000ull
#define V2_APB_IOFAB_RELBASE                       0x00000000e00000ull
#define V2_APB_IOFAB_MISC_RAM_RELBASE              0x000000001a0000ull
#define V2_MMAP_BAR0_APB_MISC_RAM_OFFSET           V2_MMAP_BAR0_APB_OFFSET + V2_APB_IOFAB_RELBASE + V2_APB_IOFAB_MISC_RAM_RELBASE

// relative to sunda address space
#define V2_APB_MISC_RAM_OFFSET 0x000ffff0fa0000ull

#define V2_MMAP_NC_SIZE 0x00000004000000ull

// Number of Neuron Core per device
#define V2_NC_PER_DEVICE 2
// Number of DMA engines per NC
#define V2_DMA_ENG_PER_NC 16

// Number of DMA queues in each engine
#define V2_DMA_QUEUE_PER_ENG 16

#define V2_NUM_DMA_ENG_PER_DEVICE (V2_NC_PER_DEVICE * V2_DMA_ENG_PER_NC) + 2
#define V2_MAX_DMA_RINGS 16

// Number of TPB engines per NC
#define V2_TPB_ENG_PER_NC 5

// Number of TOP_SP per device
#define V2_TS_PER_DEVICE 6

// max channels supported by v2 device
#define V2_MAX_DRAM_CHANNELS 2  //2 HBM's.
#define V2_MAX_DDR_REGIONS V2_NC_PER_DEVICE

#define V2_SEMAPHORE_COUNT 32
#define V2_EVENTS_COUNT 256

#define V2_ALLOWED_DESC_PER_PACKET 64

#define V2_MAX_NQ_QUEUES 16
#define V2_MAX_NQ_TYPE 5
#define V2_MAX_NQ_SUPPORTED (V2_MAX_NQ_TYPE * V2_MAX_NQ_QUEUES)

#define V2_APB_BASE 0x000ffff0000000ull
#define V2_HBM_0_BASE 0x00000000000000ull
#define V2_HBM_1_BASE 0x00001000000000ull
#define V2_HBM_0_SIZE 0x00000400000000ull
#define V2_HBM_1_SIZE 0x00000400000000ull
#define V2_PCIE_BAR0_TPB_0_OFFSET 0x000000000000000
#define V2_PCIE_BAR0_TPB_0_SIZE 0x000000004000000
#define V2_PCIE_BAR0_APB_OFFSET 0x000000030000000
#define V2_PCIE_A0_BASE 0x00100000000000ull

#define V2_TOP_SP_0_BASE 0x000fffd0000000ull
#define V2_TOP_SP_0_SIZE 0x00000000400000ull

#define V2_MMAP_TPB_OFFSET 0x000fffc0000000ull
#define V2_MMAP_TPB_SIZE 0x00000004000000ull
#define V2_MMAP_TPB_COUNT 2
#define V2_NUM_DMA_ENGINES_PER_TPB 16

#define V2_D2H_IDX 32
#define V2_H2D_IDX 33

#define V2_APB_SENG_0_UDMA_0_BASE 0x000ffff0000000ull
#define V2_APB_SENG_0_UDMA_1_BASE 0x000ffff0040000ull
#define V2_APB_SENG_0_UDMA_2_BASE 0x000ffff0080000ull
#define V2_APB_SENG_0_UDMA_3_BASE 0x000ffff00c0000ull
#define V2_APB_SENG_0_UDMA_4_BASE 0x000ffff0100000ull
#define V2_APB_SENG_0_UDMA_5_BASE 0x000ffff0140000ull
#define V2_APB_SENG_0_UDMA_6_BASE 0x000ffff0180000ull
#define V2_APB_SENG_0_UDMA_7_BASE 0x000ffff01c0000ull
#define V2_APB_SENG_0_UDMA_8_BASE 0x000ffff0200000ull
#define V2_APB_SENG_0_UDMA_9_BASE 0x000ffff0240000ull
#define V2_APB_SENG_0_UDMA_10_BASE 0x000ffff0280000ull
#define V2_APB_SENG_0_UDMA_11_BASE 0x000ffff02c0000ull
#define V2_APB_SENG_0_UDMA_12_BASE 0x000ffff0300000ull
#define V2_APB_SENG_0_UDMA_13_BASE 0x000ffff0340000ull
#define V2_APB_SENG_0_UDMA_14_BASE 0x000ffff0380000ull
#define V2_APB_SENG_0_UDMA_15_BASE 0x000ffff03c0000ull
#define V2_APB_SENG_1_UDMA_0_BASE 0x000ffff0700000ull
#define V2_APB_SENG_1_UDMA_1_BASE 0x000ffff0740000ull
#define V2_APB_SENG_1_UDMA_2_BASE 0x000ffff0780000ull
#define V2_APB_SENG_1_UDMA_3_BASE 0x000ffff07c0000ull
#define V2_APB_SENG_1_UDMA_4_BASE 0x000ffff0800000ull
#define V2_APB_SENG_1_UDMA_5_BASE 0x000ffff0840000ull
#define V2_APB_SENG_1_UDMA_6_BASE 0x000ffff0880000ull
#define V2_APB_SENG_1_UDMA_7_BASE 0x000ffff08c0000ull
#define V2_APB_SENG_1_UDMA_8_BASE 0x000ffff0900000ull
#define V2_APB_SENG_1_UDMA_9_BASE 0x000ffff0940000ull
#define V2_APB_SENG_1_UDMA_10_BASE 0x000ffff0980000ull
#define V2_APB_SENG_1_UDMA_11_BASE 0x000ffff09c0000ull
#define V2_APB_SENG_1_UDMA_12_BASE 0x000ffff0a00000ull
#define V2_APB_SENG_1_UDMA_13_BASE 0x000ffff0a40000ull
#define V2_APB_SENG_1_UDMA_14_BASE 0x000ffff0a80000ull
#define V2_APB_SENG_1_UDMA_15_BASE 0x000ffff0ac0000ull

#define V2_APB_SENG_UDMA_SIZE 0x00000000040000ull

#define V2_APB_SENG_0_SDMA_0_BASE 0x000ffff0400000ull
#define V2_APB_SENG_0_SDMA_1_BASE 0x000ffff0407000ull
#define V2_APB_SENG_0_SDMA_2_BASE 0x000ffff040e000ull
#define V2_APB_SENG_0_SDMA_3_BASE 0x000ffff0415000ull
#define V2_APB_SENG_0_SDMA_4_BASE 0x000ffff041c000ull
#define V2_APB_SENG_0_SDMA_5_BASE 0x000ffff0423000ull
#define V2_APB_SENG_0_SDMA_6_BASE 0x000ffff042a000ull
#define V2_APB_SENG_0_SDMA_7_BASE 0x000ffff0431000ull
#define V2_APB_SENG_0_SDMA_8_BASE 0x000ffff0438000ull
#define V2_APB_SENG_0_SDMA_9_BASE 0x000ffff043f000ull
#define V2_APB_SENG_0_SDMA_10_BASE 0x000ffff0446000ull
#define V2_APB_SENG_0_SDMA_11_BASE 0x000ffff044d000ull
#define V2_APB_SENG_0_SDMA_12_BASE 0x000ffff0454000ull
#define V2_APB_SENG_0_SDMA_13_BASE 0x000ffff045b000ull
#define V2_APB_SENG_0_SDMA_14_BASE 0x000ffff0462000ull
#define V2_APB_SENG_0_SDMA_15_BASE 0x000ffff0469000ull
#define V2_APB_SENG_1_SDMA_0_BASE 0x000ffff0b00000ull
#define V2_APB_SENG_1_SDMA_1_BASE 0x000ffff0b07000ull
#define V2_APB_SENG_1_SDMA_2_BASE 0x000ffff0b0e000ull
#define V2_APB_SENG_1_SDMA_3_BASE 0x000ffff0b15000ull
#define V2_APB_SENG_1_SDMA_4_BASE 0x000ffff0b1c000ull
#define V2_APB_SENG_1_SDMA_5_BASE 0x000ffff0b23000ull
#define V2_APB_SENG_1_SDMA_6_BASE 0x000ffff0b2a000ull
#define V2_APB_SENG_1_SDMA_7_BASE 0x000ffff0b31000ull
#define V2_APB_SENG_1_SDMA_8_BASE 0x000ffff0b38000ull
#define V2_APB_SENG_1_SDMA_9_BASE 0x000ffff0b3f000ull
#define V2_APB_SENG_1_SDMA_10_BASE 0x000ffff0b46000ull
#define V2_APB_SENG_1_SDMA_11_BASE 0x000ffff0b4d000ull
#define V2_APB_SENG_1_SDMA_12_BASE 0x000ffff0b54000ull
#define V2_APB_SENG_1_SDMA_13_BASE 0x000ffff0b5b000ull
#define V2_APB_SENG_1_SDMA_14_BASE 0x000ffff0b62000ull
#define V2_APB_SENG_1_SDMA_15_BASE 0x000ffff0b69000ull


#define V2_APB_D2H_UDMA_BASE   0x000ffff0600000ull
#define V2_APB_H2D_UDMA_BASE   0x000ffff0d00000ull

#define V2_APB_D2H_SDMA_BASE   0x000ffff0640000ull
#define V2_APB_H2D_SDMA_BASE   0x000ffff0d40000ull


#define V2_APB_SENG_0_SDMA_0_NOTIFIC_RELBASE 0x00000000001000ull
#define V2_APB_SENG_0_RELBASE 0x00000000000000ull
#define V2_APB_SENG_1_RELBASE 0x00000000700000ull
#define V2_APB_SENG_0_TPB_TOP_RELBASE 0x00000000530000ull
#define V2_APB_SENG_0_TPB_TOP_NOTIFIC_RELBASE 0x00000000005000ull
#define V2_APB_SENG_0_TPB_NOTIFIC_SIZE 0x00000000001000ull
#define V2_APB_SENG_0_SDMA_0_APP_RELBASE 0x00000000000000ull

#define V2_APB_IOFAB_RELBASE 0x00000000e00000ull
#define V2_APB_IOFAB_TOP_SP_0_RELBASE 0x00000000000000ull
#define V2_APB_IOFAB_TOP_SP_0_SIZE 0x00000000040000ull
#define V2_APB_IOFAB_TOP_SP_0_NOTIFIC_RELBASE 0x00000000004000ull
#define V2_APB_IOFAB_MISC_RAM_RELBASE 0x000000001a0000ull
#define V2_APB_SENG_0_RESERVED1_RELBASE 0x00000000647000ull
#endif
