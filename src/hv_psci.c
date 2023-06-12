/**
 * Copyright (c) 2023, amarioguy (AppleWOA authors), 2023.
 * 
 * Filename: hv_psci.c
 * 
 * Description: Implements PSCI services for a running guest when the hypervisor is in use. 
 * 
 * Note: for bare metal booting, we use a higher level firmware running in GL2 to provide PSCI, so this
 * file does not account for that case.
 * 
 * Implementation is based off the version in ARM Trusted Firmware-A. (https://github.com/ARM-software/arm-trusted-firmware)
 * 
 * License: SPDX-License-Identifier: MIT OR BSD-3-Clause
*/

#include "hv.h"
#include "hv_psci.h"
#include "assert.h"
#include "exception.h"
#include "cpu_regs.h"
#include "display.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "string.h"
#include "usb.h"
#include "utils.h"
#include "malloc.h"
#include "heapblock.h"
#include "smp.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "adt.h"
#include "soc.h"


uint32_t psci_capabilities;

unsigned int psci_num_cores, psci_num_clusters;

cpu_power_domain_node_t *psci_cpu_nodes;
non_cpu_power_domain_node_t *psci_non_cpu_nodes;
static platform_local_state_t *psci_requested_local_power_states[PSCI_MAX_POWER_LEVEL];
spinlock_t *psci_locks;

/**
 * Aside: Apple core topology will be defined as follows (NOTE: only for cores/clusters/system, 
 * pmgr peripherals are not accounted for) per ARM Trusted Firmware
 * requirements for PSCI.
 * 
 * Max power level - MPIDR (Aff2) (0 is run, 1 is low power, 2 is powerdown), system will always be in state 0
 * 
 * Number of nodes in power domain tree (aka clusters + cores):
 * 
 * <6-24> (number of cores) + <2-6> (number of clusters) + 1 (system power domain)
 * 
 * deepest power down state: OFF (aka affinity 2)
 * 
 * low power aka "retention" is defined as the clock gate sleep state.
 * 
 * we will only be using clock gated mode for core sleeping - power gating is more efficient but causes issues.
 * 
 * 
 * MPIDR syntax for Apple SoCs from M1 onwards:
 * 
 * bits 31:24 - bit 31, RES1, not a hyperthreading system
 * 
 * bits 23:16 - aff2, set to 1 on the non-primary clusters, 0 on the primary cluster (the set of 2 cores)
 * 
 * bits 15:8 - aff1, (die_num * 8) + local_cluster_number, indicates what cluster we are on. (for a single die system this is die 0 always)
 * 
 * bits 7:0 - aff0, core_num on the local cluster.
 * 
*/

//
// Description:
// Initializes PSCI stack. Right now, it's only setting up the power domain tree.
// 
// Return value:
// None.
//
void hv_psci_init(void) {
    //
    // Save number of cores/clusters for PSCI
    // code to keep track of.
    //
    // We're using the Trusted Firmware-A implementation of PSCI code,
    // and that implementation has number of cores and clusters per platform hardcoded.
    // (as it's expected to be ported individually to each platform and all
    // reference platforms typically have unchanging amounts of RAM/cores.)
    // 
    // Due to Apple SoCs having variable amounts of cores, and a different number of clusters
    // depending on whether it's a "Pro" chip or not, we need to instead use variables and calculate this number
    // of cores manually.
    //
    // For the cluster number, there are only three possiblities, so we can hardcode this per SoC "family"
    // (standard M-series chips have two, "Pro"/"Max" chips have three (bootstrap, auxiliary P cores, auxiliary E cores)
    // and "Ultra" chips have 6, since they are two "Max" dies interconnected together.)
    //

    int node = adt_path_offset(adt, "/cpus");
    psci_num_cores = 1;
    ADT_FOREACH_CHILD(adt, node) {
        unsigned int cpu_identifier;
        if(ADT_GETPROP(adt, node, "cpu-id", &cpu_identifier) < 0) {
            continue;
        }
        psci_num_cores++;
    }
    printf("PSCI DEBUG: Number of cores for PSCI nodes is %d\n", psci_num_cores);

    //
    // Use Chip ID to determine number of clusters on the platform.
    //
    switch(chip_id) {
        case T8103:
        case T8112:
            //
            // Two clusters for standard M series chips, one for all P cores, one for all E cores.
            //
            psci_num_clusters = 2;
            break;
        case T6000:
        case T6001:
        case T6020:
        case T6021:
            //
            // Three clusters for "Pro"/"Max" M series chips, bootstrap (two E-cores), auxiliary P-cores, auxiliary E-cores
            //
            psci_num_clusters = 3;
            break;
        case T6002:
            //
            // Six clusters for two-die SoCs, 2 * 3 clusters per "Max" die.
            //
            psci_num_clusters = 6;
            break;
    }
    psci_cpu_nodes = malloc((psci_num_cores * sizeof(cpu_power_domain_node_t)));
    psci_non_cpu_nodes = malloc((psci_num_clusters + NUM_SYSTEMS_ACTIVE) * sizeof(non_cpu_power_domain_node_t));
    for(int i = 0; i < PSCI_MAX_POWER_LEVEL; i++) {
      psci_requested_local_power_states[i] = malloc((psci_num_cores * sizeof(platform_local_state_t)));
    }
    psci_locks = malloc(((psci_num_clusters + NUM_SYSTEMS_ACTIVE) * sizeof(spinlock_t)));

}

