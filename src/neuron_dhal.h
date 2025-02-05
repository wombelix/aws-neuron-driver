#ifndef NEURON_DHAL_H
#define NEURON_DHAL_H

#include "neuron_device.h"
#include "neuron_fw_io.h"
#include "neuron_mmap.h"
#include "neuron_sysfs_metrics.h"

struct ndhal_address_map {
	// addresses
	uint64_t pci_host_base;
	uint64_t mmap_p_offset;
	uint64_t mmap_nc_event_offset;
	uint64_t mmap_nc_sema_read_offset;
	uint64_t mmap_nc_sema_set_offset;
	uint64_t mmap_nc_sema_incr_offset;
	uint64_t mmap_nc_sema_decr_offset;
	uint64_t bar0_misc_ram_offset;

	// sizes
	uint64_t mmap_nc_size;

	// counts
	int nc_per_device;
	uint64_t semaphore_count;
	uint64_t event_count;
	uint32_t ts_per_device;
	int h2t_qid;
	int dma_eng_per_nc;
	int dma_eng_per_nd;
};

struct ndhal_reset {
    uint32_t retry_count;
    int (*nr_initiate_reset) (struct neuron_device *nd);
    int (*nr_wait_for_reset_completion) (struct neuron_device *nd);
};

struct ndhal_topsp {
    int (*ts_nq_init) (struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
                       u32 on_host_memory, u32 dram_channel, u32 dram_region,
                       bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset);
    void (*ts_nq_destroy_one) (struct neuron_device *nd, u8 ts_id);
};

struct ndhal_nc {
    void *(*nc_get_semaphore_base) (struct neuron_device *nd, u8 nc_id);
    void *(*nc_get_event_addr) (struct neuron_device *nd, u8 nc_id, u16 event_index);
};

struct ndhal_nq {
   u8 (*nnq_get_nqid) (struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type);
   void (*nnq_set_hwaddr) (struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);
};

