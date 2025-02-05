// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "../udma/udma.h"
#include "address_map.h"
#include "sdma.h"

int v2_dma_init(void __iomem *bar0, struct udma *udma, int eng_id)
{
	return 0;
}
