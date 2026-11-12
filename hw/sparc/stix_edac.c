/*
 * STIX IDPU EDAC unit (memory-upset plan D-Ua, M-1): the SEC counters
 * and DeDetected latches of the three EDAC memories and the edac-at-boot
 * property (D-Uk). No upsets yet: nothing here counts up — the strobes
 * clear, the test-latch properties force. No traces: the unit must not
 * appear in the trace golden (D-Ug: never at realize, reset or in a
 * getter).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "StixIdpu.h"
#include "stix_edac.h"

/* D-Uk, ICD reset values (STIX-ICD-0026 txt:2119, txt:2202, txt:3249) */
#define STIX_EDAC_BOOT_HW_RESET (1u << STIX_EDAC_CACHE)
#define STIX_EDAC_BOOT_ALL_ON 0x7u

bool stix_edac_boot_edacon(StixEdac *e, StixEdacModule m)
{
    if (e == NULL) {
        /* no unit linked: the ICD reset values */
        return m == STIX_EDAC_CACHE;
    }
    return (e->boot_mask >> m) & 1u;
}

void stix_edac_clear(StixEdac *e, StixEdacModule m)
{
    if (e == NULL) {
        return;
    }
    e->sec[m] = 0;
    e->ded[m] = false;
}

uint32_t stix_edac_sdram_read(StixEdac *e, uint32_t stored)
{
    uint32_t v = stored;

    v &= ~((FTSDCTRL16_EDAC_SECCOUNT_MASK << FTSDCTRL16_EDAC_SECCOUNT) |
           (FTSDCTRL16_EDAC_DEDETECTED_MASK << FTSDCTRL16_EDAC_DEDETECTED) |
           (FTSDCTRL16_EDAC_EDACRST_MASK << FTSDCTRL16_EDAC_EDACRST));
    if (e == NULL) {
        return v;
    }
    return v | e->sec[STIX_EDAC_SDRAM] |
           ((e->ded[STIX_EDAC_SDRAM] ? 1u : 0u)
            << FTSDCTRL16_EDAC_DEDETECTED);
}

uint32_t stix_edac_sdram_write(StixEdac *e, uint32_t stored, uint32_t value)
{
    /*
     * Only EDACON and SCRUBRATE are stored; SECCOUNT and DEDETECTED are
     * composed, EDACRST is a write-1 strobe that self-clears and resets
     * the counter and the latch (EDAC.c:294-297 set-then-clear pair).
     */
    uint32_t keep = (FTSDCTRL16_EDAC_EDACON_MASK << FTSDCTRL16_EDAC_EDACON) |
                    (FTSDCTRL16_EDAC_SCRUBRATE_MASK
                     << FTSDCTRL16_EDAC_SCRUBRATE);
    uint32_t out = (stored & ~keep) | (value & keep);

    if ((value >> FTSDCTRL16_EDAC_EDACRST) & FTSDCTRL16_EDAC_EDACRST_MASK) {
        stix_edac_clear(e, STIX_EDAC_SDRAM);
    }
    return out;
}

uint32_t stix_edac_cache_read(StixEdac *e, uint32_t stored)
{
    uint32_t v = stored;

    v &= ~((STIXCTRL_CACHECMDSTA_SECCOUNT_MASK
            << STIXCTRL_CACHECMDSTA_SECCOUNT) |
           (STIXCTRL_CACHECMDSTA_DEDETECTED_MASK
            << STIXCTRL_CACHECMDSTA_DEDETECTED) |
           (STIXCTRL_CACHECMDSTA_EDACRESET_MASK
            << STIXCTRL_CACHECMDSTA_EDACRESET));
    if (e == NULL) {
        return v;
    }
    return v | (e->sec[STIX_EDAC_CACHE]
                << STIXCTRL_CACHECMDSTA_SECCOUNT) |
           ((e->ded[STIX_EDAC_CACHE] ? 1u : 0u)
            << STIXCTRL_CACHECMDSTA_DEDETECTED);
}

uint32_t stix_edac_cache_write(StixEdac *e, uint32_t stored, uint32_t value)
{
    /*
     * OPMODE, SCRUBRATE and EDACON are stored; SECCOUNT and DEDETECTED
     * are composed, EDACRESET is a write-1 strobe that self-clears and
     * resets the counter and the latch (EDAC.c:299-302 set-then-clear
     * pair, read-modify-write).
     */
    uint32_t keep = (STIXCTRL_CACHECMDSTA_OPMODE_MASK
                     << STIXCTRL_CACHECMDSTA_OPMODE) |
                    (STIXCTRL_CACHECMDSTA_SCRUBRATE_MASK
                     << STIXCTRL_CACHECMDSTA_SCRUBRATE) |
                    (STIXCTRL_CACHECMDSTA_EDACON_MASK
                     << STIXCTRL_CACHECMDSTA_EDACON);
    uint32_t out = (stored & ~keep) | (value & keep);

    if ((value >> STIXCTRL_CACHECMDSTA_EDACRESET) &
        STIXCTRL_CACHECMDSTA_EDACRESET_MASK) {
        stix_edac_clear(e, STIX_EDAC_CACHE);
    }
    return out;
}

