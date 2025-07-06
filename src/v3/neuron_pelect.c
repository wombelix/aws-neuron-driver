// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

/*
 * Pod Election framework (TODO - rename to nutd - Neuron UltraServer Topology Discovery)
 *
 *   The pod election framework validates connectivity between pod nodes and performs an election to determine node ids
 *   for the nodes within the pod along with the unique id of the pod.
 *
 *   There's nothing physically or materially that changes after we perform a pod election, it's more an agreement between
 *   software entities on the pod that they want to cooperate and in that spirit, assign Ids that are used to map resources
 *   and their associated connectivity within the pod in a particular way.
 *
 *   A pod election triggers this "node assignment" process.  There are more details elsewhere that describe some
 *   topology details. Here we'll just discuss the election process and node id assignment.
 *
 *   The election process requires the use of DMA engines across all devices on an instance.  The DMA engines are also used 
 *   by the runtime for model execution, so election has to occur before runtime starts executing models.  The election
 *   software has to logically own (gather) all devices before it performs an election.  Aka you can't have a model executing
 *   on a neuron device during the election.
 *
 *   The election has to be prosecuted on all four nodes at the same time (at least initially) because the nodes need to
 *   exchange information to discover topology and perform node assignment.
 *
 *   There are two methods by which we can initiate an election. The first (option) is at driver load time, the second option is "on demand" 
 *   triggered by a standalone program using pod control APIs.  Election at driver load time is performed immediately after all 
 *   devices have successfully completed reset.  For on demand Election, the standalone program should check if all cores are free
 *   prior to initiating the election via the pod control ioctl.
 *
 *   Election results are stored in miscram so that if the driver is unloaded/reloaded upgraded, the driver can reload the election
 *   results from miscram.
 *
 *   There is also an pod control ioctl that allows election results to be ignored/suppressed so that if an application wants to, it
 *   can treat the node as a standard trn2 server.  This "suppression" lasts only while the application is active (technically it 
 *   ends when crwl mark is zero).
 *
 *   Ok, so how does the election work?
 *
 *   There are logically two aspects to the election process.
 *     1. Acquistion of DMA resources (only when crwl mark = 0)
 *     2. Prosecuting the actual election down in the driver.
 *        checking links, determining topology, etc.
 *
 *   Acquiring DMA resources
 *    - The driver uses crwl "mark" count to track ownership.  The election code uses changes in the "mark" count
 *      to track allocation and freeing of cores to drive internal state.
 *    - A new election is allowed to start only after core ownership has gone to zero.
 *    - An election will abort on a timeout or (b) if an election kill request is received.
 *    - The application can poll for election completion from user space.
 *
 *   Prosecuting the election:
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
 *     - election status  - cleared at the start of a pod election.
 *     - election node_id - cleared at the start of a pod election, set at end of a pod election.
 *     - election pod_id  - set at the end of a pod election.
 *     - election data    - this is data that a node has collected and wants to show to its neighbors (both neighbor's serial numbers).
 *                          this data is populated during the election and cleared if the election fails. 
 *
 *   - Election flow (primary)
 *     - primary reads it's own serial number
 *     - primary reads neighbors serial numbers and checks link connectivity is correct
 *     - If neighbor serial number reads are successful and match up on link pairs
 *       - update local copy of neighbor serial numbers (election data) so our neighbors know we successfully read their 
 *         serial numbers.
 *       - next check neighbor's local copy (election data) to see if they successfully read their neighbors serial numbers.
 *     - otherwise, fail
 *     - primary also receives count of secondaries that passed their connectivity check.
 *     - if secondaryies report back good continue
 *     - otherwise fail
 *     - determine node id and pod unique id.
 *     - clear election data to prevent partial elections.
 *        - eg, if we left election data intact, and a neighbor rebooted and ran an election, it we cruise right through
 *          this flow and declare a successful election.  Technically there's nothing wrong with this, but it
 *          creates a inconsistent customer experience.
 *     - read election status from neighbors 
 *     - if neighbors report success, 
 *       - set pod state to "pod" and update miscram with pod serial number and our node id.
 *     - if any fail, clear our copy of data and set election status to fail
 *
 *   - Election flow (secondary)
 *     - Largely the same as primary in terms of connectivity checking.  They just don't do the node id and unique id step.
 *
 *   APIs - Pod Control and Status
 *   - The pod control APIs can 
 *     (a) request to start an election 
 *     (b) request to make the node single node (suppress election results)
 *     (c) request to kill the election.
 *
 *   Other notes:
 *   - Logically we only need to run the election itself to successful completion once.  At this point
 *     we know the topology of the nodes in the Pod which is orthogonal to whether or not the software
 *     running on the pod nodes desire to be part of the cluster.
 *   - If one node reboots and prosecutes an election again, it will succeed if it's neighbor's miscram contain
 *     successful results.
 *
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
#define NPE_POD_CTL_VERBOSE            (1<<2)
#define NPE_POD_CTL_FAULT_SEC_LNK_FAIL (1<<3)
#define NPE_POD_CTL_FAULT_PRI_LNK_FAIL (1<<4)
#define NPE_POD_CTL_SKIP_NBR_CPY_READ  (1<<5)
#define NPE_POD_CTL_CLR_MISCRAM        (1<<6)

/* Disable ultraserver auto election.  We currently are using on demand election */
int userver_ctl = 0;
module_param(userver_ctl, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(userver_ctl, "ultraserver election control");

/*TODO tmp until we do wholesale name change */
#define pod_ctl userver_ctl

/* default ultraserver election timeout 10 min */
int userver_etimeout = 600;
module_param(userver_etimeout, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(userver_etimeout, "ultraserver election timeout");

/*TODO tmp until we do wholesale name change */
#define pod_etimeout userver_etimeout

#define NPE_RETRY_WAIT_MS 1000
#define NPE_RETRY_WAIT_REPORTING_INTERVAL 10

/**
 * internal state of the pod election module
 *
 */
enum neuron_pod_state_internal {
	NEURON_NPE_POD_ST_INIT = 0,             	// initializing - before pod formation
	NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS = 1,	// on demand election in progress
	NEURON_NPE_POD_ST_ELECTION_SUCCESS = 2, 	// successful pod formation
	NEURON_NPE_POD_ST_ELECTION_FAILURE = 3, 	// failure forming pod or pod not formed yet
};

/**
 * election status reported in miscram
 *
 */
enum neuron_pod_election_mr_sts {
	NEURON_NPE_POD_ELECTION_MR_STS_INIT = 0,		// initializing - before pod formation
	NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS = 1, 	// successful pod formation
	NEURON_NPE_POD_ELECTION_MR_STS_FAILURE = 2, 	// failure forming pod
};

#define NPE_MR_STS_GET(v)  ((v) & 0xFFFF)
#define NPE_MR_NODE_ID_GET(v)  (((v)>>16) & 0xFFFF)
#define NPE_MR_STS_NODE_ID_SET(s,n) (((s) & 0xFFFF) | ((n)<<16))


struct ndhal_v3ext_pelect {
	struct mutex lock;          	// pod control api lock
	struct task_struct *thread; 	// election thread
	wait_queue_head_t wait_queue;	// election thread's wait queue
	volatile bool stop; 			// if set, election thread would exit the loop
	bool kill_election;				// if set, kill the election.
	bool suppress_election_results;	// if set, election results are suppressed temporarily
	int pod_state_internal;   		// state of the pod
	int node_id;			  		// node id
	u64  pod_serial_num;	  		// serial number of the pod (node 0's serial number)
	ktime_t nbr_data_read_to_start; // timeout start time for neighbor data read
	u64  nbr_data_read_timeout;		// timeout for the neighbor data read
	struct neuron_device *pnd[16]; 	// array of device pointers, populated at reset completion.
};

/**
 * pod election & state tracking struct.
 *   TODO - probably should encapsulate as dhal v3 extension.
 *
 */
static struct ndhal_v3ext_pelect ndhal_pelect_data = {
	.lock = __MUTEX_INITIALIZER(ndhal_pelect_data.lock),
	.thread = NULL,
	.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(ndhal_pelect_data.wait_queue),
	.stop = false,
	.kill_election = false,
	.suppress_election_results = false,
	.pod_state_internal = NEURON_NPE_POD_ST_INIT,
	.node_id = -1,
	.pod_serial_num = 0,
	.nbr_data_read_to_start = 0,
	.nbr_data_read_timeout = 0,
	.pnd = {NULL},
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

static bool npe_pod_ctl_is_set(int value)
{
	return (pod_ctl & value);
}

static bool npe_election_canceled(void)
{
	return (ndhal_pelect_data.kill_election || ndhal_pelect_data.stop);
}

static int npe_msleep_stoppable(uint32_t msec) 
{
	unsigned long timeout = msecs_to_jiffies(msec);

	while (timeout && !npe_election_canceled())
		timeout = schedule_timeout_interruptible(timeout);

	return jiffies_to_msecs(timeout);
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

		// check for driver unload or canceled election
		if (npe_msleep_stoppable(NPE_RETRY_WAIT_MS)) {
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: pod waiting on neigbors", nd->device_index);
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

		// check for driver unload or canceled election
		if (npe_msleep_stoppable(NPE_RETRY_WAIT_MS)) {
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: pod waiting on neigbor to update election data", nd->device_index);
		}

		// check for read neighbor data timeout
		if ((ndhal_pelect_data.nbr_data_read_to_start) && 
			(ktime_to_ms(ktime_sub(ktime_get(), ndhal_pelect_data.nbr_data_read_to_start)) > ndhal_pelect_data.nbr_data_read_timeout)) {
			pr_info("nd%02d: neighbor election data read timeout after %llu seconds", nd->device_index, ndhal_pelect_data.nbr_data_read_timeout);
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

			// extract status from status/node_id pair
			tmp = NPE_MR_STS_GET(tmp);

			// check if status is valid
			//
            if ((tmp != prev_election_status) && (tmp != 0xdeadbeef)) {
                election_status[i] = tmp;
			}
        }

		// got both neighbor's new election status?   This has a unimportant race with prev election status explained in the header of this file. 
		//
        if ((election_status[0] != prev_election_status) && (election_status[1] != prev_election_status)) {
            break;
		}

		// check for driver unload or canceled election
		if (npe_msleep_stoppable(NPE_RETRY_WAIT_MS)) {
			// Note: This could possibly leave our neighbors in pod state while this node is not in pod state as it's logically an ABORT transaction.
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: pod waiting on neigbor to update status", nd->device_index);
		}
	}

done:
	return ret;
}

static u32 npe_miscram_read(struct neuron_device *nd, u64 offset)
{
	return readl(nd->npdev.bar0 + V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + offset);
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

static void npe_miscram_sts_node_id_set(struct neuron_device *nd, enum neuron_pod_election_mr_sts sts, int node_id)
{
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NPE_MR_STS_NODE_ID_SET(sts,node_id));
}

static void npe_miscram_sts_node_id_get(struct neuron_device *nd, enum neuron_pod_election_mr_sts *sts, int *node_id)
{
	u32 tmp;
	tmp = npe_miscram_read(nd, FW_IO_REG_POD_ELECTION_STS);
	*sts = NPE_MR_STS_GET(tmp);
	*node_id = NPE_MR_NODE_ID_GET(tmp);
}

static void npe_miscram_pod_sernum_set(struct neuron_device *nd, u64 pod_sernum)
{
	npe_miscram_write(nd, FW_IO_REG_POD_SERNUM_HI, (pod_sernum>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_POD_SERNUM_LO, pod_sernum & 0xffffffff);
}

static void npe_miscram_pod_sernum_get(struct neuron_device *nd, u64 *pod_sernum)
{
	*pod_sernum = ((u64)npe_miscram_read(nd, FW_IO_REG_POD_SERNUM_HI))<<32 | (u64)npe_miscram_read(nd, FW_IO_REG_POD_SERNUM_LO);
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
 * npe_primary_device_do_election() - exec election on primary node
 *
 *   @nd                 - device the election is being prosecuted on
 *   @secondary_good_cnt - count of secondary devices that passed link checks
 *
 */
static int npe_primary_device_do_election(struct neuron_device *nd, int secondary_good_cnt)
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

	pr_info("nd%02d: pod election starting", nd->device_index);

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
	npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_INIT, -1);
	
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
	if (secondary_good_cnt != 15) {
		pr_err("Only %d out of 15 secondary nodes reported good links", secondary_good_cnt);
		ret = -EPIPE;
		goto done;
	}
	
	pr_info("nd%02d: pod election - all secondary links good", nd->device_index);

	// determine our node id and pod serial number
	//
	node_id = npe_get_node_id(serial_number, nbr_serial_number[0], nbr_serial_number[1], diagonal, &pod_serial_number);
	ret = 0;

	// set election status, with bad node id
	//
	npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS, -1);

	// read neighbor's election status
	//	
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status, NEURON_NPE_POD_ELECTION_MR_STS_INIT);
	if (ret) {
		goto done;
	}

	// check neighbor's election status
	//
	if ((nbr_election_status[0] != NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) || (nbr_election_status[1] != NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS)) {
		pr_err("nd%02d: election failed neighbor election status L: %s R: %s", nd->device_index,
				(nbr_election_status[0] == NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) ? "success" : "failure",
				(nbr_election_status[1] == NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) ? "success" : "failure");
		ret = -ENODEV;
		goto done;
	}

done:
	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	// clear neighbor election data to prevent partial elections
	//
	npe_miscram_neighbor_election_data_clr(nd);

	// update node_id and pod_serial_number, even in the failed case.
	//
	if (ret) {
		npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_FAILURE, -1);
		npe_miscram_pod_sernum_set(nd, 0);
		ndhal_pelect_data.node_id = -1;
		ndhal_pelect_data.pod_serial_num = 0;
	} else {
		npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS, node_id);
		npe_miscram_pod_sernum_set(nd, pod_serial_number);
		ndhal_pelect_data.node_id = node_id;
		ndhal_pelect_data.pod_serial_num = pod_serial_number;
	}

	return ret;
}

