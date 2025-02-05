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


#define NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(nds_id) nds_id
#define NON_NDS_ID_TO_SYSFS_METRIC_ID(non_nds_id)    non_nds_id + NDS_ND_COUNTER_COUNT + NDS_NC_COUNTER_COUNT

#define ATTR_INFO(_attr_name, _metric_id, _attr_type) {	\
    .attr_name = _attr_name,							\
    .metric_id = _metric_id,							\
    .attr_type = _attr_type								\
}
#define COUNTER_ATTR_INFO_TBL(_metric_id) { 				\
    ATTR_INFO(__stringify(total), _metric_id, TOTAL),		\
    ATTR_INFO(__stringify(present), _metric_id, PRESENT)	\
}
#define COUNTER_NODE_INFO(_node_name, _metric_id) {		\
    .node_name = _node_name,							\
    .metric_id = _metric_id,							\
    .attr_cnt = 2,										\
    .attr_info_tbl = COUNTER_ATTR_INFO_TBL(_metric_id)	\
}

struct metric_attribute {
    struct attribute    attr;
    ssize_t (*show) (struct nsysfsmetric_metrics *sysfs_metrics, struct metric_attribute *attr, char *buf);
    ssize_t (*store) (struct nsysfsmetric_metrics *sysfs_metrics, struct metric_attribute *attr, const char *buf, size_t count);
    int                 nc_id;
    int                 metric_id;
    int                 attr_type;
};

static unsigned long refresh_rate = 150000; // milliseconds


// Three default metrics categories: status, memory usage, and custom.
const static nsysfsmetric_node_info_t status_counter_nodes_info_tbl[] = {
    COUNTER_NODE_INFO("success",         NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_COMPLETED)),
    COUNTER_NODE_INFO("failure",         NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_GENERIC_FAIL)),
    COUNTER_NODE_INFO("timeout",         NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_TIMED_OUT)),
    COUNTER_NODE_INFO("exec_bad_input",  NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_INCORRECT_INPUT)),
    COUNTER_NODE_INFO("hw_error",        NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_HW)),
    COUNTER_NODE_INFO("infer_completed_with_error",      NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_COMPLETED_WITH_ERR)),
    COUNTER_NODE_INFO("infer_completed_with_num_error",  NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_COMPLETED_WITH_NUM_ERR)),
    COUNTER_NODE_INFO("generic_error",               NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_GENERIC)),
    COUNTER_NODE_INFO("resource_error",              NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_RESOURCE)),
    COUNTER_NODE_INFO("resource_nc_error",           NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_RESOURCE_NC)),
    COUNTER_NODE_INFO("infer_failed_to_queue",       NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFER_FAILED_TO_QUEUE)),
    COUNTER_NODE_INFO("invalid_error",               NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_INVALID)),
    COUNTER_NODE_INFO("unsupported_neff_version",    NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_ERR_UNSUPPORTED_NEFF_VERSION))
};
const static int status_counter_nodes_info_tbl_cnt = sizeof(status_counter_nodes_info_tbl) / sizeof(nsysfsmetric_node_info_t);

const static nsysfsmetric_node_info_t mem_counter_nodes_info_tbl[] = {
    COUNTER_NODE_INFO("host_mem",     NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_HOST_MEM)),
    COUNTER_NODE_INFO("device_mem",   NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_DEVICE_MEM))
};
const static int mem_counter_nodes_info_tbl_cnt = sizeof(mem_counter_nodes_info_tbl) / sizeof(nsysfsmetric_node_info_t);

const static nsysfsmetric_node_info_t custom_counter_nodes_info_tbl[] = {
    COUNTER_NODE_INFO("model_load_count",    NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_MODEL_LOAD_COUNT)),
    COUNTER_NODE_INFO("reset_count",         NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_COUNTER_RESET_COUNT)),
    COUNTER_NODE_INFO("inference_count",     NDS_NC_COUNTER_ID_TO_SYSFS_METRIC_ID(NDS_NC_COUNTER_INFERENCE_COUNT))
};
const static int custom_counter_nodes_info_tbl_cnt = sizeof(custom_counter_nodes_info_tbl) / sizeof(nsysfsmetric_node_info_t);

