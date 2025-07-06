// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

/*
 * Pod Election framework
 *
 *   The pod election framework validates connectivity between pod nodes and performs an election to determine node ids
 *   for the nodes within the pod along with the unique id of the pod.
 *
 *   There's nothing physically or materially that changes when we perform a pod election, it's more an agreement between
 *   software entities on the pod that they want to cooperate and in that spirit, assign Ids that are used to map resources
 *   and their associated connectivity within the pod.
 *
 *   A pod election triggers this "resource assignement" process.  There are more details elsewhere that describe some
 *   topology details. Here we'll just discuss the election process and node id assignment.
 *
 *   The election process requires resources (DMA engines) across all devices on an instance.  The DMA engines  are also used 
 *   by the runtime for model execution, so election has to occur before runtime starts executing models.  So the election
 *   software has to logically own (gather) all devices before it performs an election.  Aka you can't have a model executing
 *   on a neuron device during the election.
 *
 *   There's two ways in which we initiate an election. The first (option) is at driver load time, the second option is "on demand" when runtime
 *   attaches to the driver through nrt_init().
 *
 *   Election at driver load time is performed immediately after reset, which has the benefit of guarenteeing runtime access is blocked
 *   until the election is complete.  On demand Election (at nrt_init), requires the driver to have a means of waiting until all the 
 *   processes that have called nrt_init() own all the neuron cores, allowing the election to proceed.
 *
 *   Ok, so how does the election work?
 *
 *   - There are 16 devices on a node.  One device is designated as the primary (device 0) and is responsible for overall election duties,
 *     the other 15 are secondaries and are responsible for checking their neighbor links and reporting back to the primary.
 *
 *   - Secondaries report if (a) they reset successfully, (b) there links are good (c), their link is bad, 
 *     or (d) they aren't wired to the right pod neighbors
 *
 *   - Election operations are state driven to maintain consistency.
 *
 *   - The election process needs to be (more or less) transactional (ACID)
 * 
 *   - Election data:
 *     - election status - cleared at the start of a pod control request, set at end of a pod control request
 *     - election data   - this is data that a node has collected and wants to show to its neighbors.  It only
 *                         lives during the election process and is zeroed out after a pod control request
 *                         completes.
 *
 *   - Election flow (primary)
 *     - primary reads it's own serial number
 *     - primary reads neighbors serial numbers and checks link connectivity is correct
 *     - If neighbor serial number reads are successful and match up on link pairs
 *       - update local copy of neighbor serial numbers so our neighbors know we successfully read their 
 *         serial numbers.
 *       - next check neighbor's local copy to see if they successfully configured.
 *     - otherwise, fail
 *     - primary receives reports back from secondaries to ensure our node connectivity is good.  
 *     - if secondaryies report back good
 *       - update election status to success
 *     - otherwise fail
 *     - read election status from neighbors 
 *     - if neighbors report success, 
 *       - determine node id and pod unique id.
 *       - set pod state to "pod"
 *     - if any fail, clear our copy of data and set election status to fail
 *
 *   - Election flow (secondary)
 *     - largely the same as the primary.  Just vetting links
 *
 *   - other election notes
 *     - the primary has a timeout on it's wait for secondaries to report
 *     - all waits piggyback on the reset timeout mechanism so that in the event of a drivver unload, the 
 *       election process is notified so it can abort, allowing the driver to unload.
 *
 *   APIs - Pod Control and Status
 *   - The pod control APIs can (a) request to start an election, (b) request to make the node standalone
 *     (c) request to kill the election.  Any kill or standalone request basically stops an election.
 *     Start election requests will wait until all the cores have been claimed by processes requesting
 *     an election (see above).
 *
 *   General thoughts:
 *   - Logically we only need to run the election itself to successful completion once.  At this point
 *     we know the topology of the nodes in the Pod which is orthogonal to whether or not the software
 *     running on the pod nodes desire to be part of the cluster.
 *   - After that we just need to check if the links are alive and kicking which has similar characteristics
 *     to the election process link checks (which is almost a mini-election).  Maybe we just rely on runtime
 *     and collectives timeouts to catch these issues after a successful election.
 *   - So this all begs the question, why not just run the election once, then allow
 *     runtime to set pod vs. non-pod operating mode.  If the election is successful, you can 
 *     set pod or non-pod mode, if the election fails, you can only set non-pod mode.
 *
 *   Opens:
 *   - do we want to do anything special for "user aborts" - driver unloads, kills, of the election, vs. hard
 *     errors like link failures and miss-wiring?  For miss-wiring and link failures, we could permanently 
 *     populate the neighbor data (vs we currently always clear it) to force neighbor failures.  This seems
 *     a bit unnecessary?
 */
#include "share/neuron_driver_shared.h"
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/mutex.h>

#include "udma/udma.h"
#include "sdma.h"
#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "../neuron_fw_io.h"
#include "../neuron_pci.h"
#include "../neuron_trace.h"
#include "../neuron_ring.h"
#include "../neuron_mempool.h"
#include "../neuron_crwl.h"
#include "neuron_pelect.h"

/*
 * Pod ctl for 
 * - control of when election is triggered
 * - fault injection of various failure scenarios
 */
