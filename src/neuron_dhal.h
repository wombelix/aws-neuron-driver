#ifndef NEURON_DHAL_H
#define NEURON_DHAL_H

#include "neuron_device.h"

struct address_map {
    uint64_t pci_host_base;
    int nc_per_device;
};

struct reset_funcs {
    uint32_t retry_count;
    int (*nr_initiate_reset) (struct neuron_device *nd);
    int (*nr_wait_for_reset_completion) (struct neuron_device *nd);
};

struct topsp_funcs {
    uint32_t ts_per_device;
    int (*ts_nq_init) (struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
                       u32 on_host_memory, u32 dram_channel, u32 dram_region,
                       bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset);
    void (*ts_nq_destroy_one) (struct neuron_device *nd, u8 ts_id);
};

struct nc_funcs {
    uint64_t mmap_p_offset;
    uint64_t mmap_nc_event_offset;
    uint64_t mmap_nc_sema_read_offset;
    uint64_t mmap_nc_sema_set_offset;
    uint64_t mmap_nc_sema_incr_offset;
    uint64_t mmap_nc_sema_decr_offset;
    uint64_t mmap_nc_size;
    uint64_t semaphore_count;
    uint64_t event_count;
    void *(*nc_get_semaphore_base) (struct neuron_device *nd, u8 nc_id);
    void *(*nc_get_event_addr) (struct neuron_device *nd, u8 nc_id, u16 event_index);
};

struct nq_funcs {
   u8 (*nnq_get_nqid) (struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type);
   void (*nnq_set_hwaddr) (struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);
};

