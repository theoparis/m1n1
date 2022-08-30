/* SPDX-License-Identifier: MIT */

#ifndef HV_H
#define HV_H

#include "exception.h"
#include "iodev.h"
#include "types.h"
#include "uartproxy.h"

typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

#define MMIO_EVT_CPU   GENMASK(23, 16)
#define MMIO_EVT_MULTI BIT(6)
#define MMIO_EVT_WRITE BIT(5)
#define MMIO_EVT_WIDTH GENMASK(4, 0)

struct hv_evt_mmiotrace {
    u32 flags;
    u32 reserved;
    u64 pc;
    u64 addr;
    u64 data;
};

struct hv_evt_irqtrace {
    u32 flags;
    u16 type;
    u16 num;
};

#define HV_MAX_RW_SIZE  64
#define HV_MAX_RW_WORDS (HV_MAX_RW_SIZE >> 3)

struct hv_vm_proxy_hook_data {
    u32 flags;
    u32 id;
    u64 addr;
    u64 data[HV_MAX_RW_WORDS];
};

typedef enum _hv_entry_type {
    HV_HOOK_VM = 1,
    HV_VTIMER,
    HV_USER_INTERRUPT,
    HV_WDT_BARK,
    HV_CPU_SWITCH,
} hv_entry_type;

/* vGICv3 structs 
   yes this can likely be improved 
   any regs that won't be used during normal operation will have 
   "reserved_" in front of them to pad out dist to 64k size */

/**
 * Distributor registers
 * 
 * This is global to the system, accesses from the guest via MMIO writes or reads will read/write data from an instance
 * of this struct.
 * 
 */

typedef struct vgicv3_distributor {
    //0x0000-0x0010
    // Control, type, implementer ID, type register 2, error status regs
    unsigned int ctl_register;
    unsigned int type_register;
    unsigned int imp_id_register;
    unsigned int type_register_2;
    unsigned int err_sts;
    
    //0x0040 - GICD_SETSPI_NSR
    // Set SPI reg, non secure mode
    unsigned int set_spi_register;
    
    //0x0048 - GICD_CLRSPI_NSR
    // Clear SPI reg, non secure mode
    unsigned int clear_spi_register;

    //0x0080-0x00fc
    unsigned int interrupt_group_registers[32];

    //0x0100-0x017c
    unsigned int interrupt_set_enable_regs[32];

    //0x0180-0x01fc
    unsigned int interrupt_clear_enable_regs[32];

    //0x0200-0x027c
    unsigned int interrupt_set_pending_regs[32];

    //0x0280-0x02fc
    unsigned int interrupt_clear_pending_regs[32];

    //0x0300-0x037c
    unsigned int interrupt_set_active_regs[32];

    //0x038c-0x03fc
    unsigned int interrupt_clear_active_regs[32];

    //0x0400-0x07f8
    unsigned int interrupt_priority_regs[255];

    //0x0800-0x081c - GICD_ITARGETSR0-R7 (max needed for "v1" SoC versions)
    //reserved, Apple SoCs do not support legacy operation, so this is useless
    unsigned int interrupt_processor_target_regs_ro[8];

    //0x0820-0xBF8 - GICD_ITARGETSR8-R255
    //ditto above
    unsigned int interrupt_processor_target_regs[248];

    //0x0C00-0x0CFC - GICD_ICFGR0-63
    unsigned int interrupt_config_regs[64];

    //0x0D00-0x0D7C - GICD_IGRPMODR0-31
    unsigned int interrupt_group_modifier_regs[32];

    //0x0E00-0x0EFC - GICD_NSACR0-63
    //i have doubts as to whether this is necessary, given M series don't implement EL3
    unsigned int interrupt_nonsecure_access_ctl_regs[64];

    //0x0F00 - GICD_SGIR (software generated interrupts)
    unsigned int interrupt_software_generated_reg;

    //0x0F10-0x0F1C - GICD_CPENDSGIR0-3
    unsigned int interrupt_sgi_clear_pending_regs[4];

    //0x0F20-0x0F2C - GICD_SPENDSGIR0-3
    unsigned int interrupt_sgi_set_pending_regs[4];

    //0x0F80-0x0FFC - GICD_INMIR - NMI Regs
    //Apple SoCs as of 8/17/2022 do not implement NMI, these will never be used by anything but add them so that the size of the dist follows ARM spec
    unsigned int interrupt_nmi_regs[32];

    //0x1000-0x107C - GICD_IGROUPR0E-31E
    unsigned int interrupt_group_regs_ext_spi_range[32];

    //0x1200-0x127C - GICD_ISENABLER0E-31E
    unsigned int interrupt_set_enable_ext_spi_range_regs[32];

    //0x1400-0x147C - GICD_ICENABLER0E-31E
    unsigned int interrupt_clear_enable_ext_spi_range_regs[32];

    //0x1600-0x167C - GICD_ISPENDR0E-31E
    unsigned int interrupt_set_pending_ext_spi_range_regs[32];

    //0x1800-0x187C - GICD_ICPENDR0E-31E
    unsigned int interrupt_clear_pending_ext_spi_range_regs[32];

    //0x1A00-0x1A7C - GICD_ISACTIVER0E-31E
    unsigned int interrupt_set_active_ext_spi_range_regs[32];

    //0x1C00-0x1C7C - GICD_ICACTIVER0E-31E
    unsigned int interrupt_clear_active_ext_spi_range_regs[32];

    //0x2000-0x23FC - GICD_IPRIORITYR0E-255E
    unsigned int interrupt_priority_ext_spi_range_regs[256];

    //0x3000-0x30FC - GICD_ICFGR0E-63E
    unsigned int interrupt_ext_spi_config_regs[64];

    //0x3400-0x347C - GICD_IGRPMODR0E-61E
    unsigned int interrupt_group_modifier_ext_spi_range_regs[32];

    //0x3600-0x367C - GICD_NSACR0E-31E
    unsigned int non_secure_ext_spi_range_interrupt_regs[32];

    //0x3B00-0x3B7C
    //NMI regs for extended SPI range
    //ditto above point, no NMI support on Apple chips, but add it so that the size of the dist is the same as ARM spec
    unsigned int interrupt_nmi_reg_ext_spi_range[32];

    //0x6100-0x7FD8 - GICD_IROUTER(32-1019)
    unsigned long interrupt_router_regs[988];

    //0x8000-0x9FFC - GICD_IROUTER(0-1023)E
    unsigned long interrupt_router_ext_spi_range_regs[1024];


} vgicv3_dist;