/** 
 * npe_secondary_device_vet()
 *
 *   Check the secondary's neighbors to see that they have the same device id
 *   and report success or failure.
 *
 *   @nd - device the election is being prosecuted on
 *
 */
static int npe_secondary_device_vet(struct neuron_device *nd)
{
	int ret;
	int i;
	u64 nbr_serial_number[2] = {0};
	u64 nbr_serial_number_copy[2][2] = {0};
	u32 nbr_election_status[2] = {0};
	pod_neighbor_io_t pnio[2][2] = {0};

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
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_MR_STS_INIT);

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
	npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS, 0);
	
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status, NEURON_NPE_POD_ELECTION_MR_STS_INIT);
	if (ret) {
		goto done;
	}

	if ((nbr_election_status[0] != NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) || (nbr_election_status[1] != NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS)) {
		pr_err("nd%02d: election failed neighbor election status L: %s R: %s", nd->device_index,
				(nbr_election_status[0] == NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) ? "success" : "failure",
				(nbr_election_status[1] == NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) ? "success" : "failure");
		ret = -ENODEV;
		goto done;
	}

done:
	if (!ret && npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_SEC_LNK_FAIL)) {
		ret = -EPIPE;
	}

	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	if (ret) {	
		// on failure clear miscram even on secondaries
		//
		npe_miscram_neighbor_election_data_clr(nd);
		npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_FAILURE, 0);
	}
	return ret;
}