struct mpset_funcs {
    void (*mpset_set_dram_and_mpset_info) (struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
    int (*mpset_block_carveout_regions) (struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
};

struct ndmar_funcs {
    int h2t_qid;
    int nc_per_dev;
    int dma_eng_per_nc;
    int dma_eng_per_nd;
    uint32_t (*ndmar_get_h2t_eng_id) (struct neuron_device *nd, uint32_t nc_id);
    bool (*ndmar_is_nx_ring) (uint32_t eng_id, uint32_t q_id);
    int (*ndmar_quiesce_queues) (struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask);
};

struct neuron_dhal {
    int arch;
    // TODO: add structs of different driver functionalities below, including function pointers and variables, e.g. reset and dma:
    struct address_map address_map;
    struct reset_funcs reset_funcs;
    struct topsp_funcs topsp_funcs;
    struct nc_funcs nc_funcs;
    struct nq_funcs nq_funcs;
    struct mpset_funcs mpset_funcs;
    struct ndmar_funcs ndmar_funcs;
};

extern struct neuron_dhal *ndhal;       // ndhal is a global structure shared by all available neuron devices


/**
 * @brief Initialize the global ndhal for all available neuron devices
 *          - The initialization must be done only once
 *          - Mem allocation must be wrapped by the ndhal_init_lock
 * 
 * @return int 0 for success, negative for failures
 */
int neuron_dhal_init(void);

/**
 * @brief Clean up ndhal
 * The caller is to ensure that the ndhal is freed only once.
 * 
 */
void neuron_dhal_free(void);

/* Device Reset Functions */
/**
 * nr_initiate_reset() - initialize a reset
 * 
 * @param nd - Neuron device which will be reset by the thread.
 */
int nr_initiate_reset_v1(struct neuron_device *nd);
int nr_initiate_reset_v1_qemu(struct neuron_device *nd);
int nr_initiate_reset_v1_emu(struct neuron_device *nd);
int nr_initiate_reset_v2(struct neuron_device *nd);
int nr_initiate_reset_v2_qemu(struct neuron_device *nd);
int nr_initiate_reset_v2_emu(struct neuron_device *nd);

/**
 * nr_wait_for_reset_completion() - wait for a reset to be completed
 * 
 * @param nd - Neuron device which will be reset by the thread.
 */
int nr_wait_for_reset_completion_v1(struct neuron_device *nd);
int nr_wait_for_reset_completion_v1_qemu(struct neuron_device *nd);
int nr_wait_for_reset_completion_v1_emu(struct neuron_device *nd);
int nr_wait_for_reset_completion_v2(struct neuron_device *nd);
int nr_wait_for_reset_completion_v2_qemu(struct neuron_device *nd);
int nr_wait_for_reset_completion_v2_emu(struct neuron_device *nd);

/* TOPSP Functions */
/**
 * ts_nq_init() - Initialize notification queue for TopSp.
 *
 * @nd: neuron device
 * @ts_id: TopSp Id
 * @eng_index: notification engine index in the core
 * @nq_type: type of the notification queue
 * @size: size of queue in bytes
 * @on_host_memory: if true, NQ is created in host memory
 * @dram_channel: If NQ is created on device memory which DRAM channel to use.
 * @dram_region: If NQ is created on device memory which DRAM region to use.
 * @force_alloc_mem: If true, force allocate new memory (and delete already allocated memory, if any)
 * @nq_mc[out]: memchunk used by the NQ will be written here
 * @mc_ptr[out]: Pointer to memchunk backing this NQ
 *
 * Return: 0 on if initialization succeeds, a negative error code otherwise.
 */
int ts_nq_init_v1(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
	       u32 on_host_memory, u32 dram_channel, u32 dram_region,
	       bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset);
int ts_nq_init_v2(struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
	       u32 on_host_memory, u32 dram_channel, u32 dram_region,
	       bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset);

/**
 * ts_nq_destroy_one() - Disable notification in the device
 *
 * @nd: neuron device
 * @ts_id: topsp id
 *
 */
void ts_nq_destroy_one_v1(struct neuron_device *nd, u8 ts_id);
void ts_nq_destroy_one_v2(struct neuron_device *nd, u8 ts_id);

/* Neuron Core Functions */
/**
 * nc_get_semaphore_base() - get semaphore base address
 * 
 * @param nd - neuron device
 * @param nc_id - neuron core index
 * @return void* - semaphore base address
 */
void *nc_get_semaphore_base_v1(struct neuron_device *nd, u8 nc_id);
void *nc_get_semaphore_base_v2(struct neuron_device *nd, u8 nc_id);

/**
 * nc_get_event_addr() - get event address
 * 
 * @param nd - neuron device
 * @param nc_id - neuron core index
 * @param event_index - event index
 * @return void* - event address
 */
void *nc_get_event_addr_v1(struct neuron_device *nd, u8 nc_id, u16 event_index);
void *nc_get_event_addr_v2(struct neuron_device *nd, u8 nc_id, u16 event_index);

/* Notification Queue Functions */
/**
 * nnq_get_nqid() - get notification queue id
 * 
 * @param nd: neuron device
 * @param nc_id: core index in the device
 * @param index: notification engine index in the core
 * @param nq_type: type of the notification queue
 * @return u8: notification queue id
 */
u8 nnq_get_nqid_v1(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type);
u8 nnq_get_nqid_v2(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type);

/**
 * nnq_set_hwaddr() - set the physical address of the queue
 * 
 * @param nd: neuron device
 * @param nc_id: core index in the device
 * @param index: notification engine index in the core
 * @param nq_type: type of the notification queue
 * @param size: size of queue in bytes
 * @param queue_pa: physical address of the queue
 */
void nnq_set_hwaddr_v1(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);
void nnq_set_hwaddr_v2(struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);

/* Memory Pool Functions */
/**
 * mpset_set_dram_and_mpset_info() 
 *              - set the address and size of device dram
 *              - set mpset's num_channels and number of regions in the device pool
 * 
 * @param mpset: pointer to mpset
 * @param device_dram_addr: DRAM Channel 0 and 1's addresses
 * @param device_dram_size: DRAM Channel 0 and 1's sizes
 */
void mpset_set_dram_and_mpset_info_v1(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
void mpset_set_dram_and_mpset_info_v2(struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);

/**
 * mpset_block_carveout_regions() 
 *          - in v2, block carve out regions: Upper 16 MB is used internally by firmware
 *          - in v1, do nothing and just return 0 
 * 
 * @param nd: neuron device
 * @param mpset: pointer to mpset
 * @param device_dram_addr: DRAM Channel 0's and 1's addresses
 * @param device_dram_size: DRAM Channel 0's and 1's sizes
 * @param region_sz: region size
 * @return int: 0 on success, o/w on failure
 */
int mpset_block_carveout_regions_v1(struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
int mpset_block_carveout_regions_v2(struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);

/* DMA Ring Functions */
/** 
 * ndmar_get_h2t_eng_id() 
 *          - get the host-to-device or device-to-host DMA engine ID
 *          - DMA engine 33 (H2D) and 32 (D2H) are top level DMA engines that allow moving data from/to HBM.
 *
 * @param nd: Neuron device which contains the DMA engine
 * @param nc_id: Neuron core corresponding to H2T engine
 * Return DMA engine id
 */
uint32_t ndmar_get_h2t_eng_id_v1(struct neuron_device *nd, uint32_t nc_id);
uint32_t ndmar_get_h2t_eng_id_v2(struct neuron_device *nd, uint32_t nc_id);

/**
 * ndmar_is_nx_ring() - is the DMA ring reserved for NX cores
 * 
 * @param eng_id: the DMA engine id
 * @param q_id: the DMA queue id
 */
bool ndmar_is_nx_ring_v1(uint32_t eng_id, uint32_t q_id);
bool ndmar_is_nx_ring_v2(uint32_t eng_id, uint32_t q_id);

/**
 * ndmar_quiesce_queues() - Quiesce DMA queues.
 *
 * @param nd: Neuron device which contains the DMA engines
 * @param nc_id: NC id that owns the queues
 * @param engine_count: the number of elements in the queue_mask array - currently not used, always pass 0
 * @param queue_mask:   per engine queues to reset - currently not used and ignored.
 *
 * Return: 0 if queue release succeeds, a negative error code otherwise.
 */
int ndmar_quiesce_queues_v1(struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask);
int ndmar_quiesce_queues_v2(struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask);

#endif
