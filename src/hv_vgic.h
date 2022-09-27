/* SPDX-License-Identifier: MIT */

#ifndef HV_VGIC_H
#define HV_VGIC_H

/**
 * Distributor registers
 * 
 * These are global to the system, accesses from the guest via MMIO writes or reads will read/write data from an instance
 * of this struct.
 * 
 */

typedef struct vgicv3_distributor_registers {
    //0x0000-0x0010
    // Control, type, implementer ID, type register 2, error status regs

    //GICD_CTLR
    unsigned int gicd_ctl_reg;
    //GICD_TYPER
    unsigned int gicd_type_reg;
    //GICD_IIDR
    unsigned int gicd_imp_id_reg;
    //GICD_TYPER2
    unsigned int gicd_type_reg_2;
    //GICD_
    unsigned int gicd_err_sts;
    
    //0x0040 - GICD_SETSPI_NSR
    // Set SPI reg, non secure mode
    unsigned int gicd_set_spi_reg;
    
    //0x0048 - GICD_CLRSPI_NSR
    // Clear SPI reg, non secure mode
    unsigned int gicd_clear_spi_reg;

    //0x0080-0x00fc
    unsigned int gicd_interrupt_group_regs[32];

    //0x0100-0x017c
    unsigned int gicd_interrupt_set_enable_regs[32];

    //0x0180-0x01fc
    unsigned int gicd_interrupt_clear_enable_regs[32];

    //0x0200-0x027c
    unsigned int gicd_interrupt_set_pending_regs[32];

    //0x0280-0x02fc
    unsigned int gicd_interrupt_clear_pending_regs[32];

    //0x0300-0x037c
    unsigned int gicd_interrupt_set_active_regs[32];

    //0x038c-0x03fc
    unsigned int gicd_interrupt_clear_active_regs[32];

    //0x0400-0x07f8
    unsigned int gicd_interrupt_priority_regs[255];

    //0x0800-0x081c - GICD_ITARGETSR0-R7 (max needed for "v1" SoC versions)
    //reserved, Apple SoCs do not support legacy operation, so this is useless
    unsigned int gicd_interrupt_processor_target_regs_ro[8];

    //0x0820-0xBF8 - GICD_ITARGETSR8-R255
    //ditto above
    unsigned int gicd_interrupt_processor_target_regs[248];

    //0x0C00-0x0CFC - GICD_ICFGR0-63
    unsigned int gicd_interrupt_config_regs[64];

    //0x0D00-0x0D7C - GICD_IGRPMODR0-31
    unsigned int gicd_interrupt_group_modifier_regs[32];

    //0x0E00-0x0EFC - GICD_NSACR0-63
    //i have doubts as to whether this is necessary, given M series don't implement EL3
    unsigned int gicd_interrupt_nonsecure_access_ctl_regs[64];

    //0x0F00 - GICD_SGIR (software generated interrupts)
    unsigned int gicd_interrupt_software_generated_reg;

    //0x0F10-0x0F1C - GICD_CPENDSGIR0-3
    unsigned int gicd_interrupt_sgi_clear_pending_regs[4];

    //0x0F20-0x0F2C - GICD_SPENDSGIR0-3
    unsigned int gicd_interrupt_sgi_set_pending_regs[4];

    //0x0F80-0x0FFC - GICD_INMIR - NMI Regs
    //Apple SoCs as of 8/17/2022 do not implement NMI, these will never be used by anything but add them so that the size of the dist follows ARM spec
    unsigned int gicd_interrupt_nmi_regs[32];

    //0x1000-0x107C - GICD_IGROUPR0E-31E
    unsigned int gicd_interrupt_group_regs_ext_spi_range[32];

    //0x1200-0x127C - GICD_ISENABLER0E-31E
    unsigned int gicd_interrupt_set_enable_ext_spi_range_regs[32];

    //0x1400-0x147C - GICD_ICENABLER0E-31E
    unsigned int gicd_interrupt_clear_enable_ext_spi_range_regs[32];

    //0x1600-0x167C - GICD_ISPENDR0E-31E
    unsigned int gicd_interrupt_set_pending_ext_spi_range_regs[32];

    //0x1800-0x187C - GICD_ICPENDR0E-31E
    unsigned int gicd_interrupt_clear_pending_ext_spi_range_regs[32];

    //0x1A00-0x1A7C - GICD_ISACTIVER0E-31E
    unsigned int gicd_interrupt_set_active_ext_spi_range_regs[32];

    //0x1C00-0x1C7C - GICD_ICACTIVER0E-31E
    unsigned int gicd_interrupt_clear_active_ext_spi_range_regs[32];

    //0x2000-0x23FC - GICD_IPRIORITYR0E-255E
    unsigned int gicd_interrupt_priority_ext_spi_range_regs[256];

    //0x3000-0x30FC - GICD_ICFGR0E-63E
    unsigned int gicd_interrupt_ext_spi_config_regs[64];

    //0x3400-0x347C - GICD_IGRPMODR0E-61E
    unsigned int gicd_interrupt_group_modifier_ext_spi_range_regs[32];

    //0x3600-0x367C - GICD_NSACR0E-31E
    unsigned int gicd_non_secure_ext_spi_range_interrupt_regs[32];

    //0x3B00-0x3B7C
    //NMI regs for extended SPI range
    //ditto above point, no NMI support on Apple chips, but add it so that the size of the dist is the same as ARM spec
    unsigned int gicd_interrupt_nmi_reg_ext_spi_range[32];

    //0x6100-0x7FD8 - GICD_IROUTER(32-1019)
    unsigned long gicd_interrupt_router_regs[988];

    //0x8000-0x9FFC - GICD_IROUTER(0-1023)E
    unsigned long gicd_interrupt_router_ext_spi_range_regs[1024];


} vgicv3_dist_regs;

/**
 * Redistributor registers.
 * 
 * These need to be laid out contiguously.
 * 
 * Maybe have a struct per CPU that has a pointer to it's given redistributor region? Or make an array of these, then point to the array?
 * 
 */
typedef struct vgicv3_redistributor_region {
    //GICR_CTLR
    unsigned int gicr_ctl_reg;
    //GICR_IIDR
    unsigned int gicr_iidr;
} vgicv3_vcpu_redist_regs;


/**
 * 
 * vGIC device struct.
 * 
 * Note that this is just the MMIO regions, as the CPU interface is in hardware.
 * 
 */
typedef struct vgicv3_device_mmio {

    vgicv3_dist_regs distributor;

    vgicv3_vcpu_redist_regs *redistributor;

} vgicv3;


/* vGIC */

int hv_vgicv3_init(void);

void hv_vgicv3_init_dist_registers(void);

void hv_vgicv3_init_list_registers(int n);

int hv_vgicv3_enable_virtual_interrupts(void);

#endif //HV_VGIC_H