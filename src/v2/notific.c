// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <share/neuron_driver_shared.h>

#include "notific.h"

u64 notific_get_relative_offset_sdma(int nc_id, int eng_id)
{
	return 0;
}

int notific_decode_nq_head_reg_access(u64 offset, u8 *nc_id, u32 *nq_type, u8 *instance, bool *is_top_sp)
{
	return -EINVAL;
}