// root node's attrs
const static nsysfsmetric_attr_info_t root_attr_info_tbl[] = {
    ATTR_INFO("arch_type", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_ARCH_TYPE), OTHER),
    ATTR_INFO("refresh_rate", NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_REFRESH_RATE), OTHER)
};
const static int root_attr_info_tbl_cnt = sizeof(root_attr_info_tbl) / sizeof(nsysfsmetric_attr_info_t);


static void nsysfsmetric_node_release(struct kobject *kobj)
{
    struct nsysfsmetric_node *node = container_of(kobj, struct nsysfsmetric_node, kobj);

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

    return metric_attr->store(sysfs_metrics, metric_attr, buf, len);
}

static const struct sysfs_ops nsysfsmetric_generic_metric_sysfs_ops = {
    .show = nsysfsmetric_generic_metric_attr_show,
    .store = nsysfsmetric_generic_metric_attr_store,
};
static struct kobj_type nsysfsmetric_root_ktype = {
    .sysfs_ops = &nsysfsmetric_generic_metric_sysfs_ops,
};
static struct kobj_type nsysfsmetric_node_ktype = {
    .sysfs_ops = &nsysfsmetric_generic_metric_sysfs_ops,
    .release = nsysfsmetric_node_release,
};

static const char *nsysfsmetric_get_neuron_arch(struct nsysfsmetric_metrics *sysfs_metrics, struct metric_attribute *attr)
{
    struct neuron_device *nd = container_of(sysfs_metrics, struct neuron_device, sysfs_metrics);
    unsigned short device = nd->pdev->device;
    const char *neuron_arch;

    switch (device) {
        case INF1_DEVICE_ID0:
        case INF1_DEVICE_ID1:
        case INF1_DEVICE_ID2:
        case INF1_DEVICE_ID3:
            neuron_arch = "v1";
            break;
        case TRN1_DEVICE_ID0:
            neuron_arch = "v2";
            break;
        case INF2_DEVICE_ID0:
            neuron_arch = "v3";
            break;
        default:
            neuron_arch = "Unknown";
            break;
    }

    return neuron_arch;
}

static ssize_t nsysfsmetric_show_nrt_total_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                char *buf)
{
    ssize_t len = 0;

    if (attr->metric_id < 0 || attr->nc_id < 0) {
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

    if (attr->metric_id < 0 || attr->nc_id < 0) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type PRESENT\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    u64 count = sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].present;
    len = sprintf(buf, "%llu\n", count);

    return len;
}

static ssize_t nsysfsmetric_show_nrt_other_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                char *buf)
{
    ssize_t len = 0;

    if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_ARCH_TYPE)) {
        const char *neuron_arch = nsysfsmetric_get_neuron_arch(sysfs_metrics, attr);
        len = sprintf(buf, "%s\n", neuron_arch);
    } else if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_REFRESH_RATE)) {
        len = sprintf(buf, "%lu\n", refresh_rate);
    } else {
        pr_err("cannot show sysfs metrics for nc_id=%d, metric_id=%d of attr_type OTHER \n", attr->nc_id, attr->metric_id);
    }

    return len;
}

