/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU aspect system (aspect plan AS-1): the register semantics of
 * asCmdSta (0x68) and asQlData[4] (0x6C-0x7B) and the device reset
 * (D-ASa, D-ASf). The generator (the 1 ms tick, the 63-row double
 * buffer, the ROTB blocks) is AS-2; the knobs D-ASc/D-ASd are AS-3.
 *
 * asCmdSta (StixIdpu.h:667-685): ASAON (bit 0) and ASBON (bit 1) enable
 * the two ADC channels, AVGSIZE (10:8) selects the samples per summed
 * row (values 0..4; the FSW validates the value itself,
 * Aspect.c:108-118). The reserved bits 2..7 and 11..31
 * (StixIdpu.h:681-685) are dropped on write and read 0. Each asQlData
 * word is stored whole, but only its 16-bit DATA field reads back (the
 * FSW masks the upper half, Aspect.c:127-133).
 */

#include "qemu/osdep.h"
#include "stix_aspect.h"

/* the stored asCmdSta bits: ASAON, ASBON and AVGSIZE (10:8) */
#define STIX_ASPECT_CMDSTA_RW \
    ((STIXCTRL_ASCMDSTA_ASAON_MASK << STIXCTRL_ASCMDSTA_ASAON) | \
     (STIXCTRL_ASCMDSTA_ASBON_MASK << STIXCTRL_ASCMDSTA_ASBON) | \
     (STIXCTRL_ASCMDSTA_AVGSIZE_MASK << STIXCTRL_ASCMDSTA_AVGSIZE))

void stix_aspect_realize(StixAspectState *a, STIXCTRL *owner,
                         stixCtrlRegs_s *reg)
{
    a->owner = owner;
    a->reg = reg;
}

/*
 * D-ASf: a watchdog reboot cuts the aspect supply, and the FSW
 * re-powers and re-configures at every boot/SAFE/NOMINAL entry
 * (OpModeMgmt.c:1066-1068), so the registers start from 0 (channels
 * off, AVGSIZE 0). as-model=off keeps today's plain storage and no
 * reset (grlib_stixctrl.c falls through to the register file).
 */
void stix_aspect_reset(StixAspectState *a)
{
    uint32_t i;

    if (!a->model) {
        return;
    }
    a->reg->asCmdSta = 0;
    for (i = 0; i < 4; i++) {
        a->reg->asQlData[i] = 0;
    }
}

bool stix_aspect_owns(StixAspectState *a, hwaddr addr)
{
    return a->model && addr >= STIX_ASPECT_REG_FIRST &&
           addr <= STIX_ASPECT_REG_LAST;
}

uint32_t stix_aspect_read(StixAspectState *a, hwaddr addr)
{
    unsigned idx = (addr - STIX_ASPECT_REG_FIRST) / 4u;

    /* word index from 0x68 (0 = asCmdSta, 1..4 = QL), never negative */
    if (idx == 0) {
        return a->reg->asCmdSta;    /* stored masked; reserved reads 0 */
    }
    return a->reg->asQlData[idx - 1] & STIXCTRL_ASQLDATA_DATA_MASK;
}

void stix_aspect_write(StixAspectState *a, hwaddr addr, uint32_t value)
{
    unsigned idx = (addr - STIX_ASPECT_REG_FIRST) / 4u;

    if (idx == 0) {
        a->reg->asCmdSta = value & STIX_ASPECT_CMDSTA_RW;
        return;
    }
    a->reg->asQlData[idx - 1] = value;
}