#define NPE_POD_CTL_RST_SKIP_ELECTION  (1<<0)
#define NPE_POD_CTL_SPOOF_BASE         (1<<1)
#define NPE_POD_CTL_FAULT_SEC_RST_FAIL (1<<2)
#define NPE_POD_CTL_FAULT_SEC_LNK_FAIL (1<<3)
#define NPE_POD_CTL_FAULT_PRI_LNK_FAIL (1<<4)
#define NPE_POD_CTL_SKIP_NBR_CPY_READ  (1<<5)
#define NPE_POD_CTL_VERBOSE            (1<<6)

/* Disable pod auto election.  We currently are using on demand election */
int pod_ctl = NPE_POD_CTL_RST_SKIP_ELECTION;
module_param(pod_ctl, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(pod_ctl, "pod election control");

#define NPE_RETRY_WAIT_MS 1000
#define NPE_RETRY_WAIT_REPORTING_INTERVAL 10

// max time to wait for secondaries to respond (5 min)
//
#define NPE_SECONDARY_REPORT_MAX_TIME (300 * 1000)
#define NPE_SECONDARY_REPORT_WAIT_MS 1000

#define NPE_NBR_DATA_READ_MAX_TOTAL_WAIT_TIME_MS (1000 * 600)

#define NPE_ELECTION_DEVICE_INDEX 0

/**
 * internal state of the pod election module
 *
 */
enum neuron_pod_state_internal {
	NEURON_NPE_POD_ST_INIT = 0,             	// initializing - before pod formation
	NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS = 1,	// on demand election in progress
	NEURON_NPE_POD_ST_ELECTION_SUCCESS = 2, 	// successful pod formation
	NEURON_NPE_POD_ST_ELECTION_FAILURE = 3, 	// failure forming pod
	NEURON_NPE_POD_ST_WAIT_QUIESCE = 4,     	// quiescing before trying to reform pod (cleanup election data to known state)
};

/**
 * election status reported in miscram
 *
 */
enum neuron_pod_election_sts {
	NEURON_NPE_POD_ELECTION_STS_INIT = 0,		// initializing - before pod formation
	NEURON_NPE_POD_ELECTION_STS_SUCCESS = 1, 	// successful pod formation
	NEURON_NPE_POD_ELECTION_STS_FAILURE = 2, 	// failure forming pod
};

struct ndhal_v3ext_pelect {
	atomic_t election_core_cnt;		// count to track cores participating in election.  Used to trigger election start
	atomic_t sec_good_cnt;    		// count of secondary devices w/ good pod links
	atomic_t sec_bad_cnt;     		// count of secondary devices w/ bad pod links
	atomic_t sec_dead_cnt;    		// count of secondary devices w/ that failed to reset
	struct mutex lock;          	// pod control api lock
	struct neuron_device *nd; 		// link back to nd used election
	int pod_state_internal;   		// state of the pod
	int node_id;			  		// node id
	u64  pod_serial_num;	  		// serial number of the pod (node 0's serial number)
	ktime_t nbr_data_read_to_start; // timeout start time for neighbor data read
	u64  nbr_data_read_timeout;		// timeout for the neighbor data read
	wait_queue_head_t q;      		// queue to wait on for election completion
};

/**
 * pod election & state tracking struct.
 *   TODO - probably should encapsulate as dhal v3 extension.
 *
 */
static struct ndhal_v3ext_pelect ndhal_pelect_data = {
	.election_core_cnt = ATOMIC_INIT(0),
 	.sec_good_cnt = ATOMIC_INIT(0),
 	.sec_bad_cnt = ATOMIC_INIT(0),
	.sec_dead_cnt = ATOMIC_INIT(0),
	.lock = __MUTEX_INITIALIZER(ndhal_pelect_data.lock),
	.nd = NULL,
	.pod_state_internal = NEURON_NPE_POD_ST_INIT,
	.node_id = -1,
	.pod_serial_num = 0,
	.nbr_data_read_to_start = 0,
	.nbr_data_read_timeout = NPE_NBR_DATA_READ_MAX_TOTAL_WAIT_TIME_MS,
	.q = __WAIT_QUEUE_HEAD_INITIALIZER(ndhal_pelect_data.q)
};

/**
 * pod neighbor io resource tracking structure
 *
 *   all the resources needs to send a neighbor read request to miscram.
 *   If we wanted to encapsulate more, it could include ops for init/destroy/read/read_ack
 *
 */
typedef struct pod_neighbor_io {
	struct neuron_device *nd;
	u32 eng_id;
	u32 ring_size;
	size_t data_size;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rx_mc;
	struct mem_chunk *data_mc;
} pod_neighbor_io_t;

static inline int npe_atomic_add_return(int i, atomic_t * v)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	return i + atomic_fetch_add(i, v);
#else
	return atomic_add_return(i, v);
#endif 
}

static bool npe_pod_ctl_is_set(int value)
{
	return (pod_ctl & value);
}

static bool npe_election_canceled(void)
{
	return (ndhal_pelect_data.pod_state_internal > NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS);
}

/**
 * npe_pod_neighbor_io_init
 *   
 *   initialize the neighbor io structure - which is basically all the stuff
 *   you need to do a dma.
 *
 */
