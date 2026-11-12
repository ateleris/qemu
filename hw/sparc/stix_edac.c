/*
 * STIX IDPU EDAC unit (memory-upset plan D-Ua): the SEC counters and
 * DeDetected latches of the three EDAC memories and the edac-at-boot
 * property (D-Uk, M-1), since M-2 the CACHE upset machinery (D-Uc) and
 * since M-3 the SDRAM one: the sparse poison table, 4 KB page overlays
 * over the backing RAMs, the SEU correct-on-read / DE flip + DEDETECTED
 * paths (D-Ue), the SDRAM DE as an AHB error response (MEMTX_ERROR +
 * the AHBSTAT word, D-Ud) and the upset-inject/upset-clear/
 * upsets-active/edac-enable verbs (D-Ug). Since M-4 the ROTBUF upsets
 * ride the same table with no overlay (the ring has no CPU-visible RAM
 * region: stix_edac_rotb_check runs where the CPU reads the 512 B block
 * window). Since M-5 the BRAM flips ride the inject verb but nothing
 * else: no EDAC, so the host bytes flip at inject time — no poison, no
 * counters, no status (D-Uh). Since M-6a the seeded random bursts
 * (random-seed/random-count, D-Ug) ride the same arm path, and the
 * hw-state accessor feeds the stixctrl's edac section (D-PLo).
 *
 * Traces only fire from the QOM verbs and actual intercepted accesses,
 * never at realize, reset or in a getter (D-Ug, D-Ai).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "StixIdpu.h"
#include "stix_edac.h"
#include "trace.h"

/* D-Uk, ICD reset values (STIX-ICD-0026 txt:2119, txt:2202, txt:3249) */
#define STIX_EDAC_BOOT_HW_RESET (1u << STIX_EDAC_CACHE)
#define STIX_EDAC_BOOT_ALL_ON 0x7u
/*
 * M-6a (D-Ug): the burst give-up bound, counted in attempts (three
 * draws each): 1024 attempts are far more than any placeable count
 * (the table holds 256); exhaustion is only reachable when the drawn
 * space is mostly poisoned already.
 */
#define STIX_EDAC_RANDOM_MAX_DRAWS 1024u

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
 * ---- M-2: the upset machinery (D-Uc, D-Ue, D-Ug) ----
 *
 * The flip lives in the poison table, never in the backing bytes: an
 * overlay read fetches the clean word from the backing RAM and XORs the
 * mask. The EDAC word is the 32-bit unit at the poison's address; the
 * overlay forwards 1/2/4-byte accesses and any write touching a byte of
 * a poisoned word clears the whole poison (D-Uc). Zero cost uninjected:
 * no overlay exists until the first poison lands in a 4 KB page, and it
 * is removed when the page empties.
 */

static const char *const stix_edac_mem_name[STIX_EDAC_N_MODULE] = {
    "sdram", "cache", "rotbuf", "bram",
};

/* the live EDACON bit of the module's stored register word (D-Uf) */
static bool stix_edac_gate(StixEdac *e, StixEdacModule m)
{
    if (e->mems[m].edacon_reg == NULL) {
        return false;
    }
    if (m == STIX_EDAC_CACHE) {
        return (*e->mems[m].edacon_reg >>
                STIXCTRL_CACHECMDSTA_EDACON) & 1u;
    }
    if (m == STIX_EDAC_SDRAM) {
        return (*e->mems[m].edacon_reg >> FTSDCTRL16_EDAC_EDACON) & 1u;
    }
    /* ROTBUF: rotbConfig EDACON (bit 12), wired since M-4 */
    return (*e->mems[m].edacon_reg >> STIXCTRL_ROTBCONFIG_EDACON) & 1u;
}

/*
 * CACHE only: the DE status and IRQ additionally need OPMODE = 0 (cache
 * mode, StixIdpu.h:1547; the gate is the IRQ's, not the data's — with
 * OPMODE = 1 the flip is still returned, D-Ue).
 */
static bool stix_edac_cache_de_gate(StixEdac *e)
{
    return stix_edac_gate(e, STIX_EDAC_CACHE) &&
           !(*e->mems[STIX_EDAC_CACHE].edacon_reg &
             (STIXCTRL_CACHECMDSTA_OPMODE_MASK <<
              STIXCTRL_CACHECMDSTA_OPMODE));
}

/* the DE status/IRQ gate of a module: CACHE adds the OPMODE half */
static bool stix_edac_de_gate(StixEdac *e, StixEdacModule m)
{
    return m == STIX_EDAC_CACHE ? stix_edac_cache_de_gate(e)
                                : stix_edac_gate(e, m);
}

/* the module's data path is wired (the register owner's realize ran) */
static bool stix_edac_wired(StixEdac *e, StixEdacModule m)
{
    if (m == STIX_EDAC_ROTBUF) {
        return e->mems[m].ring != NULL;
    }
    return e->mems[m].backing != NULL;
}

struct StixEdacOverlay {
    MemoryRegion mr;
    StixEdac *e;
    StixEdacModule m;
    hwaddr page;                /* guest base of the covered 4 KB page */
    StixEdacOverlay *next;
};

/* the clean word from the backing RAM (big-endian, as the banks are) */
static uint32_t stix_edac_backing_word(StixEdacMem *mem, hwaddr wa)
{
    const uint8_t *p = memory_region_get_ram_ptr(mem->backing) +
                       (wa - mem->base);

    return ldl_be_p(p);
}

/* read the clean word and write it back (the EDAC write-back, D-Uh) */
static void stix_edac_backing_fix(StixEdacMem *mem, hwaddr wa, uint32_t w)
{
    stl_be_p((uint8_t *)memory_region_get_ram_ptr(mem->backing) +
             (wa - mem->base), w);
    memory_region_set_dirty(mem->backing, wa - mem->base,
                            STIX_EDAC_WORD_SIZE);
}

static StixEdacOverlay *stix_edac_overlay_find(StixEdacMem *mem, hwaddr page)
{
    StixEdacOverlay *ov;

    for (ov = mem->overlays; ov != NULL; ov = ov->next) {
        if (ov->page == page) {
            return ov;
        }
    }
    return NULL;
}

