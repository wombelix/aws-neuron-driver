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
 *   The normal election process is triggered at driver load. If the driver determines the node/instance is part of a pod,
 *   it attempts to run the election process. 
 *
 *   - Out of the 16 devices on a node, the primary device is responsible for performing election duties
 *
 *   - Secondary devices are responsible for checking their links and reporting link state back to the primary.
 *
 *   - Secondaries report if (a) they reset successfully, (b) there links are good (c), their link is bad, 
 *     or (d) they aren't wired to the right pod neighbors
 *
 *   - Election operations are state driven to maintain consistency.
 *
 *   - We only perform an election from the "initial" internal state.
 *
 *   - The election process needs to be transactional (ACID)
 *     Flow is as follows:
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
 *   - other election notes
 *     - the primary has a timeout on it's wait for secondaries to report
 *     - all waits piggyback on the reset timeout mechanism so that in the event of a drivver unload, the 
 *       election process is notified so it can abort, allowing the driver to unload.
 *
 *   caveats:
 *     - there's no locking on the pod election data, so it's only really valid when the election is complete
 *
 *   TBD:
 *   - How to handle the equivalent of a transaction abort caused by:
 *     - driver unload
 *     - ioctl call to force to not-a-pod state
 *     - any other error late in election (which would really be a glitch)
 *   - What we'll probably end up doing is using a location for an "election notice" where
 *     a node can write to it's neighbor to inform it of a state change then monitor that location
 *     for an acknowledgement.  Semantics TBD, but we need a way to go through a clean transition to
 *     a quiesce state before re-runniing an election on a loaded driver because the driver needs to 
 *     essentially hold off runtime so it can use DMA resources to run the election.
 *
 */
#include "share/neuron_driver_shared.h"
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/version.h>

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
#include "neuron_pelect.h"

/*
 * Pod ctl for Fault injection of various failure scenarios
 */
#define NPE_POD_CTL_SKIP_ELECTION      (1<<0)
#define NPE_POD_CTL_SPOOF_BASE         (1<<1)
#define NPE_POD_CTL_FAULT_SEC_RST_FAIL (1<<2)
#define NPE_POD_CTL_FAULT_SEC_LNK_FAIL (1<<3)
#define NPE_POD_CTL_FAULT_PRI_LNK_FAIL (1<<4)
#define NPE_POD_CTL_SKIP_NBR_CPY_READ  (1<<5)
#define NPE_POD_CTL_VERBOSE            (1<<6)

