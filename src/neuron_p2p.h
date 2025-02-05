// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __NEURON_P2P_H__
#define __NEURON_P2P_H__

struct neuron_p2p_va_info {
    void *virtual_address; // Virtual address for which the PA's need to be obtained
    u32 size; //size.
    u32 device_index; // Neuron Device index.
    u32 entries; // Number of pa's. Say page size is 4k, allocate 8k. this will be 2 even though for v2 we have a contiguous 8k
    u64 physical_address[]; //PAs for the VA
};

enum neuron_p2p_page_size_type {
    NEURON_P2P_PAGE_SIZE_4KB,
};

/** Given the virtual address and length returns the physical address
 *
 * @param[in] virtual_address   - Virtual address of device memory
 * @param[in] length            - Length of the memory
 * @param[out] va_info          - Set of physical addresses
 * @param[in] free_callback     - Callback function to be called. This will be called with a lock held.
 * @param[in] data              - Data to be used for the callback
 *
 * @return 0            - Success.
 */
int neuron_p2p_register_va(u64 virtual_address, u64 length, struct neuron_p2p_va_info **vainfo, void (*free_callback) (void *data), void *data);

/** Give the pa, release the pa from being used by third-party device
 *
 * @param[in] va_info           - Set of physical addresses
 *
 * @return 0            - Success.
 */
int neuron_p2p_unregister_va(struct neuron_p2p_va_info *vainfo);

#endif