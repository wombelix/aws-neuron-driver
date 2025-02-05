#include <udma/udma.h>

const static u64 udma_blocked[] = { offsetof(struct udma_rings_regs, drbp_low), offsetof(struct udma_rings_regs, drbp_high),
									offsetof(struct udma_rings_regs, crbp_low), offsetof(struct udma_rings_regs, crbp_high),
									offsetof(struct udma_rings_regs, drtp_inc) };

int dma_bar0_blocked_one_engine(u64 base, u64 off)
{
	int qid, dir;
	// check m2s and s2m
	for (dir = 0; dir < 2; dir++) {
		u64 q_start;
		u64 q_size = sizeof(union udma_q_regs);
		if (dir == 0) { // m2s
			q_start = base + offsetof(struct unit_regs_v4, m2s); // start of m2s block
			q_start += offsetof(struct udma_m2s_regs_v4, m2s_q);     // start of q registers
		} else { // s2m
			q_start = base + offsetof(struct unit_regs_v4, s2m); // start of s2m block
			q_start += offsetof(struct udma_s2m_regs_v4, s2m_q);     // start of q registers
		}
		for (qid = 0; qid < DMA_MAX_Q_V4; qid++) {
			u64 q_off = q_start + q_size * qid;
			int i;
			for (i = 0; i < sizeof(udma_blocked)/sizeof(udma_blocked[0]); i++) {
				if (off == q_off + udma_blocked[i]) {
					return -1;
				}
			}
		}
	}
	return 0;
}