static int npe_pod_neighbor_io_init(pod_neighbor_io_t* pnio, struct neuron_device *nd, u32 eng_id)
{
	int ret;

	pnio->nd = nd;		
	pnio->eng_id = eng_id;		
	pnio->ring_size = 1024;		
	pnio->data_size = PAGE_SIZE;		

	ret = ndmar_eng_init(nd, eng_id);
	if (ret) {
		pr_err("pod election io dma init failed");
		goto done;
	}

	ret = mc_alloc_align(nd, MC_LIFESPAN_LOCAL, pnio->ring_size * sizeof(union udma_desc), 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_MISC_HOST, &pnio->tx_mc);
	if (ret) {
		pr_err("pod election io memory allocation failed");
		goto done;
	}
	
	ret = mc_alloc_align(nd, MC_LIFESPAN_LOCAL, pnio->ring_size * sizeof(union udma_desc), 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_MISC_HOST, &pnio->rx_mc);
	if (ret) {
		pr_err("pod election io memory allocation failed");
		goto done;
	}
	
	ret = mc_alloc_align(nd, MC_LIFESPAN_LOCAL, pnio->data_size, 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_MISC_HOST, &pnio->data_mc);
	if (ret) {
		pr_err("pod election io memory allocation failed");
		goto done;
	}

	ret = ndmar_queue_init(nd, pnio->eng_id, 0, pnio->ring_size, pnio->ring_size, pnio->tx_mc, pnio->rx_mc, NULL, 0, true);
	if (ret) {
		pr_err("pod election io queue init failed");
		goto done;
	}

done:
	return ret;
}

static void npe_pod_neighbor_io_destroy(pod_neighbor_io_t* pnio)
{
	ndmar_queue_release(pnio->nd, pnio->eng_id, 0);

	if (pnio->tx_mc) {
		mc_free(&pnio->tx_mc);
	}
	if (pnio->rx_mc) {
		mc_free(&pnio->rx_mc);
	}
	if (pnio->data_mc) {
		mc_free(&pnio->data_mc);
	}
}

/** 
 * npe_pod_neighbor_io_read_ack_completed()
 *
 *    Ack the read descriptors
 */
static void npe_pod_neighbor_io_read_ack_completed(struct ndma_eng *eng, int qid, u32 desc_cnt)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, qid, UDMA_RX, &rxq);
	udma_cdesc_ack(rxq, desc_cnt);
	udma_cdesc_ack(txq, desc_cnt);
}

/** 
 * npe_pod_neighbor_io_read()
 *
 *   Read miscram from a neighbor
 *
 */
static int npe_pod_neighbor_io_read(pod_neighbor_io_t* pnio, u32 *buf, u64 offset, u32 size)
{
	int ret;
	int i;
	int loop;
	u32 *data_va;
	struct ndma_eng   *eng   = &pnio->nd->ndma_engine[pnio->eng_id];
	struct ndma_queue *queue = &eng->queues[0];
	struct ndma_ring  *ring  = &queue->ring_info;
	u64 engid_2_b0_base[] = {V3_PCIE_B0_0_BASE, V3_PCIE_B0_1_BASE, V3_PCIE_B0_2_BASE, V3_PCIE_B0_3_BASE};
	u64 base_addr;
	
	// size check
	if (size + sizeof(*data_va) * 2 > pnio->data_size) {
		return -E2BIG;
	}
	
	data_va = (u32*)pnio->data_mc->va;	
	if (npe_pod_ctl_is_set(NPE_POD_CTL_SPOOF_BASE)) {
		base_addr = 0;
	} else {
		base_addr = engid_2_b0_base[pnio->eng_id / V3_NUM_DMA_ENG_PER_SENG];
	}

	// clear memory and setup completion data
	//	
	memset(data_va, 0, size + sizeof(*data_va) * 2);
	data_va[0] = 1;
	data_va[1] = 0;

	// create remote read descriptors
	//
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, base_addr + V3_APB_MISC_RAM_OFFSET + offset, 
			                        (pnio->data_mc->pa+8) | ndhal->ndhal_address_map.pci_host_base, size, UDMA_M2M_BARRIER_WRITE_BARRIER, false);

	if (ret) {
		pr_err("failed to create dma descriptor");
		return ret;
	}

	// create completion descriptor
	//
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, 
									(pnio->data_mc->pa+0) | ndhal->ndhal_address_map.pci_host_base, 
									(pnio->data_mc->pa+4) | ndhal->ndhal_address_map.pci_host_base, 4, UDMA_M2M_BARRIER_WRITE_BARRIER, false);
	if (ret) {
		pr_err("failed to create dma descriptor");
		return ret;
	}
	mb();

	ret = udma_m2m_copy_start(&eng->udma, ring->qid, 2, 2);

	// loop waiting for completion
	//
	loop = 1000;
	for (i=0; i < loop; i++) {
		volatile u32 *dst = (volatile u32 *)(&data_va[1]);
		u32 dst_val = READ_ONCE(*dst);
		if (dst_val == 1) {
			// ack completed descriptors
			npe_pod_neighbor_io_read_ack_completed(eng, 0, 2);
			// copy out data		
			memcpy(buf, &data_va[2], size);
			return 0;
		}	
		udelay(4);
	}

	pr_info("neighbor read failed on eng: %d", pnio->eng_id);
	return -EIO;
}

/** 
 * npe_read_neighbor_serial_numbers()
 *
 *   read neighbor serial numbers over b-links via dma
 *
 *   caller passes in a 2x2 matrix of pnio structs that represent the neighbor 
 *   pcie b link pairs.  Basically we read the serial numbers over the link
 *   pairs, verify the wiring is correct (link pair reads the same serial number from neighbor)
 *   and report the serial numbers back.
 *
 */
