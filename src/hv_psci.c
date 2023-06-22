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
 * 
 * 
 * Implementation is basically a straight port of the implementation in ARM Trusted Firmware-A. (https://github.com/ARM-software/arm-trusted-firmware)
 * as reinventing the wheel here is not very fun.
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
#include "pmgr.h"
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

//
// PSCI variables.
//

uint32_t psci_capabilities;

unsigned int psci_num_cores, psci_num_clusters;

cpu_power_domain_node_t *psci_cpu_nodes;
non_cpu_power_domain_node_t *psci_non_cpu_nodes;
static platform_local_state_t *psci_requested_local_power_states[PSCI_MAX_POWER_LEVEL];
spinlock_t *psci_locks;
psci_per_cpu_data_t *psci_cpu_data_array;
static int adt_cpu_nodes[MAX_CPUS];
static u64 adt_pmgr_reg;
static u64 cpu_start_off;

//
// A table of valid idle states. Anything else is considered invalid.
// The states will be listed as this: (system state, cluster state, core state)
//
const unsigned int valid_idle_states[] = {
   // (On, On, Idle Standby/WFI) - core is in standby mode.
   apple_make_pwrstate_lvl2(PSCI_ON_STATE, PSCI_ON_STATE, PSCI_IDLE_STANDBY_STATE, PSCI_CPU_POWER_LEVEL, PSCI_POWER_STATE_TYPE_STANDBY),
   // (On, On, Poweroff/Deep Sleep/S2R) - level 0 is powered down
   // turned off for testing purposes.
   //apple_make_pwrstate_lvl2(PSCI_ON_STATE, PSCI_ON_STATE, PSCI_OFF_STATE, PSCI_CPU_POWER_LEVEL, PSCI_POWER_STATE_TYPE_POWERDOWN),
   // (On, Idle Retention, Idle Retention/Deep WFI.) - level 1 is in standby
   apple_make_pwrstate_lvl2(PSCI_ON_STATE, PSCI_IDLE_STANDBY_STATE, PSCI_IDLE_STANDBY_STATE, PSCI_CLUSTER_POWER_LEVEL, PSCI_POWER_STATE_TYPE_STANDBY),
   // (On, Off, Off) - level 1/cluster off
   // Not supported, pending an understanding of how to trigger this power state.

   // (Retention, Off, Off) - level 2 standby
   // Not supported, pending an understanding of how to trigger this power state.

   // (Off, Off, Off) - system off.
   apple_make_pwrstate_lvl2(PSCI_OFF_STATE, PSCI_OFF_STATE, PSCI_OFF_STATE, PSCI_MAX_POWER_LEVEL, PSCI_POWER_STATE_TYPE_POWERDOWN),
   0
};
/**
 * Aside: Apple core topology will be defined as follows (NOTE: only for cores/clusters/system, 
 * pmgr peripherals are not accounted for) per ARM Trusted Firmware
 * requirements for PSCI.
 * 
 * Max power level - MPIDR (Aff2) (0 is core, 1 is cluster, 2 is system)
 * 
 * Number of nodes in power domain tree (aka clusters + cores):
 * 
 * <6-24> (number of cores) + <2-6> (number of clusters) + 1 (system power domain)
 * 
 * deepest power down state: OFF
 * 
 * Low power sleep states:
 * - Idle WFI (aka "shallow") - when core is in WFI but not deep sleeping
 * - Deep WFI (aka deep sleep)
 * 
 * 
 * MPIDR syntax for Apple SoCs from M1 onwards:
 * 
 * bits 31:24 - bit 31, RES1, not a hyperthreading system
 * 
 * bits 23:16 - aff2, 0x1 for P-cores, 0x0 for E-cores.
 * 
 * bits 15:8 - aff1, (die_num * 8) + local_cluster_number, indicates what cluster we are on.
 * 
 * bits 7:0 - aff0, core_num on the local cluster.
 * 
*/

//
// Apple CPU suspend notes:
// 
// - can suspend cores in a "deep WFI" or "shallow WFI" state (the former clock gates the cores)
// - power gating all cores in a cluster will put the cluster into a retention state (general purpose regs are lost but cluster uncore remains powered.)
// - separate mechanism to power off a cluster completely (to start cores from RVBAR)
//
// Possible states for power domain nodes:
// CPU: can be in an on state, a shallow WFI state, a deep WFI state (aka clock gated), and OFF.
// Cluster: can be on (when any core in the cluster is on or in retention), in retention (when all cores in a cluster are OFF), or off (when all cores are in retention and turned off)
// System: ON or OFF. easy.
// 
// Still in the process of being documented.
//

//
// Description:
// Initializes PSCI module. Only expected to be called once when the hypervisor is initially started.
// (Note: this code assumes that the hypervisor will only be started up once, as shutting down the hypervisor is not
// a supported scenario, albeit doable.)
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
    // reference platforms typically have unchanging amounts of cores.)
    // 
    // Due to Apple SoCs having variable amounts of cores, and a different number of clusters
    // depending on whether it's a "Pro" chip or not, we need to instead use variables and calculate this number
    // of cores manually.
    //
    // For the cluster number, there are only three possiblities, so we can hardcode this per SoC "family"
    // (standard M-series chips have two, "Pro"/"Max" chips have three and "Ultra" chips have 6, since they are two "Max" dies interconnected together.)
    //
    // If hardcoding number of clusters dependent on Chip ID becomes infeasible, change the code below to dynamically determine
    // from ADT/board id.
    //

    int adt_pmgr_path[8];

    if (adt_path_offset_trace(adt, "/arm-io/pmgr", adt_pmgr_path) < 0) {
        panic("PSCI setup fatal error: Error getting /arm-io/pmgr node\n");
    }
    if (adt_get_reg(adt, adt_pmgr_path, "reg", 0, &adt_pmgr_reg, NULL) < 0) {
        panic("PSCI setup fatal error: Error getting /arm-io/pmgr regs\n");
    }
    int node = adt_path_offset(adt, "/cpus");
    psci_num_cores = 1;
    //
    // Set up the ADT cpu nodes, to use in cpu on and off code.
    //
    memset(adt_cpu_nodes, 0, sizeof(adt_cpu_nodes));
    ADT_FOREACH_CHILD(adt, node) {
        unsigned int cpu_identifier;
        if(ADT_GETPROP(adt, node, "cpu-id", &cpu_identifier) < 0) {
            continue;
        }
        psci_num_cores++;
        adt_cpu_nodes[cpu_identifier] = node;
    }
    switch (chip_id) {
        case T8103:
        case T6000:
        case T6001:
        case T6002:
            cpu_start_off = CPU_START_OFF_T8103;
            break;
        case T8112:
            cpu_start_off = CPU_START_OFF_T8112;
            break;
        case T6020:
        case T6021:
            cpu_start_off = CPU_START_OFF_T6020;
            break;
        default:
            panic("PSCI setup fatal error: CPU start offset is unknown for this SoC!\n");
    }
    printf("PSCI DEBUG: Number of cores for PSCI nodes is %d\n", psci_num_cores);

    //
    // Use the ADT to determine number of clusters on the platform.
    //
    if(ADT_GETPROP(adt, node, "cpu-cluster-count", &psci_num_clusters) < 0) {
      panic("PSCI DEBUG: failed to get number of clusters!!\n");
    }
    if((chip_id == 0x6002)) {
      //
      // This is an M1 Ultra, so we're on a multi-die configuration. cpu-cluster-count is the single die total number of clusters
      // so multiply that number of clusters by the number of CPU dies on the SoC. (maximum of 2 so far.)
      //
      psci_num_clusters = psci_num_clusters * 2;
    }

    //
    // Allocate memory for PSCI power domain tree based on previously obtained core/cluster count values.
    //
    printf("PSCI DEBUG: Number of clusters for PSCI nodes is %d\n", psci_num_clusters);
    psci_cpu_nodes = malloc((psci_num_cores * sizeof(cpu_power_domain_node_t)));
    psci_non_cpu_nodes = malloc((psci_num_clusters + NUM_SYSTEMS_ACTIVE) * sizeof(non_cpu_power_domain_node_t));
    for(int i = 0; i < PSCI_MAX_POWER_LEVEL; i++) {
      psci_requested_local_power_states[i] = malloc((psci_num_cores * sizeof(platform_local_state_t)));
    }
    psci_locks = malloc(((psci_num_clusters + NUM_SYSTEMS_ACTIVE) * sizeof(spinlock_t)));

    psci_cpu_data_array = malloc(((psci_num_cores) * sizeof(psci_per_cpu_data_t)));

    //
    // Save the global CPU number, local cluster core number, lower two bytes of MPIDR for each core, (the ADT "reg" value in the CPU nodes.)
    // and the die number for each of the cores here.
    //
    for(int i = 0; i < MAX_CPUS; i++) {
        int current_node = adt_cpu_nodes[i];

        if(!current_node) {
         continue;
        }
        unsigned int cpu_identifier;
        unsigned int reg_identifier;
        unsigned int cluster_num;
        unsigned int local_cluster_core_num;
        unsigned int die_id;
        if(ADT_GETPROP(adt, current_node, "cpu-id", &cpu_identifier) < 0) {
            continue;
        }
        if(ADT_GETPROP(adt, current_node, "reg", &reg_identifier) < 0) {
            continue;
        }
        if(ADT_GETPROP(adt, current_node, "die-cluster-id", &cluster_num) < 0) {
            continue;
        }
        if(ADT_GETPROP(adt, current_node, "die-id", &cluster_num) < 0) {
            continue;
        }
        if(ADT_GETPROP(adt, current_node, "cluster-core-id", &local_cluster_core_num) < 0) {
            continue;
        }
        psci_cpu_data_array[cpu_identifier].cpu_index = cpu_identifier;
        psci_cpu_data_array[cpu_identifier].reg_value = reg_identifier;
        psci_cpu_data_array[cpu_identifier].cluster_index = cluster_num;
        psci_cpu_data_array[cpu_identifier].die_index = die_id;
        psci_cpu_data_array[cpu_identifier].local_core_number = local_cluster_core_num;
    }

    printf("PSCI DEBUG: Total number of nodes in power domain tree is %d (%d cores, %d clusters, 1 system)\n", 
    (psci_num_clusters + psci_num_cores + NUM_SYSTEMS_ACTIVE), 
    psci_num_cores, 
    psci_num_clusters, 
    NUM_SYSTEMS_ACTIVE);


}

