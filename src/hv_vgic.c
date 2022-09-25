/**
 * hv_vgic.c
 * @author amarioguy (Arminder Singh)
 * 
 * Virtual Generic Interrupt Controller implementation for m1n1, to aid in running non open source operating systems.
 * 
 * Enables CPU interface before guest OS boot, sets up emulated distributor/redistributor regions
 * 
 * @version 1.0
 * @date 2022-08-29 (refactored from original code)
 * 
 * @copyright Copyright (c) amarioguy (Arminder Singh), 2022.
 * 
 * SPDX-License-Identifier: MIT
 * 
 */



#include "hv.h"
#include "hv_vgic.h"
#include "assert.h"
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

/**
 * General idea of how this should work:
 * 
 * Apple Silicon chips (M1/M1v2/M2 at the moment) implement the GIC CPU interface registers in hardware, meaning
 * only the distributor and the core specific redistributors need to be emulated by m1n1.
 * 
 * As such, this file implements most of the code needed to make this possible. The emulated distributor/redistributors
 * will need to meet a few constraints (namely it's limited by what the GIC CPU interface supports)
 * 
 * Apple's vGIC CPU interface has the following characteristics in M1/M1v2/M2:
 * - 32 levels of virtual priority and preemption priority
 * - 16 bits of virtual interrupt ID bits (meaning up to 65535 interrupts are supported theoretically, however practically limited by the number of IRQs the AIC supports)
 * - supports guest-generated SEIs (note that this can result in a hardware bug on M1 and M1v2 (unknown for M2) where violation of the state machine results in a host SError)
 * - 3 level affinity (aff2/aff1/aff0 valid, aff3 invalid/reserved as 0)
 * - legacy operation is not supported (ICC_SRE_EL2.SRE is reserved, set to 1)
 * - TDIR bit is supported
 * - extended SPI ranges are *not* supported (implying it's not a GICv3.1)
 * - 8 list registers
 * - direct injection of virtual interrupts are not supported (therefore not a GICv4, and by implication, no NMIs supported either)
 * 
 * At the moment an ITS will not be implemented or used.
 * 
 * This code makes one critical assumption: the guest that runs under m1n1 will be the only guest running on the system throughout.
 * As such, the distributor is a simple global variable (and therefore part of m1n1's state) rather than being contained in a per-VM struct.
 * 
 * TODO - figure out if redistributors should be global just like the distributor is at the moment (with compile flags determining how many should be used)
 * or if the number of vCPUs should be enumerated dynamically and the redistributor structs created from there.
 * 
 * On M1 and M2 platforms, memory range 0xF00000000-0xFFFFFFFFF is completely unmapped in both the guest's IPA (intermediate physical address) space and the real SPA 
 * (system physical address) space by default on all possible RAM configurations for all known M1 and M2 platforms. This will be the space in which the vGIC distributor
 * and redistributors will be mapped. (Note that the first 0x1FFFFFFFF bytes of RAM are also available unconditionally and if the current 
 * location is problematic down the line, the vGIC will be moved to the bottom of address space)
 * 
 * On M1v2, the distributor/redistributor regions are placed between the end of MMIO space and the start of DRAM, to keep it in
 * a region where it's known that they won't issue.
 * 
 * Current mapping for M1/M2:
 * 
 * Distributor - 0xF00000000
 * Redistributors - 0xF10000000
 * 
 * Current mapping for M1v2:
 * 
 * Distributor - 0x5000000000
 * Redistributors - 0x5100000000
 * 
 * 
 */

//a pointer to the distributor as a global variable (accessible throughout the entire program)
vgicv3_dist_regs *distributor;



/**
 * @brief handle_vgic_dist_access
 * 
 * The function that will be executed on every vGIC distributor access from the guest once mapped by hv_map_hook
 * 
 * 
 * @param ctx - exception info/context
 * @param addr
 * @param val
 * @param write - was the attempted access a read or a write? 
 * @param width - size of the access
 * @return true 
 * @return false 
 */
static bool handle_vgic_dist_access(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    return false;
}