static int npe_read_neighbor_serial_numbers(pod_neighbor_io_t pnio[][2],  u64 * nbr_serial_number)
{
	int ret = 0;
	int retry_cnt = 0;
	struct neuron_device *nd = pnio[0][0].nd;

	while (1) {
		int i,j;
		u32 tmp[2][2];  // temp buffer for storing serial number data read

		for (i=0; i<2; i++) {
			if (nbr_serial_number[i] != 0ull) continue;
	
			for (j=0; j<2; j++) {
				ret = npe_pod_neighbor_io_read(&(pnio[i][j]), tmp[j], FW_IO_REG_SERIAL_NUMBER_LO_OFFSET, sizeof(u64));
				if (ret) {
					pr_err("nd%02d: Read pod neighbor serial number failed for seng link %d\n", nd->device_index, pnio[i][j].eng_id / V3_NUM_DMA_ENG_PER_SENG);
					goto done;
				} 
			}

			// check if we got valid data, if not, neighbor is likely not up yet so we'll retry
			//
			if ((tmp[0][0] != 0xdeadbeef) && (tmp[1][0] != 0xdeadbeef) && (tmp[0][0] != 0) && (tmp[1][0] != 0)) {
				if (memcmp(tmp[0], tmp[1], 8) != 0) {
					pr_err("nd%02d: Serial numbers on %s link pair don't match: %08x%08x vs.  %08x%08x\n", nd->device_index, 
							(i==0) ? "left" : "right", tmp[0][1], tmp[0][0], tmp[1][1], tmp[1][0]);
					ret = -EPIPE;
					goto done;
				}
				nbr_serial_number[i] = ((uint64_t)tmp[0][1])<<32 | (uint64_t)tmp[0][0];
			}
		}
		
		if ((nbr_serial_number[0] != 0ull) && (nbr_serial_number[1] != 0ull)) {
			break;
		}

		// check for driver unload
		if (nr_msleep_stoppable(nd, NPE_RETRY_WAIT_MS)) {
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: pod waiting on neigbors", nd->device_index);
		}

		// check for user cancel request
		if (npe_election_canceled()) {
			ret = -EINTR;
			goto done;
		}
	}
done:
	return ret;
}

/**
 * npe_read_neighbor_election_data()
 *
 *    read election data from neighbor, which is basically the serial numbers of their
 *    neighbors (which we need to determine connectivity)
 *
 */
static int npe_read_neighbor_election_data(pod_neighbor_io_t pnio[][2],  u64 nbr_serial_number_copy[][2])
{
	int ret = 0;
	int retry_cnt = 0;
	struct neuron_device *nd = pnio[0][0].nd;

	memset(nbr_serial_number_copy, 0, sizeof(nbr_serial_number_copy[0])*2);

    while (1) {
		int i;
		u32 tmp[4] = {0};

        for (i=0; i<2; i++) {
			ret = npe_pod_neighbor_io_read(&(pnio[i][0]), tmp, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, sizeof(tmp));
            if (ret) {
            	pr_err("nd%02d: Read pod neighbor election data failed for seng link %d\n", nd->device_index, pnio[i][0].eng_id / V3_NUM_DMA_ENG_PER_SENG);
				goto done;
            } 

			// check if last word of the copy is valid
			//
            if ((tmp[3] != 0) && (tmp[3] != 0xdeadbeef)) {
                nbr_serial_number_copy[i][0] = ((uint64_t)tmp[0] << 32) | (uint64_t)tmp[1];
                nbr_serial_number_copy[i][1] = ((uint64_t)tmp[2] << 32) | (uint64_t)tmp[3];
			}
        }

		// got both neighbors election data
		//
        if ((nbr_serial_number_copy[0][0] != 0) && (nbr_serial_number_copy[1][0] != 0)) {
            break;
		}

		if (nr_msleep_stoppable(nd, NPE_RETRY_WAIT_MS)) {
            pr_info("nd%02d: user aborted pod election by unloading driver", nd->device_index);
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: pod waiting on neigbor to update election data", nd->device_index);
		}

		if (npe_election_canceled()) {
			ret = -EINTR;
			goto done;
		}

		// check for read neighbor data timeout
		if ((ndhal_pelect_data.nbr_data_read_to_start) && 
			(ktime_to_ms(ktime_sub(ktime_get(), ndhal_pelect_data.nbr_data_read_to_start)) > ndhal_pelect_data.nbr_data_read_timeout)) {
			pr_info("nd%02d: read timeout barfed %llu", nd->device_index, ndhal_pelect_data.nbr_data_read_timeout);
			ret = -EINTR;
			goto done;
		}
	}

done:
	return ret;
}

/**
 * npe_read_neighbor_read_election_status()
 *
 *    read the neighbor's election status - indicator that their node has successfully completed
 *    the election process
 */