/**
 * Description:
 * 
 * This function constructs the PSCI power state to turn off at all levels.
 * 
 * Return value:
 *    None.
*/

static void hv_psci_construct_poweroff_state(psci_power_state_status_t *state_info) {
   for(unsigned int level = 0; level <= PSCI_MAX_POWER_LEVEL; level++) {
      state_info->power_domain_state[level] = PSCI_OFF_STATE;
   }
}


/**
 * Description:
 * 
 * This function gets the parent nodes that are tied to a given CPU index.
 * 
 * Return value:
 *    None.
*/

static void hv_psci_get_parent_nodes(unsigned int cpu_index, unsigned int end_power_level, unsigned int *node_index) {
   unsigned int *node = node_index;
   unsigned int parent_node = parent_ndoe1;
   for(unsigned int i = 1; i <= end_power_level; i++) {

   }
}

/**
 * Description:
 * 
 * This function powers off the desired node in the PSCI power domain tree hierarchy.
 * 
 * Return value:
 * - PSCI_SUCCESS if CPU powered off successfully.
 * - PSCI_OPERATION_DENIED if an error occurred.
*/
int hv_psci_turn_off_cpu(void) {
   int retval = PSCI_SUCCESS; //assume success
   int mpidr = mrs(MPIDR_EL1);
   int cluster_number = mpidr & CLUSTER_NUMBER_MASK;
   int core_number = mpidr & CORE_NUMBER_MASK;
   int index = (cluster_number >> 6) + core_number + NUM_SYSTEMS_ACTIVE;
   psci_power_state_status_t power_state_info;
   unsigned int parent_nodes[PSCI_MAX_POWER_LEVEL] = {0};
   //
   // Step 0 - construct the power off state info.
   //
   hv_psci_construct_poweroff_state(&power_state_info);

   //do we need to do any poweroff early prep?
   //if needed, add later.

   //Step 1 - gather parent nodes of cpu to be powered down

   //step 2 - acquire spinlocks

   //step 3 - negotiate power states.

   //step 4 - 
}

/**
 * Description:
 * 
 * This function powers on the core.
 * 
 * Return value:
 * - PSCI_SUCCESS if CPU powered on successfully.
 * - PSCI_OPERATION_DENIED if an error occurred.
*/
int hv_psci_turn_on_cpu(void) {
   int retval = PSCI_SUCCESS; //assume success

}

/**
 * Description:
 * 
 * This function handles the PSCI function ID call to suspend a core.
 * 
 * Arguments:
 * 
 * - power_state - desired power state to set a CPU to
 * - cpu_reentry_addr - the address to resume a CPU's execution at
 * - context - CPU context.
 * 
 * Note that "context" is only valid if the desired state is power down, per ARM document DEN0022
*/
int hv_psci_suspend_cpu(uint64_t power_state, uint64_t cpu_reentry_addr, uint64_t context) {
   int retval = PSCI_INVALID_PARAMETERS;
}

static bool hv_handle_psci_smc(struct exc_info *ctx) {
   uint64_t psci_func_id = ctx->regs[0]; //PSCI function ID to be called will always be in X0.

    /**
     * Brief explanation on PSCI for those unfamiliar:
     * 
     * on most modern ARM platforms, the actual power management switches, transitions, and such are usually
     * handled by a trusted firmware running in a higher privilege level than the OS (EL3 on most)
     * 
     * As Apple platforms do not have EL3, we need to use EL2 (and by proxy, m1n1) with the OS running in EL1
     * for PSCI support (at least when we have a guest OS running)
     * 
     * For bare metal booting, this will be handled by "trusted" firmware 
     * that we will be running in GL2 (Apple's idea of "secure EL2").
     * 
     * In the hypervisor-assisted boot case, this file will contain most of that logic.
     * The below code checks if it's a 32 bit or 64 bit function ID (the handling does differ) and then contains the switch statement
     * to handle every function ID.
    */
   



   if((psci_func_id & SMC_64_FUNCTION) == 0) {
      /**
       * This is a SMC32 PSCI call.
       * Clear the upper 32 bits of X1, X2, and X3 since we're only permitted 32 bit parameters
       * per the SMC32 calling convention defined in ARM document DEN0028.
      */
      uint32_t w1 = (uint32_t)ctx->regs[1];
      uint32_t w2 = (uint32_t)ctx->regs[2];
      uint32_t w3 = (uint32_t)ctx->regs[3];
      switch(psci_func_id) {
         case PSCI_GET_VERSION_FUNCTION_ID:
            //always called as SMC32 even on ARM64
            ctx->regs[0] = PSCI_VERSION;
            break;
         case PSCI_SUSPEND_CPU_ARM32_FUNCTION_ID:
            int ret = hv_psci_suspend_cpu(ctx->regs[1], ctx->regs[2], ctx->regs[3]);
            ctx->regs[0] = ret;
            break;
         case PSCI_CPU_OFF_FUNCTION_ID:
            int retval = hv_psci_turn_off_cpu();
            break;
         case PSCI_CPU_ON_ARM32_FUNCTION_ID:
            int retval = hv_psci_turn_on_cpu();
            break;

      }

   }
   else {
      /**
       * This is an SMC64 PSCI call. Leave X1, X2, X3 alone and proceed to see what function is
       * being requested by the SMC.
      */
   }


}