/**
 * npe_initiate_election()
 *
 *   change election state to in progress and kick the election thread unless election is 
 *   already in progress.
 *   This must be called with the lock held
 */
static void npe_initiate_election(u64  nbr_data_read_timeout)
{
	if (ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS) {
		pr_info("initiating pod election.  State: %d", ndhal_pelect_data.pod_state_internal);
		ndhal_pelect_data.kill_election = false;
		ndhal_pelect_data.nbr_data_read_to_start = ktime_get();
		ndhal_pelect_data.nbr_data_read_timeout = nbr_data_read_timeout;
		ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS;
		wake_up(&ndhal_pelect_data.wait_queue);
	}
}

/**
 * npe_all_rst_complete()
 *
 *   returns true if all devices have been successfully reset
 *   indicated by the npe module having a cached copy of all
 *   neuron device pointers.
 */
static bool npe_all_rst_complete(void)
{
	int i;
	for (i=0; i < 16; i++) {
		if (ndhal_pelect_data.pnd[i] == NULL) {
			return false;
		}
	}
	return true;
}

/** 
 * npe_election_exec_on_rst() 
 *
 *   Populdate the pod code's neuron device table.  Then if we are in the 
 *   initial state and election post reset was request, star the election process.
 *
 *
 */
int npe_election_exec_on_rst(struct neuron_device *nd, bool reset_successful)
{
	enum neuron_pod_election_mr_sts sts; 
	int node_id;
	u64 pod_serial_number;
	
	if (!reset_successful) {
		return 0;
	}

	mutex_lock(&ndhal_pelect_data.lock);

	// populate the device
	//
	ndhal_pelect_data.pnd[nd->device_index] = nd;

	// Device 0 is the primary actor in the election/topology discovery process, so 
	// when we process Device 0 reset completions, we need to do some bookkeeping.
	//
	if (nd->device_index == 0) {
		// Prior election results are cached in miscram, for testing purposes, 
		// we can clear the results through a module parameter, allowing us
		// to ignore the cached results.
		//
		if (npe_pod_ctl_is_set(NPE_POD_CTL_CLR_MISCRAM)) {
			pr_info("clearing miscram election results");
			npe_miscram_neighbor_election_data_clr(nd);
			npe_miscram_sts_node_id_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_INIT, 0);
			npe_miscram_pod_sernum_set(nd, 0);
		}

		// If there are cached election results, use them in lieu of running a new election.
		//
		npe_miscram_sts_node_id_get(nd, &sts, &node_id);
		npe_miscram_pod_sernum_get(nd, &pod_serial_number);

		if (sts == NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) {
			ndhal_pelect_data.node_id = node_id;
			ndhal_pelect_data.pod_serial_num = pod_serial_number;
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_SUCCESS;
			goto done;
		}

		// Check if module parameter has been set to skip election at driver load (post device reset). 
		// Primarily used for testing.
		//
		if (npe_pod_ctl_is_set(NPE_POD_CTL_RST_SKIP_ELECTION)) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;  
			goto done;
		}
	}
	
	// if we aren't kicking off election on first driver reset (testing) or 
	// if we aren't in init state then we've already made an election decision.
	//
	if ((ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_INIT) || npe_pod_ctl_is_set(NPE_POD_CTL_RST_SKIP_ELECTION)) {
		goto done;
	}

	// if all devices are done with reset, start the election.
	//
	if (!npe_all_rst_complete()) {
			goto done;
	}

	npe_initiate_election(ndhal_pelect_data.nbr_data_read_timeout);

