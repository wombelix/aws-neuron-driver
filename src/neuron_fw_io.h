// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019-2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef __FWIO_H__
#define __FWIO_H__

#include <linux/types.h>

struct fw_io_request {
	u8 sequence_number; // sequence number to be copied in the next response.
	u8 command_id; // command to hw.
	u16 size; // request size in bytes including the header.
	u32 crc32; // crc32 of the entire request, crc32 must be set to 0 before calculating
	u8 data[0];
};

struct fw_io_response {
	u8 sequence_number; // request sequence number
	u8 error_code; // 0 means request was successfully completed
	u16 size; // response size in bytes including this header
	u8 data[0]; // response data if any
};

enum { FW_IO_CMD_READ = 1, // read a register value
	FW_IO_CMD_POST_TO_CW = 2 // post given blob as metrics to CloudWatch
};

enum { FW_IO_SUCCESS = 0, // completed successfully
	FW_IO_FAIL, // request failed, no further information should be returned
	FW_IO_UNKNOWN_COMMAND // request failed because command is not supported
};

// Bitmap of PIR reset types to be written to FW_IO_REG_RESET_OFFSET
enum { FW_IO_RESET_TYPE_DEVICE = 1,
	FW_IO_RESET_TYPE_TPB = 2  // Requires FW_IO_REG_RESET_TPB_MAP_OFFSET to be populated with a tpb map prior to use
};

// offsets in MISC RAM for FWIO
enum {
	FW_IO_REG_DEVICE_ID_OFFSET = 0x24,

	// MISC RAM slots for ECC error counters for V2
	//   - ECC counters for V2 which are currently being placed in MISC RAM register 16, 17 and 18 by Pacific. 
	//   - The upper 16 bits of each register represent corrected errors, and the lower 16 bits represent uncorrected errors.
	FW_IO_REG_SRAM_ECC_OFFSET = 0x40, // 16 * 4 bytes
	FW_IO_REG_HBM0_ECC_OFFSET = 0x44, // 17 * 4 bytes
	FW_IO_REG_HBM1_ECC_OFFSET = 0x48, // 18 * 4 bytes

	FW_IO_REG_METRIC_OFFSET = 0x100, // 0x100 to 0x17F, 128 bytes
	FW_IO_REG_RESET_TPB_MAP_OFFSET = 0x1d8,
	FW_IO_REG_RESET_OFFSET = 0x1ec,
	FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET = 0x1f4,
	FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET = 0x1f0,
	FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET = 0x1fc,
	FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET = 0x1f8,
	FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET = 0x800,
};

struct fw_io_ctx {
	void __iomem *bar0;
	u8 next_seq_num;
	struct fw_io_request *request;
	struct fw_io_response *response;
	u64 request_addr;
	u64 response_addr;
	u32 request_response_size; // for simplicity always use the same buffer size for request and response
	u64 fw_io_err_count;
	struct mutex lock;
};

#define UINT64_LOW(x) ((u32)(((u64)(x)) & 0xffffffffULL))
#define UINT64_HIGH(x) ((u32)((x) >> 32))

/**
 * fw_io_read_csr_array() - Read CSR(s) and return the value(s).
 *
 * @ptrs: Array of register address to read
 * @values: Read values stored here
 * @num_csrs: Number of CSRs to read
 * @operational: true if the read expects the device to be in operational state
 *
 * Return: 0 if CSR read is successful, a negative error code otherwise.
 */
int fw_io_read_csr_array(void **ptrs, u32 *values, u32 num_csrs, bool operational);

/** Read the list of addresses given in the address list and returns it's values in the value list
 *
 * @param ctx[in]	- FWIO context
 * @param addr_in[in]	- List of registers to read
 * @param values[out]	- Buffer to store results.
 * @param num_req[in]	- Total number of registers in the addr_in
 *
 * @return 0 on success 1 on error
 */