static int npe_read_neighbor_read_election_status(pod_neighbor_io_t pnio[][2], u32 *election_status, u32 prev_election_status)
{
	int ret = 0;
	int retry_cnt = 0;
	struct neuron_device *nd = pnio[0][0].nd;

    memset(election_status, 0, sizeof(u32)*2);

    while (1) {
		int i;
		u32 tmp = {0};

        for (i=0; i<2; i++) {
			ret = npe_pod_neighbor_io_read(&(pnio[i][0]), &tmp, FW_IO_REG_POD_ELECTION_STS, sizeof(tmp));
            if (ret) {
            	pr_err("nd%02d: Read pod neighbor election status failed for seng link %d\n", nd->device_index, pnio[i][0].eng_id / V3_NUM_DMA_ENG_PER_SENG);
				goto done;
            } 

			// check if data is valid
			//
            if ((tmp != prev_election_status) && (tmp != 0xdeadbeef)) {
                election_status[i] = tmp;
			}
        }

		// got both neighbor's election status? 
		//
        if ((election_status[0] != prev_election_status) && (election_status[1] != prev_election_status)) {
            break;
		}

		if (nr_msleep_stoppable(nd, NPE_RETRY_WAIT_MS)) {
			// Note: This could possibly leave our neighbors in pod state while this node is not in pod state as it's logically an ABORT transaction.
            pr_info("nd%02d: user aborted pod election by unloading driver", nd->device_index);
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: pod waiting on neigbor to update status", nd->device_index);
		}

		if (npe_election_canceled()) {
			ret = -EINTR;
			goto done;
		}
	}

done:
	return ret;
}

static void npe_miscram_write(struct neuron_device *nd, u64 offset, u32 data)
{
	writel(data, nd->npdev.bar0 + V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + offset);
}

static void npe_miscram_neighbor_election_data_set(struct neuron_device *nd, u64 lh_val, u64 rh_val)
{
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, (lh_val>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_LO, lh_val & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_HI, (rh_val>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_LO, rh_val & 0xffffffff);
}

static void npe_miscram_neighbor_election_data_clr(struct neuron_device *nd)
{
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_LO, 0);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_HI, 0);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_LO, 0);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, 0);
}

/** npe_get_node_id() - determine node id and return it along w/ pod serial number
 *
 *    determine node id of a group of nodes connected in a ring topology.  
 *    Lowest node id is leader (0). Leader's two neighbors 1 & 2.  The 
 *    Lower of the Leader's two neighbors is 1.  Node ids 1 & 2 will 
 *    have their die addressing flipped.
 *
 */
static int npe_get_node_id(u64 self, u64 left, u64 right, u64 diagonal, u64 *pod_serial_number)
{
	if (self < diagonal) {
        // My diagonal node is not a leader.
		if ((self < left) && (self < right)) {
			*pod_serial_number = self;
            // I'm a leader
			return 0;
		}
    }  else {
    /* my diagnoal node is a leader or not */
        if ((diagonal < left) && (diagonal < right)) {
            *pod_serial_number = diagonal;
            /* my diagonal is a leader. I'm 3. */
            return 3;
        }
    }
    if (left < right) {
        //the other node in the same rack is a leader.
        *pod_serial_number = left;
        return 1;
    }
    /* the lead is in the different rack. */
    *pod_serial_number = right;
    return 2;
}

/** 
 * npe_wait_on_secondaries()
 *
 *   wait for secondary devices to report their status.  We need to know if
 *   all the devices have happy links.
 *
 */
static int npe_wait_on_secondaries(struct neuron_device *nd)
{
	int ret = 0;
	ktime_t start_time;

	start_time = ktime_get();

	do {
		if (atomic_read(&ndhal_pelect_data.sec_good_cnt) + atomic_read(&ndhal_pelect_data.sec_bad_cnt) + atomic_read(&ndhal_pelect_data.sec_dead_cnt) == 15) {
			if (atomic_read(&ndhal_pelect_data.sec_good_cnt) == 15) {
				goto done;
			}
			ret = -ENODEV;
			break;
		}
		if (nr_msleep_stoppable(nd, NPE_SECONDARY_REPORT_WAIT_MS)) {
			//driver unload
			ret = -EINTR;
			goto done;
		}

		if (npe_election_canceled()) {
			ret = -EINTR;
			goto done;
		}
		
	} while (ktime_to_ms(ktime_sub(ktime_get(), start_time)) < NPE_SECONDARY_REPORT_MAX_TIME);

	pr_err("nd%02d: not all secondaries reported good link status: link good %02d link bad %02d, reset failed %02d", nd->device_index,
		    atomic_read(&ndhal_pelect_data.sec_good_cnt), atomic_read(&ndhal_pelect_data.sec_bad_cnt), atomic_read(&ndhal_pelect_data.sec_dead_cnt));

done:
	// clear the counts
	//
	atomic_set(&ndhal_pelect_data.sec_good_cnt, 0);
	atomic_set(&ndhal_pelect_data.sec_bad_cnt, 0);
	atomic_set(&ndhal_pelect_data.sec_dead_cnt, 0);
	return ret;
}

/** 
 * npe_primary_device_do_election() - exec election on primary node
 *
 *   @nd               - device the election is being prosecuted on
 *   @reset_successful - flag indicating if device reset successfull
 *
 */