//
// Helpers to get CPU specific data.
//

static inline unsigned int hv_psci_get_suspend_power_level(void)
{
   unsigned int cpu_identifier = hv_psci_get_core_position();
	return psci_cpu_data_array[cpu_identifier].target_power_level;
}

static inline void hv_psci_set_suspend_power_level(unsigned int target_level)
{
   unsigned int cpu_identifier = hv_psci_get_core_position();
	psci_cpu_data_array[cpu_identifier].target_power_level = target_level;
}

static inline void hv_psci_set_cpu_local_state(platform_local_state_t state)
{
	unsigned int cpu_identifier = hv_psci_get_core_position();
   psci_cpu_data_array[cpu_identifier].local_cpu_state = state;
}

static inline platform_local_state_t hv_psci_get_cpu_local_state(void)
{
   unsigned int cpu_identifier = hv_psci_get_core_position();
	return psci_cpu_data_array[cpu_identifier].local_cpu_state;
}

//
// Description:
// PSCI power state helper function to sanity check the power state.
//
// Return value:
//    None.
//
static inline unsigned int hv_psci_power_state_sanity_check(unsigned int power_state) {
   return ((power_state) & PSCI_STATE_VALID_MASK);
}

//
// Description:
// PSCI power state helper function to get the power state type.
//
// Return value:
//    None.
//
static inline unsigned int hv_psci_power_state_get_type(unsigned int power_state) {
   return (((power_state) >> PSCI_STATE_TYPE_SHIFT) & PSCI_STATE_TYPE_MASK);
}