uint32_t stix_edac_rotb_status(StixEdac *e)
{
    if (e == NULL) {
        return 0;
    }
    return (e->sec[STIX_EDAC_ROTBUF] << STIXCTRL_ROTBCMDSTA_SECCOUNT) |
           ((e->ded[STIX_EDAC_ROTBUF] ? 1u : 0u)
            << STIXCTRL_ROTBCMDSTA_DEDETECTED);
}

/*
 * The unit is not on a bus, so the resettable tree never reaches it: its
 * reset is this hook. Registered at realize, it runs on the initial
 * reset and on every guest reset (power-on, watchdog). The EDACON bits
 * themselves are applied by the register owners' device resets, which
 * the sysbus bus reaches first (machine_run_board_init registers the bus
 * after all realizes, machine.c:1771 — a hook registered here would run
 * before them, so the owners apply the property, D-Uk).
 */
static void stix_edac_reset_hook(void *opaque)
{
    StixEdac *e = opaque;
    StixEdacModule m;

    for (m = 0; m < STIX_EDAC_N_MODULE; m++) {
        stix_edac_clear(e, m);
    }
}

static void stix_edac_realize(DeviceState *dev, Error **errp)
{
    StixEdac *e = STIX_EDAC(dev);

    qemu_register_reset(stix_edac_reset_hook, e);
}

static void stix_edac_unrealize(DeviceState *dev)
{
    qemu_unregister_reset(stix_edac_reset_hook, STIX_EDAC(dev));
}

/*
 * edac-at-boot (D-Uk). A bad value is refused at set time (like the
 * adc-inject range check), so a typo fails the qom-set, not a boot.
 */
static void stix_edac_boot_get(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    char *str = g_strdup(e->boot_str ? e->boot_str : "hw-reset");

    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void stix_edac_boot_set(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    char *str;
    const char *end;
    unsigned long val;
    int ret;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    if (strcmp(str, "hw-reset") == 0) {
        e->boot_mask = STIX_EDAC_BOOT_HW_RESET;
    } else if (strcmp(str, "all-on") == 0) {
        e->boot_mask = STIX_EDAC_BOOT_ALL_ON;
    } else {
        ret = qemu_strtoul(str, &end, 0, &val);
        if (ret < 0 || *end != '\0' || val > STIX_EDAC_BOOT_ALL_ON) {
            error_setg(errp, "%s: expected hw-reset, all-on or a 3-bit "
                       "mask (1 = sdram, 2 = cache, 4 = rotb), not %s",
                       name, str);
            g_free(str);
            return;
        }
        e->boot_mask = val;
    }
    g_free(e->boot_str);
    e->boot_str = str;
}

/*
 * Test-only latch forcing (the plan's edac-strobe qtest; M-1 has no
 * upsets, so nothing else moves the counters). Value: bit 31 =
 * DeDetected, 7:0 = SecCount.
 */
static void stix_edac_latch_get(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    StixEdacModule m = GPOINTER_TO_UINT(opaque);
    uint32_t value = ((uint32_t)e->ded[m] << 31) | e->sec[m];

    visit_type_uint32(v, name, &value, errp);
}

static void stix_edac_latch_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    StixEdacModule m = GPOINTER_TO_UINT(opaque);
    uint32_t value;

    if (!qtest_enabled()) {
        error_setg(errp, "%s is only available under qtest", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    e->ded[m] = (value >> 31) & 1u;
    e->sec[m] = value & FTSDCTRL16_EDAC_SECCOUNT_MASK;
}

static void stix_edac_instance_init(Object *obj)
{
    static const char *latch[STIX_EDAC_N_MODULE] = {
        "test-latch-sdram", "test-latch-cache", "test-latch-rotb",
    };
    StixEdac *e = STIX_EDAC(obj);
    StixEdacModule m;

    e->boot_str = g_strdup("hw-reset");
    e->boot_mask = STIX_EDAC_BOOT_HW_RESET;

    object_property_add(obj, "edac-at-boot", "string", &str_type_info,
                        stix_edac_boot_get, stix_edac_boot_set, NULL, NULL);
    object_property_set_description(obj, "edac-at-boot",
        "EDACON hand-over state at every reset: hw-reset (ICD), "
        "all-on, or a 3-bit mask (1 = sdram, 2 = cache, 4 = rotb)");

    for (m = 0; m < STIX_EDAC_N_MODULE; m++) {
        object_property_add(obj, latch[m], "uint32", &uint32_type_info,
                            stix_edac_latch_get, stix_edac_latch_set, NULL,
                            GUINT_TO_POINTER(m));
        object_property_set_description(obj, latch[m],
            "test-only, under qtest only: force the latch, "
            "bit 31 = DeDetected, 7:0 = SecCount");
    }
}

static void stix_edac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = stix_edac_realize;
    dc->unrealize = stix_edac_unrealize;
}

static const TypeInfo stix_edac_info = {
    .name = TYPE_STIX_EDAC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(StixEdac),
    .instance_init = stix_edac_instance_init,
    .class_init = stix_edac_class_init,
};

static void stix_edac_register_types(void)
{
    type_register_static(&stix_edac_info);
}

type_init(stix_edac_register_types)