static int npe_primary_device_do_election(struct neuron_device *nd, bool reset_successful)
{
	int ret;
	int i;
	int node_id = -1;
	u32 routing_id;
	u64 serial_number;
	u64 diagonal;
	u64 pod_serial_number = 0;
	u64 nbr_serial_number[2] = {0};
	u64 nbr_serial_number_copy[2][2] = {0};
	u32 nbr_election_status[2] = {0};
	pod_neighbor_io_t pnio[2][2] = {0};

	ndhal_pelect_data.nd = nd;

	pr_info("nd%02d: pod election starting", nd->device_index);

	if (!reset_successful) {
		pr_info("nd%02d: election primary node failed to reset", nd->device_index);
		ret = -ENODEV;
		goto done;
	}

    // Read local routing id and serial number
	//
	ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
	if (ret) {
		pr_err("nd%02d: local routing id read failed", nd->device_index);
		goto done;
	}
	ret = fw_io_serial_number_read(nd->npdev.bar0, &serial_number);
	if (ret) {
		pr_err("nd%02d: local serial number read failed", nd->device_index);
		goto done;
	}

	pr_info("nd%02d: Routing id: %02d SerialNumber %016llx", nd->device_index, routing_id, serial_number);

	// Initialize neighbor io structures
    // Left
	ret = npe_pod_neighbor_io_init(&(pnio[0][0]), nd, 36);
	ret |= npe_pod_neighbor_io_init(&(pnio[0][1]), nd, 68);
    // Right
	ret |= npe_pod_neighbor_io_init(&(pnio[1][0]), nd, 4);
	ret |= npe_pod_neighbor_io_init(&(pnio[1][1]), nd, 100);
	if (ret) {
		pr_err("neighbor io initialization failed");
		goto done;
	}

	// clear election status before we start
	//
    npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_STS_INIT);
	
    // read neighbors serial numbers
    // 
	ret = npe_read_neighbor_serial_numbers(pnio,  nbr_serial_number);
	if (ret || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_PRI_LNK_FAIL)) {
		goto done;
	}

	pr_info("nd%02d: acquired pod neighbor serial numbers L: %016llx  R:  %016llx", nd->device_index, nbr_serial_number[0], nbr_serial_number[1]);

    // update LH/RH neighbor unique values in our local miscram copy 
    //
	npe_miscram_neighbor_election_data_set(nd, nbr_serial_number[0], nbr_serial_number[1]);

	if (npe_pod_ctl_is_set(NPE_POD_CTL_SKIP_NBR_CPY_READ)) {
		goto done;
	}

	// read neighbor's LH/RH copy of serial numbers to determine if neighbors successfully picked up both neighbors serial numbers
	// indicating we have a ring on the primary
    // 
	ret = npe_read_neighbor_election_data(pnio, nbr_serial_number_copy);
	if (ret) {
		goto done;
	}

    //  check cabling
    // 
	for (i=0; i<2; i++) {
    	if (nbr_serial_number_copy[i][0] <= 15) {
			u32 dev_id = (u32)(nbr_serial_number_copy[i][0]);
			pr_err("nd%02d: %c pod link is miss-wired to nd%02d (%016llx)", 
					nd->device_index, (i==0) ? 'L':'R', (dev_id > 15) ? 0 : dev_id, nbr_serial_number[i]);
			ret = -EPIPE;
		}
	}
	if (ret) {
		goto done;
	}

	diagonal = nbr_serial_number_copy[0][1];

    //  secondaries all good?
    // 
	ret = npe_wait_on_secondaries(nd);
    if (ret) {
		goto done;
	}
	
	pr_info("nd%02d: pod election - all secondary links good", nd->device_index);

	// set election status and read neighbor's election status
	//
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_STS_SUCCESS);
	
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status, NEURON_NPE_POD_ELECTION_STS_INIT);
	if (ret) {
		goto done;
	}

	if ((nbr_election_status[0] != NEURON_NPE_POD_ELECTION_STS_SUCCESS) || (nbr_election_status[1] != NEURON_NPE_POD_ELECTION_STS_SUCCESS)) {
		pr_err("nd%02d: election failed neighbor election status L: %s R: %s", nd->device_index,
				(nbr_election_status[0] == NEURON_NPE_POD_ELECTION_STS_SUCCESS) ? "success" : "failure",
				(nbr_election_status[1] == NEURON_NPE_POD_ELECTION_STS_SUCCESS) ? "success" : "failure");
		ret = -ENODEV;
		goto done;
	}

	// determine our node id
	//
    node_id = npe_get_node_id(serial_number, nbr_serial_number[0], nbr_serial_number[1], diagonal, &pod_serial_number);
	ret = 0;

done:
	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	npe_miscram_neighbor_election_data_clr(nd);

	// if election was successful, we set node_id, pod_serial_number, and flag pod
	// if unsuccessful, we set state to not a pod
	//
	if (ret) {
		npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_STS_FAILURE);
		ndhal_pelect_data.node_id = node_id;
		ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
		pr_info("nd%02d: pod election failed", nd->device_index);
	} else {
		ndhal_pelect_data.node_id = node_id;
		ndhal_pelect_data.pod_serial_num = pod_serial_number;
		ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_SUCCESS;

		pr_info("nd%02d: pod election complete.  node id: %d Pod Unique id: %016llx", nd->device_index, 
				ndhal_pelect_data.node_id, ndhal_pelect_data.pod_serial_num);
	}

    // notify secondary devices that election is complete
    // 
	wake_up_all(&ndhal_pelect_data.q);

	return ret;
}

/** 
 * npe_secondary_device_vet_n_wait()
 *
 *   Check the secondary's neighbors to see that they have the same device id
 *   and report success or failure.
 *   Wait for the primary device to complete the election process if we are
 *   multi-threaded.
 *
 *   @nd               - device the election is being prosecuted on
 *   @reset_successful - flag indicating if device reset successfull
 *
 */