//
// Description:
// PSCI power state helper function to get the power state ID.
//
// Return value:
//    None.
//
static inline unsigned int hv_psci_power_state_get_id(unsigned int power_state) {
   return ((power_state) & PSCI_STATE_ID_MASK);
}


//
// Description:
// Validate that the power state is good.
//
// Return value:
//    PSCI_SUCCESS - the power state is good.
//    PSCI_INVALID_PARAMETERS - the power state is NOT valid.
//
int hv_psci_validate_power_state(unsigned int power_state, psci_power_state_status_t *power_state_info) {
   unsigned int power_state_id;
   int i;
   if(hv_psci_power_state_sanity_check(power_state) != 0) {
      printf("PSCI DEBUG: power state sanity check failed or code buggy\n");
      return PSCI_STATUS_INVALID_PARAMETERS;
   }
   for(i = 0; !!valid_idle_states[i]; i++) {
      if(power_state == valid_idle_states[i]) {
         break;
      }
   }

   if(!valid_idle_states[i]) {
      return PSCI_STATUS_INVALID_PARAMETERS;
   }
   i = 0;

   power_state_id = hv_psci_power_state_get_id(power_state);

   for(i = PSCI_CPU_POWER_LEVEL; i <= PSCI_MAX_POWER_LEVEL; i++) {
      power_state_info->power_domain_state[i] = power_state_id & PLAT_LOCAL_PSTATE_MASK;
      power_state_id = power_state_id >> PLAT_LOCAL_PSTATE_WIDTH;
   }
   return PSCI_STATUS_SUCCESS;

}

//
// Description:
// Helper function to calculate "core position" for PSCI code.
//
// Return value:
//    Core position as an integer.
//
unsigned int hv_psci_get_core_position(void) {
   unsigned int mpidr_calculated = mrs(MPIDR_EL1);
   unsigned int reg_value_calculated = mpidr_calculated & GENMASK(15, 0);
   unsigned int core_position = 0xfe;
   for(int i = 0; i < MAX_CPUS; i++) {
      int current_node = adt_cpu_nodes[i];

      if(!current_node) {
         continue;
      }
      unsigned int cpu_identifier;
      unsigned int reg;
      if(ADT_GETPROP(adt, current_node, "cpu-id", &cpu_identifier) < 0) {
         continue;
      }
      if(psci_cpu_data_array[cpu_identifier].reg_value == reg_value_calculated) {
         core_position = cpu_identifier;
         break;
      }
    }
    if(core_position == 0xfe) {
      panic("Core position was not found! (Or there's a bug in the code.)\n");
    }
    return core_position;


}

//
// Description:
// Sets the local power state array to the desired/requested state.
// (Note: does not apply to CPU power levels as those aren't stored in the array.)
//
// Return value:
//    None.
//
static void hv_psci_set_requested_local_power_state(unsigned int power_level, unsigned int cpu_index, platform_local_state_t requested_power_state) {
   //
   // Do not allow access to CPU power level (as this array does not store the requested state for that.)
   //
   assert(power_level > 0);
   if((power_level > 0) && (power_level <= PSCI_MAX_POWER_LEVEL) && (cpu_index < psci_num_cores)) {
      psci_requested_local_power_states[power_level - 1][cpu_index] = requested_power_state;
   }
}

