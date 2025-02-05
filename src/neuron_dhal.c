#include <linux/slab.h>
	 
#include "neuron_arch.h"
#include "neuron_dhal.h"
#include "neuron_ring.h"

struct neuron_dhal *ndhal = NULL;

static int ndhal_register_funcs_v1(void) {
    if (!ndhal) {
        pr_err("ndhal is null. Can't register functions for V1.");
        return -EINVAL;
    }

    // TODO: register silicon, qemu, and emu to different functions and variables
    ndhal->address_map.pci_host_base = PCIEX8_0_BASE;
    ndhal->address_map.nc_per_device = V1_NC_PER_DEVICE;
    ndhal->reset_funcs.retry_count = NR_RESET_RETRY_COUNT;
    ndhal->topsp_funcs.ts_per_device = 0;
    ndhal->topsp_funcs.ts_nq_init = ts_nq_init_v1;
    ndhal->topsp_funcs.ts_nq_destroy_one = ts_nq_destroy_one_v1;
    ndhal->nc_funcs.mmap_p_offset = V1_MMAP_P_OFFSET;
    ndhal->nc_funcs.mmap_nc_event_offset = V1_MMAP_NC_EVENT_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_read_offset = V1_MMAP_NC_SEMA_READ_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_set_offset = V1_MMAP_NC_SEMA_SET_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_incr_offset = V1_MMAP_NC_SEMA_INCR_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_decr_offset = V1_MMAP_NC_SEMA_DECR_OFFSET;
    ndhal->nc_funcs.mmap_nc_size = V1_MMAP_NC_SIZE;
    ndhal->nc_funcs.semaphore_count = V1_SEMAPHORE_COUNT;
    ndhal->nc_funcs.event_count = V1_EVENTS_COUNT;
    ndhal->nc_funcs.nc_get_semaphore_base = nc_get_semaphore_base_v1;
    ndhal->nc_funcs.nc_get_event_addr = nc_get_event_addr_v1;
    ndhal->nq_funcs.nnq_get_nqid = nnq_get_nqid_v1;
    ndhal->nq_funcs.nnq_set_hwaddr = nnq_set_hwaddr_v1;
    ndhal->mpset_funcs.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v1;
    ndhal->mpset_funcs.mpset_block_carveout_regions = mpset_block_carveout_regions_v1;
    ndhal->ndmar_funcs.h2t_qid = V1_MAX_DMA_RINGS - 1;
    ndhal->ndmar_funcs.dma_eng_per_nd = V1_NUM_DMA_ENG_PER_DEVICE;
    ndhal->ndmar_funcs.dma_eng_per_nc = V1_DMA_ENG_PER_NC;
    ndhal->ndmar_funcs.ndmar_get_h2t_eng_id = ndmar_get_h2t_eng_id_v1;
    ndhal->ndmar_funcs.ndmar_is_nx_ring = ndmar_is_nx_ring_v1;
    ndhal->ndmar_funcs.ndmar_quiesce_queues = ndmar_quiesce_queues_v1;

    if (narch_is_qemu()) {
        ndhal->reset_funcs.nr_initiate_reset = nr_initiate_reset_v1_qemu;
        ndhal->reset_funcs.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1_qemu; 
    } else if (narch_is_emu()) {
        ndhal->reset_funcs.nr_initiate_reset = nr_initiate_reset_v1_emu;
        ndhal->reset_funcs.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1_emu;
    } else {
        ndhal->reset_funcs.nr_initiate_reset = nr_initiate_reset_v1;
        ndhal->reset_funcs.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v1;
    }
    return 0;
}