/* Disable pod auto election until election code is stabilized */
int pod_ctl = NPE_POD_CTL_SKIP_ELECTION;
module_param(pod_ctl, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(pod_ctl, "pod election control");

#define NPE_RETRY_WAIT_MS 1000
#define NPE_RETRY_WAIT_REPORTING_INTERVAL 10

// max time to wait for secondaries to respond (5 min)
//
#define NPE_SECONDARY_REPORT_MAX_TIME (300 * 1000)
#define NPE_SECONDARY_REPORT_WAIT_MS 1000

#define NPE_ELECTION_DEVICE_INDEX 0

/**
 * internal state of the pod election module
 *
 */
enum neuron_pod_state_internal {
	NEURON_NPE_POD_ST_INIT = 0,             // initializing - before pod formation
	NEURON_NPE_POD_ST_ELECTION_SUCCESS = 1, // successful pod formation
	NEURON_NPE_POD_ST_ELECTION_FAILURE = 2, // failure forming pod
	NEURON_NPE_POD_ST_WAIT_QUIESCE = 3,     // quiescing before trying to reform pod (cleanup election data to known state)
};

enum neuron_pod_force_req {
	NEURON_NPE_POD_FORCE_INACTIVE = 0, // no force pod state request is active
	NEURON_NPE_POD_FORCE_POD = 1,      // force pod state to pod (try to re-run election)
	NEURON_NPE_POD_FORCE_NON_POD = 2,  // force pod state to not a pod 
};

struct ndhal_v3ext_pelect {
	atomic_t sec_good_cnt;    // count of secondary devices w/ good pod links
	atomic_t sec_bad_cnt;     // count of secondary devices w/ bad pod links
	atomic_t sec_dead_cnt;    // count of secondary devices w/ that failed to reset
	struct neuron_device *nd; // link back to nd used election
	int pod_state_internal;   // state of the pod
	int pod_force_req;        // force request to change pod state
	int node_id;			  // node id
	u64  pod_serial_num;	  // serial number of the pod (node 0's serial number)
	wait_queue_head_t q;      // queue to wait on for election completion
};

/**
 * pod election & state tracking struct.
 *   TODO - probably should encapsulate as dhal v3 extension.
 *
 */
static struct ndhal_v3ext_pelect ndhal_pelect_data = {
 	.sec_good_cnt = ATOMIC_INIT(0),
 	.sec_bad_cnt = ATOMIC_INIT(0),
	.sec_dead_cnt = ATOMIC_INIT(0),
	.nd = NULL,
	.pod_state_internal = NEURON_NPE_POD_ST_INIT,
	.pod_force_req = NEURON_NPE_POD_FORCE_INACTIVE,
	.node_id = -1,
	.pod_serial_num = 0,
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

static bool npe_pod_ctl_is_set(int value)
{
	return (pod_ctl & value);
}

static bool npe_election_canceled(void)
{
	return (ndhal_pelect_data.pod_force_req == NEURON_NPE_POD_FORCE_POD);
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

		// got both neighbor serial numbers?
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

/**
 * npe_read_neighbor_read_election_status()
 *
 *    read the neighbor's election status - indicator that their node has successfully completed
 *    the election process
 */
static int npe_read_neighbor_read_election_status(pod_neighbor_io_t pnio[][2], u32 *election_status)
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
            if ((tmp != 0) && (tmp != 0xdeadbeef)) {
                election_status[i] = tmp;
			}
        }

		// got both neighbor's election status? 
		//
        if ((election_status[0] != 0) && (election_status[1] != 0)) {
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
			// Note: This could possibly leave our neighbors in pod state while this node is not in pod state as it's logically an ABORT transaction.
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

static void npe_miscram_neighbor_election_data_clr(struct neuron_device *nd)
{
    npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, 0);
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
	ktime_t start_time;
	
	start_time = ktime_get();

	do {
		if (atomic_read(&ndhal_pelect_data.sec_good_cnt) + atomic_read(&ndhal_pelect_data.sec_bad_cnt) + atomic_read(&ndhal_pelect_data.sec_dead_cnt) == 15) {
			if (atomic_read(&ndhal_pelect_data.sec_good_cnt) == 15) {
				return 0;
			}
			break;
		}
		if (nr_msleep_stoppable(nd, NPE_SECONDARY_REPORT_WAIT_MS)) {
			//driver unload
			return -EINTR;
		}

		if (npe_election_canceled()) {
			return -EINTR;
		}
	} while (ktime_to_ms(ktime_sub(ktime_get(), start_time)) < NPE_SECONDARY_REPORT_MAX_TIME);

	pr_err("nd%02d: not all secondaries reported in: link good %02d link bad %02d, reset failed %02d", nd->device_index,
		    atomic_read(&ndhal_pelect_data.sec_good_cnt), atomic_read(&ndhal_pelect_data.sec_bad_cnt), atomic_read(&ndhal_pelect_data.sec_dead_cnt));
	return -1;
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

    // read neighbors serial numbers
    // 
	ret = npe_read_neighbor_serial_numbers(pnio,  nbr_serial_number);
	if (ret || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_PRI_LNK_FAIL)) {
		goto done;
	}
	pr_info("nd%02d: acquired pod neighbor serial numbers L: %016llx  R:  %016llx", nd->device_index, nbr_serial_number[0], nbr_serial_number[1]);

    // update LH/RH neighbor unique values in our local miscram copy 
    //
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, (nbr_serial_number[0]>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_LO, nbr_serial_number[0] & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_HI, (nbr_serial_number[1]>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_LO, nbr_serial_number[1] & 0xffffffff);

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
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ST_ELECTION_SUCCESS);
	
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status);
	if (ret) {
		goto done;
	}

	if ((nbr_election_status[0] != NEURON_NPE_POD_ST_ELECTION_SUCCESS) || (nbr_election_status[1] != NEURON_NPE_POD_ST_ELECTION_SUCCESS)) {
		pr_err("nd%02d: election failed neighbor election status L: %s R: %s", nd->device_index,
				(nbr_election_status[0] == NEURON_NPE_POD_ST_ELECTION_SUCCESS) ? "success" : "failure",
				(nbr_election_status[0] == NEURON_NPE_POD_ST_ELECTION_SUCCESS) ? "success" : "failure");
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

	// if election was successful, we set node_id, pod_serial_number, and flag pod
	// if unsuccessful, we set state to not a pod and clear our sernum information.
	//
	if (ret) {
		npe_miscram_neighbor_election_data_clr(nd);
		npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ST_ELECTION_FAILURE);
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
 *    check the secondary's neighbors to see that they have the same device id
 *    and report success or failure
 *    then wait for the primary device to complete the election process.
 *
 */
static int npe_secondary_device_vet_n_wait(struct neuron_device *nd, bool reset_successful)
{
	int ret;
	int i;
	u64 nbr_serial_number[2] = {0};
	u64 nbr_serial_number_copy[2][2] = {0};
	pod_neighbor_io_t pnio[2][2] = {0};

	if (!reset_successful || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_SEC_RST_FAIL)) {
		atomic_fetch_add(1, &ndhal_pelect_data.sec_dead_cnt);
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

	// if neighbor reads are good the link is good (or at least wired in pairs)
	//
	ret = npe_read_neighbor_serial_numbers(pnio,  nbr_serial_number);
	if (ret) {
		goto done;
	}

	// populate election data with device index to detect miss-cabling
	//
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, 0);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_LO, nd->device_index);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_HI, 0);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_LO, nd->device_index);

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