//
// Description:
// Returns a pointer to local power states requested by CPUs for a given power domain tree node.
// (Note: CPU power levels not part of this array, assertion to prevent this kind of access.)
//
// Return value:
//    A pointer inside the array containing local power states requested by a given CPU.
//
static platform_local_state_t *hv_psci_get_requested_local_power_states(unsigned int power_level, unsigned int cpu_index) {
   assert(power_level > 0);
   if((power_level > 0) && (power_level <= PSCI_MAX_POWER_LEVEL) && (cpu_index < psci_num_cores)) {
      return &psci_requested_local_power_states[power_level - 1U][cpu_index];
   }
   else {
      return NULL;
   }
}

//
// Description:
// Coordinates the platform specific local power states requested by each CPU and returns
// the coordinated state.
// TODO: Check if we need to do platform specific things here.
//
// Return value:
//    Target power state after coordination.
//
platform_local_state_t hv_psci_get_target_power_state(unsigned int level, const platform_local_state_t *states, unsigned int num_cpu_siblings) {
   platform_local_state_t target_state = PSCI_OFF_STATE, temp;
   const platform_local_state_t *state1 = states;
   unsigned int siblings = num_cpu_siblings;
   assert(num_cpu_siblings > 0);

   do {
      temp = *state1;
      state1++;
      if(temp < target_state) {
         target_state = temp;
      }
      siblings--;
   } while (siblings > 0U);

   return target_state;

}

//
// Description:
// Get non-CPU power domain local state.
//
// Return value:
//    Local state of requested non-CPU power domain node.
//
static platform_local_state_t hv_psci_get_non_cpu_power_domain_local_state(unsigned int parent_index) {
   dc_civac_range((void *)&psci_non_cpu_nodes[parent_index], sizeof(psci_non_cpu_nodes[parent_index]));
   return psci_non_cpu_nodes[parent_index].local_power_state;
}

//
// Description:
// Update non-CPU power domain local state.
//
// Return value:
//    None.
//
static void hv_psci_set_non_cpu_power_domain_node_local_state(unsigned int parent_index, platform_local_state_t state) {
   psci_non_cpu_nodes[parent_index].local_power_state = state;
   //
   // Flush and invalidate caches here for safety in case we're not cache coherent.
   //
   dc_civac_range((void *)&psci_non_cpu_nodes[parent_index], sizeof(psci_non_cpu_nodes[parent_index]));

}

//
// Description:
// Helper function to set per-CPU local state to a desired state.
// 
// Return value:
//    None.
//

static void hv_psci_set_cpu_local_state(platform_local_state_t desired_state) {
   psci_cpu_data_array[mrs(TPIDR_EL2)].local_cpu_state = desired_state;
}

//
// Description:
// Helper function to find the highest power level that will be turned off.
//
// Return value:
//    Highest power level to be powered down.
//
unsigned int hv_psci_find_max_off_level(const psci_power_state_status_t *state_info) {
   for(int i = PSCI_MAX_POWER_LEVEL; i >= PSCI_CPU_POWER_LEVEL; i--) {
      int result = (((state_info->power_domain_state[i]) > PSCI_MAX_RETENTION_STATE) && ((state_info->power_domain_state[i]) <= PSCI_MAX_OFF_STATE)) ? 1 : 0;
      if(result != 0) {
         return (unsigned int)i;
      }
   }
}

//
// Description:
// Helper that sets the target local power state to be entered by power domains from current CPU to ancestor.
// Must be called after coordination of power states.
//
// Return value:
//    None.
//

static void hv_psci_set_target_local_power_states(unsigned int end_power_level, const psci_power_state_status_t *target_state) {
   unsigned int parent_index, level;
   const platform_local_state_t *power_domain_state = target_state->power_domain_state;

   hv_psci_set_cpu_local_state(power_domain_state[PSCI_CPU_POWER_LEVEL]);

   //
   // Flush data caches to prevent weirdness with cached local states.
   //
   dc_civac_range((void *)&psci_cpu_data_array[mrs(TPIDR_EL2)].local_cpu_state, sizeof(psci_cpu_data_array[mrs(TPIDR_EL2)].local_cpu_state));

   parent_index = psci_cpu_nodes[hv_psci_get_core_position()].parent_node;

   //
   // Copy local state over from the state info array.
   //
   for(level = 1; level <= end_power_level; level++) {
      hv_psci_set_non_cpu_power_domain_node_local_state(parent_index, power_domain_state[level]);
      parent_index = psci_non_cpu_nodes[parent_index].parent_node;
   }
}