static int npe_secondary_device_vet_n_wait(struct neuron_device *nd, bool reset_successful)
{
	int ret;
	int i;
	u64 nbr_serial_number[2] = {0};
	u64 nbr_serial_number_copy[2][2] = {0};
	u32 nbr_election_status[2] = {0};
	pod_neighbor_io_t pnio[2][2] = {0};

	if (!reset_successful || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_SEC_RST_FAIL)) {
		npe_atomic_add_return(1, &ndhal_pelect_data.sec_dead_cnt);
		return -1;
	}

	// Initialize neighbor io structures
	// Left
	ret = npe_pod_neighbor_io_init(&(pnio[0][0]), nd, 36);
	ret |= npe_pod_neighbor_io_init(&(pnio[0][1]), nd, 68);
	// Right
	ret |= npe_pod_neighbor_io_init(&(pnio[1][0]), nd, 4);
	ret |= npe_pod_neighbor_io_init(&(pnio[1][1]), nd, 100);
	if (ret) {
		pr_err("neighbor io initialization failed");
		goto done;
	}

	// clear election status before we start
	//
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_STS_INIT);

	// if neighbor reads are good the link is good (or at least wired in pairs)
	//
	ret = npe_read_neighbor_serial_numbers(pnio,  nbr_serial_number);
	if (ret) {
		goto done;
	}

	if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
		pr_info("nd%02d: acquired pod neighbor serial numbers L: %016llx  R:  %016llx", nd->device_index, nbr_serial_number[0], nbr_serial_number[1]);
	}

	// populate election data with device index to detect miss-cabling
	//
	npe_miscram_neighbor_election_data_set(nd, nd->device_index, nd->device_index);

	ret = npe_read_neighbor_election_data(pnio, nbr_serial_number_copy);
	if (ret) {
		goto done;
	}

	//  check cabling
	//
	for (i=0; i<2; i++) {
		if (nbr_serial_number_copy[i][0] != nd->device_index) {
			u32 dev_id = (u32)(nbr_serial_number_copy[i][0]);
			pr_err("nd%02d: %c pod link is miss-wired to nd%02d (%016llx)", 
					nd->device_index, (i==0) ? 'L':'R', (dev_id > 15) ? 0 : dev_id, nbr_serial_number[i]);
			ret = -EPIPE;
		}
	}

	// set election status and read neighbor's election status
	//
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_STS_SUCCESS);
	
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status, NEURON_NPE_POD_ELECTION_STS_INIT);
	if (ret) {
		goto done;
	}

	if ((nbr_election_status[0] != NEURON_NPE_POD_ELECTION_STS_SUCCESS) || (nbr_election_status[1] != NEURON_NPE_POD_ELECTION_STS_SUCCESS)) {
		pr_err("nd%02d: election failed neighbor election status L: %s R: %s", nd->device_index,
				(nbr_election_status[0] == NEURON_NPE_POD_ELECTION_STS_SUCCESS) ? "success" : "failure",
				(nbr_election_status[1] == NEURON_NPE_POD_ELECTION_STS_SUCCESS) ? "success" : "failure");
		ret = -ENODEV;
		goto done;
	}

done:
	if (ret || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_SEC_LNK_FAIL)) {
		npe_atomic_add_return(1, &ndhal_pelect_data.sec_bad_cnt);
	} else {
		npe_atomic_add_return(1, &ndhal_pelect_data.sec_good_cnt);
	}

	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	// clear miscram election data
	//
	npe_miscram_neighbor_election_data_clr(nd);
	
	if (!ret) {	
		// wait for the primary to wake us up if links are good - when we are running election from reset
		//
		wait_event_interruptible(ndhal_pelect_data.q, ndhal_pelect_data.pod_state_internal >= NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS);
	} else {
		// on failure set miscram status, even on secondaries
		//
		npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_STS_FAILURE);
	}
	
	return ret;
}

/** 
 * npe_election_exec_on_rst() 
 *
 *   If we are in pod state init (first reset at dirver load), perform an election.
 *
 */
int npe_election_exec_on_rst(struct neuron_device *nd, bool reset_successful)
{
	if (npe_pod_ctl_is_set(NPE_POD_CTL_RST_SKIP_ELECTION)) {
		if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_INIT) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE; // not pod formation
		}
		return 0;
	}
	if (ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_INIT) {
		return 0;
	}
	
	npe_miscram_neighbor_election_data_clr(nd);

	
	if (nd->device_index == NPE_ELECTION_DEVICE_INDEX) {
		ndhal_pelect_data.nbr_data_read_to_start = ktime_get();
		return npe_primary_device_do_election(nd, reset_successful);
	}
	return npe_secondary_device_vet_n_wait(nd, reset_successful);
}

void npe_cleanup(void)
{
	if (ndhal_pelect_data.nd != NULL) {
		npe_miscram_neighbor_election_data_clr(ndhal_pelect_data.nd);
		ndhal_pelect_data.nd = NULL;
	}
}

/**
 * npe_handle_no_reset_param()
 *
 *   If no reset param is in play, force pod election to known state that we would have if we 
 *   skipped the initial election.
 */
void npe_handle_no_reset_param(void)
{
	ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
}

/**
 * npe_get_pod_id()
 *
 *  return the pod id.  This is only valid if the election is complete
 *  (either successful or failed)
 */
int npe_get_pod_id(u8 *pod_id)
{
	memcpy(pod_id, &ndhal_pelect_data.pod_serial_num, sizeof(ndhal_pelect_data.pod_serial_num));
	return 0;
}

