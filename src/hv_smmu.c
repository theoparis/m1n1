/**
 * hv_smmu.c
 * 
 * amarioguy (Arminder Singh) <arminders208@outlook.com>
 * 
 * This file implements the logic needed to make an SMMU abstraction layer for guest VMs.
 * This is intended to help run OSes that require 4k pages and cannot be made to support 16k pages easily.
 * 
 * This is primarily intended to support scenarios where the DARTs are required for an unsupported OS (without patches)
 * 
 * The idea is that we can map an SMMU into the IPA space of the guest (this will support 4k page granularity) and the guest will set up stage 1 tables as such.
 * The SMMU will then "pass through" writes from SMMU to the DARTs and then the hardware.
 * 
 * This is slower than true DMA through the IOMMUs but this is a cost of using the layrt
 * 
 * This should *only* be used if 16k pages cannot be reasonably allocated or the kernel cannot be reasonably patched, and will be gated behind a compile time define. 
 * 
 * 
 * @copyright Copyright amarioguy (Arminder Singh) (c) 2022.
 * 
 * SPDX-License-Identifier: MIT
 * 
 */
#include "hv.h"
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
#include "smp.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"

int hv_smmu_init(void);

//right now this is a stub, this will be filled out later.
int hv_smmu_init(void)
{
    return 0;
}