struct ndhal_mpset {
    int mp_min_alloc_size;
    void (*mpset_set_dram_and_mpset_info) (struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
    int (*mpset_block_carveout_regions) (struct neuron_device *nd, struct mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
};

struct ndhal_ndmar {
    uint32_t (*ndmar_get_h2t_eng_id) (struct neuron_device *nd, uint32_t nc_id);
    bool (*ndmar_is_nx_ring) (uint32_t eng_id, uint32_t q_id);
    int (*ndmar_quiesce_queues) (struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask);
    void (*ndmar_set_model_started) (struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc);
};

struct ndhal_fw_io {
    int (*fw_io_topology) (struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count);
    int (*fw_io_register_readless_read_region) (struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size);
    int (*fw_io_read_csr_array) (void **addrs, u32 *values, u32 num_csrs, bool operational);
};

struct ndhal_reg_access {
    int (*reg_read32_array) (void **addr, u32 *value, u32 num_values);
};

struct ndhal_mmap {
    struct neuron_dm_special_mmap_ent *dm_mmap_special;
    int (*mmap_get_bar4_offset) (u64 start_addr, u64 size, u64 *offset);
};

struct ndhal_sysfs_metrics {
    char *arch_nd_type_suffix;
    char *arch_nc_type_suffix;
    char *arch_instance_suffix;
    char *arch_device_name_suffix;
    int root_info_node_attrs_info_tbl_cnt;
    nsysfsmetric_attr_info_t *root_info_node_attrs_info_tbl;

    int (*nsysfsmetric_add_ecc_nodes) (struct nsysfsmetric_metrics *metrics, 
                                       struct nsysfsmetric_node *stats_node,
                                       int ecc_attrs_info_tbl_cnt,
                                       const nsysfsmetric_attr_info_t *attr_info_tbl);
};

struct ndhal_pci {
    int apb_bar;
    int axi_bar;
    int dram_bar;

    int (*neuron_pci_release_bar) (struct pci_dev *dev, int bar);
    int (*neuron_pci_reserve_bar) (struct pci_dev *dev, int bar, const char *res_name);
    int (*neuron_pci_set_npdev) (struct pci_dev *dev,
                                int bar,
                                const char *res_name,
                                phys_addr_t *bar_pa,
                                void __iomem **bar_ioaddr,
                                u64 *bar_size);
    int (*neuron_pci_get_device_id) (struct neuron_device *nd, struct pci_dev *dev);
};

struct ndhal_cdev {
    struct ncdev_mem_region *ncdev_mem_regions;
    u64 *ncdev_bar0_write_blocked_addrs;

    void (*ncdev_compatible_version) (struct neuron_ioctl_compatible_version *arg);
    void (*ncdev_quiesce_exec_on_proc_exit) (void);
    int (*ncdev_bar_write_data) (struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count);
};

struct ndhal_udma {
    void (*udma_m2s_data_rd_cfg_boundaries_set) (struct udma *udma);
    void (*udma_q_config) (struct udma_q *udma_q);
};

struct ndhal_ndma {
    bool ndma_retry_memcpy;

    void (*ndma_get_wait_for_completion_time) (u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
    int (*ndma_validate_pa) (struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type);
    int (*ndma_init) (void __iomem *bar0, struct udma *udma, int eng_id);
    int (*ndma_is_bar0_write_blocked) (u64 off);
    int (*ndma_get_m2m_barrier_type) (bool set_dmb);
};

struct neuron_dhal {
    int arch;

    struct ndhal_address_map ndhal_address_map;
    struct ndhal_reset ndhal_reset;
    struct ndhal_topsp ndhal_topsp;
    struct ndhal_nc ndhal_nc;
    struct ndhal_nq ndhal_nq;
    struct ndhal_mpset ndhal_mpset;
    struct ndhal_ndmar ndhal_ndmar;
    struct ndhal_fw_io ndhal_fw_io;
    struct ndhal_reg_access ndhal_reg_access;
    struct ndhal_mmap ndhal_mmap;
    struct ndhal_sysfs_metrics ndhal_sysfs_metrics;
    struct ndhal_pci ndhal_pci;
    struct ndhal_cdev ndhal_cdev;
    struct ndhal_udma ndhal_udma;
    struct ndhal_ndma ndhal_ndma;
};

extern struct neuron_dhal *ndhal;       // ndhal is a global structure shared by all available neuron devices


/**
 * @brief Initialize the global ndhal for all available neuron devices
 *          - The initialization must be done only once
 *          - Mem allocation must be wrapped by the ndhal_init_lock
 * 
 * @param pci_device_id: the PCI DEVICE ID
 * 
 * @return int 0 for success, negative for failures
 */
int neuron_dhal_init(unsigned int pci_device_id);

/**
 * @brief Clean up ndhal
 * The caller is to ensure that the ndhal is freed only once.
 * 
 */
void neuron_dhal_free(void);

/**
 * ndhal_register_funcs() - Register functions v1 (or inf1) v2 (or trn1 inf2) to the ndhal
 * 
 * @param pci_device_id: the PCI DEVICE ID
 * 
 * @return int 0 on success, negative for failures
 */
int ndhal_register_funcs_v1(unsigned int pci_device_id);
int ndhal_register_funcs_v2(unsigned int pci_device_id);

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

/** ndmar_set_model_started()
 *
 * Checks to see if the pa belongs to PE IRAM FIFO offset. If so, then these
 * descs are used to load the iram. The mem chunk is going to have all the descriptors
 * to load the instructions in iram. So go through all the dma queues and check if this mem chunk is
 * in that queue. Once we have the queue we set that queue to have descs
 * for iram. The actual copy start of the queue would come when model is started and at that time
 * set the state of model start for this nc.
 *
 * @nd: Neuron device which contains the DMA engine
 * @pa: pa to check
 * @mc: mem chunk that has descs
 *
 * Return: None
 */
void ndmar_set_model_started_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc);
void ndmar_set_model_started_v2(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *mc);


/* FWIO Functions */
/**
 * fw_io_topology() - Discovers devices connected to the given device.
 *
 * @ctx: FWIO context of the device for which topology
 * @pdev_index: the pci device id
 * @device_id: The index of the neuron device
 * @connected_device_ids:  Connected device IDs are stored here.
 * @count: Number of devices connected to the given device.
 *
 * @return int: 0 on success. -1 on failure
 *
 */
int fw_io_topology_v1(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count);
int fw_io_topology_v2(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count);

/**
 * fw_io_register_readless_read_region() - Register readless read BAR regions
 * 
 * @param ctx: FWIO context
 * 
 * @return int: 0 on success. -1 on failure
 */
int fw_io_register_readless_read_region_v1(struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size);
int fw_io_register_readless_read_region_v2(struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size);

/**
 * fw_io_read_csr_array() - Read the CSR array
 * 
 * @param addrs: array of CSR addresses to be read
 * @param values: output array
 * @param num_csrs; number of CSR addresses
 * @param operational: true if the read expects the device to be in operational state;
 *                     it's used to distinguish between when the driver first discovers the device (possibly unknown state) and when it's successfully been reset "operational"
 * 
 * @return int: 0 on success, -1 on failure
 */
int fw_io_read_csr_array_v1(void **ptrs, u32 *values, u32 num_csrs, bool operational);
int fw_io_read_csr_array_v2(void **ptrs, u32 *values, u32 num_csrs, bool operational);


/* Register Access (read and write) Functions */
/**
 * reg_read32_array() - read an array of 32bit registers.
 * 
 * @addr: register address.
 * @value: read value would be stored here.
 * @num_values: num values to read
 *
 * Return: 0 if read succeeds, a negative error code otherwise.
 */
int reg_read32_array_v1(void **addr, u32 *value, u32 num_values);
int reg_read32_array_v2(void **addr, u32 *value, u32 num_values);
int reg_read32_array_v2_qemu_emu(void **addr, u32 *value, u32 num_values);


/* Memory Map Functions */
/**
 * mmap_get_bar4_offset() - calculate the offset of BAR4
 * 
 * @param start_addr: start address
 * @param size: size of memory
 * @param offset: offset of BAR4
 * @return int: 0 on success; negative on failure
 */
int mmap_get_bar4_offset_v1(u64 start_addr, u64 size, u64 *offset);
int mmap_get_bar4_offset_v2(u64 start_addr, u64 size, u64 *offset);


/* Sysfs Metrics Functions */
/**
 * nsysfsmetric_add_ecc_nodes() - add neuron{0, 1, ...}/stats/hardware/{sram_ecc_uncorrected, mem_ecc_uncorrected} to sysfs directory
 * 
 * @param metrics: the sysfs metrics structure
 * @param stats_node: the sysfs node structure of the stats directory
 * @param ecc_attrs_info_tbl_cnt: number of the ecc attributes
 * @param attr_info_tbl: the ecc attributes as an array
 * @return int 0 on success; otherwise on failure
 * 
 * Note: ecc errors are only supported by sysfs for V2. TODO: V1 support will be added 
 */
int nsysfsmetric_add_ecc_nodes_v1(struct nsysfsmetric_metrics *metrics, 
                               struct nsysfsmetric_node *stats_node,
                               int ecc_attrs_info_tbl_cnt,
                               const nsysfsmetric_attr_info_t *attr_info_tbl);
int nsysfsmetric_add_ecc_nodes_v2(struct nsysfsmetric_metrics *metrics, 
                               struct nsysfsmetric_node *stats_node,
                               int ecc_attrs_info_tbl_cnt,
                               const nsysfsmetric_attr_info_t *attr_info_tbl);

/* PCI Functions */
/**
 * neuron_pci_release_bar() - Release a PCI BAR
 * 
 * @param dev: PCI device whose resources were previously reserved by pci_request_region()
 * @param bar: BAR to be reserved
 * 
 * for V2, this function is dummy
 */
int neuron_pci_release_bar_v1(struct pci_dev *dev, int bar);
int neuron_pci_release_bar_v2(struct pci_dev *dev, int bar);

/**
 * neuron_pci_reserve_bar() - Mark the PCI region associated with PCI BAR as being reserved
 * 
 * @param dev: PCI device whose resources are to be reserved
 * @param bar: BAR to be reserved
 * @param res_name: Name to be associated with resource.
 * @return int: Returns 0 on success, otherwise failure
 */
int neuron_pci_reserve_bar_v1(struct pci_dev *dev, int bar, const char *res_name);
int neuron_pci_reserve_bar_v2(struct pci_dev *dev, int bar, const char *res_name);

 /**
 * neuron_pci_set_npdev() - set BAR's physical addr, io addr, and size of neuron_pci_device
 * 
 * @param dev: PCI device that owns the BAR
 * @param bar: BAR number
 * @param res_name: Name associated with resource
 * @param bar_pa: start physical address of BAR
 * @param bar_ioaddr: __iomem address to device BAR
 * @param bar_size: size of BAR
 * @return int: Returns 0 on success, otherwise failure
 */
int neuron_pci_set_npdev_v1(struct pci_dev *dev,
                            int bar,
                            const char *res_name,
                            phys_addr_t *bar_pa,
                            void __iomem **bar_ioaddr,
                            u64 *bar_size);
int neuron_pci_set_npdev_v2(struct pci_dev *dev,
                            int bar,
                            const char *res_name,
                            phys_addr_t *bar_pa,
                            void __iomem **bar_ioaddr,
                            u64 *bar_size);

/**
 * neuron_pci_get_device_id() - get device id from pacific and set nd->device_index
 * 
 * @param dev: PCI device
 * @param nd: neuron device
 * @return int: 0 on success, otherwise on failure
 * 
 * for V1, this function is dummy
 */
int neuron_pci_get_device_id_v1(struct neuron_device *nd, struct pci_dev *dev);
int neuron_pci_get_device_id_v2(struct neuron_device *nd, struct pci_dev *dev);


/* Char Device (cdev) Functions */
/**
 * ncdev_compatible_version() - fill in the compatible version of the RT with the current driver version
 * 
 * @param arg: min and max compatible versions to be filled in
 */
void ncdev_compatible_version_v1(struct neuron_ioctl_compatible_version *arg);
void ncdev_compatible_version_v2(struct neuron_ioctl_compatible_version *arg);

/**
 * ncdev_quiesce_exec_on_proc_exit() - for V1, before resetting DMA, allow current NeuronCore execution to finish and settle
 * 
 * Note:
 *      When a process is killed, the driver resets DMA but there is no
 *      way to soft reset neuron cores. This causes problem if the
 *      process was executing serial TPB or switching activation tables,
 *      which result in abrubtly stopping DMA engines hence engines are
 *      are blocked on semaphores. This results in next model
 *      load failure or inference timeout.
 * 
 *      Proper way is clearing out semaphore, events after resetting
 *      DMA engines. However, it is a lot of code change, hence
 *      adding a sleep for 1 second when process exits, which allows
 *      the NeuronCore to continue to execute for a second. Since
 *      no new inference can be submitted during this time, NeuronCore
 *      state would be cleared out.
 * 
 */
void ncdev_quiesce_exec_on_proc_exit_v1(void);
void ncdev_quiesce_exec_on_proc_exit_v2(void);

/**
 * ncdev_bar_write_data() - write data to bar
 * 
 * @param nd: neuron device
 * @param bar: the BAR to write to
 * @param reg_addresses
 * @param data: the data to be written into the bar
 * @param data_count: the number of data to be written
 * @return 0 on success, otherwise failure
 * 
 * V1:
 *    For BAR0 the addresses are passed as array(random access).
 *    For BAR2 a single address is provided and driver does sequential writes.
 * V2:
 *    Only BAR0 is used right now. TODO: change runtime ioctl
*/
int ncdev_bar_write_data_v1(struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count);
int ncdev_bar_write_data_v2(struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count);


/* UDMA Functions */
/**
 * udma_m2s_data_rd_cfg_boundaries_set(): set data_rd_cfg to break at 256B boundaries
 * 
 * @param udma: the UDMA structure
 * 
 * for V1, this function is dummy
 */
void udma_m2s_data_rd_cfg_boundaries_set_v1(struct udma *udma);
void udma_m2s_data_rd_cfg_boundaries_set_v2(struct udma *udma);

/**
 * udma_q_config() - set misc queue configurations
 *
 * @param udma_q udma_q: the queue data structure
 *
 * for V1, this function is dummy
 */
void udma_q_config_v1(struct udma_q *udma_q);
void udma_q_config_v2(struct udma_q *udma_q);


/* NDMA Functions */
/**
 * ndma_get_wait_for_completion_time() - calculate the first and the following wait times for a DMA tranfer completion
 * 
 *      One full descriptor takes ~4 usec to transfer (64K at 16G/sec) on V2  and ~16 usec to transfer on V1.
 *      The last descriptor may be partial, so wait 1/4 64K transfer time for that descriptor.
 *      Also, count includes the completion descriptor so don't include that in the count.
 * 
 * @param first_wait_time: the wait time for the first sleep
 * @param wait_time: the wait time for the following sleeps
 */
void ndma_get_wait_for_completion_time_v1(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
void ndma_get_wait_for_completion_time_v1_qemu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
void ndma_get_wait_for_completion_time_v1_emu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
void ndma_get_wait_for_completion_time_v2(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
void ndma_get_wait_for_completion_time_v2_qemu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
void ndma_get_wait_for_completion_time_v2_emu(u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);

/**
 * ndma_validate_pa() - check the validity of the desc physical addresses
 *      V1:
 *         west side: PCIEX4_1_BASE: 0x00c00000000000 host: PCIEX8_0_BASE: 0x00400000000000
 *         If west side is set then even host bit is set. When mc_alloc is called we set only the host bit
 *         and insert into tree.. If some one sets the west side on that PA, then there is no way to check that,
 *         since there could be a tdram address that could have the west side set
 *         (that will look as though host is also set)
 *      V2:
 *         similar idea.  Just check for valid address allocated in host memory
 *
 * @param nd: the neuron device
 * @param pa: the desc physical addresses
 * @param dst_mc: the mc that backs the dma queue
 * @return int: return 0 if the pa is valid; otherwise return negative
 */
int ndma_validate_pa_v1(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type);
int ndma_validate_pa_v2(struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type);

/**
 * ndma_init() - Initialize a DMA engine
 * 
 * @param bar0: BAR0
 * @param udma: UDMA structure
 * @param eng_id: DMA engine index to initialize
 * @return int: 0 on success, otherwise on failure
 */
int ndma_init_v1(void __iomem *bar0, struct udma *udma, int eng_id);
int ndma_init_v2(void __iomem *bar0, struct udma *udma, int eng_id);

/**
 * ndma_is_bar0_write_blocked() - is BAR0 blocked for write access
 *      1. Block write access from user space to some APB MISC RAMs
 *      2. Block write access from user space to DMA queues
 *    Both of these accesses are only allowed through the driver
 *
 * @param offset: offset to be checked as blocked or not
 * @return int: return -1 if the access should be blocked, otherwise return 0.
 */
int ndma_is_bar0_write_blocked_v1(u64 off);
int ndma_is_bar0_write_blocked_v2(u64 off);

/**
 * ndma_get_m2m_barrier_type() - get the m2m barrier type
 * 
 * @param set_dmb 
 * @return int 
 */
int ndma_get_m2m_barrier_type_v1(bool set_dmb);
int ndma_get_m2m_barrier_type_v2(bool set_dmb);

#endif