void hv_psci_coordinate_power_states(unsigned int end_power_level, psci_power_state_status_t *current_state_info) {
   unsigned int level, parent_index, cpu_index = hv_psci_get_core_position();
   unsigned int start_index;
   unsigned int num_cpu_siblings;
   platform_local_state_t target_state, *requested_states;

   //
   // Get the parent node of the current CPU node.
   //
   parent_index = psci_cpu_nodes[cpu_index].parent_node;

   for(level = 1; level <= end_power_level; level++) {
      //
      // Update requested power state.
      //
      hv_psci_set_requested_local_power_state(level, cpu_index, current_state_info->power_domain_state[level]);

      start_index = psci_non_cpu_nodes[parent_index].first_cpu_idx;
      requested_states = hv_psci_get_requested_local_power_states(level, start_index);

      //
      // Coordinate requested states at the power level, and return the target state.
      //
      num_cpu_siblings = psci_non_cpu_nodes->num_cpu_siblings;
      target_state = hv_psci_get_target_power_state(level, requested_states, num_cpu_siblings);
      current_state_info->power_domain_state[level] = target_state;

      //
      // If the coordinated state is normal running operaiton, break out early.
      //
      if((current_state_info->power_domain_state[level]) == PSCI_ON_STATE) {
         printf("PSCI DEBUG: current state info says: %d, evaluated as PSCI on state\n", (current_state_info->power_domain_state[level]));
         break;
      }

      parent_index = psci_non_cpu_nodes[parent_index].parent_node;
   }

   //
   // If the targeted power state is the run state, when power level is < than end power level,
   // update the requested power state and set that target state.
   //
   for(level = level + 1; level <= end_power_level; level++) {
      hv_psci_set_requested_local_power_state(level, cpu_index, current_state_info->power_domain_state[level]);
      current_state_info->power_domain_state[level] = PSCI_ON_STATE;
   }

   //
   // Finally, update target state in power domain nodes
   //
   hv_psci_set_target_local_power_states(end_power_level, current_state_info);
}

//
// Description:
//
// Releases a spinlock on a non-CPU power domain node in the tree. (CPU nodes do not need a spinlock)
//
// Return value:
//    None.
static void hv_psci_release_lock(non_cpu_power_domain_node_t *non_cpu_power_domain_node) {
   spin_unlock(&psci_locks[non_cpu_power_domain_node->lock_index]);
}

//
// Description:
//
// Gets a spinlock on a non-CPU power domain node in the tree. (CPU nodes do not need a spinlock)
//
// Return value:
//    None.
static void hv_psci_get_lock(non_cpu_power_domain_node_t *non_cpu_power_domain_node) {
   spin_lock(&psci_locks[non_cpu_power_domain_node->lock_index]);
}

//
// Description:
// This function does the architectural preparation to power down the CPU.
//
// Return value:
// None.
//

void hv_psci_power_down_cpu_maintenance(unsigned int power_level) {
   unsigned int cpu_index = hv_psci_get_core_position();

   //
   // Disable data caching.
   //
   uint64_t sctlr_old = read_sctlr();
   uint64_t sctlr_cache_disable = sctlr_old & ~(SCTLR_C);
   write_sctlr(sctlr_cache_disable);

   dcsw_op_all(DCSW_OP_DCISW);

   return;

}

/**
 * Description:
 * This function constructs the PSCI power state to turn off at all levels.
 * 
 * Return value:
 *    None.
*/

static void hv_psci_construct_poweroff_state(psci_power_state_status_t *state_info) {
   //
   // Iterate through every level, and set every state in the state_info struct
   // to the poweroff state (returned to caller which will then perform state coordination)
   //
   for(unsigned int level = 0; level <= PSCI_MAX_POWER_LEVEL; level++) {
      state_info->power_domain_state[level] = PSCI_OFF_STATE;
   }
}

static platform_local_state_type_t hv_psci_power_state_categorize_type(platform_local_state_t state) {
   if(state != PSCI_ON_STATE) {
      if(state > PSCI_IDLE_STANDBY_STATE) {
         return STATE_TYPE_OFF;
      }
      else {
         return STATE_TYPE_RETN;
      }
   }
   else {
      return STATE_TYPE_RUN;
   }
}

void hv_psci_get_target_local_power_states(unsigned int end_power_level, psci_power_state_status_t *target_state) {
   unsigned int parent_index, level;
   platform_local_state_t *power_domain_current_state = target_state->power_domain_state;
   unsigned int cpu_index = hv_psci_get_core_position();
   power_domain_current_state[PSCI_CPU_POWER_LEVEL] = hv_psci_get_cpu_local_state();
   parent_index = psci_cpu_nodes[cpu_index].parent_node;

   for(level = PSCI_CPU_POWER_LEVEL + 1U; level <= end_power_level; level++) {
      power_domain_current_state[level] = hv_psci_get_non_cpu_power_domain_local_state(parent_index);
      parent_index = psci_non_cpu_nodes[parent_index].parent_node;
   }

   for(; level <= PSCI_MAX_POWER_LEVEL; level++) {
      target_state->power_domain_state[level] = PSCI_ON_STATE;
   }

}

//
// Description:
// This helper function sets the affinity info state for a given CPU.
//
// Return value:
//    None.
//
static void hv_psci_set_affinity_info_state(affinity_info_state_t state) {
   unsigned int cpu_identifier = hv_psci_get_core_position();
   psci_cpu_data_array[cpu_identifier].affinity_state = state;
}


//
// Description:
// This helper function releases locks for each power level in reverse order.
//
// Return value:
//    None.
//
static void hv_psci_release_power_domain_tree_locks(unsigned int end_power_level, const unsigned int *parent_nodes) {
   unsigned int parent_index;
   for(unsigned int level = end_power_level; level >= (PSCI_CPU_POWER_LEVEL + 1U); level--) {
      parent_index = parent_nodes[level - 1U];
      hv_psci_release_lock(&psci_non_cpu_nodes[parent_index]);
   }
}

