// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/ctype.h>

#include "neuron_metrics.h"
#include "neuron_device.h"

unsigned int nmetric_metric_post_delay = 300000; // milliseconds

module_param(nmetric_metric_post_delay, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(nmetric_metric_post_delay, "Minimum time to wait (in milliseconds) before posting metrics again");

static int nmetric_counters_buf_size = sizeof(u64) * NMETRIC_COUNTER_COUNT;
static int nmetric_versions_buf_size = sizeof(struct nmetric_versions);
static int nmetric_constants_buf_size = sizeof(char) * NMETRIC_CONSTANTS_COUNT * (NEURON_METRICS_VERSION_STRING_MAX_LEN + 1);

static char nmetric_constant_metrics[NMETRIC_CONSTANTS_COUNT][NEURON_METRICS_VERSION_STRING_MAX_LEN + 1];
static const char nmetric_instance_id_path[] = "/sys/devices/virtual/dmi/id/board_asset_tag";
extern const char driver_version[];

enum nmetric_metric_type {
	COUNTER = 0, // counter type metrics
	VERSION = 1, // version type metrics
	CONSTANT = 2 // constant type metrics (relative to device)
};

enum nmetric_cw_id {
	NMETRIC_CW_ID_UNUSED = 0,
	NMETRIC_CW_ID_RT_VERSION = 1, // runtime version
	NMETRIC_CW_ID_INSTANCE_ID = 12, // instance id
	NMETRIC_CW_ID_DRIVER_VERSION = 13, // driver version

	// Extra versions
	// extra space for reporting multiple versions of the same type in one post
	NMETRIC_CW_ID_RT_VERSION_BASE = 180, // base id for rt version
	NMETRIC_CW_ID_RT_VERSION_0 = NMETRIC_CW_ID_RT_VERSION_BASE,
	NMETRIC_CW_ID_RT_VERSION_1,
	NMETRIC_CW_ID_RT_VERSION_2,
	NMETRIC_CW_ID_RT_VERSION_3,
	NMETRIC_CW_ID_RT_VERSION_LAST = NMETRIC_CW_ID_RT_VERSION_3, // inclusive of last version

	// Return codes
	NMETRIC_CW_ID_NERR_OK = 200, // status ok
	NMETRIC_CW_ID_NERR_FAIL = 201 // status fail
};

struct nmetric_cw_metric {
	u8 id;
	u8 len;
	u8 data[];
} __attribute__((__packed__));

union nmetric_version {
	struct {
		u64 build_num : 32;
		u64 minor_ver : 8;
		u64 major_ver : 8;
		u64 reserved : 16;
	};
	u64 all;
};

/**
 * nmetric_id_to_ds_index() - Converts metric aggregation internal datastucture metric id to index of metric in datastore
 *
 * @internal_metric_id: index of metric inside internal data structure 
 * @metric_type: type of metric being converted
 *
 * Returns corresponding datastore index, -1 if no mapping exists
 */
static int nmetric_id_to_ds_index(int internal_metric_id, enum nmetric_metric_type metric_type)
{
	if (metric_type == COUNTER) {
		switch (internal_metric_id) {
		case NMETRIC_NERR_OK:
			return NDS_NC_COUNTER_INFER_COMPLETED;
		case NMETRIC_NERR_GENERIC_FAIL:
			return NDS_NC_COUNTER_ERR_GENERIC;
		default:
			pr_err("No mapping between datastore counter and counter metric id %d\n", internal_metric_id);
		}
	} else if (metric_type == VERSION) {
		switch (internal_metric_id) {
		case NMETRIC_RT_VERSION:
			return NDS_ND_COUNTER_RUNTIME_VERSION;
		default:
			pr_err("No mapping between datastore counter and version metric id %d\n", internal_metric_id);
		}
	}

	return -1; // invalid id
}

/**
 * nmetric_id_to_cw_id() - Converts metric aggregation internal datastucture metric id to cloudwatch recognized index for metric
 *
 * @internal_metric_id: index of metric inside internal data structure 
 * @metric_type: type of metric being converted
 *
 * Returns corresponding cloudwatch index, 0 if no mapping exists
 */
static enum nmetric_cw_id nmetric_id_to_cw_id(int internal_metric_id, enum nmetric_metric_type metric_type)
{
	if (metric_type == COUNTER) {
		switch (internal_metric_id) {
		case NMETRIC_NERR_OK:
			return NMETRIC_CW_ID_NERR_OK;
		case NMETRIC_NERR_GENERIC_FAIL:
			return NMETRIC_CW_ID_NERR_FAIL;
		default:
			pr_err("No mapping between cloudwatch id and counter metric id %d\n", internal_metric_id);
		}
	} else if (metric_type == VERSION) {
		switch (internal_metric_id) {
		case NMETRIC_RT_VERSION:
			return NMETRIC_CW_ID_RT_VERSION_BASE;
		default:
			pr_err("No mapping between cloudwatch id and version metric id %d\n", internal_metric_id);
		}
	} else if (metric_type == CONSTANT) {
		switch (internal_metric_id) {
		case NMETRIC_DRIVER_VERSION:
			return NMETRIC_CW_ID_DRIVER_VERSION;
		case NMETRIC_INSTANCE_ID:
			return NMETRIC_CW_ID_INSTANCE_ID;
		}
	}

	return 0; // if metric mapping to cw id doesn't exist, default unused id will be used
}

void nmetric_init_constants_metrics()
{
	// initiate buffer to 0
	memset(nmetric_constant_metrics, 0, nmetric_constants_buf_size);

	// read instance id from sysfile
	int read_size;
	loff_t offset = 0;
	struct file *f = filp_open(nmetric_instance_id_path, O_RDONLY, 0);
	if (IS_ERR_OR_NULL(f) || (read_size = kernel_read(f, nmetric_constant_metrics[NMETRIC_INSTANCE_ID], NEURON_METRICS_VERSION_STRING_MAX_LEN, &offset)) <= 0)
		memset(nmetric_constant_metrics[NMETRIC_INSTANCE_ID], '0', sizeof(char)); // if instance id could not be read, default to 0
	else if (isspace(nmetric_constant_metrics[NMETRIC_INSTANCE_ID][read_size - 1])) // remove trailing space if present
		nmetric_constant_metrics[NMETRIC_INSTANCE_ID][read_size - 1] = '\0';

	if (!IS_ERR_OR_NULL(f))
		filp_close(f, NULL);

	// record driver version
	int driver_ver_str_len = strlen(driver_version);
	BUG_ON(driver_ver_str_len > NEURON_METRICS_VERSION_STRING_MAX_LEN); // check for buffer overflow
	memcpy(nmetric_constant_metrics[NMETRIC_DRIVER_VERSION], driver_version, min(driver_ver_str_len, (int)NEURON_METRICS_VERSION_STRING_MAX_LEN));
}

/**
 * nmetric_aggregate_version_metrics() - Gathers and stores version information of specified version metric in specified buffer for specified datastore entry
 *
 * @entry: valid initialized datastore entry to aggregate version info from
 * @ds_index: ds index of version metric to be recorded
 * @capacity: maximum number of versions to be recorded per posting session
 * @versions_buf: specified buffer where versions will be recorded
 *
 */
static void nmetric_aggregate_version_metrics(struct neuron_datastore_entry *entry, int ds_index, int capacity, struct nmetric_versions *versions_buf)
{
	void *ds_base_ptr = entry->mc->va;

	// check if storage capacity has been reached for metric
	int version_curr_count = versions_buf->curr_count;
	if (version_curr_count == capacity)
		return;

	// temporarily store version
	u64 version_info = NDS_ND_COUNTERS(ds_base_ptr)[ds_index]; // decode version information

	// check if version has already been stored
	int i;
	for (i = 0; i < version_curr_count; i++)
		if (version_info == versions_buf->version_metrics[i])
			return;

	// save runtime version
	versions_buf->version_metrics[version_curr_count] = version_info;
	versions_buf->curr_count++;
}

/**
 * nmetric_aggregate_entry()
 * 
 * Aggregates all metrics in specified datastore entry to specified buffer. Counter metrics are added together. 
 * Multiple version metrics are can be gathered per posting session up to a predefined limit. Any excess versions will be discarded
 * 
 * @nd: neuron device
 * @entry: valid initialized datastore entry to aggregate metrics from
 * @dest_buf: destination buffer to recieve all aggregated data from datastore entry, must be large enough to accommodate all counters being tracked
 * 
 */
static void nmetric_aggregate_entry(struct neuron_device *nd, struct neuron_datastore_entry *entry, u64 *dest_buf)
{
	int nc_id;
	enum nmetric_counter_type internal_metric_id;

	// aggregate all counter metrics from given datastore entry into extension array
	void *ds_base_ptr = entry->mc->va;
	for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
		for (internal_metric_id = NMETRIC_COUNTER_FIRST; internal_metric_id < NMETRIC_COUNTER_COUNT; internal_metric_id++) {
			int ds_metric_index = nmetric_id_to_ds_index(internal_metric_id, COUNTER);
			if (ds_metric_index != -1) // check if metric mapping to datastore counter exists
				dest_buf[internal_metric_id] += NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[ds_metric_index];
		}
	}

	// aggregate version metrics
	nmetric_aggregate_version_metrics(entry, NDS_ND_COUNTER_RUNTIME_VERSION, NEURON_METRICS_RT_VERSION_CAPACITY, &nd->metrics.runtime_versions);
}