done:
	mutex_unlock(&ndhal_pelect_data.lock);
	return 0;
}

/**
 * npe_notify_mark
 *
 *   crwl mark/unmark operations represent a change in core ownership
 *   which is tracked in the crwl mark table.  This function is called
 *   every time there is an ownership change.
 *
 *   The election code currently uses mark notifications to clear
 *   "SINGLE_NODE" behavior which asks the election code to
 *   temporarily suppresses election results until all cores
 *   have been released (unmarked).
 */
void npe_notify_mark(int mark_cnt, bool mark)
{
	mutex_lock(&ndhal_pelect_data.lock);
	if (!mark && (mark_cnt == 0)) {
		ndhal_pelect_data.suppress_election_results = false;
	}
	mutex_unlock(&ndhal_pelect_data.lock);
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
 * _npe_get_pod_status()
 *
 *    return state of the pod along w/ node id.
 */
static int _npe_get_pod_status(u32 *state, u8 *node_id)
{
	int ret = 0;

	switch (ndhal_pelect_data.pod_state_internal) {
		case NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS:
			*state = NEURON_POD_E_STATE_IN_PROGRESS;
			ret = -EBUSY;
			break;

		case NEURON_NPE_POD_ST_ELECTION_SUCCESS:
			*state = NEURON_POD_E_STATE_ULTRASERVER;
			if (ndhal_pelect_data.suppress_election_results) {
				*state = NEURON_POD_E_STATE_SINGLE_NODE;
				*node_id = -1;
				return 0;
			}
			break;

		case NEURON_NPE_POD_ST_INIT:
			*state = NEURON_POD_E_STATE_IN_PROGRESS;
			ret = -EBUSY;
			break;

		case NEURON_NPE_POD_ST_ELECTION_FAILURE:
			*state = NEURON_POD_E_STATE_SINGLE_NODE;
			break;

		default: 
			ret = -EINVAL;
			break;
	}

	*node_id = ndhal_pelect_data.node_id;
	return ret;	
}

int npe_get_pod_status(u32 *state, u8 *node_id)
{
	int ret;
	mutex_lock(&ndhal_pelect_data.lock);
	ret = _npe_get_pod_status(state, node_id);
	mutex_unlock(&ndhal_pelect_data.lock);
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
int npe_pod_ctrl(struct neuron_device *nd, u32 ctrl, u32 timeout, u32 *state)
{
	int ret = 0;
	u8 node_id;

	mutex_lock(&ndhal_pelect_data.lock);

	if (ctrl == NEURON_NPE_POD_CTRL_REQ_KILL) {
		if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS) {
			ndhal_pelect_data.kill_election = true;
		} else if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_INIT) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
		}
	} else if (ctrl == NEURON_NPE_POD_CTRL_REQ_SINGLE_NODE) {
		ndhal_pelect_data.suppress_election_results = true;
	} else if (ctrl == NEURON_NPE_POD_CTRL_REQ_POD) {
		int mark_cnt = ncrwl_range_mark_cnt_get();

		if ((mark_cnt == 0) && npe_all_rst_complete()) {
			npe_initiate_election(timeout * 1000);
			ret = 0;
		} else {
			pr_info("Pod Election request failed. %d Neuron cores are still in use or not all devices are reset. Election can only be initiated when all cores are free and all devices are reset", 
					mark_cnt);
			ret = -EAGAIN;
		}
	} else {
		pr_err("Invalid pod control request value %d", ctrl);
		ret = -EINVAL;
	}

	_npe_get_pod_status(state, &node_id);
	mutex_unlock(&ndhal_pelect_data.lock);
	return ret;
}