/* remove the overlay once its page holds no armed poison (D-Uc) */
static void stix_edac_overlay_trim(StixEdac *e, StixEdacModule m, hwaddr page)
{
    StixEdacMem *mem = &e->mems[m];
    StixEdacOverlay *ov = stix_edac_overlay_find(mem, page);
    StixEdacOverlay **link;
    StixEdacPoison *p;
    uint32_t i;

    if (ov == NULL) {
        return;
    }
    for (i = 0; i < STIX_EDAC_N_POISON; i++) {
        p = &e->poison[i];
        if (p->armed && p->mem == m &&
            (p->addr & ~(hwaddr)(STIX_EDAC_PAGE_SIZE - 1)) == page) {
            return;
        }
    }
    for (link = &mem->overlays; *link != NULL; link = &(*link)->next) {
        if (*link == ov) {
            *link = ov->next;
            memory_region_del_subregion(mem->backing, &ov->mr);
            /*
             * parked, not freed: the dispatch running this very handler
             * may still touch the MR (an SEU retire happens on its own
             * read); the next add reuses the struct
             */
            ov->next = mem->spares;
            mem->spares = ov;
            return;
        }
    }
}

static StixEdacPoison *stix_edac_poison_get(StixEdac *e, StixEdacModule m,
                                            hwaddr wa)
{
    StixEdacPoison *p;
    uint32_t i;

    for (i = 0; i < STIX_EDAC_N_POISON; i++) {
        p = &e->poison[i];
        if (p->armed && p->mem == m && p->addr == wa) {
            return p;
        }
    }
    return NULL;
}

/* retire one poison; the overlay goes when its page empties */
static void stix_edac_poison_retire(StixEdac *e, StixEdacPoison *p)
{
    hwaddr page = p->addr & ~(hwaddr)(STIX_EDAC_PAGE_SIZE - 1);

    p->armed = false;
    e->n_armed--;
    stix_edac_overlay_trim(e, p->mem, page);
}

/*
 * retire every poison whose word intersects [off, off + len): a write
 * touching any byte of the word cures the whole cell (D-Uc)
 */
static void stix_edac_poison_drop(StixEdac *e, StixEdacModule m, hwaddr off,
                                  hwaddr len, const char *kind)
{
    StixEdacPoison *p;
    uint32_t i;

    if (e == NULL || e->n_armed == 0) {
        return;
    }
    for (i = 0; i < STIX_EDAC_N_POISON; i++) {
        p = &e->poison[i];
        if (p->armed && p->mem == m &&
            p->addr < off + len && off < p->addr + STIX_EDAC_WORD_SIZE) {
            trace_stix_edac_effect(stix_edac_mem_name[m], p->addr, kind);
            stix_edac_poison_retire(e, p);
        }
    }
}

/*
 * M-3 (D-Ud): with attrs, so a read can fail the bus transaction. The
 * SDRAM DE with EDACON = 1 latches as before, records the failing
 * address in the AHBSTAT word and returns MEMTX_ERROR — the AHB error
 * response the LEON3 CPU turns into the trap 0x09
 * (sparc_cpu_do_transaction_failed); the FSW's trap handler then reads
 * AHBSTAT 0x80000F00 (WDT.c:117-123). With EDACON = 0 the flip is
 * returned raw and MEMTX_OK (D-Uf). CACHE is unchanged: its DE is the
 * IRQ 6 pulse, never a bus error. The data under an error response is
 * the flipped word (undefined on real hardware; nothing observes it on
 * the trap path, qtest does).
 */
static MemTxResult stix_edac_overlay_read(void *opaque, hwaddr addr,
                                          uint64_t *pdata, unsigned size,
                                          MemTxAttrs attrs)
{
    StixEdacOverlay *ov = opaque;
    StixEdac *e = ov->e;
    StixEdacModule m = ov->m;
    StixEdacMem *mem = &e->mems[m];
    hwaddr ga = ov->page + addr;
    hwaddr wa = ga & ~(hwaddr)(STIX_EDAC_WORD_SIZE - 1);
    StixEdacPoison *p = stix_edac_poison_get(e, m, wa);
    uint32_t word, out = 0;
    MemTxResult ret = MEMTX_OK;
    unsigned k;

    if (p == NULL) {
        *pdata = ldn_be_p(memory_region_get_ram_ptr(mem->backing) +
                          (ga - mem->base), size);
        return MEMTX_OK;
    }

    word = stix_edac_backing_word(mem, wa);
    if (p->de) {
        if (stix_edac_de_gate(e, ov->m)) {
            word ^= p->mask;
            e->ded[m] = true;
            e->mems[m].detected++;
            trace_stix_edac_effect(stix_edac_mem_name[m], wa, "ded");
            if (m == STIX_EDAC_SDRAM) {
                e->ahbstat = (uint32_t)wa;
                ret = MEMTX_ERROR;
            } else if (mem->irq != NULL) {
                qemu_irq_pulse(*mem->irq);
            }
            if (!p->persist) {
                stix_edac_poison_retire(e, p);
            }
        } else {
            /* EDACON = 0 or OPMODE = 1: the flip, raw, stays armed */
            word ^= p->mask;
            trace_stix_edac_effect(stix_edac_mem_name[m], wa, "raw");
        }
    } else {
        if (stix_edac_gate(e, m)) {
            if (e->sec[m] < 0xFF) {
                e->sec[m]++;            /* 8-bit fields saturate (D-Ue) */
            }
            e->mems[m].corrected++;
            trace_stix_edac_effect(stix_edac_mem_name[m], wa, "sec");
            stix_edac_backing_fix(mem, wa, word);
            if (!p->persist) {
                stix_edac_poison_retire(e, p);
            }
        } else {
            word ^= p->mask;            /* EDACON = 0: raw, stays armed */
            trace_stix_edac_effect(stix_edac_mem_name[m], wa, "raw");
        }
    }

    /* slice the presented word big-endian, as the dispatch expects */
    k = ga - wa;
    while (size-- > 0) {
        out = (out << 8) | ((word >> (24 - 8 * k)) & 0xffu);
        k = (k + 1) & 3u;
    }
    *pdata = out;
    return ret;
}

