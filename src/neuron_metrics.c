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

#include "neuron_metrics.h"
#include "neuron_device.h"

#define NEURON_METRICS_COUNTER_STRING_MAX_LEN 31

unsigned int nmetric_metric_post_delay = 300000; // milliseconds

module_param(nmetric_metric_post_delay, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(nmetric_metric_post_delay, "Minimum time to wait (in milliseconds) before posting metrics again");

static int nmetrics_counters_buf_size = sizeof(u64) * NMETRIC_COUNTER_LAST;

enum nmetric_metric_type {
	COUNTER = 0, // counter type metrics
	VERSION = 1 // version type metrics
};

enum nmetric_cw_id {
	NMETRIC_CW_ID_UNUSED = 0,
	NMETRIC_CW_ID_RT_VERSION = 1, // runtime version

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
	return -1;
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
			return NMETRIC_CW_ID_RT_VERSION;
		default:
			pr_err("No mapping between cloudwatch id and version metric id %d\n", internal_metric_id);
		}
	}

	return 0;
}

/**
 * nmetric_init_version_metrics() - Gathers and stores version information in corresponding device buffer for specified datastore entry
 *
 * @nd: neuron device
 * @entry: valid initialized datastore entry to aggregate version info from
 *
 */
static void nmetric_init_version_metrics(struct neuron_device *nd, struct neuron_datastore_entry *entry)
{
	// store remaining version numbers
	enum nmetric_version_type internal_metric_id;
	union nmetric_version version_info;
	void *ds_base_ptr = entry->mc->va;
	for (internal_metric_id = NMETRIC_VERSION_FIRST; internal_metric_id < NMETRIC_VERSION_LAST; internal_metric_id++) {
		int ds_index = nmetric_id_to_ds_index(internal_metric_id, VERSION);
		if (ds_index != -1)
			version_info.all = NDS_ND_COUNTERS(ds_base_ptr)[ds_index]; // decode version information
		else // default to 0 if no mapping to datastore entries exists for version metric
			version_info.all = 0;
		int version_str_size = snprintf(nd->metrics.version_metrics[internal_metric_id],
						NEURON_METRICS_VERSION_STRING_MAX_LEN + 1, 
						"%d.%d.%d",
						version_info.major_ver, 
						version_info.minor_ver,
						version_info.build_num);

		// Check version string size
		if (version_str_size > NEURON_METRICS_VERSION_STRING_MAX_LEN)
				pr_err("Version string for version metric id %d is too large for metrics buffer (with size %d) and has been truncated to fit\n",
					internal_metric_id, NEURON_METRICS_VERSION_STRING_MAX_LEN);
	}
}

/**
 * nmetric_aggregate_entry() - Aggregates all metrics in specified datastore entry to specified buffer. Counter metrics are added together. 
 * 									The first initial version metrics are gathered and cached for the lifespan of the kernel module
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
		for (internal_metric_id = NMETRIC_COUNTER_FIRST; internal_metric_id < NMETRIC_COUNTER_LAST; internal_metric_id++) {
			int ds_metric_index = nmetric_id_to_ds_index(internal_metric_id, COUNTER);
			if (ds_metric_index != -1) // check if metric mapping to datastore counter exists
				dest_buf[internal_metric_id] += NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[ds_metric_index];
		}
	}

	// aggregate version metrics if it has not been done
	if (!nd->metrics.is_version_metrics_initiated) {
		nmetric_init_version_metrics(nd, entry);
		nd->metrics.is_version_metrics_initiated = true;
	}
}

/**
 * nmetric_full_aggregation() - Aggregates all metrics in all datastore entries in device to specified buffer
 * 
 * @nd: neuron device
 * @curr_metrics: destination buffer to recieve all aggregated data from datastore entry, must be large enough to accommodate all counters being tracked
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
 * Version metrics are always posted once valid version information has been gathered.
 * 
 * @nd: neuron device
 * @curr_metrics: buffer containing metrics of the current session not yet posted to fw
 * @prev_metrics: buffer containing metrics of the previous session, last posted
 * @freed_metrics: buffer containing metrics that were freed before being posted in the current session and not captured in current metrics buf 
 * 
 */