/**
 * nmetric_full_aggregation() - Aggregates all metrics in all datastore entries in device to specified buffer
 * 
 * @nd: neuron device
 * @curr_metrics: destination buffer to recieve all aggregated data from datastore entry, must be large enough to accommodate all counters being tracked
 * 
 */
static void nmetric_full_aggregate(struct neuron_device *nd, u64 *curr_metrics)
{
	// aggregate counter metrics in all cores of all entries of the datastore into current count array
	int i;
	for (i = 0; i < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; i++)
		if (neuron_ds_check_entry_in_use(&nd->datastore, i)) // ensure that datastore entry is in use and valid
			nmetric_aggregate_entry(nd, &nd->datastore.entries[i], curr_metrics);
}

// Wrapper function for entry aggregate function
void nmetric_partial_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry)
{
	nmetric_aggregate_entry(nd, entry, nd->metrics.ds_freed_metrics_buf);
}

/**
 * nmetric_post_metrics()  
 * 
 * Sends a byte array of metrics in string form to fw. Differential counter metrics are sent (as compared to the last posting); 
 * counter metrics with 0 difference from last posting are not posted. Extremely large counter metrics may be truncated and will log an error.
 * Multiple version metrics may be posted at once up to a predefined limit, versions beyond this limit will be discarded.
 * 
 * @nd: neuron device
 * @curr_metrics: buffer containing metrics of the current session not yet posted to fw
 * @prev_metrics: buffer containing metrics of the previous session, last posted
 * @freed_metrics: buffer containing metrics that were freed before being posted in the current session and not captured in current metrics buf 
 * @versions: buffer containing version metrics gathered from the current session
 * @constants_metrics: buffer containing metrics constant to the device
 * 
 */
