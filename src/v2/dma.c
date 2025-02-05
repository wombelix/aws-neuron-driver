// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "../udma/udma.h"
#include "address_map.h"
#include "sdma.h"

const static uint64_t seng_udma_base[V2_MMAP_TPB_COUNT][V2_NUM_DMA_ENGINES_PER_TPB] = {
	{ V2_APB_SENG_0_UDMA_0_BASE, V2_APB_SENG_0_UDMA_1_BASE, V2_APB_SENG_0_UDMA_2_BASE,
	  V2_APB_SENG_0_UDMA_3_BASE, V2_APB_SENG_0_UDMA_4_BASE, V2_APB_SENG_0_UDMA_5_BASE,
	  V2_APB_SENG_0_UDMA_6_BASE, V2_APB_SENG_0_UDMA_7_BASE, V2_APB_SENG_0_UDMA_8_BASE,
	  V2_APB_SENG_0_UDMA_9_BASE, V2_APB_SENG_0_UDMA_10_BASE, V2_APB_SENG_0_UDMA_11_BASE,
	  V2_APB_SENG_0_UDMA_12_BASE, V2_APB_SENG_0_UDMA_13_BASE,
	  V2_APB_SENG_0_UDMA_14_BASE, V2_APB_SENG_0_UDMA_15_BASE },
	{ V2_APB_SENG_1_UDMA_0_BASE, V2_APB_SENG_1_UDMA_1_BASE, V2_APB_SENG_1_UDMA_2_BASE,
	  V2_APB_SENG_1_UDMA_3_BASE, V2_APB_SENG_1_UDMA_4_BASE, V2_APB_SENG_1_UDMA_5_BASE,
	  V2_APB_SENG_1_UDMA_6_BASE, V2_APB_SENG_1_UDMA_7_BASE, V2_APB_SENG_1_UDMA_8_BASE,
	  V2_APB_SENG_1_UDMA_9_BASE, V2_APB_SENG_1_UDMA_10_BASE, V2_APB_SENG_1_UDMA_11_BASE,
	  V2_APB_SENG_1_UDMA_12_BASE, V2_APB_SENG_1_UDMA_13_BASE,
	  V2_APB_SENG_1_UDMA_14_BASE, V2_APB_SENG_1_UDMA_15_BASE }
};
const static uint64_t seng_sdma_base[V2_MMAP_TPB_COUNT][V2_NUM_DMA_ENGINES_PER_TPB] = {
	{ V2_APB_SENG_0_SDMA_0_BASE, V2_APB_SENG_0_SDMA_1_BASE, V2_APB_SENG_0_SDMA_2_BASE,
	  V2_APB_SENG_0_SDMA_3_BASE, V2_APB_SENG_0_SDMA_4_BASE, V2_APB_SENG_0_SDMA_5_BASE,
	  V2_APB_SENG_0_SDMA_6_BASE, V2_APB_SENG_0_SDMA_7_BASE, V2_APB_SENG_0_SDMA_8_BASE,
	  V2_APB_SENG_0_SDMA_9_BASE, V2_APB_SENG_0_SDMA_10_BASE, V2_APB_SENG_0_SDMA_11_BASE,
	  V2_APB_SENG_0_SDMA_12_BASE, V2_APB_SENG_0_SDMA_13_BASE,
	  V2_APB_SENG_0_SDMA_14_BASE, V2_APB_SENG_0_SDMA_15_BASE },
	{ V2_APB_SENG_1_SDMA_0_BASE, V2_APB_SENG_1_SDMA_1_BASE, V2_APB_SENG_1_SDMA_2_BASE,
	  V2_APB_SENG_1_SDMA_3_BASE, V2_APB_SENG_1_SDMA_4_BASE, V2_APB_SENG_1_SDMA_5_BASE,
	  V2_APB_SENG_1_SDMA_6_BASE, V2_APB_SENG_1_SDMA_7_BASE, V2_APB_SENG_1_SDMA_8_BASE,
	  V2_APB_SENG_1_SDMA_9_BASE, V2_APB_SENG_1_SDMA_10_BASE, V2_APB_SENG_1_SDMA_11_BASE,
	  V2_APB_SENG_1_SDMA_12_BASE, V2_APB_SENG_1_SDMA_13_BASE,
	  V2_APB_SENG_1_SDMA_14_BASE, V2_APB_SENG_1_SDMA_15_BASE }
};
int v2_dma_init(void __iomem *bar0, struct udma *udma, int eng_id)
{
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	const bool d2h = (eng_id == V2_D2H_IDX);
	const bool h2d = (eng_id == V2_H2D_IDX);


	void __iomem *udma_base = NULL;
	void __iomem *sdma_base = NULL;

	if (h2d || d2h) {
		const uint64_t seng_udma_relbase = ( h2d ? V2_APB_H2D_UDMA_BASE : V2_APB_D2H_UDMA_BASE) - V2_APB_BASE;
		const uint64_t seng_sdma_relbase = ( h2d ? V2_APB_H2D_SDMA_BASE : V2_APB_D2H_SDMA_BASE) - V2_APB_BASE;

		udma_base = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_udma_relbase;
		sdma_base = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_sdma_relbase;
	} else {
		const int nc_id = eng_id / V2_DMA_ENG_PER_NC;
		const int eid = eng_id % V2_DMA_ENG_PER_NC;

		const uint64_t seng_udma_relbase = seng_udma_base[nc_id][eid] - V2_APB_BASE;
		const uint64_t seng_sdma_relbase = seng_sdma_base[nc_id][eid] - V2_APB_BASE;

		udma_base = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_udma_relbase;
		sdma_base  = ((void __iomem *)bar0 + V2_PCIE_BAR0_APB_OFFSET) + seng_sdma_relbase;

		ret = sdma_configure_broadcast(sdma_base, eid);
		if (ret) {
			pr_err("SDMA BCAST:%d init failed\n", eng_id);
			goto done;
		}
	}

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(udma, udma_base, DMA_MAX_Q_MAX, udma_name, 0,
				   V2_ALLOWED_DESC_PER_PACKET + 1, true); // we add one to allow for MD descriptor
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = sdma_init_engine(sdma_base);
	if (ret) {
		pr_err("SDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
done:
	return ret;
}

extern int dma_bar0_blocked_one_engine(u64 base, u64 off);

int v2_dma_bar0_blocked(u64 off)
{
	int eid;
	// check NC 0
	u64 start_off = V2_APB_SENG_0_UDMA_0_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	u64 end_off = V2_APB_SENG_0_UDMA_15_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_UDMA_SIZE;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V2_DMA_ENG_PER_NC; eid++) {
			if (dma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V2_APB_SENG_UDMA_SIZE;
		}
	}
	// check NC 1
	start_off = V2_APB_SENG_1_UDMA_0_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	end_off = V2_APB_SENG_1_UDMA_15_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET + V2_APB_SENG_UDMA_SIZE;
	if (off >= start_off && off < end_off) {
		for (eid = 0; eid < V2_DMA_ENG_PER_NC; eid++) {
			if (dma_bar0_blocked_one_engine(start_off, off)) {
				return -1;
			}
			start_off += V2_APB_SENG_UDMA_SIZE;
		}
	}
	// check D2H
	start_off = V2_APB_D2H_UDMA_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	end_off = start_off + V2_APB_SENG_UDMA_SIZE;
	if (dma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}
	// check H2D
	start_off = V2_APB_H2D_UDMA_BASE - V2_APB_BASE + V2_PCIE_BAR0_APB_OFFSET;
	end_off = start_off + V2_APB_SENG_UDMA_SIZE;
	if (dma_bar0_blocked_one_engine(start_off, off)) {
		return -1;
	}
	return 0;
}