/**
 * Redistributor registers.
 * 
 * These need to be laid out contiguously, so that the guest sees in the IPA space that they're contiguous.
 * 
 * Maybe have a struct per CPU that has a pointer to it's given redistributor region? Or make an array of these, then point to the array?
 * 
 */
typedef struct vgicv3_redistributor_region {
    //8 of these on M1/M2, 10-20 on M1v2
} vgicv3_vcpu_redist;

/* VM */
void hv_pt_init(void);
int hv_map(u64 from, u64 to, u64 size, u64 incr);
int hv_unmap(u64 from, u64 size);
int hv_map_hw(u64 from, u64 to, u64 size);
int hv_map_sw(u64 from, u64 to, u64 size);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
u64 hv_translate(u64 addr, bool s1only, bool w);
u64 hv_pt_walk(u64 addr);
bool hv_handle_dabort(struct exc_info *ctx);
bool hv_pa_write(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_read(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_rw(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

/* AIC events through tracing the MMIO event address */
bool hv_trace_irq(u32 type, u32 num, u32 count, u32 flags);

/* Virtual peripherals */
void hv_vuart_poll(void);
void hv_map_vuart(u64 base, int irq, iodev_id_t iodev);

/* Exceptions */
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra);
void hv_set_time_stealing(bool enabled, bool reset);

/* WDT */
void hv_wdt_pet(void);
void hv_wdt_suspend(void);
void hv_wdt_resume(void);
void hv_wdt_init(void);
void hv_wdt_start(int cpu);
void hv_wdt_stop(void);
void hv_wdt_breadcrumb(char c);

/* Utilities */
void hv_write_hcr(u64 val);
u64 hv_get_spsr(void);
void hv_set_spsr(u64 val);
u64 hv_get_esr(void);
u64 hv_get_far(void);
u64 hv_get_elr(void);
u64 hv_get_afsr1(void);
void hv_set_elr(u64 val);

/* HV main */
void hv_init(void);
void hv_start(void *entry, u64 regs[4]);
void hv_start_secondary(int cpu, void *entry, u64 regs[4]);
void hv_rendezvous(void);
bool hv_switch_cpu(int cpu);
void hv_pin_cpu(int cpu);
void hv_arm_tick(void);
void hv_rearm(void);
void hv_maybe_exit(void);
void hv_tick(struct exc_info *ctx);

#endif