static int ndhal_register_funcs_v2(void) {
    if (!ndhal) {
        pr_err("ndhal is null. Can't register functions for V2.");
        return -EINVAL;
    }

    // TODO: register silicon, qemu, and emu to different functions and variables
    ndhal->address_map.pci_host_base = V2_PCIE_A0_BASE;
    ndhal->address_map.nc_per_device = V2_NC_PER_DEVICE;
    ndhal->reset_funcs.retry_count = NR_RESET_RETRY_COUNT;
    ndhal->topsp_funcs.ts_per_device = V2_TS_PER_DEVICE;
    ndhal->topsp_funcs.ts_nq_init = ts_nq_init_v2;
    ndhal->topsp_funcs.ts_nq_destroy_one = ts_nq_destroy_one_v2;
    ndhal->nc_funcs.mmap_p_offset = V2_MMAP_P_OFFSET;
    ndhal->nc_funcs.mmap_nc_event_offset = V2_MMAP_NC_EVENT_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_read_offset = V2_MMAP_NC_SEMA_READ_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_set_offset = V2_MMAP_NC_SEMA_SET_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_incr_offset = V2_MMAP_NC_SEMA_INCR_OFFSET;
    ndhal->nc_funcs.mmap_nc_sema_decr_offset = V2_MMAP_NC_SEMA_DECR_OFFSET;
    ndhal->nc_funcs.mmap_nc_size = V2_MMAP_NC_SIZE;
    ndhal->nc_funcs.semaphore_count = V2_SEMAPHORE_COUNT;
    ndhal->nc_funcs.event_count = V2_EVENTS_COUNT;
    ndhal->nc_funcs.nc_get_semaphore_base = nc_get_semaphore_base_v2;
    ndhal->nc_funcs.nc_get_event_addr = nc_get_event_addr_v2;
    ndhal->nq_funcs.nnq_get_nqid = nnq_get_nqid_v2;
    ndhal->nq_funcs.nnq_set_hwaddr = nnq_set_hwaddr_v2;
    ndhal->mpset_funcs.mpset_set_dram_and_mpset_info = mpset_set_dram_and_mpset_info_v2;
    ndhal->mpset_funcs.mpset_block_carveout_regions = mpset_block_carveout_regions_v2;
    ndhal->ndmar_funcs.h2t_qid = 0;
    ndhal->ndmar_funcs.dma_eng_per_nc = V2_DMA_ENG_PER_NC;
    ndhal->ndmar_funcs.ndmar_get_h2t_eng_id = ndmar_get_h2t_eng_id_v2;
    ndhal->ndmar_funcs.ndmar_is_nx_ring = ndmar_is_nx_ring_v2;
    ndhal->ndmar_funcs.ndmar_quiesce_queues = ndmar_quiesce_queues_v2;

    if (narch_is_qemu()) {
        ndhal->reset_funcs.nr_initiate_reset = nr_initiate_reset_v2_qemu;
        ndhal->reset_funcs.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v2_qemu;
        ndhal->ndmar_funcs.dma_eng_per_nd = V2_NC_PER_DEVICE * V2_DMA_ENG_PER_NC;
    } else if (narch_is_emu()) {
        ndhal->reset_funcs.retry_count *= 1000; // wait longer on the emulator
        ndhal->reset_funcs.nr_initiate_reset = nr_initiate_reset_v2_emu;
        ndhal->reset_funcs.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v2_emu;
        ndhal->ndmar_funcs.dma_eng_per_nd = nc_per_dev_param * V2_DMA_ENG_PER_NC;
        ndhal->address_map.nc_per_device = nc_per_dev_param;
    } else {
        ndhal->reset_funcs.nr_initiate_reset = nr_initiate_reset_v2;
        ndhal->reset_funcs.nr_wait_for_reset_completion = nr_wait_for_reset_completion_v2;
        ndhal->ndmar_funcs.dma_eng_per_nd = V2_NC_PER_DEVICE * V2_DMA_ENG_PER_NC;
    }
    return 0;
}

static DEFINE_MUTEX(ndhal_init_lock);   // mutex lock to ensure single init of ndhal
int neuron_dhal_init(void) {
    int ret = 0;

    // ndhal is a global struct so its init must be done only once
    if (ndhal) {
        return 0;
    }

    mutex_lock(&ndhal_init_lock);
    if (!ndhal) { // double check ndhal to prevent race condition
        // allocate memory for ndhal
        ndhal = kmalloc(sizeof(struct neuron_dhal), GFP_KERNEL);
        if (ndhal == NULL) {
            pr_err("Can't allocate memory for neuron_dhal.\n");
            mutex_unlock(&ndhal_init_lock);
            return -ENOMEM;
        }
    } else {
        mutex_unlock(&ndhal_init_lock);
        return 0;
    }
    mutex_unlock(&ndhal_init_lock);

    ndhal->arch = narch_get_arch();
    switch (ndhal->arch) {
        case NEURON_ARCH_V1:
            ret = ndhal_register_funcs_v1();
            break;
        case NEURON_ARCH_V2:
            ret = ndhal_register_funcs_v2();
            break;
        default:
            pr_err("Unknown HW architecture: %d. Can't init neuron_dhal.\n", ndhal->arch);
            return -EINVAL;
    }
    return ret;
}

void neuron_dhal_free(void)
{
    if (ndhal)
        kfree(ndhal);
}