static int npe_election_thread_fn(void *arg)
{
	int ret;
	int i;
	int secondary_good_cnt = 0;

	while (!kthread_should_stop() && !ndhal_pelect_data.stop) {
		
retry:
		wait_event_interruptible(ndhal_pelect_data.wait_queue, ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS || ndhal_pelect_data.stop);
		if (kthread_should_stop() || ndhal_pelect_data.stop)
			break;

		// Cheap sanity check to make sure we don't allow a election to commence unless all devices have been reset.
		// There are checks in pod_ctrl logic to prevent this, but no harm in having a sanity check.
		//
		if (!npe_all_rst_complete()) {
			WARN_ONCE(1, "Pod election attempted while some devices have not completed reset");
			mutex_lock(&ndhal_pelect_data.lock);
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
			mutex_unlock(&ndhal_pelect_data.lock);
			goto retry;
		}

		pr_info("pod election in progress.  State: %d", ndhal_pelect_data.pod_state_internal);

		// prosecute the election
		//
		if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
			pr_info("vetting secondaries. Internal state: %d", ndhal_pelect_data.pod_state_internal);
		}

		// vet secondaries
		//
		for (i = 1; i < 16; i++) {
			ret = npe_secondary_device_vet(ndhal_pelect_data.pnd[i]);
			if (!ret) {
				secondary_good_cnt++;
			}
		}

		// run election on primary
		//
		ret = npe_primary_device_do_election(ndhal_pelect_data.pnd[0], secondary_good_cnt);

		secondary_good_cnt = 0;
	
		mutex_lock(&ndhal_pelect_data.lock);
		if (ret) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
			pr_info("nd%02d: pod election failed", ndhal_pelect_data.pnd[0]->device_index);
		} else {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_SUCCESS;
			pr_info("nd%02d: pod election complete.  node id: %d Pod Unique id: %016llx", ndhal_pelect_data.pnd[0]->device_index, 
					ndhal_pelect_data.node_id, ndhal_pelect_data.pod_serial_num);
		}
		mutex_unlock(&ndhal_pelect_data.lock);
	}

	ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
	return 0;
}

