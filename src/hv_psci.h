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
 * License: SPDX-License-Identifier: MIT
*/


#ifndef HV_PSCI_H
#define HV_PSCI_H

#define PSCI_MAJOR_VER_1 (1U << 16)
#define PSCI_MINOR_VER_0 0
#define PSCI_MINOR_VER_1 1

/**
 * PSCI definition macros.
*/

#define PSCI_VERSION (PSCI_MAJOR_VER_1 | PSCI_MINOR_VER_1)
#define SMC_64_FUNCTION BIT(30)
#define PSCI_MAX_POWER_LEVEL 2U //corresponds to MPIDR Aff2 (this is how ARM Trusted Firmware defines it, it permits a on, standby, and off state)
#define CLUSTER_NUMBER_MASK (0xff << 8)
#define CORE_NUMBER_MASK 0xff
#define NUM_SYSTEMS_ACTIVE 1
/**
 * PSCI return values.
*/
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

/**
 * PSCI function IDs.
 */
#define PSCI_GET_VERSION_FUNCTION_ID 0x84000000
#define PSCI_SUSPEND_CPU_ARM32_FUNCTION_ID 0x84000001
#define PSCI_CPU_OFF_FUNCTION_ID 0x84000002
#define PSCI_CPU_ON_ARM32_FUNCTION_ID 0x84000003
#define PSCI_SYSTEM_POWEROFF_FUNCTION_ID 0x84000008
#define PSCI_SYSTEM_RESET_FUNCTION_ID 0x84000009


#endif //HV_PSCI_H