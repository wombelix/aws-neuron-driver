#ifndef NEURON_DHAL_H
#define NEURON_DHAL_H

struct neuron_device;

struct neuron_dhal {
    int arch;
    // TODO: add structs of different driver functionalities below, including function pointers and variables, e.g. reset and dma:
};

extern struct neuron_dhal *ndhal;       // ndhal is a global structure shared by all available neuron devices


/**
 * @brief Initialize the global ndhal for all available neuron devices
 *          - The initialization must be done only once
 *          - Mem allocation must be wrapped by the ndhal_init_lock
 * 
 * @return int 0 for success, negative for failures
 */
int neuron_dhal_init(void);

/**
 * @brief Clean up ndhal
 * The caller is to ensure that the ndhal is freed only once.
 * 
 */
void neuron_dhal_free(void);

#endif