//
// Description:
// This function acquires locks for the desired power level in the power domain tree.
//
// Return value:
//    None.
//
static void hv_psci_acquire_power_domain_tree_locks(unsigned int end_power_level, const unsigned int *parent_nodes) {
   unsigned int parent_index;

   //
   // Acquire the spinlock for levels above the CPU (clusters + system)
   //
   for(unsigned int level = 1U; level < end_power_level; level++) {
      printf("PSCI DEBUG: current level: %d, ending power level %d\n", level, end_power_level);
      parent_index = parent_nodes[level - 1U];
      hv_psci_get_lock(&psci_non_cpu_nodes[parent_index]);
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
   unsigned int parent_node = psci_cpu_nodes[cpu_index].parent_node;
   //
   // Traverse the power domain tree backwards to find all the parent nodes of the current node.
   //
   for(unsigned int i = 1; i <= end_power_level; i++) {
      *node = parent_node;
      node++;
      parent_node = psci_non_cpu_nodes[parent_node].parent_node;
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
   int retval = PSCI_STATUS_SUCCESS; //assume success
   int index = hv_psci_get_core_position();
   psci_power_state_status_t power_state_info;
   unsigned int parent_nodes[PSCI_MAX_POWER_LEVEL] = {0};
   //
   // Step 0 - construct the power off state info.
   //
   hv_psci_construct_poweroff_state(&power_state_info);

   //do we need to do any poweroff early prep?
   //if needed, add later.

   //
   // Step 1 - gather parent nodes of cpu to be powered down.
   //

   hv_psci_get_parent_nodes(index, PSCI_MAX_POWER_LEVEL, parent_nodes);
   
   //
   // Step 2 - acquire spinlocks.
   //

   hv_psci_acquire_power_domain_tree_locks(PSCI_MAX_POWER_LEVEL, parent_nodes);
   
   //
   // Step 3 - negotiate power states.
   //

   hv_psci_coordinate_power_states(PSCI_MAX_POWER_LEVEL, &power_state_info);

   //
   // Step 4 - prepare for powering off the CPU.
   //

   hv_psci_power_down_cpu_maintenance(hv_psci_find_max_off_level(&power_state_info));

   //
   // Step 5 - release power level locks.
   //
   hv_psci_release_power_domain_tree_locks(PSCI_MAX_POWER_LEVEL, parent_nodes);

   //
   // Verify all is good to power off the CPU.
   //
   if (retval == PSCI_STATUS_SUCCESS) {
      //
      // Set affinity info state to off, note that caches are off now,
      // so we need to ensure that maintenance of caches are done to ensure
      // the state is read correctly.
      //
      dc_civac_range((void *)&psci_cpu_data_array[index].affinity_state, sizeof(affinity_info_state_t));
      hv_psci_set_affinity_info_state(AFFINITY_STATE_OFF);
      sysop("dsb ish");
      dc_ivac_range((void *)&psci_cpu_data_array[index].affinity_state, sizeof(affinity_info_state_t));
      //
      // Temporary ifdef: Try to prepare a total CPU sleep when PSCI CPU off is called.
      // If this turns out to not be viable, remove the ifdef dependent code and instead do
      // "deep WFI" sleep instead.
      //
      unsigned int cpu_start_addr_base = adt_pmgr_reg + cpu_start_off;
      unsigned int die_num = psci_cpu_data_array[index].die_index;
      unsigned int cluster_index = psci_cpu_data_array[index].cluster_index;
      unsigned int local_cluster_core_num = psci_cpu_data_array[index].local_core_number;
      cpu_start_addr_base += die_num * PMGR_DIE_OFFSET;
      write32(cpu_start_addr_base + 0x0, 1 << (4 * cluster_index + local_cluster_core_num));
   
      //
      // Default to deep sleep (will be stopped automatically). Not expected to return.
      //
      cpu_sleep(true);
      //
      // Do we need to add a contingency plan if we escape the WFI loop?
      //
      printf("PSCI DEBUG: left the WFI loop after CPU power off\n");
   }
   else {
      //
      // Return a operation denied error. Not possible right now but as insurance, add this.
      //
      retval = PSCI_STATUS_OPERATION_DENIED;
   }
   return retval;


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
   int retval = PSCI_STATUS_SUCCESS; //assume success

}

//
// Description:
// Finds the highest power domain to be placed in low power state.
//
// Return value:
// Target power level to be suspended.
//
unsigned int hv_psci_find_target_suspend_level(const psci_power_state_status_t *power_state_info) {
   for(int i = (int)PSCI_MAX_POWER_LEVEL; i >= (int)PSCI_CPU_POWER_LEVEL; i--) {
      if(hv_psci_is_local_state_run(power_state_info->power_domain_state[i]) == 0) {
         return (unsigned int) i;
      }
   }
   return PSCI_INVALID_LEVEL;
}
//
// Description:
// Validates and prepares the PSCI CPU suspend entry point.
//
// Return value:
//    PSCI_SUCCESS if validated, PSCI_STATUS_INVALID_ADDRESS otherwise.
//

int hv_psci_validate_entry_point(entry_point_info_t *entry_point, unsigned long long cpu_reentry_addr, uint64_t context) {
   int retval;
   uint64_t entry_point_attr, sctlr;
   unsigned int daif;
   const unsigned int el_mode = 0x2; //EL2 mode.
   uint64_t hcr_el2 = mrs(HCR_EL2);
   sctlr = read_sctlr();
   entry_point_attr = 0x1U; //corresponds to "not secure bit" set, which is unimportant on Apple platforms, but better safe than sorry since TF-A sets it.
   entry_point->header.type = (uint8_t)PARAMETER_ENTRY_POINT; // sets it as an entry point
   entry_point->header.version = (uint8_t)0x01U;
   entry_point->header.size = (uint16_t)sizeof(*(entry_point));
   entry_point->header.attributes = (uint32_t)entry_point_attr;

   entry_point->pc = cpu_reentry_addr;
   memset(&entry_point->arguments, 0, sizeof(entry_point->arguments));
   entry_point->arguments.arg0 = context;
   entry_point->spsr = SPSR_64((uint64_t)el_mode, SPSR_MODE_SP_ELX, SPSR_DAIF_DISABLE_ALL_EXCEPTIONS);
   return PSCI_STATUS_SUCCESS;

}

//
// Description:
// Saves the context for re-entry from suspend.
//
// Return value:
// None.
//
void hv_psci_build_saved_cpu_context(const entry_point_info_t *entry_point) {

}

//
// Description:
// Does preparation to do a "power down" suspend.
//
// Return value:
// None.
//
static void hv_psci_start_suspend_to_power_down(unsigned int end_power_level, const entry_point_info_t *entry_point, const psci_power_state_status_t *power_state_info) {
   unsigned int max_off_level = hv_psci_find_max_off_level(power_state_info);
   unsigned int cpu_identifier = hv_psci_get_core_position();
   hv_psci_set_suspend_power_level(end_power_level);
   dc_civac_range(((void *)psci_cpu_data_array[cpu_identifier].target_power_level), sizeof(((void *)psci_cpu_data_array[cpu_identifier].target_power_level)));
   //
   // ARM TF-A uses this, but do we need to save this?
   //
   //hv_psci_build_saved_cpu_context(entry_point);
   hv_psci_power_down_cpu_maintenance(max_off_level);

}

//
// TODO: add a description.
//
void hv_psci_set_power_domains_to_on_state(unsigned int end_power_level) {
   unsigned int parent_index;
   unsigned int cpu_index = hv_psci_get_core_position();
   unsigned int level;
   parent_index = psci_cpu_nodes[cpu_index].parent_node;

   for(level = PSCI_CPU_POWER_LEVEL + 1U; level <= end_power_level; level++) {
      hv_psci_set_non_cpu_power_domain_node_local_state(parent_index, PSCI_ON_STATE);
      hv_psci_set_requested_local_power_state(level, cpu_index, PSCI_ON_STATE);
      parent_index = psci_non_cpu_nodes[parent_index].parent_node;
   }

   hv_psci_set_affinity_info_state(AFFINITY_STATE_ON);
   hv_psci_set_cpu_local_state(PSCI_ON_STATE);
   dc_civac_range((void *)&psci_cpu_data_array, sizeof(psci_cpu_data_array));

}

//
// Description:
// Validates the PSCI suspend request and makes sure no higher power level is turned off if
// the request is for a CPU to be put on standby.
//
// Return value:
//    PSCI_SUCCESS if validated successfully, PSCI_INVALID_PARAMETERS otherwise.
//
int hv_psci_validate_suspend_request(const psci_power_state_status_t *power_state_info, unsigned int is_power_down_state) {
   unsigned int max_power_off_level, target_level, max_retention_level;
   platform_local_state_t platform_state;
   platform_local_state_type_t requested_state_type, lowest_state_type;

   target_level = hv_psci_find_target_suspend_level(power_state_info);
   if(target_level == PSCI_INVALID_LEVEL) {
      return PSCI_STATUS_INVALID_PARAMETERS;
   }
   lowest_state_type = PSCI_ON_STATE;
   for(int i = (int)target_level; i >= (int)PSCI_CPU_POWER_LEVEL; i--) {
      platform_state = power_state_info->power_domain_state[i];
      requested_state_type = hv_psci_power_state_categorize_type(platform_state);
      if(requested_state_type < lowest_state_type) {
         return PSCI_STATUS_INVALID_PARAMETERS;
      }
      lowest_state_type = requested_state_type;
   }
   max_power_off_level = hv_psci_find_max_off_level(power_state_info);

   max_retention_level = PSCI_INVALID_LEVEL;
   if(target_level != max_power_off_level) {
      max_retention_level = target_level;
   }

   if((is_power_down_state == 0U) && ((max_power_off_level != PSCI_INVALID_LEVEL) || (max_retention_level == PSCI_INVALID_LEVEL))) {
      return PSCI_STATUS_INVALID_PARAMETERS;
   }
   return PSCI_STATUS_SUCCESS;
}

//
// Description:
// Operations to be done after wake up from standby/s2idle state.
//
// Return value:
// None.
//
static void hv_psci_finish_cpu_suspend(unsigned int cpu_index, unsigned int end_power_level) {
   unsigned int parent_nodes[PSCI_MAX_POWER_LEVEL] = {0};
   psci_power_state_status_t power_state_info;

   hv_psci_get_parent_nodes(cpu_index, end_power_level, parent_nodes);

   hv_psci_acquire_power_domain_tree_locks(end_power_level, parent_nodes);

   hv_psci_get_target_power_state(end_power_level, &power_state_info);

   //
   // Set power domain state to ON state.
   //
   hv_psci_set_power_domains_to_on_state(end_power_level);
   
   hv_psci_release_power_domain_tree_locks(end_power_level, parent_nodes);

}

//
// Description:
// Suspends a power domain node in the PSCI power domiin tree.
//
// Return value:
// Status of the final suspend operation.
//
int hv_psci_start_cpu_suspend(const entry_point_info_t *entry_point, unsigned int end_power_level, psci_power_state_status_t *power_state_info, unsigned int is_power_down_state) {
   int retval = PSCI_STATUS_SUCCESS;
   bool skip_wfi = false;
   unsigned int cpu_index = hv_psci_get_core_position();
   unsigned int parent_nodes[PSCI_MAX_POWER_LEVEL] = {0};

   hv_psci_get_parent_nodes(cpu_index, end_power_level, parent_nodes);

   //
   // Acquire power domain spinlocks to get a static snapshot to manage the states.
   //
   hv_psci_acquire_power_domain_tree_locks(end_power_level, parent_nodes);

   //
   // If there's any pending interrupt to be serviced, stop the suspend early.
   //
   uint64_t isr = mrs(ISR_EL1);
   if(isr != 0) {
      skip_wfi = true;
      goto exit_suspend;
   }

   hv_psci_coordinate_power_states(end_power_level, power_state_info);

   if(is_power_down_state != 0) {
      //
      // Do preparation for a "power down" suspend.
      //
      hv_psci_start_suspend_to_power_down(end_power_level, entry_point, power_state_info);
   }
   if(power_state_info->power_domain_state[PSCI_CPU_POWER_LEVEL] == PSCI_IDLE_STANDBY_STATE) {
      //
      // TODO: Deep sleep specific operations, only going to use "shallow sleep" for now.
      // For shallow sleep there's nothing to really do.
      //
   }
exit_suspend:
   hv_psci_release_power_domain_tree_locks(end_power_level, parent_nodes);
   if(skip_wfi == true) {
      return retval;
   }
   if(is_power_down_state != 0U) {
      //
      // We're going to be doing a deep sleep. Note this is not implemented yet, so this code path right now will do nothing.
      //

      //cpu_sleep(true);
   }
   //
   // At this point we are about to execute a context-retaining/shallow WFI. Do so now.
   //
   sysop("isb");
   __asm__ ("wfi");

   hv_psci_finish_cpu_suspend(cpu_index, end_power_level);
   return retval;
   
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
   int retval;
   unsigned int target_power_level, is_power_down_state;
   entry_point_info_t entry_point;
   psci_power_state_status_t power_state_info = { {PSCI_ON_STATE} };
   platform_local_state_t cpu_power_domain_state;
   bool is_cpu_standby_requested;

   retval = hv_psci_validate_power_state(power_state, &power_state_info);
   if(retval != PSCI_STATUS_SUCCESS) {
      printf("PSCI DEBUG: power state validation failed or bug found\n");
      return retval;
   }

   is_power_down_state = hv_psci_power_state_get_type(power_state);

   //
   // Sanity check the suspend request
   //
   assert(hv_psci_validate_suspend_request(&power_state_info, is_power_down_state) == PSCI_STATUS_SUCCESS);

   target_power_level = hv_psci_find_target_suspend_level(&power_state_info);
   if(target_power_level == PSCI_INVALID_LEVEL) {
      panic("PSCI DEBUG: invalid target suspend power level (or buggy code)\n");
   }


   //
   // Check to see if we're requesting standby or a deeper retention of a core.
   // If so, fast track the standby.
   //
   is_cpu_standby_requested = hv_psci_is_cpu_standby_requested(is_power_down_state, target_power_level);
   if(is_cpu_standby_requested == true) {
      cpu_power_domain_state = power_state_info.power_domain_state[PSCI_CPU_POWER_LEVEL];
      hv_psci_set_cpu_local_state(cpu_power_domain_state);
      //
      // Actually put the CPU in standby mode. (For now we're doing shallow WFI sleep)
      //
      sysop("isb");
      __asm__ ("wfi");

      //
      // When exiting standby, set state back to ON state.
      //
      hv_psci_set_cpu_local_state(PSCI_ON_STATE);

      return PSCI_STATUS_SUCCESS;
   }

   //
   // If we're powering down, make sure the entry point is correct.
   //
   if(is_power_down_state != 0U) {
      retval = hv_psci_validate_entry_point(&entry_point, cpu_reentry_addr, context);
   }

   //
   // Actually begin performing the suspend operation.
   //
   retval = hv_psci_start_cpu_suspend(&entry_point, target_power_level, &power_state_info, is_power_down_state);
   return retval;

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
            int retval = hv_psci_turn_on_cpu(ctx->regs[1], ctx->regs[2], ctx->regs[3]);
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