int fw_io_read(struct fw_io_ctx *ctx, u64 addr_in[], u32 val_out[], u32 num_req);


/**
 * fw_io_setup() - Setup new FWIO for given device.
 *
 * @bar0: BAR0 virtual address
 * @bar0_size: Size of BAR0
 * @bar2: BAR2 virtual address
 * @bar2_size: Size of BAR2
 *
 * Return: fwio context on success, NULL on failure.
 */
struct fw_io_ctx *fw_io_setup(void __iomem *bar0, u64 bar0_size,
				  void __iomem *bar2, u64 bar2_size);

/**
 * fw_io_destroy() - Removes previously setup FWIO.
 *
 * @ctx: fwio context
 */
void fw_io_destroy(struct fw_io_ctx *ctx);

/**
 * fw_io_post_metric() - Post given block data as metric to FWIO
 *
 * @ctx: fwio context
 * @data: data to post
 * @size: size of the data
 *
 * Return: 0 if metric is successfully posted, a negative error code otherwise.
 */
int fw_io_post_metric(struct fw_io_ctx *ctx, u8 *data, u32 size);

/**
 * fw_io_initiate_reset() - Initiate device local reset.
 *
 * @bar0: Device's BAR0 base address
 * @device_reset: True if we are doing a device-level reset
 * @tpb_reset_map: If device_reset is false (tpb reset), bitmap of blocks to reset
 *     [1:0] NC mask
 *     [13:8] TopSp mask
 */
void fw_io_initiate_reset(void __iomem *bar0, bool device_reset, u32 tpb_reset_map);

/**
 * fw_io_is_reset_initiated() - Check if local reset is initiated or not.
 *
 * @bar0: Device's BAR0 base address
 *
 * Return: true if reset is initiated, false if reset is not yet started.
 */
bool fw_io_is_reset_initiated(void __iomem *bar0);

/**
 * fw_io_read_counters() - Reads hardware counters
 *
 * @ctx - FWIO context of the device for which counters are read.
 * @addr_in: hardware counter addresses to read
 * @val_out: counters values
 * @num_ctrs: number of counters to read
 *
 * Return: 0 on success.
 *
 */
int fw_io_read_counters(struct fw_io_ctx *ctx, uint64_t addr_in[], uint32_t val_out[],
			uint32_t num_counters);

/**
 * fw_io_topology() - Discovers devices connected to the given device.
 *
 * @ctx: FWIO context of the device for which topology
 * @pdev_index: the nd->pdev->device index
 * @device_id: The index of the neuron device
 * @connected_device_ids:  Connected device IDs are stored here.
 * @count: Number of devices connected to the given device.
 *
 * Return: 0 on success.
 *
 */
int fw_io_topology(struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count);

/**
 * fw_io_device_id_read() - Read device id
 * @param bar - from bar
 * @param device_id  - output device id
 * @return  0 on success.
 */
int fw_io_device_id_read(void *bar0, u32 *device_id);

/**
 * fw_io_device_id_write() - Read device id
 * @param bar - to bar
 * @param device_id  - output device id
 */
void fw_io_device_id_write(void *bar0, u32 device_id);

/**
 * fw_io_get_err_count() - gets the fw io error count
 * @ctx - FWIO context of the device for which counters are read.
 * @return  fw io error count on success.
 */
u64 fw_io_get_err_count(struct fw_io_ctx *ctx);

/**
 * fw_io_ecc_read() - Read ECC errors
 * 
 * @param bar: from bar
 * @param ecc_offset: one of FW_IO_REG_SRAM_ECC_OFFSET, FW_IO_REG_HBM0_ECC_OFFSET, and FW_IO_REG_HBM1_ECC_OFFSET
 * @param ecc_err_count: output ecc error count
 * 
 * @return 0 on success
 */
int fw_io_ecc_read(void *bar0, uint64_t ecc_offset, uint32_t *ecc_err_count);

#endif