static void nmetric_post_metrics(struct neuron_device *nd, u64 *curr_metrics, u64 *prev_metrics, u64 *freed_metrics)
{
	u32 aggregated_diff_buffer[NMETRIC_COUNTER_LAST]; // used internally for aggregating and processing before being converted and posted
	bool version_metrics_initiated = nd->metrics.is_version_metrics_initiated; // cache state of whetever version metrics have been gathered or not, as value is volatile
	char temp_buf[NEURON_METRICS_COUNTER_STRING_MAX_LEN + 1];

	// calculate total length of data buffers needed for version metrics
	enum nmetric_version_type internal_version_metric_id;
	enum nmetric_counter_type internal_counter_metric_id;
	int data_size = 0;
	int num_metrics = 0;

	// calculation for other version metrics
	if (version_metrics_initiated) {
		for (internal_version_metric_id = NMETRIC_VERSION_FIRST; internal_version_metric_id < NMETRIC_VERSION_LAST; internal_version_metric_id++) {
			num_metrics++;
			data_size += strlen(nd->metrics.version_metrics[internal_version_metric_id]);
		}
	}

	// as current counter extension buffer is volatile, cache the metrics to be sent
	// calculate total length of data buffers needed for counter metrics
	for (internal_counter_metric_id = NMETRIC_COUNTER_FIRST; internal_counter_metric_id < NMETRIC_COUNTER_LAST; internal_counter_metric_id++) {
		int metric_value = curr_metrics[internal_counter_metric_id] +
				   freed_metrics[internal_counter_metric_id] -
				   prev_metrics[internal_counter_metric_id];
		aggregated_diff_buffer[internal_counter_metric_id] = metric_value;
		if (metric_value) {
			num_metrics++;
			int metric_size = snprintf(NULL, 0, "%d", metric_value);
			data_size += min(metric_size, (int)NEURON_METRICS_COUNTER_STRING_MAX_LEN);

			// check that counter metric fits buffer
			if (metric_size > NEURON_METRICS_COUNTER_STRING_MAX_LEN)
				pr_err("Counter value for counter metric id %d is too large for metric post buffer, results have been truncated\n", internal_counter_metric_id);
		}
	}

	// allocate metric post byte array
	int total_size = data_size + sizeof(struct nmetric_cw_metric) * num_metrics;
	u8 *metrics_buf = kmalloc(total_size, GFP_KERNEL);
	if (!metrics_buf) {
		pr_err("Memory allocation of metric posting byte array buffer failed\n");
		return;
	}
	struct nmetric_cw_metric *metric = (struct nmetric_cw_metric *)metrics_buf;

	// record version metrics
	if (version_metrics_initiated) {
		for (internal_version_metric_id = NMETRIC_VERSION_FIRST; internal_version_metric_id < NMETRIC_VERSION_LAST; internal_version_metric_id++) {
			metric->id = nmetric_id_to_cw_id(internal_version_metric_id, VERSION); // if metric mapping to cw id doesn't exist, default unused id will be used
			metric->len = strlen(nd->metrics.version_metrics[internal_version_metric_id]);
			memcpy(metric->data, nd->metrics.version_metrics[internal_version_metric_id], metric->len);
			metric = (struct nmetric_cw_metric *)(((uint8_t *)metric) + sizeof(struct nmetric_cw_metric) + metric->len);
		}
	}

	// record counter metrics
	for (internal_counter_metric_id = NMETRIC_COUNTER_FIRST; internal_counter_metric_id < NMETRIC_COUNTER_LAST; internal_counter_metric_id++) {
		int metric_value = aggregated_diff_buffer[internal_counter_metric_id];
		if (!metric_value)
			continue;

		metric->id = nmetric_id_to_cw_id(internal_counter_metric_id, COUNTER); // if metric mapping to cw id doesn't exist, default unused id will be used
		metric->len = min(snprintf(NULL, 0, "%d", metric_value), (int)NEURON_METRICS_COUNTER_STRING_MAX_LEN);

		// remove the null character from the sprintf by copying to a temporary buffer than memcopying to metric post byte array
		snprintf(temp_buf, NEURON_METRICS_COUNTER_STRING_MAX_LEN + 1, "%d", metric_value);
		memcpy(metric->data, temp_buf, metric->len);

		metric = (struct nmetric_cw_metric *)(((uint8_t *)metric) + sizeof(struct nmetric_cw_metric) + metric->len);
	}

	// post metrics
	int ret = fw_io_post_metric(nd->fw_io_ctx, metrics_buf, total_size);
	if (ret < 0)
		pr_err("Metric posting failed with error code: %d\n", ret);
	
	kfree(metrics_buf);
}