static MemTxResult stix_edac_overlay_write(void *opaque, hwaddr addr,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    StixEdacOverlay *ov = opaque;
    StixEdacMem *mem = &ov->e->mems[ov->m];
    hwaddr ga = ov->page + addr;
    hwaddr wa = ga & ~(hwaddr)(STIX_EDAC_WORD_SIZE - 1);
    StixEdacPoison *p = stix_edac_poison_get(ov->e, ov->m, wa);

    /*
     * A sub-word store is a read-modify-write of the EDAC word: the
     * bytes it does not touch are written back as the read half saw
     * them. An SEU with EDACON = 1 is corrected by that read (the word
     * stays clean, no SEC counted: the FSW never observes an RMW); with
     * EDACON = 0, or for a DE, the flipped bits are re-encoded with
     * fresh check bits and become the cell's content. A full-word store
     * overwrites everything, so the fold is moot there.
     */
    if (p != NULL && size < STIX_EDAC_WORD_SIZE &&
        (p->de || !stix_edac_gate(ov->e, ov->m))) {
        stix_edac_backing_fix(mem, wa,
                              stix_edac_backing_word(mem, wa) ^ p->mask);
    }
    stn_be_p((uint8_t *)memory_region_get_ram_ptr(mem->backing) +
             (ga - mem->base), size, value);
    memory_region_set_dirty(mem->backing, ga - mem->base, size);
    stix_edac_poison_drop(ov->e, ov->m, ga, size, "write-clear");
    return MEMTX_OK;
}

static const MemoryRegionOps stix_edac_overlay_ops = {
    .read_with_attrs = stix_edac_overlay_read,
    .write_with_attrs = stix_edac_overlay_write,
    .endianness = DEVICE_BIG_ENDIAN,
    /*
     * valid up to 8: an aligned SPARC ldd/std (and lddf/stdf) is one
     * 8-byte io access; memory.c splits it into two 4-byte calls of
     * the handlers above (impl max 4), so both words see their poisons
     * and a MEMTX_ERROR from either half still traps
     */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* the priority-1 page overlay over the backing RAM (D-Uc) */
static void stix_edac_overlay_add(StixEdac *e, StixEdacModule m, hwaddr page)
{
    StixEdacMem *mem = &e->mems[m];
    StixEdacOverlay *ov = mem->spares;

    /*
     * a parked spare keeps its initialized MR (ops and opaque are the
     * struct itself): only the page changes, so the MR is initialized
     * exactly once per struct
     */
    if (ov == NULL) {
        ov = g_new0(StixEdacOverlay, 1);
        memory_region_init_io(&ov->mr, OBJECT(e), &stix_edac_overlay_ops,
                              ov, "stix-edac-overlay", STIX_EDAC_PAGE_SIZE);
    } else {
        mem->spares = ov->next;
    }
    ov->e = e;
    ov->m = m;
    ov->page = page;
    ov->next = mem->overlays;
    /*
     * the parent is a RAM leaf, not a container: render_memory_region
     * (system/memory.c) renders a region's subregions before its own
     * RAM, so the priority-1 overlay wins for its page
     */
    memory_region_add_subregion_overlap(mem->backing, page - mem->base,
                                        &ov->mr, 1);
    mem->overlays = ov;
}

/* volatile: stixctrl's register block is declared volatile (StixIdpu.h) */
void stix_edac_cache_link(StixEdac *e, volatile uint32_t *cache_cmdsta,
                          MemoryRegion *sram, qemu_irq *edac_irq)
{
    StixEdacMem *mem = &e->mems[STIX_EDAC_CACHE];

    mem->backing = sram;
    mem->base = STIXCTRL_SRAM_START;
    mem->edacon_reg = cache_cmdsta;
    mem->irq = edac_irq;
}

/* volatile: the SDRAM controller's register block is declared volatile */
void stix_edac_sdram_link(StixEdac *e, volatile uint32_t *sdram_edac,
                          MemoryRegion *ram)
{
    StixEdacMem *mem = &e->mems[STIX_EDAC_SDRAM];

    mem->backing = ram;
    /* the machine RAM maps at 0x40000000 (leon3.c LEON3_RAM_OFFSET) */
    mem->base = SDRAM_AHB_BASE;
    mem->edacon_reg = sdram_edac;
    /* no IRQ: the SDRAM DE is the AHB error/trap path (D-Ud) */
}

/*
 * M-4: the ROTBUF link. The poisons are keyed by ring byte offsets
 * (block * STIXCTRL_BLOCK_SIZE + offset), so base stays 0 and there is
 * no backing MemoryRegion and no overlay — the clean word comes from the
 * ring when stix_edac_rotb_check needs it. The DE pulses the same sysbus
 * slot as the CACHE DE: IRQ_STIXCTRL_EDAC = 6 is shared (the StixIdpu.h
 * IRQ table; the FSW's one ISR checks CACHE then ROTBUF on it,
 * EDAC.c:51-87).
 */
void stix_edac_rotb_link(StixEdac *e, volatile uint32_t *rotb_config,
                         uint8_t *ring, uint32_t blocks, qemu_irq *edac_irq)
{
    StixEdacMem *mem = &e->mems[STIX_EDAC_ROTBUF];

    mem->base = 0;
    mem->edacon_reg = rotb_config;
    mem->ring = ring;
    mem->ring_bytes = blocks * STIXCTRL_BLOCK_SIZE;
    mem->irq = edac_irq;
}

/*
 * M-5: the BRAM link. Only backing/base: no EDAC, so there is no gate
 * register and no IRQ — the upset flips the host bytes at inject time
 * and nothing is armed or reported. The lvar window covers LVAR0-3 plus
 * the BRAM areas behind them: the WDT UNCHANGED_STORAGE (0xE02000A8, the
 * record WDT_callAfterReset reads at every boot, WDT.c:239-263) and the
 * MemBram record (0xE02003C0, SuSW-only, no ASW reader).
 */
void stix_edac_bram_link(StixEdac *e, MemoryRegion *lvar)
{
    StixEdacMem *mem = &e->mems[STIX_EDAC_BRAM];

    mem->backing = lvar;
    mem->base = STIXCTRL_LVAR0_START;
    /* no edacon_reg, no irq: a flip is silent (the BRAM has no EDAC) */
}

/*
 * M-4 (D-Uc/D-Ue): the reactions for the CPU's read through the 512 B
 * block window — the only path the CPU sees the ring through. The
 * semantics are the overlay read's, minus the overlay: the flip lives in
 * the poison table, the clean word comes from the ring, SEU with
 * EDACON = 1 counts and self-retires (nothing to write back: the ring
 * already holds the corrected word), DE latches DEDETECTED, pulses IRQ 6
 * and returns the flip — ROTBUF never errors the bus (no trap path, the
 * plan's matrix). EDACON = 0 leaves the poison armed ("raw").
 */
void stix_edac_rotb_check(StixEdac *e, uint32_t block, uint32_t off,
                          unsigned size, uint64_t *val)
{
    StixEdacMem *mem;
    StixEdacPoison *p;
    hwaddr wa;
    uint32_t word, out = 0;
    unsigned k;

    /* zero cost uninjected: one branch on the armed count */
    if (e == NULL || e->n_armed == 0 ||
        e->mems[STIX_EDAC_ROTBUF].ring == NULL) {
        return;
    }
    mem = &e->mems[STIX_EDAC_ROTBUF];
    wa = (hwaddr)block * STIXCTRL_BLOCK_SIZE +
         (off & ~(hwaddr)(STIX_EDAC_WORD_SIZE - 1));
    p = stix_edac_poison_get(e, STIX_EDAC_ROTBUF, wa);
    if (p == NULL) {
        return;
    }

    word = ldl_be_p(mem->ring + wa);
    if (p->de) {
        if (stix_edac_de_gate(e, STIX_EDAC_ROTBUF)) {
            word ^= p->mask;
            e->ded[STIX_EDAC_ROTBUF] = true;
            mem->detected++;
            trace_stix_edac_effect(stix_edac_mem_name[STIX_EDAC_ROTBUF],
                                   wa, "ded");
            if (mem->irq != NULL) {
                qemu_irq_pulse(*mem->irq);
            }
            if (!p->persist) {
                stix_edac_poison_retire(e, p);
            }
        } else {
            /* EDACON = 0: the flip, raw, stays armed */
            word ^= p->mask;
            trace_stix_edac_effect(stix_edac_mem_name[STIX_EDAC_ROTBUF],
                                   wa, "raw");
        }
    } else {
        if (stix_edac_gate(e, STIX_EDAC_ROTBUF)) {
            if (e->sec[STIX_EDAC_ROTBUF] < 0xFF) {
                e->sec[STIX_EDAC_ROTBUF]++;  /* 8-bit fields saturate */
            }
            mem->corrected++;
            trace_stix_edac_effect(stix_edac_mem_name[STIX_EDAC_ROTBUF],
                                   wa, "sec");
            if (!p->persist) {
                stix_edac_poison_retire(e, p);
            }
        } else {
            word ^= p->mask;            /* EDACON = 0: raw, stays armed */
            trace_stix_edac_effect(stix_edac_mem_name[STIX_EDAC_ROTBUF],
                                   wa, "raw");
        }
    }

    /* slice the presented word big-endian, as the window reads are */
    k = off & (STIX_EDAC_WORD_SIZE - 1);
    while (size-- > 0) {
        out = (out << 8) | ((word >> (24 - 8 * k)) & 0xffu);
        k = (k + 1) & 3u;
    }
    *val = out;
}

/* M-4 (D-Uc): the window's TESTMODE store cures the words it touches */
void stix_edac_rotb_write(StixEdac *e, uint32_t block, uint32_t off,
                          unsigned size)
{
    if (e == NULL || e->n_armed == 0 ||
        e->mems[STIX_EDAC_ROTBUF].ring == NULL) {
        return;
    }
    stix_edac_poison_drop(e, STIX_EDAC_ROTBUF,
                          (hwaddr)block * STIXCTRL_BLOCK_SIZE + off, size,
                          "write-clear");
}

MemoryRegion *stix_edac_ahbstat_mr(StixEdac *e)
{
    return &e->ahbstat_mr;
}

/*
 * D-Ud: the AHBSTAT failing-address word at 0x80000F00 — the one 32-bit
 * read the FSW's trap handler does (WDT.c:117-123, per GRLIB AN-0004;
 * the full layout stays [I]). The latch is written by the SDRAM DE
 * overlay read only; writes drop (the FSW never writes it).
 */
static uint64_t stix_edac_ahbstat_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    StixEdac *e = opaque;

    return e->ahbstat;
}