static ssize_t nsysfsmetric_set_nrt_total_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                    struct metric_attribute *attr,
                                                    const char *buf, size_t size)
{
    if (attr->metric_id < 0 || attr->nc_id < 0) {
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
    if (attr->metric_id < 0 || attr->nc_id < 0) {
        pr_err("invalid metric_id %d or nc_id %d of attr_type PRESENT\n", attr->metric_id, attr->nc_id);
        return 0;
    }
    sysfs_metrics->nrt_metrics[attr->metric_id][attr->nc_id].present = 0;

    return size;
}

static ssize_t nsysfsmetric_set_nrt_other_metrics(struct nsysfsmetric_metrics *sysfs_metrics,
                                                struct metric_attribute *attr,
                                                const char *buf, size_t size)
{
    if (attr->metric_id == NON_NDS_ID_TO_SYSFS_METRIC_ID(NON_NDS_OTHER_REFRESH_RATE)) {
        unsigned long new_refresh_rate = 0;
        int ret = kstrtoul(buf, 10, &new_refresh_rate);
        if (ret || new_refresh_rate < 1000) {
            pr_err("received invalid refresh rate for sysfs metrics: %s\n", buf);
            return size;
        }
        refresh_rate = new_refresh_rate;
    } else {
        pr_err("cannot set sysfs metrics for nc_id=%d, metric_id=%d of attr_type OTHER \n", attr->nc_id, attr->metric_id);
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

static struct attribute_group *nsysfsmetric_init_attr_group(int attr_info_tbl_cnt, const nsysfsmetric_attr_info_t *attr_info_tbl, int nc_id)
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

static struct nsysfsmetric_node *nsysfsmetric_init_and_add_one_node(struct nsysfsmetric_node *parent_node,
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

    new_node->is_root = is_root;
    new_node->child_node_num = 0;

    ret = kobject_init_and_add(&new_node->kobj, &nsysfsmetric_node_ktype, &parent_node->kobj, "%s", node_name);
    if (ret) {
        pr_err("failed to init and add a directory kobj for %s\n", node_name);
        return NULL;
    }

    struct attribute_group *attr_group = nsysfsmetric_init_attr_group(attr_info_tbl_cnt, attr_info_tbl, nc_id);
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

static int nsysfsmetric_init_and_add_nodes(struct nsysfsmetric_node *parent_node,
                                        int child_node_info_tbl_cnt,
                                        const nsysfsmetric_node_info_t *child_node_info_tbl,
                                        int nc_id)
{
    int i;

    // Create a list of nodes specified by child_node_info_tbl under parent_node
    for (i = 0; i < child_node_info_tbl_cnt; i++) {
        struct nsysfsmetric_node *new_node = nsysfsmetric_init_and_add_one_node(parent_node, 
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

    for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
        // add the neuron_core node
        char nc_name[32];
        snprintf(nc_name, sizeof(nc_name), "neuron_core%d", nc_id);
        struct nsysfsmetric_node *nc_node = nsysfsmetric_init_and_add_one_node(parent_node, nc_name, false, nc_id, 0, NULL);
        if (!nc_node) {
            pr_err("failed to add the %s node under %s\n", nc_name, parent_node->kobj.name);
            return -1;
        }

        // add the status node and its children
        struct nsysfsmetric_node *status_node = nsysfsmetric_init_and_add_one_node(nc_node, "status", false, nc_id, 0, NULL);
        if (!status_node) {
            pr_err("failed to create the status node under %s\n", nc_name);
            return -1;
        }
        ret = nsysfsmetric_init_and_add_nodes(status_node, status_counter_nodes_info_tbl_cnt, status_counter_nodes_info_tbl, nc_id);
        if (ret) {
            pr_err("failed to add the status node's children under %s\n", nc_name);
            return ret;
        }

        // add the memory_usage node and its children
        struct nsysfsmetric_node *mem_node = nsysfsmetric_init_and_add_one_node(nc_node, "memory_usage", false, nc_id, 0, NULL);
        if (!mem_node) {
            pr_err("failed to create the memory_usage node under %s\n", nc_name);
            return -1;
        }
        ret = nsysfsmetric_init_and_add_nodes(mem_node, mem_counter_nodes_info_tbl_cnt, mem_counter_nodes_info_tbl, nc_id);
        if (ret) {
            pr_err("failed to add the memory_usage node's children under %s\n", nc_name);
            return ret;
        }

        // add the custom node and its children
        struct nsysfsmetric_node *custom_node = nsysfsmetric_init_and_add_one_node(nc_node, "other_info", false, nc_id, 0, NULL);
        if (!custom_node) {
            pr_err("failed to create the other_info node under %s\n", nc_name);
            return -1;
        }
        ret = nsysfsmetric_init_and_add_nodes(custom_node, custom_counter_nodes_info_tbl_cnt, custom_counter_nodes_info_tbl, nc_id);
        if (ret) {
            pr_err("failed to add the other_info node's children under %s\n", nc_name);
            return ret;
        }

        // add a dynamic node to store dynamic metrics
        nd->sysfs_metrics.dynamic_metrics_dirs[nc_id] = nsysfsmetric_init_and_add_one_node(nc_node, "dynamic_metrics", false, nc_id, 0, NULL);
        if (!nd->sysfs_metrics.dynamic_metrics_dirs[nc_id]) {
            pr_err("failed to add an empty dynamic node under %s\n", nc_name);
            return ret;
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

static int nsysfsmetric_init_and_add_one_dynamic_counter_node(struct nsysfsmetric_node *parent_node, int metric_id, int nc_id)
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
    struct nsysfsmetric_node *new_node = nsysfsmetric_init_and_add_one_node(parent_node, node_name, false, nc_id, 2, counter_attr_tbl);
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
    int ret;

    metrics->root.is_root = true;
    metrics->root.child_node_num = 0;
    metrics->bitmap = 0;

    ret = kobject_init_and_add(&metrics->root.kobj, &nsysfsmetric_root_ktype, nd_kobj, "metrics");
    if (ret) {
        pr_err("failed to init and add sysfs metrics kobj for root\n");
        kobject_put(&metrics->root.kobj);
        return ret;
    }

    struct attribute_group *attr_group = nsysfsmetric_init_attr_group(root_attr_info_tbl_cnt, root_attr_info_tbl, -1);
    if (!attr_group) {
        pr_err("failed to allocate an attr group for sysfs metrics root\n");
        return -1;
    }
    metrics->root.attr_group = attr_group;

    ret = sysfs_create_group(&metrics->root.kobj, attr_group);
    if (ret) {
        pr_err("failed to create group for sysfs metrics root\n");
        kobject_put(&metrics->root.kobj);
        return ret;
    }

    memset(metrics->nrt_metrics, 0, MAX_METRIC_ID * MAX_NC_PER_DEVICE);
    memset(metrics->dev_metrics, 0, MAX_METRIC_ID * MAX_NC_PER_DEVICE);

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
        case NON_NDS_METRIC:
            metric_id = NON_NDS_ID_TO_SYSFS_METRIC_ID(id);
            break;
        default:
            metric_id = -1;
            pr_err("metric_id_category not found! %d\n", metric_id_category);
            break;
    }

    if (metric_id > MAX_METRIC_ID) {
        metric_id = -1;
        pr_err("received out of bound metric id: %d\n", metric_id);
    }

    return metric_id;
}

int nsysfsmetric_register(struct neuron_device *nd, struct kobject *nd_kobj)
{
    int ret;
    struct nsysfsmetric_metrics *metrics = &nd->sysfs_metrics;

    ret = nsysfsmetric_init_and_add_root_node(metrics, nd_kobj);
    if (ret) {
        pr_err("cannot create the root node called metrics for neuron_device %d\n", nd->device_index);
        return ret;
    }

    ret = nsysfsmetric_init_and_add_nc_default_nodes(nd, &metrics->root);
    if (ret) {
        pr_err("failed to init and add the neuron_cores directories and the default subdirectories under them\n");
        return ret;
    }

    return 0;
}

void nsysfsmetric_destroy(struct neuron_device *nd)
{
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
            for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
                ret = nsysfsmetric_init_and_add_one_dynamic_counter_node(metrics->dynamic_metrics_dirs[nc_id], metric_id, nc_id);
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

void nsysfsmetric_nds_aggregate(struct neuron_device *nd, struct neuron_datastore_entry *entry)
{
    int i;
    int nc_id;
    int ds_id;
    u64 delta;
    void *ds_base_ptr = entry->mc->va;

    // read dynamic sysfs metric bitmap from nds
    ds_id = NDS_ND_COUNTER_DYNAMIC_SYSFS_METRIC_BITMAP;
    if (NDS_ND_COUNTERS(ds_base_ptr)[ds_id] > 0) {
        nsysfsmetric_init_and_add_dynamic_counter_nodes(nd, NDS_ND_COUNTERS(ds_base_ptr)[ds_id]);
    }

    for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
        // read status counters from nds
        for (i = 0; i < status_counter_nodes_info_tbl_cnt; i++) {
            ds_id = status_counter_nodes_info_tbl[i].metric_id;
            delta = NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[ds_id];
            nsysfsmetric_nc_inc_counter(nd, NDS_NC_METRIC, ds_id, nc_id, delta);
        }

        // read the two custom sysfs metrics from nds: model load count and inference count
        nsysfsmetric_nc_inc_counter(nd, NDS_NC_METRIC, NDS_NC_COUNTER_MODEL_LOAD_COUNT, nc_id, NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[NDS_NC_COUNTER_MODEL_LOAD_COUNT]);
        nsysfsmetric_nc_inc_counter(nd, NDS_NC_METRIC, NDS_NC_COUNTER_INFERENCE_COUNT, nc_id, NDS_NEURONCORE_COUNTERS(ds_base_ptr, nc_id)[NDS_NC_COUNTER_INFERENCE_COUNT]);
    }
}

void nsysfsmetric_nc_inc_counter(struct neuron_device *nd, int metric_id_category, int id, int nc_id, u64 delta)
{
    if (delta == 0) {
        return;
    }

    int metric_id = nsysfsmetric_get_metric_id(metric_id_category, id);
    if (metric_id < 0) {
        pr_err("cannot increment counter at nc%d, metric_id_category=%d, id%d. Metric id not found\n", nc_id, metric_id_category, id);
        return;
    }

    mutex_lock(&nd->sysfs_metrics.root.lock);
    
    nd->sysfs_metrics.nrt_metrics[metric_id][nc_id].total += delta;
    nd->sysfs_metrics.nrt_metrics[metric_id][nc_id].present = delta;

    mutex_unlock(&nd->sysfs_metrics.root.lock);
}

void nsysfsmetric_nc_dec_counter(struct neuron_device *nd, int metric_id_category, int id, int nc_id, u64 delta)
{
    if (delta == 0) {
        return;
    }

    int metric_id = nsysfsmetric_get_metric_id(metric_id_category, id);
    if (metric_id < 0) {
        pr_err("cannot decrement counter at nc%d, metric_id_category%d, id%d. Metric id not found\n", nc_id, metric_id_category, id);
        return;
    }

    mutex_lock(&nd->sysfs_metrics.root.lock);

    nd->sysfs_metrics.nrt_metrics[metric_id][nc_id].total -= delta;
    nd->sysfs_metrics.nrt_metrics[metric_id][nc_id].present = delta;

    mutex_unlock(&nd->sysfs_metrics.root.lock);
}

void nsysfsmetric_inc_counter(struct neuron_device *nd, int metric_id_category, int id, int delta)
{
    int nc_id;

    if (delta == 0) {
        return;
    }

    for (nc_id = 0; nc_id < NC_PER_DEVICE(nd); nc_id++) {
        nsysfsmetric_nc_inc_counter(nd, metric_id_category, id, nc_id, delta);
    }
}

void nsysfsmetric_inc_reset_count(struct neuron_device *nd)
{
    nsysfsmetric_inc_counter(nd, NON_NDS_METRIC, NON_NDS_COUNTER_RESET_COUNT, 1); 
}