/**
 * @brief handle_vgic_redist_access
 * 
 * The function that will be executed on every vGIC redistributor access from the guest once mapped by hv_map_hook
 * 
 * 
 * @param ctx - exception info/context
 * @param addr
 * @param val
 * @param write - was the attempted access a read or a write? 
 * @param width - size of the access
 * @return true 
 * @return false 
 */
static bool handle_vgic_redist_access(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    return false;
}



/**
 * @brief hv_vgicv3_init
 * 
 * Initializes the vGIC and prepares it for use by the guest OS.
 * 
 * Note that this function is only expected to be called once and subsequent calls will have undefined behavior
 * 
 * @return 
 * 
 * 0 - success, vGIC is ready for use by the guest
 * -1 - an error has occurred during vGIC initialization, refer to m1n1 output log for details on the specific error 
 */

int hv_vgicv3_init(void)
{
    /* Distributor setup */
    //TODO: most distributor setup
    distributor = heapblock_alloc(sizeof(vgicv3_dist_regs));
    hv_vgicv3_init_dist_registers();

    //all distributor structs are ready, map it into the guest IPA space
    hv_map_hook(0xF00000000, handle_vgic_dist_access, sizeof(vgicv3_dist_regs));


    /* Redistributor setup */
    //TODO: all redistributor setup
    //all redistributors are ready, map them into the guest IPA space
    hv_map_hook(0xF10000000, handle_vgic_redist_access, sizeof(vgicv3_vcpu_redist_regs));

    //vGIC setup is successful, let the caller know
    return 0;
}

/**
 * @brief hv_vgicv3_init_dist_registers
 * 
 * Sets up the initial values for the distributor registers.
 * 
 * For registers that deal with unsupported features, set them to 0 and just never interact with them
 * 
 * For write only registers, set them to 0, and emulate the effect upon attempting to write that register.
 * 
 */
void hv_vgicv3_init_dist_registers(void)
{
    distributor->ctl_register = (BIT(6) | BIT(4) | BIT(1) | BIT(0));
    distributor->type_register = (BIT(22) | BIT(21) | BIT(20) | BIT(19));
    distributor->imp_id_register = (BIT(10) | BIT(5) | BIT(4) | BIT(3) | BIT(1) | BIT(0));
    distributor->type_register_2 = 0;
    distributor->err_sts = 0;
    //set all SPIs to group 1, disable all SPIs
    for(int i = 0; i < 32; i++)
    {
        distributor->interrupt_group_registers[i] = 0;
        distributor->interrupt_set_enable_regs[i] = 0;
    }

}


/**
 * @brief hv_vgicv3_init_list_registers
 * 
 * Enables the platform's list registers for use by the guest OS.
 * 
 * @param n - the number of the list register to be turned on
 */
void hv_vgicv3_init_list_registers(int n)
{
    switch(n)
        {
            case 0:
                msr(ICH_LR0_EL2, 0);
            case 1:
                msr(ICH_LR1_EL2, 0);
            case 2:
                msr(ICH_LR2_EL2, 0);
            case 3:
                msr(ICH_LR3_EL2, 0);
            case 4:
                msr(ICH_LR4_EL2, 0);
            case 5:
                msr(ICH_LR5_EL2, 0);
            case 6:
                msr(ICH_LR6_EL2, 0);
            case 7:
                msr(ICH_LR7_EL2, 0);
        }
}


/**
 * @brief hv_vgicv3_enable_virtual_interrupts
 * 
 * Enables virtual interrupts for the guest.
 * 
 * Note that actual interrupts are always handled by m1n1, then passed onto the vGIC which will signal the virtual interrupt to the OS.
 * 
 * @return
 * 0 - success
 * -1 - there was an error.
 */

int hv_vgicv3_enable_virtual_interrupts(void)
{
    //set VMCR to reset values, then enable virtual group 0 and 1 interrupts
    msr(ICH_VMCR_EL2, 0);
    msr(ICH_VMCR_EL2, (BIT(1)));
    //bit 0 enables the virtual CPU interface registers
    //AMO/IMO/FMO set by m1n1 on boot
    msr(ICH_HCR_EL2, (BIT(0)));


    return 0;
}