static void stix_edac_ahbstat_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
}

static const MemoryRegionOps stix_edac_ahbstat_ops = {
    .read = stix_edac_ahbstat_read,
    .write = stix_edac_ahbstat_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void stix_edac_host_write(StixEdac *e, StixEdacModule m, hwaddr off,
                          hwaddr len)
{
    if (e == NULL || !stix_edac_wired(e, m)) {
        return;
    }
    /*
     * the caller speaks backing offsets (ring byte offsets for ROTBUF);
     * the poisons carry addresses
     */
    stix_edac_poison_drop(e, m, e->mems[m].base + off, len, "host-clear");
}

/*
 * M-6a (D-Ug): the edac section of the stixctrl hw-state (D-PLo) — the
 * same JSON as upsets-active, through this one accessor. Pure: no state
 * is touched, so two hw-state reads with no accesses in between are
 * equal, and nothing traces (a getter never traces, D-Ai).
 */
void stix_edac_hw_state(StixEdac *e, QDict *root)
{
    QDict *counters = qdict_new();
    QList *poisons = qlist_new();
    StixEdacPoison *p;
    uint32_t i, id = 0;
    StixEdacModule m;

    if (e != NULL) {
        for (i = 0; i < STIX_EDAC_N_POISON; i++) {
            p = &e->poison[i];
            if (!p->armed) {
                continue;
            }
            QDict *rec = qdict_new();

            qdict_put_int(rec, "id", id++);
            qdict_put_str(rec, "memory", stix_edac_mem_name[p->mem]);
            qdict_put_int(rec, "address", p->addr);
            if (p->mem == STIX_EDAC_ROTBUF) {
                /* the rotbuf address in its grammar's terms (M-4) */
                qdict_put_int(rec, "block", p->addr / STIXCTRL_BLOCK_SIZE);
                qdict_put_int(rec, "offset", p->addr % STIXCTRL_BLOCK_SIZE);
            }
            qdict_put_int(rec, "mask", p->mask);
            qdict_put_str(rec, "mode", p->de ? "de" : "seu");
            qdict_put_bool(rec, "persist", p->persist);
            qlist_append(poisons, rec);
        }

        for (m = 0; m < STIX_EDAC_N_MODULE; m++) {
            QDict *c;

            /* the BRAM has no EDAC and no counters (M-5) */
            if (m == STIX_EDAC_BRAM) {
                continue;
            }
            c = qdict_new();
            qdict_put_int(c, "injected", e->mems[m].injected);
            qdict_put_int(c, "corrected", e->mems[m].corrected);
            qdict_put_int(c, "detected", e->mems[m].detected);
            qdict_put(counters, stix_edac_mem_name[m], c);
        }
    }
    qdict_put(root, "poisons", poisons);
    qdict_put(root, "counters", counters);
}

/*
 * upsets-active (D-Ug): the armed poisons as records plus the per-memory
 * injected/corrected/detected counters.
 */
static char *stix_edac_upsets_json(StixEdac *e)
{
    g_autoptr(QDict) root = qdict_new();
    GString *json;

    stix_edac_hw_state(e, root);
    json = qobject_to_json(QOBJECT(root));
    return g_string_free(json, false);
}

/*
 * M-5 (D-Uh): the BRAM flip — the one memory with no EDAC and no read
 * interceptor, so the upset is applied to the host bytes at inject time
 * (the stix_det pattern: ram_ptr + set_dirty). The mask XORs the four
 * bytes at the address, big-endian, so an inl at the address reads the
 * original XORed with the mask. Nothing is armed: no poison record, no
 * counters, no status, no IRQ, and no effect trace (nothing is ever
 * intercepted). The window survives a system reset (grlib_stixctrl_reset
 * leaves the AHB RAM alone); a fresh QEMU zeroes it at power-on.
 */
static void stix_edac_bram_flip(StixEdac *e, hwaddr addr, uint32_t mask)
{
    StixEdacMem *mem = &e->mems[STIX_EDAC_BRAM];
    uint8_t *p = memory_region_get_ram_ptr(mem->backing) +
                 (addr - mem->base);
    unsigned i;

    for (i = 0; i < STIX_EDAC_WORD_SIZE; i++) {
        p[i] ^= (mask >> (24 - 8 * i)) & 0xFFu;
    }
    memory_region_set_dirty(mem->backing, addr - mem->base,
                            STIX_EDAC_WORD_SIZE);
    trace_stix_edac_inject(stix_edac_mem_name[STIX_EDAC_BRAM],
                           (uint32_t)addr, mask, "flip");
}

/*
 * Arm one poison — the shared tail of upset-inject and random-count, so
 * a drawn upset rides the exact inject path (D-Ug, no second
 * implementation). A fresh event on an already poisoned word replaces
 * its record (mask/mode/persist, counted as a fresh injected, allowed
 * with a full table); otherwise the first free slot.
 */
static bool stix_edac_poison_arm(StixEdac *e, const char *name,
                                 StixEdacModule m, hwaddr addr,
                                 uint32_t mask, bool de, bool persist,
                                 Error **errp)
{
    uint32_t slot = STIX_EDAC_N_POISON;
    bool replace = false;
    uint32_t i;

    for (i = 0; i < STIX_EDAC_N_POISON; i++) {
        StixEdacPoison *p = &e->poison[i];

        if (p->armed && p->mem == m && p->addr == addr) {
            slot = i;
            replace = true;
            break;
        }
        if (slot == STIX_EDAC_N_POISON && !p->armed) {
            slot = i;
        }
    }
    if (slot == STIX_EDAC_N_POISON) {
        error_setg(errp, "%s: poison table full (%u upsets)", name,
                   STIX_EDAC_N_POISON);
        return false;
    }
    e->poison[slot] = (StixEdacPoison) {
        .mem = m,
        .addr = addr,
        .mask = mask,
        .de = de,
        .persist = persist,
        .armed = true,
    };
    if (e->mems[m].backing != NULL &&
        !stix_edac_overlay_find(&e->mems[m],
                                addr & ~(hwaddr)(STIX_EDAC_PAGE_SIZE - 1))) {
        stix_edac_overlay_add(e, m,
                              addr & ~(hwaddr)(STIX_EDAC_PAGE_SIZE - 1));
    }
    if (!replace) {
        e->n_armed++;
    }
    e->mems[m].injected++;
    trace_stix_edac_inject(stix_edac_mem_name[m], (uint32_t)addr, mask,
                           de ? "de" : "seu");
    return true;
}

/*
 * upset-inject (D-Ug): "memory,address,bitmask[,mode][,persist]" with
 * memory in {sdram,cache,rotbuf,bram}. rotbuf (M-4) takes
 * "<block>:<offset>" as its address — the block index into the ring
 * (bounds from the ring size) and the byte offset 0..511 in the block,
 * 4-aligned; the poison carries the ring byte offset
 * block * STIXCTRL_BLOCK_SIZE + offset. bram (M-5) takes an absolute
 * address in the BRAM window (STIXCTRL_LVAR0_START+0x400, any alignment
 * — no EDAC word); its mask XORs the four bytes at the address,
 * big-endian, and the flip lands in the host bytes at once: no poison
 * record, no counters, no status, no mode and no persist (silent, D-Uh
 * — it survives a system reset and dies at power cycle). Unsupported or
 * unwired memories are rejected so a caller is never left with a
 * silently unarmed upset. A rejected set changes nothing. The arming
 * itself is stix_edac_poison_arm, shared with random-count (M-6a).
 */
static void stix_edac_inject_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    StixEdacModule m = STIX_EDAC_N_MODULE;
    char *str, **fields;
    uint64_t addr, mask;
    bool de, persist = false;
    uint32_t i;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    fields = g_strsplit(str, ",", 6);
    for (i = 0; fields[i] != NULL; i++) {
    }
    if (i < 3 || i > 5) {
        error_setg(errp, "%s: expected \"memory,address,bitmask"
                   "[,mode][,persist]\", not \"%s\"", name, str);
        goto out;
    }
    if (strcmp(fields[0], "cache") == 0) {
        m = STIX_EDAC_CACHE;
    } else if (strcmp(fields[0], "sdram") == 0) {
        m = STIX_EDAC_SDRAM;
    } else if (strcmp(fields[0], "rotbuf") == 0) {
        m = STIX_EDAC_ROTBUF;   /* "<block>:<offset>", parsed below */
    } else if (strcmp(fields[0], "bram") == 0) {
        m = STIX_EDAC_BRAM;     /* absolute address, parsed below */
    } else {
        error_setg(errp, "%s: unknown memory %s (expected sdram, cache, "
                   "rotbuf or bram)", name, fields[0]);
        goto out;
    }
    if (!stix_edac_wired(e, m)) {
        error_setg(errp, "%s: memory %s is not wired in this machine",
                   name, fields[0]);
        goto out;
    }
    if (m == STIX_EDAC_ROTBUF) {
        /*
         * M-4 grammar: "<block>:<offset>", both numbers in the strtoull
         * base-0 syntax. Every rejection names the field; the bounds
         * come from the ring size (the rotb-blocks property).
         */
        const char *colon, *end;
        uint64_t block, offset;

        if (qemu_strtou64(fields[1], &colon, 0, &block) < 0 ||
            *colon != ':') {
            error_setg(errp, "%s: rotbuf takes \"<block>:<offset>\", "
                       "not \"%s\"", name, fields[1]);
            goto out;
        }
        if (qemu_strtou64(colon + 1, &end, 0, &offset) < 0 ||
            *end != '\0') {
            error_setg(errp, "%s: bad rotbuf offset in \"%s\"",
                       name, fields[1]);
            goto out;
        }
        if (block >= e->mems[m].ring_bytes / STIXCTRL_BLOCK_SIZE) {
            error_setg(errp, "%s: rotbuf block %" PRIu64 " is out of "
                       "range (the ring holds %u blocks)", name, block,
                       e->mems[m].ring_bytes / STIXCTRL_BLOCK_SIZE);
            goto out;
        }
        if (offset >= STIXCTRL_BLOCK_SIZE) {
            error_setg(errp, "%s: rotbuf offset %" PRIu64 " is out of "
                       "range (0..%u)", name, offset,
                       STIXCTRL_BLOCK_SIZE - 1);
            goto out;
        }
        if (offset & (STIX_EDAC_WORD_SIZE - 1)) {
            error_setg(errp, "%s: rotbuf offset %" PRIu64 " is not "
                       "%u-aligned", name, offset, STIX_EDAC_WORD_SIZE);
            goto out;
        }
        addr = block * STIXCTRL_BLOCK_SIZE + offset;
    } else {
        if (qemu_strtou64(fields[1], NULL, 0, &addr) < 0) {
            error_setg(errp, "%s: bad address %s", name, fields[1]);
            goto out;
        }
        hwaddr end = e->mems[m].base +
                     memory_region_size(e->mems[m].backing);

        if (m == STIX_EDAC_BRAM) {
            /*
             * no EDAC word, so any alignment goes; the four mask bytes
             * must fit the window (bounds from the region size, the
             * span form avoids the addr + 4 overflow)
             */
            if (addr < e->mems[m].base ||
                addr > end - STIX_EDAC_WORD_SIZE) {
                error_setg(errp, "%s: address 0x%" PRIx64 " (+%u mask "
                           "bytes) is outside %s (0x%" HWADDR_PRIx
                           "..0x%" HWADDR_PRIx ")", name, addr,
                           STIX_EDAC_WORD_SIZE, stix_edac_mem_name[m],
                           e->mems[m].base, end);
                goto out;
            }
        } else {
            if (addr & (STIX_EDAC_WORD_SIZE - 1)) {
                error_setg(errp, "%s: address 0x%" PRIx64 " is not %u-aligned",
                           name, addr, STIX_EDAC_WORD_SIZE);
                goto out;
            }
            if (addr < e->mems[m].base || addr >= end) {
                error_setg(errp, "%s: address 0x%" PRIx64 " is outside %s "
                           "(0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx ")",
                           name, addr, stix_edac_mem_name[m],
                           e->mems[m].base, end);
                goto out;
            }
        }
    }
    if (qemu_strtou64(fields[2], NULL, 0, &mask) < 0 || mask == 0 ||
        mask > 0xFFFFFFFFull) {
        error_setg(errp, "%s: bitmask %s must be nonzero and fit the "
                   "32-bit word", name, fields[2]);
        goto out;
    }
    de = ctpop32(mask) != 1;
    if (i >= 4) {
        if (m == STIX_EDAC_BRAM) {
            /* no EDAC, so neither mode nor persist applies (M-5) */
            error_setg(errp, "%s: bram takes no mode (no EDAC: the flip "
                       "is silent and lives until a power cycle)", name);
            goto out;
        }
        if (strcmp(fields[3], "seu") == 0) {
            de = false;
        } else if (strcmp(fields[3], "de") == 0) {
            de = true;
        } else {
            error_setg(errp, "%s: bad mode %s (expected seu or de)",
                       name, fields[3]);
            goto out;
        }
    }
    if (i == 5) {
        if (strcmp(fields[4], "0") == 0) {
            persist = false;
        } else if (strcmp(fields[4], "1") == 0) {
            persist = true;
        } else {
            error_setg(errp, "%s: bad persist %s (expected 0 or 1)",
                       name, fields[4]);
            goto out;
        }
    }
    if (m == STIX_EDAC_BRAM) {
        /* the flip is the whole upset: applied now, nothing is armed */
        stix_edac_bram_flip(e, (hwaddr)addr, (uint32_t)mask);
        goto out;
    }
    if (!stix_edac_poison_arm(e, name, m, addr, (uint32_t)mask, de, persist,
                              errp)) {
        goto out;
    }

