// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <udma/udma.h>
#include "address_map.h"
#include "tdma.h"

int v1_dma_init(void __iomem *bar0, struct udma *udma, int eng_id)
{
	char udma_name[UDMA_INSTANCE_NAME_LEN];
	int ret = 0;
	void __iomem *udma_base[V1_NUM_DMA_ENG_PER_DEVICE];
	void __iomem *tdma_base[V1_NUM_DMA_ENG_PER_DEVICE];
	const u64 teng_udma_base[] = { P_0_APB_TENG_0_UDMA_0_RELBASE, P_0_APB_TENG_1_UDMA_0_RELBASE,
				       P_0_APB_TENG_2_UDMA_0_RELBASE,
				       P_0_APB_TENG_3_UDMA_0_RELBASE };
	const u64 teng_tdma_base[] = { P_0_APB_TENG_0_TDMA_0_RELBASE, P_0_APB_TENG_1_TDMA_0_RELBASE,
				       P_0_APB_TENG_2_TDMA_0_RELBASE,
				       P_0_APB_TENG_3_TDMA_0_RELBASE };

	int nc_id = eng_id / V1_DMA_ENG_PER_NC;
	int eid = eng_id % V1_DMA_ENG_PER_NC;
	udma_base[eng_id] = (void __iomem *)bar0 + teng_udma_base[nc_id] + (eid * P_0_APB_TENG_0_UDMA_0_SIZE);
	tdma_base[eng_id] = (void __iomem *)bar0 + teng_tdma_base[nc_id] + (eid * P_0_APB_TENG_0_TDMA_0_SIZE);

	snprintf(udma_name, UDMA_INSTANCE_NAME_LEN, "UDMA_ENG_%d", eng_id);
	ret = udma_m2m_init_engine(udma, udma_base[eng_id], DMA_MAX_Q_MAX, udma_name, 0, V1_ALLOWED_DESC_PER_PACKET);
	if (ret) {
		pr_err("UDMA ENG:%d init failed\n", eng_id);
		goto done;
	}
	ret = tdma_init_engine(tdma_base[eng_id]);
	if (ret) {
		pr_err("TDMA ENG:%d init failed\n", eng_id);
		goto done;
	}

done:
	return ret;
}
