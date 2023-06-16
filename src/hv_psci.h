/**
 * Copyright (c) 2023, amarioguy (AppleWOA authors), 2023.
 * 
 * Filename: hv_psci.c
 * 
 * Description: PSCI definition header file. 
 * 
 * Note: for bare metal booting, we use a higher level firmware running in GL2 to provide PSCI, so this
 * file does not account for that case.
 * 
 * Implementation is based off the version in ARM Trusted Firmware-A. (https://github.com/ARM-software/arm-trusted-firmware)
 * 
 * License: SPDX-License-Identifier: MIT OR BSD-3-Clause
*/


#ifndef HV_PSCI_H
#define HV_PSCI_H

#define PSCI_MAJOR_VER_1 (1U << 16)
#define PSCI_MINOR_VER_0 0
#define PSCI_MINOR_VER_1 1

//
// PSCI definition macros.
//

#define PSCI_VERSION (PSCI_MAJOR_VER_1 | PSCI_MINOR_VER_1)
#define SMC_64_FUNCTION BIT(30)
#define PSCI_MAX_POWER_LEVEL 2U //corresponds to MPIDR Aff2 (this is how ARM Trusted Firmware defines it, it permits a on, standby, and off state)
#define CLUSTER_NUMBER_MASK (0xff << 8)
#define CORE_NUMBER_MASK 0xff
#define NUM_SYSTEMS_ACTIVE 1
#define PSCI_ON_STATE 0
#define PSCI_RETENTION_STATE 1
#define PSCI_OFF_STATE 2
#define PSCI_CPU_POWER_LEVEL 0U
#define PSCI_MAX_RETENTION_STATE 1U
#define PSCI_MAX_OFF_STATE 2U

//
// PSCI return values.
//
#define PSCI_SUCCESS 0
#define PSCI_NOT_SUPPORTED -1
#define PSCI_INVALID_PARAMETERS -2
#define PSCI_OPERATION_DENIED -3
#define PSCI_ALREADY_ON -4
#define PSCI_ON_PENDING -5
#define PSCI_INTERNAL_FAILURE -6
#define PSCI_NOT_PRESENT -7
#define PSCI_DISABLED -8
#define PSCI_INVALID_ADDRESS -9

//
// PSCI function IDs.
//
#define PSCI_GET_VERSION_FUNCTION_ID 0x84000000
#define PSCI_SUSPEND_CPU_ARM32_FUNCTION_ID 0x84000001
#define PSCI_CPU_OFF_FUNCTION_ID 0x84000002
#define PSCI_CPU_ON_ARM32_FUNCTION_ID 0x84000003
#define PSCI_SYSTEM_POWEROFF_FUNCTION_ID 0x84000008
#define PSCI_SYSTEM_RESET_FUNCTION_ID 0x84000009

//
// PSCI enums.
//

typedef enum {
    AFFINITY_STATE_ON = 0U,
    AFFINITY_STATE_OFF = 1U,
    AFFINITY_STATE_ON_PENDING = 2U
} affinity_info_state_t;

//
// PSCI typedefs and structs.
//

//
// A typedef indicating a platform local state.
//
typedef uint8_t platform_local_state_t;

//
// Struct/typedef storing current CPU's desired power state.
// Taken from ARM Trusted Firmware-A, include\lib\psci\psci.h
//
typedef struct psci_power_state_status {
    //
    // There are a maximum of three power levels (core, cluster, system).
    // This will store the state for each level of the CPU.
    //
    unsigned char power_domain_state[3];

    //
    // highest power level at which current CPU is the last running one.
    //
    unsigned int last_cpu_at_power_level;

} psci_power_state_status_t;

//
// Struct/typedef indicating a CPU power domain node.
//

typedef struct psci_cpu_power_domain_node {
    unsigned long mpidr;
    unsigned int parent_node;
    spinlock_t lock_for_cpu;
} cpu_power_domain_node_t;

//
// Struct describing a non-CPU power domain node.
// Implementation is the one used in Trusted Firmware-A.
//
typedef struct psci_non_cpu_power_domain_node {
    //
    // The first CPU which has this node as its parent.
    //
    unsigned int first_cpu_idx;

    //
    // Sibling nodes of the first CPU.
    //
    unsigned int num_cpu_siblings;

    //
    // Index of the parent of this node.
    //
    unsigned int parent_node;

    //
    // The local power state.
    //
    platform_local_state_t local_power_state;

    //
    // current power level (on/retention/off)
    //
    unsigned char power_level;

    uint16_t lock_index;

} non_cpu_power_domain_node_t;

//
// Struct for holding per CPU information for PSCI code.
//
typedef struct psci_per_cpu_data {
    affinity_info_state_t affinity_state;
    unsigned int target_power_level;
    platform_local_state_t local_cpu_state;
} psci_per_cpu_data_t;

//
// PSCI function prototypes.
//

//
// Nothing right now.
//

//
// Miscellaneous defines.
//

#define PAGE_SIZE       0x4000
#define CACHE_LINE_SIZE 64

#define CACHE_RANGE_OP(func, op)                                                                   \
    void func(void *addr, size_t length)                                                           \
    {                                                                                              \
        u64 p = (u64)addr;                                                                         \
        u64 end = p + length;                                                                      \
        while (p < end) {                                                                          \
            cacheop(op, p);                                                                        \
            p += CACHE_LINE_SIZE;                                                                  \
        }                                                                                          \
    }

CACHE_RANGE_OP(ic_ivau_range, "ic ivau")
CACHE_RANGE_OP(dc_ivac_range, "dc ivac")
CACHE_RANGE_OP(dc_zva_range, "dc zva")
CACHE_RANGE_OP(dc_cvac_range, "dc cvac")
CACHE_RANGE_OP(dc_cvau_range, "dc cvau")
CACHE_RANGE_OP(dc_civac_range, "dc civac")

static inline u64 read_sctlr(void)
{
    sysop("isb");
    return mrs(SCTLR_EL1);
}

static inline void write_sctlr(u64 val)
{
    msr(SCTLR_EL1, val);
    sysop("isb");
}

#endif //HV_PSCI_H