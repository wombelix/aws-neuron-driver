/*
* Copyright 2022, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

#include "neuron_device.h"
#include "neuron_ds.h"
#include "neuron_sysfs_metrics.h"
#include "neuron_dhal.h"

#define NEURON_ARCH_MAX_LEN       20

#define SYSFS_METRIC_ID_TO_NDS_NC_COUNTER_ID(metric_id) (metric_id)
#define SYSFS_METRIC_ID_TO_NDS_ND_COUNTER_ID(metric_id) (metric_id - NDS_NC_COUNTER_COUNT)

#define ATTR_INFO(_attr_name, _metric_id, _attr_type) { \
    .attr_name = _attr_name,                            \
    .metric_id = _metric_id,                            \
    .attr_type = _attr_type                             \
}
#define TOTAL_ATTR_INFO(_metric_id)                     \
    ATTR_INFO(__stringify(total), _metric_id, TOTAL)
#define PRESENT_ATTR_INFO(_metric_id)                   \
    ATTR_INFO(__stringify(present), _metric_id, PRESENT)
#define PEAK_ATTR_INFO(_metric_id)                      \
    ATTR_INFO(__stringify(peak), _metric_id, PEAK)
#define COUNTER_ATTR_INFO_TBL(_metric_id) {             \
    TOTAL_ATTR_INFO(_metric_id),                        \
    PRESENT_ATTR_INFO(_metric_id)                       \
}
#define MEM_USAGE_COUNTER_ATTR_INFO_TBL(_metric_id) {   \
    TOTAL_ATTR_INFO(_metric_id),                        \
    PRESENT_ATTR_INFO(_metric_id),                      \
    PEAK_ATTR_INFO(_metric_id)                          \
}
#define COUNTER_NODE_INFO(_node_name, _metric_id) {     \
    .node_name = _node_name,                            \
    .metric_id = _metric_id,                            \
    .attr_cnt = 2,                                      \
    .attr_info_tbl = COUNTER_ATTR_INFO_TBL(_metric_id)  \
}
#define MEM_COUNTER_NODE_INFO(_node_name, _metric_id) { \
    .node_name = _node_name,                            \
    .metric_id = _metric_id,                            \
    .attr_cnt = 3,                                      \
    .attr_info_tbl = MEM_USAGE_COUNTER_ATTR_INFO_TBL(_metric_id)\
}

bool nsysfsmetric_notify = false;

struct metric_attribute {
    struct attribute attr;
    ssize_t          (*show) (struct nsysfsmetric_metrics *sysfs_metrics, struct metric_attribute *attr, char *buf);
    ssize_t          (*store) (struct nsysfsmetric_metrics *sysfs_metrics, struct metric_attribute *attr, const char *buf, size_t count);
    int              nc_id;
    int              metric_id;
    int              attr_type;
};

// Three default metrics categories: status, memory usage, and custom.
const static nsysfsmetric_counter_node_info_t status_counter_nodes_info_tbl[] = {
    COUNTER_NODE_INFO("success",         NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_COMPLETED)),
    COUNTER_NODE_INFO("failure",         NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_GENERIC_FAIL)),
    COUNTER_NODE_INFO("timeout",         NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_TIMED_OUT)),
    COUNTER_NODE_INFO("exec_bad_input",  NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_INCORRECT_INPUT)),
    COUNTER_NODE_INFO("hw_error",        NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_HW)),
    COUNTER_NODE_INFO("execute_completed_with_error",      NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_COMPLETED_WITH_ERR)),
    COUNTER_NODE_INFO("execute_completed_with_num_error",  NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_COMPLETED_WITH_NUM_ERR)),
    COUNTER_NODE_INFO("generic_error",               NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_GENERIC)),
    COUNTER_NODE_INFO("resource_error",              NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_RESOURCE)),
    COUNTER_NODE_INFO("resource_nc_error",           NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_RESOURCE_NC)),
    COUNTER_NODE_INFO("execute_failed_to_queue",     NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_FAILED_TO_QUEUE)),
    COUNTER_NODE_INFO("invalid_error",               NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_INVALID)),
    COUNTER_NODE_INFO("unsupported_neff_version",    NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_UNSUPPORTED_NEFF_VERSION))
};
const static int status_counter_nodes_info_tbl_cnt = sizeof(status_counter_nodes_info_tbl) / sizeof(nsysfsmetric_counter_node_info_t);

const static nsysfsmetric_counter_node_info_t device_mem_category_counter_nodes_info_tbl[] = {
    MEM_COUNTER_NODE_INFO("model_code",       NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MEM_USAGE_CODE_DEVICE)),
    MEM_COUNTER_NODE_INFO("tensors",    NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MEM_USAGE_TENSORS_DEVICE)),
    MEM_COUNTER_NODE_INFO("constants",  NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MEM_USAGE_CONSTANTS_DEVICE)),
    MEM_COUNTER_NODE_INFO("model_shared_scratchpad", NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MEM_USAGE_SCRATCHPAD_DEVICE)),
    MEM_COUNTER_NODE_INFO("runtime_memory",       NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MEM_USAGE_MISC_DEVICE))
};
const static int device_mem_category_counter_nodes_info_tbl_cnt = sizeof(device_mem_category_counter_nodes_info_tbl) / sizeof(nsysfsmetric_counter_node_info_t);

const static nsysfsmetric_counter_node_info_t host_mem_category_counter_nodes_info_tbl[] = {
    MEM_COUNTER_NODE_INFO("dma_buffers",       NDS_ND_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_ND_COUNTER_MEM_USAGE_CODE_HOST)),
    MEM_COUNTER_NODE_INFO("tensors",    NDS_ND_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_ND_COUNTER_MEM_USAGE_TENSORS_HOST)),
    MEM_COUNTER_NODE_INFO("constants",  NDS_ND_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_ND_COUNTER_MEM_USAGE_CONSTANTS_HOST)),
    MEM_COUNTER_NODE_INFO("application_memory",       NDS_ND_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_ND_COUNTER_MEM_USAGE_MISC_HOST))
};
const static int host_mem_category_counter_nodes_info_tbl_cnt = sizeof(host_mem_category_counter_nodes_info_tbl) / sizeof(nsysfsmetric_counter_node_info_t);

const static nsysfsmetric_counter_node_info_t custom_counter_nodes_info_tbl[] = {
    COUNTER_NODE_INFO("model_load_count",    NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MODEL_LOAD_COUNT)),
    COUNTER_NODE_INFO("reset_req_count",     NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_RESET_REQ_COUNT)),
    COUNTER_NODE_INFO("reset_fail_count",    NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_RESET_FAIL_COUNT)),
    COUNTER_NODE_INFO("inference_count",     NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFERENCE_COUNT)),
    COUNTER_NODE_INFO("flop_count",          NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MAC_COUNT)),
};
const static int custom_counter_nodes_info_tbl_cnt = sizeof(custom_counter_nodes_info_tbl) / sizeof(nsysfsmetric_counter_node_info_t);

const static nsysfsmetric_attr_info_t arch_info_attrs_info_tbl[] = {
    ATTR_INFO("arch_type", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE), OTHER),
};
const static int arch_info_attrs_info_tbl_cnt = sizeof(arch_info_attrs_info_tbl) / sizeof(nsysfsmetric_attr_info_t);

const static nsysfsmetric_attr_info_t ecc_attrs_info_tbl[] = {
    ATTR_INFO("sram_ecc_uncorrected", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_ECC_SRAM_UNCORRECTED), OTHER),
    ATTR_INFO("mem_ecc_uncorrected",  NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_ECC_HBM_UNCORRECTED),  OTHER),
};
const static int ecc_attrs_info_tbl_cnt = sizeof(ecc_attrs_info_tbl) / sizeof(nsysfsmetric_attr_info_t);

// Root node's attrs and its child nodes' attrs
const static nsysfsmetric_attr_info_t root_info_node_attrs_info_tbl[] = {
    ATTR_INFO("notify_delay", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY), OTHER),
};
const static int root_info_node_attrs_info_tbl_cnt = sizeof(root_info_node_attrs_info_tbl) / sizeof(nsysfsmetric_attr_info_t);

const static nsysfsmetric_attr_info_t root_arch_node_attrs_info_tbl[] = {
    ATTR_INFO("arch_type", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE), OTHER),
    ATTR_INFO("instance_type", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_INSTANCE_TYPE), OTHER),
    ATTR_INFO("device_name", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_DEVICE_NAME), OTHER),
};
const static int root_arch_node_attrs_info_tbl_cnt = sizeof(root_arch_node_attrs_info_tbl) / sizeof(nsysfsmetric_attr_info_t);


static void nsysfsmetric_node_release(struct kobject *kobj)
{
    struct nsysfsmetric_node *node = container_of(kobj, struct nsysfsmetric_node, kobj);

    mutex_destroy(&node->lock);
    struct attribute **attrs = node->attr_group->attrs;
    struct metric_attribute *attr = container_of(attrs[0], struct metric_attribute, attr);
    int i = 0;
    while (attr != NULL) {
        kfree(attr);
        attr = container_of(attrs[++i], struct metric_attribute, attr);
    }
    kfree(attrs);
    kfree(node->attr_group);

    kfree(node);
}

static struct nsysfsmetric_metrics *nsysfsmetric_find_metrics(struct kobject *node_kobj)
{
    struct nsysfsmetric_node *cur_node = container_of(node_kobj, struct nsysfsmetric_node, kobj);

    while (cur_node->is_root == false) {
        cur_node = container_of(cur_node->kobj.parent, struct nsysfsmetric_node, kobj);
    }

    struct nsysfsmetric_metrics *sysfs_metrics = container_of(cur_node, struct nsysfsmetric_metrics, root);
    return sysfs_metrics;
}

static ssize_t nsysfsmetric_generic_metric_attr_show(struct kobject *node_kobj, struct attribute *attr, char *buf)
{
    struct nsysfsmetric_metrics *sysfs_metrics = nsysfsmetric_find_metrics(node_kobj);
    struct metric_attribute *metric_attr = container_of(attr, struct metric_attribute, attr);

    if (!metric_attr->show) {
        return -EIO;
    }

    return metric_attr->show(sysfs_metrics, metric_attr, buf);
}

static ssize_t nsysfsmetric_generic_metric_attr_store(struct kobject *node_kobj, struct attribute *attr, const char *buf, size_t len)
{
    struct nsysfsmetric_metrics *sysfs_metrics = nsysfsmetric_find_metrics(node_kobj);
    struct metric_attribute *metric_attr = container_of(attr, struct metric_attribute, attr);

    if (!metric_attr->store) {
        return -EIO;
    }

    ssize_t length = metric_attr->store(sysfs_metrics, metric_attr, buf, len);
    if (nsysfsmetric_notify)
        sysfs_notify(node_kobj, NULL, attr->name);

    return length;
}

static const struct sysfs_ops nsysfsmetric_generic_metric_sysfs_ops = {
    .show = nsysfsmetric_generic_metric_attr_show,
    .store = nsysfsmetric_generic_metric_attr_store,
};
static struct kobj_type nsysfsmetric_node_ktype = {
    .sysfs_ops = &nsysfsmetric_generic_metric_sysfs_ops,
    .release = nsysfsmetric_node_release,
};

static void nsysfsmetric_get_neuron_architecture(struct nsysfsmetric_metrics *sysfs_metrics, struct metric_attribute *attr, int metric_id, char *arch)
{
    struct neuron_device *nd = container_of(sysfs_metrics, struct neuron_device, sysfs_metrics);
    unsigned short device = nd->pdev->device;
    char *arch_prefix = "";
    char *arch_suffix = "";

    if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE)) {
        int nc_id = attr->nc_id;
        if (nc_id >= 0) {
            arch_prefix = "NC";
        } else {
            arch_prefix = "ND";
        }
    }

    switch (device) {
        case INF1_DEVICE_ID0:
        case INF1_DEVICE_ID1:
        case INF1_DEVICE_ID2:
        case INF1_DEVICE_ID3:
            if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE)) {
                arch_suffix = "v1";
            } else if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_INSTANCE_TYPE)) {
                arch_suffix = "Inf1";
            } else if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_DEVICE_NAME)) {
                arch_suffix = "Inferentia";
            }
            break;
        case TRN1_DEVICE_ID0:
            if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE)) {
                arch_suffix = "v2";
            } else if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_INSTANCE_TYPE)) {
                arch_suffix = "Trn1";
            } else if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_DEVICE_NAME)) {
                arch_suffix = "Trainium1";
            }
            break;
        case INF2_DEVICE_ID0:
            if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE)) {
                arch_suffix = "v3";
            } else if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_INSTANCE_TYPE)) {
                arch_suffix = "Inf2";
            } else if (metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_DEVICE_NAME)) {
                arch_suffix = "Inferentia2";
            }
            break;
        default:
            break;
    }

    snprintf(arch, NEURON_ARCH_MAX_LEN, "%s%s", arch_prefix, arch_suffix);
}

static ssize_t nsysfsmetric_show_nrt_total_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                char *buf)
{
    ssize_t len = 0;

    if (attr->metric_id < 0 || attr->metric_id >= MAX_METRIC_ID || attr->nc_id < 0 || attr->nc_id >= MAX_NC_PER_DEVICE) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type TOTAL\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    u64 count = sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].total;
    len = sprintf(buf, "%llu\n", count);

    return len;
}

static ssize_t nsysfsmetric_show_nrt_present_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                    struct metric_attribute *attr,
                                                    char *buf)
{
    ssize_t len = 0;

    if (attr->metric_id < 0 || attr->metric_id >= MAX_METRIC_ID || attr->nc_id < 0 || attr->nc_id >= MAX_NC_PER_DEVICE) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type PRESENT\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    u64 count = sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].present;
    len = sprintf(buf, "%llu\n", count);

    return len;
}

static ssize_t nsysfsmetric_show_nrt_peak_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                  struct metric_attribute *attr,
                                                  char *buf)
{
    ssize_t len = 0;

    if (attr->metric_id < 0 || attr->metric_id >= MAX_METRIC_ID || attr->nc_id < 0 || attr->nc_id >= MAX_NC_PER_DEVICE) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type PEAK\n", attr->metric_id, attr->nc_id);
        return 0;
    }

    u64 count = sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].peak;
    len = sprintf(buf, "%llu\n", count);

    return len;
}

static ssize_t nsysfsmetric_show_nrt_other_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                char *buf)
{
    ssize_t len = 0;

    if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_ARCH_TYPE) 
        || attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_INSTANCE_TYPE)
        || attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NEURON_DEVICE_NAME)) {
        char neuron_arch[NEURON_ARCH_MAX_LEN];
        nsysfsmetric_get_neuron_architecture(sysfs_metrics, attr, attr->metric_id, neuron_arch);
        len = sprintf(buf, "%s\n", neuron_arch);
    } else if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY)) {
        if (nsysfsmetric_notify)
            len = sprintf(buf, "0\n");
        else
            len = sprintf(buf, "-1\n");
    } else if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_ECC_SRAM_UNCORRECTED)) {
        struct neuron_device *nd = container_of(sysfs_metrics, struct neuron_device, sysfs_metrics);
        uint64_t ecc_offset = FW_IO_REG_SRAM_ECC_OFFSET;
        uint32_t ecc_err_count = 0;
        int ret = fw_io_ecc_read(nd->npdev.bar0, ecc_offset, &ecc_err_count);
        if (ret) {
            ecc_err_count = 0;
            pr_err("sysfs failed to read ECC SRAM error from FWIO\n");
        } else if (ecc_err_count == 0xdeadbeef) {
            ecc_err_count = 0;
        }
        len = sprintf(buf, "%u\n", ecc_err_count & 0x0000ffff);
    } else if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_ECC_HBM_UNCORRECTED)) {
        struct neuron_device *nd = container_of(sysfs_metrics, struct neuron_device, sysfs_metrics);

        uint32_t total_uncorrected_ecc_err_count = 0;
        uint32_t ecc_err_count = 0;
        uint64_t ecc_offset = FW_IO_REG_HBM0_ECC_OFFSET;
        int ret = fw_io_ecc_read(nd->npdev.bar0, ecc_offset, &ecc_err_count);
        if (ret) {
            pr_err("sysfs failed to read ECC HBM0 error from FWIO\n");
        } else if (ecc_err_count != 0xdeadbeef) {
            total_uncorrected_ecc_err_count += (ecc_err_count & 0x0000ffff);
        }

        ecc_err_count = 0;
        ecc_offset = FW_IO_REG_HBM1_ECC_OFFSET;
        ret = fw_io_ecc_read(nd->npdev.bar0, ecc_offset, &ecc_err_count);
        if (ret) {
            pr_err("sysfs failed to read ECC HBM1 error from FWIO\n");
        } else if (ecc_err_count != 0xdeadbeef) {
            total_uncorrected_ecc_err_count += (ecc_err_count & 0x0000ffff);
        }

        len = sprintf(buf, "%u\n", total_uncorrected_ecc_err_count);
    } else {
        pr_err("cannot show sysfs metrics for nc_id=%d, metric_id=%d of attr_type OTHER \n", attr->nc_id, attr->metric_id);
    }

    return len;
}

static ssize_t nsysfsmetric_set_nrt_total_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                    struct metric_attribute *attr,
                                                    const char *buf, size_t size)
{
    if (attr->metric_id < 0 || attr->metric_id >= MAX_METRIC_ID || attr->nc_id < 0 || attr->nc_id >= MAX_NC_PER_DEVICE) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type TOTAL\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].total = 0;

    return size;
}

static ssize_t nsysfsmetric_set_nrt_present_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                const char *buf, size_t size)
{
    if (attr->metric_id < 0 || attr->metric_id >= MAX_METRIC_ID || attr->nc_id < 0 || attr->nc_id >= MAX_NC_PER_DEVICE) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type PRESENT\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].present = 0;

    return size;
}

static ssize_t nsysfsmetric_set_nrt_peak_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                 struct metric_attribute *attr,
                                                 const char *buf, size_t size)
{
    if (attr->metric_id < 0 || attr->metric_id >= MAX_METRIC_ID || attr->nc_id < 0 || attr->nc_id >= MAX_NC_PER_DEVICE) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type PEAK\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].peak = 0;

    return size;   
}

static ssize_t nsysfsmetric_set_nrt_other_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                const char *buf, size_t size)
{
    if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_NOTIFY_DELAY)) {
        int64_t new_notify_delay = 0;
        int ret = kstrtoll(buf, 10, &new_notify_delay);
        if (ret || (new_notify_delay != -1 && new_notify_delay != 0)) {
            pr_err("received invalid notify delay for sysfs metrics: %s", buf);
            return size;
        } else if (new_notify_delay == -1) {
            nsysfsmetric_notify = false;
        } else if (new_notify_delay == 0) {
            nsysfsmetric_notify = true;
        }
    } else {
        pr_err("cannot set sysfs metrics for nc_id=%d, metric_id=%d of attr_type OTHER\n", attr->nc_id, attr->metric_id);
    }

    return size;
}

static struct metric_attribute *nsysfsmetric_create_attr(const char *metric_name, int nc_id, int metric_id, int attr_type)
{
    struct metric_attribute *metric_attr = kzalloc(sizeof(struct metric_attribute), GFP_KERNEL);
    if (!metric_attr) {
        pr_err("failed to allocate memory for metric attribute with nc_id=%d, metric_id=%d, attr_type=%d\n", nc_id, metric_id, attr_type);
        return NULL;
    }

    metric_attr->attr.name = metric_name;
    metric_attr->attr.mode = VERIFY_OCTAL_PERMISSIONS(S_IWUSR | S_IRUSR);
    switch (attr_type) {
        case TOTAL:
            metric_attr->show = nsysfsmetric_show_nrt_total_metrics;
            metric_attr->store = nsysfsmetric_set_nrt_total_metrics;			
            break;
        case PRESENT:
            metric_attr->show = nsysfsmetric_show_nrt_present_metrics;
            metric_attr->store = nsysfsmetric_set_nrt_present_metrics;			
            break;
        case PEAK:
            metric_attr->show = nsysfsmetric_show_nrt_peak_metrics;
            metric_attr->store = nsysfsmetric_set_nrt_peak_metrics;			
            break;
        case OTHER:
            metric_attr->show = nsysfsmetric_show_nrt_other_metrics;
            metric_attr->store = nsysfsmetric_set_nrt_other_metrics;			
            break;
        default:
            metric_attr->show = NULL;
            metric_attr->store = NULL;
            break;
    }
    metric_attr->metric_id = metric_id;
    metric_attr->nc_id = nc_id;
    metric_attr->attr_type = attr_type;

    return metric_attr;
}

static struct attribute_group *nsysfsmetric_init_attr_group(int attr_info_tbl_cnt,
                                                            const nsysfsmetric_attr_info_t *attr_info_tbl,
                                                            int nc_id,
                                                            struct nsysfsmetric_node *new_node,
                                                            struct nsysfsmetric_metrics *sysfs_metrics)
{
    int i;

    struct attribute **attrs = kzalloc(sizeof(struct attribute *) * (attr_info_tbl_cnt + 1), GFP_KERNEL);
    if (!attrs) {
        pr_err("failed to allocate memory for metric attrs\n");
        return NULL;
    }

    // create and add attributes
    for (i = 0; i < attr_info_tbl_cnt; i++) {
        struct metric_attribute *metric_attr = nsysfsmetric_create_attr(attr_info_tbl[i].attr_name, nc_id, attr_info_tbl[i].metric_id, attr_info_tbl[i].attr_type);
        if (!metric_attr) {
            kfree(attrs);
            return NULL;
        }
        attrs[i] = &metric_attr->attr;
        if (nc_id >= 0) {
            sysfs_metrics->nrt_metrics[attr_info_tbl[i].metric_id][nc_id].node = new_node;
        }
    }
    attrs[attr_info_tbl_cnt] = NULL;

    // create attribute group
    struct attribute_group *attr_group = kzalloc(sizeof(struct attribute_group), GFP_KERNEL);
    if (!attr_group) {
        pr_err("failed to allocate memory for metric attr_group\n");
        kfree(attrs);
        return NULL;
    }
    attr_group->attrs = attrs;

    return attr_group;
}

static struct nsysfsmetric_node *nsysfsmetric_init_and_add_one_node(struct nsysfsmetric_metrics *sysfs_metrics,
                                                                    struct nsysfsmetric_node *parent_node,
                                                                    const char *node_name,
                                                                    bool is_root,
                                                                    int nc_id,
                                                                    int attr_info_tbl_cnt,
                                                                    const nsysfsmetric_attr_info_t *attr_info_tbl)
{
    int ret;

    struct nsysfsmetric_node *new_node = kzalloc(sizeof(struct nsysfsmetric_node), GFP_KERNEL);
    if (!new_node) {
        pr_err("failed to allocate memory for a sysfs directory called %s\n", node_name);
        return NULL;
    }

    mutex_init(&new_node->lock);
    new_node->is_root = is_root;
    new_node->child_node_num = 0;

    ret = kobject_init_and_add(&new_node->kobj, &nsysfsmetric_node_ktype, &parent_node->kobj, "%s", node_name);
    if (ret) {
        pr_err("failed to init and add a directory kobj for %s\n", node_name);
        return NULL;
    }

    struct attribute_group *attr_group = nsysfsmetric_init_attr_group(attr_info_tbl_cnt, attr_info_tbl, nc_id, new_node, sysfs_metrics);
    if (!attr_group) {
        pr_err("failed to allocate an attr group for %s\n", node_name);
        return NULL;
    }
    new_node->attr_group = attr_group;

    ret = sysfs_create_group(&new_node->kobj, attr_group);
    if (ret) {
        pr_err("failed to create a group for new_node kobj via sysfs_create_group() for %s\n", node_name);
        kobject_put(&new_node->kobj);
        return NULL;
    }

    parent_node->child_nodes[parent_node->child_node_num] = new_node;
    parent_node->child_node_num++;

    return new_node;
}

static int nsysfsmetric_init_and_add_nodes(struct nsysfsmetric_metrics *sysfs_metrics,
                                           struct nsysfsmetric_node *parent_node,
                                           int child_node_info_tbl_cnt,
                                           const nsysfsmetric_counter_node_info_t *child_node_info_tbl,
                                           int nc_id)
{
    int i;

    // Create a list of nodes specified by child_node_info_tbl under parent_node
    for (i = 0; i < child_node_info_tbl_cnt; i++) {
        struct nsysfsmetric_node *new_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics,
                                                                                parent_node,
                                                                                child_node_info_tbl[i].node_name, 
                                                                                false,
                                                                                nc_id,
                                                                                child_node_info_tbl[i].attr_cnt, 
                                                                                child_node_info_tbl[i].attr_info_tbl);
        if (!new_node) {
            pr_err("failed to add a sysfs subdirectory %s under %s\n", child_node_info_tbl[i].node_name, parent_node->kobj.name);
            return -1;
        }
    }

    return 0;
}

static int nsysfsmetric_init_and_add_nc_default_nodes(struct neuron_device *nd, struct nsysfsmetric_node *parent_node)
{
    int ret;
    int nc_id;

    struct nsysfsmetric_metrics *sysfs_metrics = &nd->sysfs_metrics;

    for (nc_id = 0; nc_id < ndhal->address_map.nc_per_device; nc_id++) {
        // add the neuron_core{0,1, ...} node
        char nc_name[32];
        snprintf(nc_name, sizeof(nc_name), "neuron_core%d", nc_id);
        struct nsysfsmetric_node *nc_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, parent_node, nc_name, false, nc_id, 0, NULL);
        if (!nc_node) {
            pr_err("failed to add the %s node under %s\n", nc_name, parent_node->kobj.name);
            return -1;
        }

        // add neuron_core{0,1, ...}/stats node
        struct nsysfsmetric_node *stats_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, nc_node, "stats", false, -1, 0, NULL);
        if (!stats_node) {
            pr_err("failed to create the stats node under %s\n", nc_name);
            return -1;
        }

        // add the neuron_core{0,1, ...}/stats/status node and its children
        struct nsysfsmetric_node *status_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, stats_node, "status", false, nc_id, 0, NULL);
        if (!status_node) {
            pr_err("failed to create the status node under %s\n", nc_name);
            return -1;
        }
        ret = nsysfsmetric_init_and_add_nodes(sysfs_metrics, status_node, status_counter_nodes_info_tbl_cnt, status_counter_nodes_info_tbl, nc_id);
        if (ret) {
            pr_err("failed to add the status node's children under %s\n", nc_name);
            return ret;
        }

        // add the neuron_core{0,1, ...}/stats/memory_usage node and its children
        struct nsysfsmetric_node *mem_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, stats_node, "memory_usage", false, nc_id, 0, NULL);
        if (!mem_node) {
            pr_err("failed to create the memory_usage node under %s\n", nc_name);
            return -1;
        }
        nsysfsmetric_attr_info_t host_mem_counter_attr_tbl[3] = MEM_USAGE_COUNTER_ATTR_INFO_TBL(NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_HOST_MEM));
        struct nsysfsmetric_node *host_mem_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, mem_node, "host_mem", false, nc_id, 3, host_mem_counter_attr_tbl);
        if (!host_mem_node) {
            pr_err("failed to create the host_mem node under %s\n", nc_name);
            return -1;
        }
        nsysfsmetric_attr_info_t device_mem_counter_attr_tbl[3] = MEM_USAGE_COUNTER_ATTR_INFO_TBL(NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_DEVICE_MEM));
        struct nsysfsmetric_node *device_mem_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, mem_node, "device_mem", false, nc_id, 3, device_mem_counter_attr_tbl);
        if (!device_mem_node) {
            pr_err("failed to create the device_mem node under %s\n", nc_name);
            return -1;
        }
        ret = nsysfsmetric_init_and_add_nodes(sysfs_metrics, device_mem_node, device_mem_category_counter_nodes_info_tbl_cnt, device_mem_category_counter_nodes_info_tbl, nc_id);
        if (ret) {
            pr_err("failed to add memory usage categories (code, misc, tensors, constants, and scratchpad) under device_mem node under %s\n", nc_name);
            return ret;
        }

        // add the neuron_core{0,1, ...}/stats/other_info node and its children
        struct nsysfsmetric_node *custom_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, stats_node, "other_info", false, nc_id, 0, NULL);
        if (!custom_node) {
            pr_err("failed to create the other_info node under %s\n", nc_name);
            return -1;
        }
        ret = nsysfsmetric_init_and_add_nodes(sysfs_metrics, custom_node, custom_counter_nodes_info_tbl_cnt, custom_counter_nodes_info_tbl, nc_id);
        if (ret) {
            pr_err("failed to add the other_info node's children under %s\n", nc_name);
            return ret;
        }

        // add the neuron_core{0,1, ...}/info node
        struct nsysfsmetric_node *info_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, nc_node, "info", false, -1, 0, NULL);
        if (!info_node) {
            pr_err("failed to add info under sysfs metric root\n");
            return -1;
        }
        // add the neuron_core{0,1, ...}/info/architecture node and its attributes
        struct nsysfsmetric_node *arch_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, info_node, "architecture", false, nc_id, arch_info_attrs_info_tbl_cnt, arch_info_attrs_info_tbl);
        if (!arch_node) {
            pr_err("failed to create the architecture node and its attributes under %s\n", nc_name);
            return -1;
        }
    }

    return ret;
}

static int nsysfsmetric_find_attr(int metric_id, struct nsysfsmetric_node *node)
{
    int i = 0;
    struct attribute *attr = node->attr_group->attrs[0];

    while (attr) {
        struct metric_attribute *metric_attr = container_of(attr, struct metric_attribute, attr);
        if (metric_attr->metric_id == metric_id) {
            pr_err("attribute with metric_id=%d already exists\n", metric_id);
            return -1;
        }
        attr = node->attr_group->attrs[++i];
    }

    return 0;
}

static int nsysfsmetric_find_node(int metric_id, struct nsysfsmetric_node *parent_node)
{
    int ret;
    int i;

    for (i = 0; i < parent_node->child_node_num; i++) {
        struct nsysfsmetric_node *node = parent_node->child_nodes[i];
        ret = nsysfsmetric_find_attr(metric_id, node);
        if (ret) {
            pr_err("node with attribute of metric_id=%d already exists\n", metric_id);
            return -1;
        }
    }
    return 0;
}

static int nsysfsmetric_init_and_add_one_dynamic_counter_node(struct nsysfsmetric_metrics *sysfs_metrics,
                                                              struct nsysfsmetric_node *parent_node,
                                                              int metric_id,
                                                              int nc_id)
{
    int ret;

    if (MAX_CHILD_NODES_NUM <= parent_node->child_node_num) {
        pr_err("failed to dynamically add the sysfs_metric node with metric_id=%d. Not enough sysfs_metric storage.\n", metric_id);
        return -1;
    }

    mutex_lock(&parent_node->lock);

    // dedupe by metric_id
    ret = nsysfsmetric_find_node(metric_id, parent_node);
    if (ret) {
        pr_err("can't add the dynamic node with metric_id=%d. Attr with the same metric_id already exists\n", metric_id);
        ret = 0;
        goto done;
    }

    // add the new dynamic counter node
    char node_name[16];
    snprintf(node_name, sizeof(node_name), "new_metric_%d", metric_id);
    nsysfsmetric_attr_info_t counter_attr_tbl[2] = COUNTER_ATTR_INFO_TBL(metric_id);
    struct nsysfsmetric_node *new_node = nsysfsmetric_init_and_add_one_node(sysfs_metrics, parent_node, node_name, false, nc_id, 2, counter_attr_tbl);
    if (!new_node) {
        pr_err("can't add the dynamic node with metric_id=%d. Failed to init and add the new node\n", metric_id);
        ret = -1;
        goto done;
    }

done:
    mutex_unlock(&parent_node->lock);
    return ret;
}

static int nsysfsmetric_init_and_add_root_node(struct nsysfsmetric_metrics *metrics, struct kobject *nd_kobj)
{
    metrics->root.kobj = *nd_kobj;
    metrics->root.is_root = true;
    metrics->root.child_node_num = 0;
    mutex_init(&metrics->root.lock);
    metrics->bitmap = 0;

    memset(metrics->nrt_metrics, 0, MAX_METRIC_ID * MAX_NC_PER_DEVICE * sizeof(struct nsysfsmetric_counter));
    memset(metrics->dev_metrics, 0, MAX_METRIC_ID * sizeof(struct nsysfsmetric_counter));

    return 0;
}

static void nsysfsmetric_destroy_nodes(struct nsysfsmetric_node *node)
{
    int i;

    if (node->child_node_num == 0) {
        return;
    }

    mutex_lock(&node->lock);

    for (i = 0; i < node->child_node_num; i++) {
        struct nsysfsmetric_node *child_node = node->child_nodes[i];
        nsysfsmetric_destroy_nodes(child_node); // destroy node's children recursively
        kobject_put(&child_node->kobj);
    }
    node->child_node_num = 0;

    mutex_unlock(&node->lock);
}

static int nsysfsmetric_get_metric_id(int metric_id_category, int id)
{
    int metric_id;

    switch (metric_id_category) {
        case NDS_NC_METRIC:
            metric_id = NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(id);
            break;
        case NDS_ND_METRIC:
            metric_id = NDS_ND_COUNTER_ID_TO_SYSFS_METRIC_ID(id);
            break;
        case NON_NDS_METRIC:
            metric_id = NON_NDS_ID_TO_SYSFS_METRIC_ID(id);
            break;
        default:
            metric_id = -1;
            pr_err("metric_id_category not found. metric_id_category=%d, id=%d\n", metric_id_category, id);
            break;
    }

    if (metric_id < 0 || metric_id >= MAX_METRIC_ID) {
        metric_id = -1;
        pr_err("received out of bound metric id. metric_id_category=%d, id=%d\n", metric_id_category, id);
    }

    return metric_id;
}

static void nsysfsmetric_set_counter(struct neuron_device *nd, int metric_id_category, int metric_id, int nc_id, u64 val)
{
    if (metric_id < 0) {
        pr_err("cannot set sysfs counter at neuron core%d: invalid metric_id=%d\n", nc_id, metric_id);
        return;
    }
    mutex_lock(&nd->sysfs_metrics.root.lock);

    struct nsysfsmetric_counter *counter = &nd->sysfs_metrics.nrt_metrics[metric_id][nc_id];

    if (counter->total == val) {
        mutex_unlock(&nd->sysfs_metrics.root.lock);
        return;
    }

    counter->total = val;
    counter->present = val;
    if (val > counter->peak) {
        counter->peak = val;
        if (nsysfsmetric_notify && counter->node) {
            sysfs_notify(&counter->node->kobj, NULL, "peak");
        }
    }
    if (nsysfsmetric_notify && counter->node) {
        sysfs_notify(&counter->node->kobj, NULL, "total");
        sysfs_notify(&counter->node->kobj, NULL, "present");
    }

    mutex_unlock(&nd->sysfs_metrics.root.lock);
}

static void nsysfsmetric_fetch_mem_from_nds(struct neuron_device *nd, struct neuron_datastore_entry *entry)
{
    int i;
    int nc_id;
    int ds_id;
    int metric_id;
    u64 val;
    void *ds_base_ptr = entry->mc->va;

    for (nc_id = 0; nc_id < ndhal->address_map.nc_per_device; nc_id++) {
        // read device_mem category counters from nds
        for (i = 0; i < device_mem_category_counter_nodes_info_tbl_cnt; i++) {
            metric_id = device_mem_category_counter_nodes_info_tbl[i].metric_id;
            ds_id = SYSFS_METRIC_ID_TO_NDS_NC_COUNTER_ID(metric_id);
            val = NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[ds_id];
            nsysfsmetric_set_counter(nd, NDS_NC_METRIC, metric_id, nc_id, val);
        }
    }

    // read the host_mem category data from nds
    for (i = 0; i < host_mem_category_counter_nodes_info_tbl_cnt; i++) {
        metric_id = host_mem_category_counter_nodes_info_tbl[i].metric_id;
        ds_id = SYSFS_METRIC_ID_TO_NDS_ND_COUNTER_ID(metric_id);
        val = NDS_ND_COUNTERS(ds_base_ptr)[ds_id];
        nsysfsmetric_set_counter(nd, NDS_ND_METRIC, metric_id, 0, val);
    }

}

static int nsysfsmetric_thread_fn(void *arg)
{
    int idx;
    struct neuron_device *nd = (struct neuron_device *)arg;
    while (!kthread_should_stop() && !nd->sysfs_metrics.mem_thread.stop) {
        wait_event_interruptible_timeout(nd->sysfs_metrics.mem_thread.wait_queue, nd->sysfs_metrics.mem_thread.stop, msecs_to_jiffies(1000)); // 1 second
        if (kthread_should_stop() || nd->sysfs_metrics.mem_thread.stop)
            break;

        struct neuron_datastore *nds = &nd->datastore;
        struct neuron_datastore_entry *entry = NULL;
        for(idx = 0; idx < NEURON_MAX_DATASTORE_ENTRIES_PER_DEVICE; idx++) {
            entry = &nds->entries[idx];
            if (entry->mc == NULL) {
                pr_err("entry's mc is NULL\n");
                continue;
            }
            if (entry->pid != 0) {
                nsysfsmetric_fetch_mem_from_nds(nd, entry);
            }
        }
    }
    return 0;
}

static int nsysfsmetric_create_thread(struct neuron_device *nd)
{
    init_waitqueue_head(&nd->sysfs_metrics.mem_thread.wait_queue);
    nd->sysfs_metrics.mem_thread.stop = false;
    nd->sysfs_metrics.mem_thread.thread = kthread_run(nsysfsmetric_thread_fn, nd, "nd%d sysfs memory metrics", nd->device_index);
    if (IS_ERR_OR_NULL(nd->sysfs_metrics.mem_thread.thread)) {
        pr_err("nd%d sysfs metrics memory thread creation failed\n", nd->device_index);
        return -1;
    }
    return 0;
}

void nsysfsmetric_stop_thread(struct neuron_device *nd)
{
    if (nd->sysfs_metrics.mem_thread.thread == NULL)
        return;
    nd->sysfs_metrics.mem_thread.stop = true;
    wake_up(&nd->sysfs_metrics.mem_thread.wait_queue);
    kthread_stop(nd->sysfs_metrics.mem_thread.thread); //blocks till the thread exits
    nd->sysfs_metrics.mem_thread.thread = NULL;
}

int nsysfsmetric_register(struct neuron_device *nd, struct kobject *neuron_device_kobj)
{
    int ret;
    struct nsysfsmetric_metrics *metrics = &nd->sysfs_metrics;

    ret = nsysfsmetric_init_and_add_root_node(metrics, neuron_device_kobj);
    if (ret) {
        pr_err("cannot init the root node for neuron_device %d\n", nd->device_index);
        return ret;
    }

    // neuron{0, 1, ...}/info/
    struct nsysfsmetric_node *info_node = nsysfsmetric_init_and_add_one_node(metrics, &metrics->root, "info", false, -1, root_info_node_attrs_info_tbl_cnt, root_info_node_attrs_info_tbl);
    if (!info_node) {
        pr_err("failed to add info node under sysfs metric root\n");
        return -1;
    }
    // neuron{0, 1, ...}/info/architecture/
    struct nsysfsmetric_node *arch_node = nsysfsmetric_init_and_add_one_node(metrics, info_node, "architecture", false, -1, root_arch_node_attrs_info_tbl_cnt, root_arch_node_attrs_info_tbl);
    if (!arch_node) {
        pr_err("failed to add architecture node under info\n");
        return -1;
    }

    // neuron{0, 1, ...}/stats/
    struct nsysfsmetric_node *stats_node = nsysfsmetric_init_and_add_one_node(metrics, &metrics->root, "stats", false, -1, 0, NULL);
    if (!info_node) {
        pr_err("failed to add stats node under sysfs metric root\n");
        return -1;
    }
    // neuron{0, 1, ...}/stats/memory_usage/
    struct nsysfsmetric_node *mem_node = nsysfsmetric_init_and_add_one_node(metrics, stats_node, "memory_usage", false, -1, 0, NULL);
    if (!mem_node) {
        pr_err("failed to create the memory_usage node under stats\n");
        return -1;
    }
    // neuron{0, 1, ...}/stats/memory_usage/host_mem/
    struct nsysfsmetric_node *host_mem_node = nsysfsmetric_init_and_add_one_node(metrics, mem_node, "host_mem", false, -1, 0, NULL);
    if (!host_mem_node) {
        pr_err("failed to create the host_mem node under memory_usage\n");
        return -1;
    }
    // neuron{0, 1, ...}/stats/memory_usage/host_mem/{application_memory, constants, dma_buffers, tensors}
    ret = nsysfsmetric_init_and_add_nodes(metrics, host_mem_node, host_mem_category_counter_nodes_info_tbl_cnt, host_mem_category_counter_nodes_info_tbl, 0);
    if (ret) {
        pr_err("failed to add memory usage categories (code, misc, tensors, constants, and scratchpad) under memory_usage/host_mem\n");
        return ret;
    }
    // neuron{0, 1, ...}/stats/hardware/{sram_ecc_uncorrected, mem_ecc_uncorrected}
    if (narch_get_arch() == NEURON_ARCH_V2) { // TODO: will add V1 support soon
        struct nsysfsmetric_node *hardware_node = nsysfsmetric_init_and_add_one_node(metrics, stats_node, "hardware", false, -1, ecc_attrs_info_tbl_cnt, ecc_attrs_info_tbl);
        if (!hardware_node) {
            pr_err("failed to add hardware node its attributes under stats\n");
            return -1;
        }
    }

    // neuron{0, 1, ...}/neuron_core{0, 1, ...}/
    ret = nsysfsmetric_init_and_add_nc_default_nodes(nd, &metrics->root);
    if (ret) {
        pr_err("failed to init and add the neuron_cores directories and the default subdirectories under root\n");
        return ret;
    }

    ret = nsysfsmetric_create_thread(nd);
    if  (ret) {
        pr_err("failed to create memory thread\n");
        return ret;
    }

    return 0;
}

void nsysfsmetric_destroy(struct neuron_device *nd)
{
    nsysfsmetric_stop_thread(nd);
    nsysfsmetric_destroy_nodes(&nd->sysfs_metrics.root);
}

int nsysfsmetric_init_and_add_dynamic_counter_nodes(struct neuron_device *nd, uint64_t ds_val)
{
    int ret = 0;
    int metric_id = 0;
    int nc_id;

    struct nsysfsmetric_metrics *metrics = &nd->sysfs_metrics;
    nd->sysfs_metrics.bitmap |= ds_val;

    while (metrics->bitmap != 0) {
        if (metrics->bitmap & 1) {
            for (nc_id = 0; nc_id < ndhal->address_map.nc_per_device; nc_id++) {
                ret = nsysfsmetric_init_and_add_one_dynamic_counter_node(metrics, metrics->dynamic_metrics_dirs[nc_id], metric_id, nc_id);
                if (ret) {
                    pr_err("failed to add a new dynamic metric, where metric_id=%d, nd=%d, nc=%d\n", metric_id, nd->device_index, nc_id);
                    return ret;
                }
            }
        }
        metrics->bitmap >>= 1;
        metric_id++;
    }

    return ret;
}

void nsysfsmetric_inc_counter(struct neuron_device *nd, int metric_id_category, int id, int nc_id, u64 delta)
{
    if (delta == 0) {
        return;
    }

    int metric_id = nsysfsmetric_get_metric_id(metric_id_category, id);
    if (metric_id < 0) {
        pr_err("cannot increment sysfs counter at nc%d, metric_id_category%d, id%d. Metric id not found\n", nc_id, metric_id_category, id);
        return;
    }

    mutex_lock(&nd->sysfs_metrics.root.lock);

    struct nsysfsmetric_counter *counter = &nd->sysfs_metrics.nrt_metrics[metric_id][nc_id];
    counter->total += delta;
    counter->present = delta;
    if (counter->total > counter->peak) {
        counter->peak = counter->total;
        if (nsysfsmetric_notify && counter->node) {
            sysfs_notify(&counter->node->kobj, NULL, "peak");
        }
    }
    if (nsysfsmetric_notify && counter->node) {
        sysfs_notify(&counter->node->kobj, NULL, "total");
        sysfs_notify(&counter->node->kobj, NULL, "present");
    }

    mutex_unlock(&nd->sysfs_metrics.root.lock);
}

void nsysfsmetric_dec_counter(struct neuron_device *nd, int metric_id_category, int id, int nc_id, u64 delta)
{
    if (delta == 0) {
        return;
    }

    int metric_id = nsysfsmetric_get_metric_id(metric_id_category, id);
    if (metric_id < 0) {
        pr_err("cannot decrement sysfs counter at nc%d, metric_id_category%d, id%d. Metric id not found\n", nc_id, metric_id_category, id);
        return;
    }

    mutex_lock(&nd->sysfs_metrics.root.lock);

    struct nsysfsmetric_counter *counter = &nd->sysfs_metrics.nrt_metrics[metric_id][nc_id];
    counter->total -= delta;
    counter->present = delta;
    if (nsysfsmetric_notify && counter->node) {
        sysfs_notify(&counter->node->kobj, NULL, "total");
        sysfs_notify(&counter->node->kobj, NULL, "present");
    }

    mutex_unlock(&nd->sysfs_metrics.root.lock);
}

static void nsysfsmetric_update_counter(struct neuron_device *nd, int metric_id_category, int id, int delta)
{
    int nc_id;

    if (delta == 0) {
        return;
    }

    for (nc_id = 0; nc_id < ndhal->address_map.nc_per_device; nc_id++) {
        if (delta > 0)
            nsysfsmetric_inc_counter(nd, metric_id_category, id, nc_id, delta);
        else
            nsysfsmetric_dec_counter(nd, metric_id_category, id, nc_id, delta);
    }
}

void nsysfsmetric_inc_reset_fail_count(struct neuron_device *nd)
{
    nsysfsmetric_update_counter(nd, NON_NDS_METRIC, NON_NDS_COUNTER_RESET_FAIL_COUNT, 1);
}

void nsysfsmetric_inc_reset_req_count(struct neuron_device *nd, int nc_id)
{
    nsysfsmetric_inc_counter(nd, NON_NDS_METRIC, NON_NDS_COUNTER_RESET_REQ_COUNT, nc_id, 1);
}

void nsysfsmetric_nds_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry)
{
    int i;
    int nc_id;
    int ds_id;
    int metric_id;
    u64 delta;
    u64 val;
    void *ds_base_ptr = entry->mc->va;

    // read dynamic sysfs metric bitmap from nds
    ds_id = NDS_ND_COUNTER_DYNAMIC_SYSFS_METRIC_BITMAP;
    if (NDS_ND_COUNTERS(ds_base_ptr)[ds_id] > 0) {
        nsysfsmetric_init_and_add_dynamic_counter_nodes(nd, NDS_ND_COUNTERS(ds_base_ptr)[ds_id]);
    }

    for (nc_id = 0; nc_id < ndhal->address_map.nc_per_device; nc_id++) {
        // read status counters from nds
        for (i = 0; i < status_counter_nodes_info_tbl_cnt; i++) {
            metric_id = status_counter_nodes_info_tbl[i].metric_id;
            ds_id = SYSFS_METRIC_ID_TO_NDS_NC_COUNTER_ID(metric_id);
            delta = NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[ds_id];
            nsysfsmetric_inc_counter(nd, NDS_NC_METRIC, ds_id, nc_id, delta);
        }

        // read device_mem category counters from nds
        for (i = 0; i < device_mem_category_counter_nodes_info_tbl_cnt; i++) {
            metric_id = device_mem_category_counter_nodes_info_tbl[i].metric_id;
            ds_id = SYSFS_METRIC_ID_TO_NDS_NC_COUNTER_ID(metric_id);
            val = NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[ds_id];
            nsysfsmetric_set_counter(nd, NDS_NC_METRIC, metric_id, nc_id, val);
        }

        // read the two custom sysfs metrics from nds: model load count and inference count
        nsysfsmetric_inc_counter(nd, NDS_NC_METRIC, NDS_NC_COUNTER_MODEL_LOAD_COUNT, nc_id, NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[NDS_NC_COUNTER_MODEL_LOAD_COUNT]);
        nsysfsmetric_inc_counter(nd, NDS_NC_METRIC, NDS_NC_COUNTER_INFERENCE_COUNT, nc_id, NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[NDS_NC_COUNTER_INFERENCE_COUNT]);
        nsysfsmetric_inc_counter(nd, NDS_NC_METRIC, NDS_NC_COUNTER_MAC_COUNT, nc_id, 2 * NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[NDS_NC_COUNTER_MAC_COUNT]);  // one MAC has two floating point operations (multiply and add)
    }

    // read the host_mem category data from nds
    for (i = 0; i < host_mem_category_counter_nodes_info_tbl_cnt; i++) {
        metric_id = host_mem_category_counter_nodes_info_tbl[i].metric_id;
        ds_id = SYSFS_METRIC_ID_TO_NDS_ND_COUNTER_ID(metric_id);
        val = NDS_ND_COUNTERS(ds_base_ptr)[ds_id];
        nsysfsmetric_set_counter(nd, NDS_ND_METRIC, metric_id, 0, val);
    }
}