done:
	if (ret || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_SEC_LNK_FAIL)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		atomic_fetch_add(1, &ndhal_pelect_data.sec_bad_cnt);
#else
		atomic_add_return(1, &ndhal_pelect_data.sec_bad_cnt);
#endif 
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		atomic_fetch_add(1, &ndhal_pelect_data.sec_good_cnt);
#else
		atomic_add_return(1, &ndhal_pelect_data.sec_good_cnt);
#endif 
		
		if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
			pr_info("nd%02d: acquired pod neighbor serial numbers L: %016llx  R:  %016llx", nd->device_index, nbr_serial_number[0], nbr_serial_number[1]);
		}
	}

	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	// wait for the primary to wake us up
	//
	wait_event_interruptible(ndhal_pelect_data.q, ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_INIT);
	
	return ret;
}

/** 
 * npe_election_exec() 
 *
 *    If we are in pod state init, perform an election.
 *
 */
int npe_election_exec(struct neuron_device *nd, bool reset_successful)
{
	if (npe_pod_ctl_is_set(NPE_POD_CTL_SKIP_ELECTION)) {
		return 0;
	}
	if (ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_INIT) {
		return 0;
	}

	if (nd->device_index == NPE_ELECTION_DEVICE_INDEX) {
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
			*state = NEURON_POD_E_STATE_IN_PROGRESS;
			ret = -EBUSY;
			break;

		case NEURON_NPE_POD_ST_ELECTION_SUCCESS:
			*state = NEURON_POD_E_STATE_SUCCESS;
			break;

		case NEURON_NPE_POD_ST_ELECTION_FAILURE:
			//*state = NEURON_POD_E_STATE_FAILED:
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

int npe_pod_ctrl(u32 ctrl, u32 *state)
{
	u8 node_id;

	if (ndhal_pelect_data.pod_force_req != NEURON_NPE_POD_FORCE_INACTIVE) {
		return -EBUSY;
	}

	// if the request is to force non-pod, we kill any election in progress
	// wait for it to finish, pull state,
	//
	if (ctrl == NEURON_NPE_POD_FORCE_NON_POD) {
		if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_INIT) {
			ndhal_pelect_data.pod_force_req = NEURON_NPE_POD_FORCE_NON_POD;
			wait_event_interruptible(ndhal_pelect_data.q, ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_INIT);
			ndhal_pelect_data.pod_force_req = NEURON_NPE_POD_FORCE_INACTIVE;
		}
		// FIXME - eventually we want to force to failed state even if not in election, including miscram update. still TBD

		npe_get_pod_status(state, &node_id);
		return 0;
	}

	return -EINVAL;	
}