/**
 * nmetric_start_new_session() - copies metrics in the buffer of the current session to the reference buffer, resets all buffers containing metrics of the current session
 * 
 * @curr_metrics: buffer containing metrics of the current session
 * @prev_metrics: reference buffer
 * @freed_metrics: cache of buffer containing metrics of freed datastore entries
 *
 */
static void nmetric_start_new_session(u64 *curr_metrics, u64 *prev_metrics, u64 *freed_metrics)
{
	// save metrics to reference array
	memcpy(prev_metrics, curr_metrics, nmetrics_counters_buf_size);

	// reset all current metrics
	memset(curr_metrics, 0, nmetrics_counters_buf_size);
	memset(freed_metrics, 0, nmetrics_counters_buf_size);
}

/**
 * nmetric_thread_fn() - periodically aggregates and posts metric at rate specified by module parameter
 */
static int nmetric_thread_fn(void *arg)
{
	struct neuron_device *nd = (struct neuron_device *)arg;
	u64 curr[NMETRIC_COUNTER_LAST]; // metrics for the current session so far
	u64 prev[NMETRIC_COUNTER_LAST]; // recorded metrics from the last post
	u64 freed[NMETRIC_COUNTER_LAST]; // cache holding metrics that were freed before the posting period was reached

	// initialize all aggregation buffers
	memset(prev, 0, nmetrics_counters_buf_size);
	memset(curr, 0, nmetrics_counters_buf_size);
	memset(freed, 0, nmetrics_counters_buf_size);
	memset(nd->metrics.ds_freed_metrics_buf, 0, nmetrics_counters_buf_size);

	// metrics are only sent once at rate specified by module param, new metric data may be saved without being immediately sent
	while (!kthread_should_stop() && nd->metrics.neuron_aggregation.running) {
		wait_event_interruptible_timeout(nd->metrics.neuron_aggregation.wait_queue, !nd->metrics.neuron_aggregation.running, msecs_to_jiffies(nmetric_metric_post_delay));
		if (kthread_should_stop() || !nd->metrics.neuron_aggregation.running) 
			break;

		// aggregate and post metrics
		neuron_ds_acquire_lock(&nd->datastore);
		nmetric_full_aggregate(nd, curr);
		memcpy(freed, nd->metrics.ds_freed_metrics_buf, nmetrics_counters_buf_size); // cache freed metrics
		memset(nd->metrics.ds_freed_metrics_buf, 0, nmetrics_counters_buf_size); // reset freed metrics buf
		neuron_ds_release_lock(&nd->datastore);

		nmetric_post_metrics(nd, curr, prev, freed);
		nmetric_start_new_session(curr, prev, freed); // reset all current metrics
	}
	return 0;
}

static int nmetric_create_thread(struct neuron_device *nd)
{
	init_waitqueue_head(&nd->metrics.neuron_aggregation.wait_queue);
	nd->metrics.neuron_aggregation.running = true;
	nd->metrics.neuron_aggregation.thread =
		kthread_run(nmetric_thread_fn, nd, "nd%d metrics", nd->device_index);
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
	if (narch_get_arch() == NEURON_ARCH_TRN) {
		pr_info("skipping metric initialization for v2 until fw_io is supported");
		return 0;
	}
	nd->metrics.is_version_metrics_initiated = false;

	// initiate metric aggregator thread
	int ret = nmetric_create_thread(nd);

	return ret;
}