out:
    g_strfreev(fields);
    g_free(str);
}

/*
 * upset-clear (D-Ug): "all", or the index of a record in upsets-active.
 * A bad index is refused and changes nothing.
 */
static void stix_edac_clear_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    char *str;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    if (strcmp(str, "all") == 0) {
        uint32_t i;

        for (i = 0; i < STIX_EDAC_N_POISON; i++) {
            StixEdacPoison *p = &e->poison[i];

            if (p->armed) {
                trace_stix_edac_clear(stix_edac_mem_name[p->mem], p->addr);
                stix_edac_poison_retire(e, p);
            }
        }
    } else {
        uint64_t id;
        uint32_t i, seen = 0;

        if (qemu_strtou64(str, NULL, 0, &id) < 0) {
            error_setg(errp, "%s: expected \"all\" or an index into "
                       "upsets-active, not \"%s\"", name, str);
            goto out;
        }
        for (i = 0; i < STIX_EDAC_N_POISON; i++) {
            StixEdacPoison *p = &e->poison[i];

            if (!p->armed) {
                continue;
            }
            if (seen == id) {
                trace_stix_edac_clear(stix_edac_mem_name[p->mem], p->addr);
                stix_edac_poison_retire(e, p);
                goto out;
            }
            seen++;
        }
        error_setg(errp, "%s: no upset %" PRIu64 " armed (upsets-active holds "
                   "%u)", name, id, e->n_armed);
    }
