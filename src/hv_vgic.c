/* SPDX-License-Identifier: MIT and GPLv2 */
// dual licensed just to cover my bases

// temporary dev comments, to be removed later
/* vGIC setup and initialization for EL1, to run certain proprietary OSes that
   do not support AIC and cannot be made to do so easily.

   dynamic patchfinding: am i a joke to you?
   developer: your time will come

   Objectives:
        emulate vGIC distributor/redistributors
        redirect virtual interrupts to HW


*/

//temporary set of includes, subject to change, from hv.c
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

//initialize distributor globally (struct defined in hv.h)
//warning this is likely the wrong way to go about it
static vgicv3_dist distributor;

// initializes the vgic registers, maps them to memory (one VM only)
void vgicv3_init()
{
    

    //success case, placeholder
    return 0;
}