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
	uint64_t port_1_base;

	// sizes
	uint64_t mmap_nc_size;

	// counts
	int nc_per_device;
	uint64_t semaphore_count;
	uint64_t event_count;
	uint32_t ts_per_device;
	int dma_eng_per_nc;
	int dma_eng_per_nd;
	int dram_channels; 
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
	u8 (*ts_nq_get_nqid)(struct neuron_device *nd, u8 index, u32 nq_type);
    void (*ts_nq_set_hwaddr) (struct neuron_device *nd, u8 ts_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);
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
    int (*ndmar_get_h2t_qid) (uint32_t nc_id);
    bool (*ndmar_is_h2t_q) (struct neuron_device *nd, uint32_t eng_id, uint32_t q_id);
    bool (*nr_init_h2t_eng) ( int nc_idx, uint32_t nc_map); 
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
    u64 dram_bar_size;

    int (*neuron_pci_release_bar) (struct pci_dev *dev, int bar);
    int (*neuron_pci_reserve_bar) (struct pci_dev *dev, int bar, const char *res_name);
    int (*neuron_pci_set_npdev) (struct pci_dev *dev,
                                int bar,
                                const char *res_name,
                                phys_addr_t *bar_pa,
                                void __iomem **bar_ioaddr,
                                u64 *bar_size);
    int (*neuron_pci_get_device_id) (struct neuron_device *nd, struct pci_dev *dev);
    int (*neuron_pci_device_id_to_rid_map) (uint32_t * count, uint32_t * did_to_rid_map);
};

struct ndhal_cdev {
    struct ncdev_mem_region *ncdev_mem_regions;
    u64 *ncdev_bar0_write_blocked_addrs;

    void (*ncdev_compatible_version) (struct neuron_ioctl_compatible_version *arg);
    void (*ncdev_quiesce_exec_on_proc_exit) (void);
    int (*ncdev_bar_write_data) (struct neuron_device *nd, u8 bar, u64 *reg_addresses, u32 *data, u32 data_count);
};

struct ndhal_udma {
	unsigned int num_beats;
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
    unsigned int pci_device_id;

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
 * @return int 0 on success, negative for failures
 */
int ndhal_register_funcs_v1(void);
int ndhal_register_funcs_v2(void);
int ndhal_register_funcs_v3(void);

#endif
