// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __NOTIFIC_H__

#define __NOTIFIC_H__

/** Neuron device can generate 4 type of notifications during program execution
 * 1. Errors - Any accelerator generated error such as infinity, NaN
 * 2. Events - Accelerator set or cleared an event.
 * 3. Explicit - Program had an instruction which explicitly generated notification.
 * 4. Implicit - If configured all instructions would generate notification.
 *
 * Each NeuronCore has one error and event notification queue and multiple
 * implicit and explicit notification queues.
 *
 * The notifications are stored in a circular buffer.
 * The following functions enables setting up the circular buffer.
 */

#include "address_map.h"
#include "../neuron_reg_access.h"

/** Returns NOTIFIC relative offset for given the DMA engine for given NC.
 */
u64 notific_get_relative_offset_sdma(int nc_id, int eng_id);

/** Returns NOTIFIC relative offset for given the NC.
 */
static inline u64 notific_get_relative_offset(int nc_idx)
{
	return 0;
}

/** Returns NOTIFIC relative offset for given TOP_SP.
 */
static inline u64 notific_get_relative_offset_topsp(int ts_idx)
{
	return 0;
}

#define NOTIFIC_NQ_SIZE 0x28
#define NOTIFIC_NQ_BASE_ADDR_LO_OFFSET_START 0x100
#define NOTIFIC_NQ_BASE_ADDR_LO_OFFSET(index) (NOTIFIC_NQ_BASE_ADDR_LO_OFFSET_START + ((index)*NOTIFIC_NQ_SIZE) + 0)

#define NOTIFIC_NQ_BASE_ADDR_LO_RESET_VALUE 0x00000000

static inline void notific_write_nq_base_addr_lo(void __iomem *base, size_t index,
								  uint32_t value)
{
}

#define NOTIFIC_NQ_BASE_ADDR_HI_OFFSET_START 0x104
#define NOTIFIC_NQ_BASE_ADDR_HI_OFFSET(index) (NOTIFIC_NQ_BASE_ADDR_HI_OFFSET_START + ((index)*NOTIFIC_NQ_SIZE) + 0)

#define NOTIFIC_NQ_BASE_ADDR_HI_RESET_VALUE 0x00000000

static inline void notific_write_nq_base_addr_hi(void __iomem *base, size_t index,
								  uint32_t value)
{
}

#define NOTIFIC_NQ_F_SIZE_OFFSET_START 0x108
#define NOTIFIC_NQ_F_SIZE_OFFSET(index) (NOTIFIC_NQ_F_SIZE_OFFSET_START + ((index)*NOTIFIC_NQ_SIZE) + 0)

#define NOTIFIC_F_SIZE_RESET_VALUE 0x00000000

static inline void notific_write_nq_f_size(void __iomem *base, size_t index,
							    uint32_t value)
{
}

int notific_decode_nq_head_reg_access(u64 offset, u8 *nc_id, u32 *nq_type, u8 *instance,
				      bool *is_top_sp);

#endif
