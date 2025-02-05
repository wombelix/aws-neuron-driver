
// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

#ifndef NEURON_PELECT_H
#define NEURON_PELECT_H

struct neuron_device;

/**
 * npe_election_exec() - execute election code for a primary/secondary device
 *
 * @nd: Neuron device
 * @reset_successful: indicates the device reset successfully
 *
 * Return: 0 if election was successful.  Currently not used.
 */
int npe_election_exec(struct neuron_device *nd, bool reset_successful);

/**
 * npe_cleanup() - cleanup and pod state left around (miscram)
 *
 */
void npe_cleanup(void);

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
 * npe_pod_ctrl() - force a change to pod state
 *
 * @ctrl:   control change request
 * @state:  state of the election
 */
int npe_pod_ctrl(u32 ctrl, u32 *state);
#endif