/**
 * npe_get_pod_status()
 *
 *    return state of the pod along w/ node id.
 */
int npe_get_pod_status(u32 *state, u8 *node_id)
{
	int ret = 0;

	switch (ndhal_pelect_data.pod_state_internal) {
		case NEURON_NPE_POD_ST_INIT:
		case NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS:
			*state = NEURON_POD_E_STATE_IN_PROGRESS;
			ret = -EBUSY;
			break;

		case NEURON_NPE_POD_ST_ELECTION_SUCCESS:
			*state = NEURON_POD_E_STATE_SUCCESS;
			break;

		case NEURON_NPE_POD_ST_ELECTION_FAILURE:
			//*state = NEURON_POD_E_STATE_FAILED;
			*state = NEURON_POD_E_STATE_STANDALONE;
			break;

		case NEURON_NPE_POD_ST_WAIT_QUIESCE:
			*state = NEURON_POD_E_STATE_QUIESCING;
			break;
		default: 
			ret = -EINVAL;
			break;
	}

	*node_id = ndhal_pelect_data.node_id;
	return ret;	
}

/**
 * npe_pod_ctrl() - on-demand request to change pod state
 *
 *   From nrt_init() this function is called to request execution in either a pod or non-pod configuration.  
 *   For a pod configuration on-demand request, we need agreement from all neuron core owners that they want 
 *   a pod.  Once we get that, we start an election.  
 *
 *   Another criteria is we can't start the election if we all the cores aren't owned...
 *
 * @pnd - array of neuron devices 
 *
 *
 *
 */
int npe_pod_ctrl(struct neuron_device **pnd, u32 ctrl, u32 timeout, u32 *state)
{
	int ret = 0;
	u8 node_id;
	int process_cores;
	int total_cores;

	// we use cwrl mark as a form of predicate lock on the cores
	//
    process_cores = ncrwl_current_process_range_mark_cnt();

	// kill request - requires no core ownership (as in you can't kill if you own a core)
	// This is racy by nature, so we just read count of cores trying to run election and kill the election
	// if we see any active participants...
	//
	if (ctrl == NEURON_NPE_POD_CTRL_REQ_KILL) {
		if ((process_cores == 0) && (atomic_read(&ndhal_pelect_data.election_core_cnt) != 0)) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
			wake_up_all(&ndhal_pelect_data.q);
		}
		npe_get_pod_status(state, &node_id);
		return 0;
	}

	// if you don't own a core, you don't get a vote.
	//
	if (!process_cores) {
		return -ESRCH;
	}

	mutex_lock(&ndhal_pelect_data.lock);

	if (ctrl == NEURON_NPE_POD_CTRL_REQ_STANDALONE) {
		// we have at least some cores so force state to non-pod
		//
		ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;

		if (atomic_read(&ndhal_pelect_data.election_core_cnt) != 0) {
			wake_up_all(&ndhal_pelect_data.q);
		}
		mutex_unlock(&ndhal_pelect_data.lock);
		ret = 0;

	} else if (ctrl == NEURON_NPE_POD_CTRL_REQ_POD) {
    	total_cores = npe_atomic_add_return(process_cores, &ndhal_pelect_data.election_core_cnt);

		//  If the call is the first trying to kick off the election, set election in process
		//
		if (total_cores == process_cores) {
			// now it's possible that we are trying to form a pod while it's in no-pod state...
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS;
			pr_info("recieved initial pod election request");
		}

		// run election process if we've grabbed all the cores.
		//
    	if (total_cores == V3_NC_PER_DEVICE * 16) {
			int i;

			pr_info("initiating pod election.  State: %d", ndhal_pelect_data.pod_state_internal );

			ndhal_pelect_data.nbr_data_read_timeout = timeout * 1000;
			ndhal_pelect_data.nbr_data_read_to_start = ktime_get();

       		for (i = 0; i < 16; i++) {
				ret = nr_wait(pnd[i], NEURON_RESET_REQUEST_ALL, true);
				if (ret) {
					pr_info("reset wait failed for nd%02d", pnd[i]->device_index);
					ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
					goto reset_failed;
				}
			}

			if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
				pr_info("vetting secondaries. Internal state: %d", ndhal_pelect_data.pod_state_internal);
			}

			// vet secondaries
			//
       		for (i = 1; i < 16; i++) {
				npe_secondary_device_vet_n_wait(pnd[i], true);
			}

			// run election on primary.
			//
			ret = npe_primary_device_do_election(pnd[0], true);
reset_failed:
			wake_up_all(&ndhal_pelect_data.q);
    		npe_atomic_add_return(-process_cores, &ndhal_pelect_data.election_core_cnt);
			mutex_unlock(&ndhal_pelect_data.lock);
    	} else {
			mutex_unlock(&ndhal_pelect_data.lock);
			ret = wait_event_interruptible(ndhal_pelect_data.q, ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS);
			
    		npe_atomic_add_return(-process_cores, &ndhal_pelect_data.election_core_cnt);
		}
		
		// return a consistent error value on failure (TODO improve this)
		//
		if (ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_ELECTION_SUCCESS) {
			pr_info("pod election failed");
			ret = -ENODEV;
		} else {
			pr_info("pod election successful");
		}
	} else {
		mutex_unlock(&ndhal_pelect_data.lock);
		pr_err("invalid pod control request value %d", ctrl);
		ret = -EINVAL;
	}

	npe_get_pod_status(state, &node_id);
	return ret;
}