static void nmetric_post_metrics(struct neuron_device *nd, u64 *curr_metrics, u64 *prev_metrics, u64 *freed_metrics, struct nmetric_versions versions)
{
	enum nmetric_constants_type internal_constants_metric_id;
	enum nmetric_counter_type internal_counter_metric_id;
	int data_size = 0;
	struct nmetric_cw_metric *metric = (struct nmetric_cw_metric *)nd->metrics.posting_buffer;

	// record constant metrics
	for (internal_constants_metric_id = NMETRIC_CONSTANTS_FIRST; internal_constants_metric_id < NMETRIC_CONSTANTS_COUNT; internal_constants_metric_id++) {
		// check if there is enough space in buffer
		int expected_len = strlen(nmetric_constant_metrics[internal_constants_metric_id]);
		if (NEURON_METRICS_MAX_POSTING_BUF_SIZE - data_size < sizeof(struct nmetric_cw_metric) + expected_len)
			break;

		// save metrics to buffer
		metric->id = nmetric_id_to_cw_id(internal_constants_metric_id, CONSTANT);
		metric->len = expected_len;
		memcpy(metric->data, nmetric_constant_metrics[internal_constants_metric_id], metric->len);

		data_size += sizeof(struct nmetric_cw_metric) + metric->len;
		metric = (struct nmetric_cw_metric *)&nd->metrics.posting_buffer[data_size];
	}

	// record version metrics
	int version_i;
	for (version_i = 0; version_i < versions.curr_count; version_i++) {
		union nmetric_version version_info;
		version_info.all = versions.version_metrics[version_i];

		// check if there is enough space in buffer
		int expected_len = snprintf(NULL, 0, "%d.%d.%d", version_info.major_ver, version_info.minor_ver, version_info.build_num);
		if (NEURON_METRICS_MAX_POSTING_BUF_SIZE - data_size < sizeof(struct nmetric_cw_metric) + (expected_len + 1))
			break;

		// save metrics to buffer
		metric->id = NMETRIC_CW_ID_RT_VERSION_BASE + version_i;
		metric->len = expected_len; // null char will be replaced by next metric and should not be considered in the length
		snprintf(metric->data, metric->len + 1, "%d.%d.%d", version_info.major_ver, version_info.minor_ver, version_info.build_num);

		data_size += sizeof(struct nmetric_cw_metric) + metric->len;
		metric = (struct nmetric_cw_metric *)&nd->metrics.posting_buffer[data_size];
	}

	// record counter metrics
	for (internal_counter_metric_id = NMETRIC_COUNTER_FIRST; internal_counter_metric_id < NMETRIC_COUNTER_COUNT; internal_counter_metric_id++) {
		int metric_value = curr_metrics[internal_counter_metric_id] +
				   freed_metrics[internal_counter_metric_id] -
				   prev_metrics[internal_counter_metric_id];
		if (!metric_value)
			continue;

		// check if there is enough space in buffer
		int expected_len = snprintf(NULL, 0, "%d", metric_value);
		if (NEURON_METRICS_MAX_POSTING_BUF_SIZE - data_size < sizeof(struct nmetric_cw_metric) + (expected_len + 1))
			break;

		// save metrics to buffer
		metric->id = nmetric_id_to_cw_id(internal_counter_metric_id, COUNTER); // if metric mapping to cw id doesn't exist, default unused id will be used
		metric->len = expected_len; // null char will be replaced by next metric and should not be considered in the length
		snprintf(metric->data, metric->len + 1, "%d", metric_value);

		data_size += sizeof(struct nmetric_cw_metric) + metric->len;
		metric = (struct nmetric_cw_metric *)&nd->metrics.posting_buffer[data_size];
	}

	// post metrics
	int ret = fw_io_post_metric(nd->fw_io_ctx, nd->metrics.posting_buffer, data_size);
	if (ret < 0)
		pr_err("Metric posting failed with error code: %d\n", ret);
}