static int npe_create_thread(void)
{
	ndhal_pelect_data.thread = kthread_run(npe_election_thread_fn, NULL, "neuron election");
	if (IS_ERR_OR_NULL(ndhal_pelect_data.thread)) {
		pr_err("election thread creation failed\n");
		return -1;
	}
	return 0;
}

static void npe_stop_thread(void)
{
	if (ndhal_pelect_data.thread == NULL)
		return;

	ndhal_pelect_data.stop = true;
	wake_up(&ndhal_pelect_data.wait_queue);
	kthread_stop(ndhal_pelect_data.thread); //blocks till the thread exits
	ndhal_pelect_data.thread = NULL;
}

ssize_t npe_class_node_id_show_data(char *buf)
{
	if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS) {
		return dhal_sysfs_emit(buf, "busy\n");
	}
	return dhal_sysfs_emit(buf, "%d\n",ndhal_pelect_data.node_id);
}

ssize_t npe_class_server_id_show_data(char *buf)
{
	return dhal_sysfs_emit(buf, "%016llx\n",ndhal_pelect_data.pod_serial_num);
}

int npe_init(void)
{
	ndhal_pelect_data.nbr_data_read_timeout = pod_etimeout * 1000;
	return npe_create_thread();
}

void npe_cleanup(void)
{
	npe_stop_thread();
	if (ndhal_pelect_data.pnd[0] != NULL) {
		ndhal_pelect_data.pnd[0] = NULL;
	}
}
