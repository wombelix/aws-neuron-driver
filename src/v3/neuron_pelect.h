
// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#ifndef NEURON_PELECT_H
#define NEURON_PELECT_H

struct neuron_device;

/**
 * npe_election_exec_on_rst() - execute election code for a primary/secondary device after the initial reset
 *
 * @nd: Neuron device
 * @reset_successful: indicates the device reset successfully
 *
 * Return: 0 if election was successful.  Currently not used.
 */
int npe_election_exec_on_rst(struct neuron_device *nd, bool reset_successful);

/**
 * npe_init() - initialize pod election code.
 *
 */
int npe_init(void);

/**
 * npe_cleanup() - cleanup and pod state left around (miscram)
 *
 */
void npe_cleanup(void);

/**
 * npe_notify_mark() - notify election code of a change in the crwl mark count
 *
 */
void npe_notify_mark(int mark_cnt, bool mark);

/**
 * npe_get_pod_id() - return pod id
 *
 * @pod_id: 
 */
int npe_get_pod_id(u8 *pod_id);

/**
 * npe_get_pod_status() - return pod status information
 *
 * @state:   state of the election
 * @node_id: pod node id or -1 - valid when election is complete
 */
int npe_get_pod_status(u32 *state, u8 *node_id);

/**
 * npe_pod_ctrl() - request a change to pod state
 *
 * @nd:		    neuron device
 * @ctrl:    	control change request
 * @timeout: 	timeout for the control operation
 * @state:   	state of the election
 */
int npe_pod_ctrl(struct neuron_device *nd, u32 ctrl, u32 timeout, u32 *state);

/**
 * npe_class_node_id_show_data() - return sysfs class node_id
 *
 * @buf:		    sysfs buffer
 */
ssize_t npe_class_node_id_show_data(char *buf);

/**
 * npe_class_server_id_show_data() - return sysfs class server_id
 *
 * @buf:		    sysfs buffer
 */
ssize_t npe_class_server_id_show_data(char *buf);
#endif