/** 
 * nmetric_cache_shared_bufs() - Caches neuron device buffer values to avoid needing extra locks
 * 
 * @nd: neuron device 
 * @freed_metrics[out]: will contain freed counter data copied from neuron device aggregation
 * @versions[out]: will contain version metrics data copied from neuron device aggregation
 * 
 */
static void nmetric_cache_shared_bufs(struct neuron_device *nd, u64 *freed_metrics, struct nmetric_versions *versions)
{
	// cache and reset freed metrics buf
	memcpy(freed_metrics, nd->metrics.ds_freed_metrics_buf, nmetric_counters_buf_size);
	memset(nd->metrics.ds_freed_metrics_buf, 0, nmetric_counters_buf_size);

	// cache and reset version metrics buf
	memcpy(versions, &nd->metrics.runtime_versions, nmetric_versions_buf_size);
	memset(&nd->metrics.runtime_versions, 0, nmetric_versions_buf_size);
}

/**
 * nmetric_start_new_session() - Copies metrics in the buffer of the current session to the reference buffer, resets all buffers containing metrics of the current session
 * 
 * @curr_metrics: buffer containing metrics of the current session
 * @prev_metrics: reference buffer
 * @freed_metrics: cache of buffer containing metrics of freed datastore entries
 *
 */