out:
    g_free(str);
}

static void stix_edac_upsets_get(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    char *str = stix_edac_upsets_json(e);

    visit_type_str(v, name, &str, errp);
    g_free(str);
}

/*
 * M-6a (D-Ug): the seeded random bursts. The draw is xorshift32,
 * Marsaglia's 13/17/5 triple
 *     x ^= x << 13; x ^= x >> 17; x ^= x << 5;
 * and the state restarts from random-seed at every random-count set, so
 * a given seed always draws the same sequence (the qtest edac-random
 * pins it; no host randomness, no timer — D-Ai). Seed 0 is off (D-Ug).
 *
 * Each upset costs three draws, in this order: the memory (uniform
 * among the wired EDAC memories, in enum order sdram/cache/rotbuf —
 * never the BRAM: it has no EDAC and no record to observe, D-Uh), the
 * 4-aligned word (uniform in the memory's extent: the backing region's
 * size, the ring bytes for ROTBUF), the bit (uniform 0..31, so every
 * drawn upset is a one-bit SEU; the model stores only the 32-bit data
 * word, the EDAC check bits are implicit in the correction machinery
 * and cannot be drawn). A word already poisoned is skipped (D-Ug) and
 * the attempt's three draws are spent. The burst places the upsets
 * through stix_edac_poison_arm — the exact inject path.
 */
