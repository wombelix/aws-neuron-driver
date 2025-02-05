// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#include "neuron_ds.h"

#ifndef _NEURON_METRICS_H
#define _NEURON_METRICS_H

#define NEURON_METRICS_VERSION_STRING_MAX_LEN 63
#define NEURON_METRICS_MAX_POSTING_BUF_SIZE 128
#define NEURON_METRICS_RT_VERSION_CAPACITY 4
#define NEURON_METRICS_MAX_CAPACITY 4 // largest version capacity value

// Version information that will be recaptured every posting session
enum nmetric_version_type {
	NMETRIC_VERSION_FIRST,
	NMETRIC_RT_VERSION = 0, // neuron-rt version
	NMETRIC_VERSION_COUNT
};

// Version information captured only once that remains constant relative to the device
enum nmetric_constants_type {
	NMETRIC_CONSTANTS_FIRST,
	NMETRIC_DRIVER_VERSION = 0, // driver version
	NMETRIC_INSTANCE_ID = 1, // instance id
	NMETRIC_CONSTANTS_COUNT
};

enum nmetric_counter_type {
	NMETRIC_COUNTER_FIRST,
	NMETRIC_NERR_INFER_OK = 0, // inference completed with no errors
	NMETRIC_NERR_GENERIC_FAIL = 1, // inference completed with a non-specific error
	NMETRIC_TIMED_OUT = 2,
	NMETRIC_BAD_INPUT = 3,
	NMETRIC_NUM_ERR = 4,
	NMETRIC_MODEL_ERR = 5,
	NMETRIC_TRANSIENT_ERR = 6,
	NMETRIC_HW_ERR = 7,
	NMETRIC_RT_ERR = 8,
	NMETRIC_COMPLETED_WITH_ERR = 9,
	NMETRIC_COMPLETED_WITH_NUMERIC_ERR = 10,
	NMETRIC_NERR_GENERIC_TPB_ERR = 11,
	NMETRIC_NERR_RESOURCE = 12,
	NMETRIC_NERR_RESOURCE_NC = 13,
	NMETRIC_NERR_QUEUE_FULL = 14,
	NMETRIC_NERR_INVALID = 15,
	NMETRIC_NERR_UNSUPPORTED_NEFF = 16,
	NMETRIC_FW_IO_ERR = 17,

	NMETRIC_COUNTER_COUNT
};

struct nmetric_versions {
	int curr_count; // number of versions currently stored
	u64 version_metrics[NEURON_METRICS_MAX_CAPACITY];
};

struct nmetric_aggregation_thread {
	struct task_struct *thread; // aggregation thread that sends metrics every ~5 minutes
	wait_queue_head_t wait_queue;
	volatile bool running; // if cleared, thread would exit the loop
};

struct neuron_metrics {
	struct nmetric_versions runtime_versions;
	u64 ds_freed_metrics_buf[NMETRIC_COUNTER_COUNT]; // stores unsent metrics about to be freed from datastore
	struct nmetric_aggregation_thread neuron_aggregation; // aggregation thread that periodically aggregates and posts metrics
	u8 posting_buffer[NEURON_METRICS_MAX_POSTING_BUF_SIZE];
};

/**
 * nmetric_init_constants_metrics() - Gathers and stores device constant informatation for metric posting. 
 * 
 * @note Should be called before metric posting is initialized
 * 
 */
void nmetric_init_constants_metrics(void);

/**
 * nmetric_partial_aggregate() - Aggregates all metrics in specified datastore entry. Expects datastore lock to have been acquired
 * 
 * @nd: neuron device
 * @entry: valid initialized datastore entry to aggregate metrics from
 * 
 */
void nmetric_partial_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry);

/**
 * neuron_stop_thread() - Stop aggregation thread of specified neuron device
 *
 * @nd: neuron device 
 *
 */
void nmetric_stop_thread(struct neuron_device *nd);

/** nmetric_init() - Initializes neuron metric aggregation for the driver
 *
 * @nd: neuron device 
 *
 * Return: 0 on success, < 0 error code on failure
 */
int nmetric_init(struct neuron_device *nd);

#endif