static void nmetric_start_new_session(u64 *curr_metrics, u64 *prev_metrics, u64 *freed_metrics)
{
	// save metrics to reference array
	memcpy(prev_metrics, curr_metrics, nmetric_counters_buf_size);

	// reset all current metrics
	memset(curr_metrics, 0, nmetric_counters_buf_size);
	memset(freed_metrics, 0, nmetric_counters_buf_size);
}

/**
 * nmetric_thread_fn() - periodically aggregates and posts metric at rate specified by module parameter
 * 
 * @arg: expected to be a pointer to neuron device
 * 
 */
static int nmetric_thread_fn(void *arg)
{
	struct neuron_device *nd = (struct neuron_device *)arg;
	u64 curr[NMETRIC_COUNTER_COUNT]; // metrics for the current session so far
	u64 prev[NMETRIC_COUNTER_COUNT]; // recorded metrics from the last post
	u64 freed[NMETRIC_COUNTER_COUNT]; // cache holding metrics that were freed before the posting period was reached
	struct nmetric_versions runtime_versions;

	// initialize all aggregation buffers
	memset(prev, 0, nmetric_counters_buf_size);
	memset(curr, 0, nmetric_counters_buf_size);
	memset(freed, 0, nmetric_counters_buf_size);
	memset(&runtime_versions, 0, nmetric_versions_buf_size);

	// metrics are only sent once at rate specified by module param, new metric data may be saved without being immediately sent
	while (!kthread_should_stop() && nd->metrics.neuron_aggregation.running) {
		wait_event_interruptible_timeout(nd->metrics.neuron_aggregation.wait_queue, !nd->metrics.neuron_aggregation.running, msecs_to_jiffies(nmetric_metric_post_delay));
		if (kthread_should_stop() || !nd->metrics.neuron_aggregation.running)
			break;

		// aggregate and post metrics
		neuron_ds_acquire_lock(&nd->datastore);
		nmetric_full_aggregate(nd, curr);
		nmetric_cache_shared_bufs(nd, freed, &runtime_versions);
		neuron_ds_release_lock(&nd->datastore);

		nmetric_post_metrics(nd, curr, prev, freed, runtime_versions);
		nmetric_start_new_session(curr, prev, freed); // reset all current metrics
	}
	return 0;
}

static int nmetric_create_thread(struct neuron_device *nd)
{
	init_waitqueue_head(&nd->metrics.neuron_aggregation.wait_queue);
	nd->metrics.neuron_aggregation.running = true;
	nd->metrics.neuron_aggregation.thread = kthread_run(nmetric_thread_fn, nd, "nd%d metrics", nd->device_index);
	if (IS_ERR_OR_NULL(nd->metrics.neuron_aggregation.thread)) {
		pr_err("nd%d metrics aggregation thread creation failed\n", nd->device_index);
		return -1;
	}
	return 0;
}

void nmetric_stop_thread(struct neuron_device *nd)
{
	if (nd->metrics.neuron_aggregation.thread == NULL)
		return;
	nd->metrics.neuron_aggregation.running = false;
	wake_up(&nd->metrics.neuron_aggregation.wait_queue);
	kthread_stop(nd->metrics.neuron_aggregation.thread); //blocks till the thread exits
	nd->metrics.neuron_aggregation.thread = NULL;
}

int nmetric_init(struct neuron_device *nd)
{
	BUG_ON(NMETRIC_CW_ID_RT_VERSION_LAST - NMETRIC_CW_ID_RT_VERSION_BASE + 1 < NEURON_METRICS_RT_VERSION_CAPACITY); // ensure there exists enough cw id space for requested runtime version posting capacity

	if (narch_get_arch() == NEURON_ARCH_TRN) {
		pr_info("skipping metric initialization for v2 until fw_io is supported");
		return 0;
	}

	memset(nd->metrics.ds_freed_metrics_buf, 0, nmetric_counters_buf_size);

	// initiate metric aggregator thread
	int ret = nmetric_create_thread(nd);

	return ret;
}