static uint32_t stix_edac_rand(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void stix_edac_random_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    static const StixEdacModule order[] = {
        STIX_EDAC_SDRAM, STIX_EDAC_CACHE, STIX_EDAC_ROTBUF,
    };
    StixEdac *e = STIX_EDAC(obj);
    StixEdacModule wired[ARRAY_SIZE(order)];
    uint32_t count, placed = 0, draws = 0, state;
    unsigned n_wired = 0, w;

    if (!visit_type_uint32(v, name, &count, errp)) {
        return;
    }
    if (count == 0) {
        error_setg(errp, "%s: 0 injects nothing", name);
        return;
    }
    if (e->random_seed == 0) {
        error_setg(errp, "%s: random-seed is 0 (off): set a nonzero "
                   "seed first", name);
        return;
    }
    if (count > STIX_EDAC_N_POISON - e->n_armed) {
        error_setg(errp, "%s: %u upsets do not fit the poison table "
                   "(%u free of %u)", name, count,
                   STIX_EDAC_N_POISON - e->n_armed, STIX_EDAC_N_POISON);
        return;
    }
    for (w = 0; w < ARRAY_SIZE(order); w++) {
        if (stix_edac_wired(e, order[w])) {
            wired[n_wired++] = order[w];
        }
    }
    if (n_wired == 0) {
        error_setg(errp, "%s: no EDAC memory is wired in this machine",
                   name);
        return;
    }
    state = e->random_seed;
    while (placed < count && draws < STIX_EDAC_RANDOM_MAX_DRAWS) {
        StixEdacModule m = wired[stix_edac_rand(&state) % n_wired];
        StixEdacMem *mem = &e->mems[m];
        uint64_t size = m == STIX_EDAC_ROTBUF ? mem->ring_bytes
                        : memory_region_size(mem->backing);
        hwaddr wa = mem->base +
            (hwaddr)(stix_edac_rand(&state) %
                     (uint32_t)(size / STIX_EDAC_WORD_SIZE)) *
            STIX_EDAC_WORD_SIZE;
        uint32_t mask = 1u << (stix_edac_rand(&state) % 32);

        draws++;
        if (stix_edac_poison_get(e, m, wa) != NULL) {
            continue;               /* already poisoned: skipped (D-Ug) */
        }
        /* the capacity was checked up front; a full table cannot happen */
        if (!stix_edac_poison_arm(e, name, m, wa, mask, false, false, errp)) {
            return;
        }
        placed++;
    }
    if (placed < count) {
        error_setg(errp, "%s: placed %u of %u upsets in %u attempts "
                   "(three draws each, all poisoned); upset-clear or "
                   "reseed first", name, placed, count, draws);
    }
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

    /*
     * M-3 (D-Ud): the AHBSTAT window. The unit is not on a bus, so
     * leon3.c maps the region (at 0x80000F00, StixIdpu.h
     * SDRAM_AHBSTAT_BASE) through stix_edac_ahbstat_mr().
     */
    memory_region_init_io(&e->ahbstat_mr, OBJECT(e), &stix_edac_ahbstat_ops,
                          e, "stix-edac-ahbstat", STIX_EDAC_WORD_SIZE);

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
 * edac-enable (D-Ud, the 2026-11-12 user decision): the runtime EDACON
 * set the ASW never does — a platform fault verb (not qtest-only, like
 * edac-at-boot), so a campaign can arm the SDRAM DE -> trap path
 * mid-run without rebuilding the image. Value: "all-on" or a 3-bit mask
 * (1 = sdram, 2 = cache, 4 = rotb), like edac-at-boot. It ORs the
 * EdacOn bit of exactly the given modules into the owners' stored
 * register words — as much as a software write could set — and never
 * clears a bit: a module left out keeps its current EDACON state (the
 * off path is the FSW's own TC(236) write or a reset). Since M-4 all
 * three modules are wired in the leon3 machine, so "all-on" works (M-3
 * refused it while rotbuf was unwired); a module whose register owner
 * is not wired is still refused and nothing changes (a caller is never
 * left with a silently missed module). There is deliberately no "trap
 * with EDACON = 0" knob: with EDAC off the controller detects nothing,
 * so that would be unphysical.
 */
static void stix_edac_enable_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    StixEdac *e = STIX_EDAC(obj);
    char *str;
    const char *end;
    unsigned long val;
    uint32_t mask, m;
    int ret;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    if (strcmp(str, "all-on") == 0) {
        mask = STIX_EDAC_BOOT_ALL_ON;
    } else {
        ret = qemu_strtoul(str, &end, 0, &val);
        if (ret < 0 || *end != '\0' || val > STIX_EDAC_BOOT_ALL_ON) {
            error_setg(errp, "%s: expected all-on or a 3-bit mask "
                       "(1 = sdram, 2 = cache, 4 = rotb), not %s",
                       name, str);
            g_free(str);
            return;
        }
        mask = val;
    }
    if (mask == 0) {
        error_setg(errp, "%s: an empty mask enables nothing", name);
        g_free(str);
        return;
    }
    for (m = 0; m < STIX_EDAC_N_MODULE; m++) {
        if ((mask & (1u << m)) && e->mems[m].edacon_reg == NULL) {
            error_setg(errp, "%s: memory %s is not wired in this machine",
                       name, stix_edac_mem_name[m]);
            g_free(str);
            return;
        }
    }
    for (m = 0; m < STIX_EDAC_N_MODULE; m++) {
        unsigned bit = m == STIX_EDAC_CACHE ? STIXCTRL_CACHECMDSTA_EDACON
                     : m == STIX_EDAC_ROTBUF ? STIXCTRL_ROTBCONFIG_EDACON
                     : FTSDCTRL16_EDAC_EDACON;

        if (mask & (1u << m)) {
            *e->mems[m].edacon_reg |= 1u << bit;
        }
    }
    trace_stix_edac_enable(mask);
    g_free(str);
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
    /* the three EDAC modules only: the BRAM has no latch (no EDAC, M-5) */
    static const char *const latch[] = {
        "test-latch-sdram", "test-latch-cache", "test-latch-rotb",
    };
    StixEdac *e = STIX_EDAC(obj);
    unsigned li;

    e->boot_str = g_strdup("hw-reset");
    e->boot_mask = STIX_EDAC_BOOT_HW_RESET;

    object_property_add(obj, "edac-at-boot", "string", &str_type_info,
                        stix_edac_boot_get, stix_edac_boot_set, NULL, NULL);
    object_property_set_description(obj, "edac-at-boot",
        "EDACON hand-over state at every reset: hw-reset (ICD), "
        "all-on, or a 3-bit mask (1 = sdram, 2 = cache, 4 = rotb)");

    /*
     * M-3 (D-Ud, the user decision): the runtime EDACON set. Platform
     * fault verb, write-only like the upset verbs (a get is meaningless
     * — the registers themselves are the state).
     */
    object_property_add(obj, "edac-enable", "string", &str_type_info,
                        NULL, stix_edac_enable_set, NULL, NULL);
    object_property_set_description(obj, "edac-enable",
        "set the EdacOn bits at run time, as a software write would: "
        "all-on, or a 3-bit mask (1 = sdram, 2 = cache, 4 = rotb); "
        "only sets, never clears");

    for (li = 0; li < ARRAY_SIZE(latch); li++) {
        object_property_add(obj, latch[li], "uint32", &uint32_type_info,
                            stix_edac_latch_get, stix_edac_latch_set, NULL,
                            GUINT_TO_POINTER(li));
        object_property_set_description(obj, latch[li],
            "test-only, under qtest only: force the latch, "
            "bit 31 = DeDetected, 7:0 = SecCount");
    }

    /*
     * M-2 (D-Ug): the fault verbs on /machine/stixctrl/edac. They are
     * write-only (a get of a verb is meaningless); upsets-active is the
     * read-only face. Platform fault verbs, not qtest-only.
     */
    object_property_add(obj, "upset-inject", "string", &str_type_info,
                        NULL, stix_edac_inject_set, NULL, NULL);
    object_property_set_description(obj, "upset-inject",
        "inject an upset: \"memory,address,bitmask[,mode][,persist]\" "
        "(memory: cache, sdram, rotbuf, bram; rotbuf takes "
        "\"<block>:<offset>\" as the address: the block index into the "
        "ring and the byte offset 0..511 in the block, 4-aligned; bram "
        "takes an absolute address in the BRAM window 0xE0200000+0x400, "
        "any alignment, and the bitmask XORs the 4 bytes at it, "
        "big-endian, applied at once - silent: no mode/persist, no "
        "status, survives reset until power cycle; bitmask bit count 1 = "
        "seu, >=2 = de, mode overrides; persist=1 keeps the poison "
        "after its effect)");
    object_property_add(obj, "upset-clear", "string", &str_type_info,
                        NULL, stix_edac_clear_set, NULL, NULL);
    object_property_set_description(obj, "upset-clear",
        "clear armed upsets: \"all\", or an index into upsets-active");
    object_property_add(obj, "upsets-active", "string", &str_type_info,
                        stix_edac_upsets_get, NULL, NULL, NULL);
    object_property_set_description(obj, "upsets-active",
        "read-only JSON: the armed poisons and the per-memory "
        "injected/corrected/detected counters");

    /*
     * M-6a (D-Ug): the seeded random bursts. The seed is plain state
     * (0 = off, D-Ug) and survives a reset like edac-at-boot; the count
     * is a verb like the upset verbs (write-only: the burst fires on
     * the set and leaves no count to read back).
     */
    object_property_add_uint32_ptr(obj, "random-seed", &e->random_seed,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "random-seed",
        "the seed of the random upset bursts (xorshift32); "
        "0 (the default) = off");
    object_property_add(obj, "random-count", "uint32", &uint32_type_info,
                        NULL, stix_edac_random_set, NULL, NULL);
    object_property_set_description(obj, "random-count",
        "inject count one-bit upsets drawn from random-seed (the "
        "sequence restarts at every set; the qtest edac-random pins "
        "it): per upset one draw picks the memory uniformly among the "
        "wired EDAC memories (sdram, cache, rotbuf - never bram), one "
        "the 4-aligned word uniform in its extent, one the bit; words "
        "already poisoned are skipped");
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
