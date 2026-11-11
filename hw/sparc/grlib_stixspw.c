#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qapi/qapi-types-common.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qnull.h"
#include "system/physmem.h"
#include "system/qtest.h"
#include "StixIdpu.h"
#include "tmtc_io_server.h"

#include "trace.h"
#include "qom/object.h"

#define STIXSPW_REG_SIZE 256 /* Size of memory mapped registers */ // TODO: Set to right size

/* Taken from SPW.c */
#define RXDSC_TR 31       /**< (R) Truncated - Packet was truncated due to maximum length violation */
#define RXDSC_DC 30       /**< (R) Data CRC - 1 if a CRC error was detected for the data. */
#define RXDSC_HC 29       /**< (R) Header CRC - 1 if a CRC error was detected for the header */
#define RXDSC_EP 28       /**< (R) EEP termination - This packet ended with an Error End of Packet character */
#define RXDSC_IE 27       /**< Interrupt enable - If set, an interrupt will be generated when \
                             a packet has been received if the receive interrupt enable bit   \
                             in the DMA channel control register is set. */
#define RXDSC_WR 26       /**< Wrap */
#define RXDSC_EN 25       /**< Enable - Set to one to activate this descriptor. \
                             Cleared by GRSPW on read completed. */
#define RXDSC_PACKETLEN 0 /**< (R) 25 bits field */

#define TXDSC_DC 17       /**< Append data CRC, don't use */
#define TXDSC_HC 16       /**< Append header CRC, don't use */
#define TXDSC_LE 15       /**< (R)A Link error occurred during the transmission of this packet. */
#define TXDSC_IE 14       /**< Interrupt enable - If set, an interrupt will be generated when the packet \
                             has been transmitted and the transmitter interrupt enable bit in            \
                             the DMA control register is set. */
#define TXDSC_WR 13       /**< Wrap */
#define TXDSC_EN 12       /**< Enable transmit descriptor. \
                             The GRSPW clears this bit when the transmission has finished. */
#define TXDSC_NONCRCLEN 8 /**< 4 bits field = 0*/
#define TXDSC_HEADERLEN 0 /**< 8 bits field = 0*/

/*
 * Register write rules (StixIdpu.h bit positions; FSW contract in
 * recon-phase2-fsw-rx §1.2):
 * - status: TO/CE/ER/DE/PE/WE/IA/EE are write-1-to-clear; LS (23:21)
 *   is not writable: it reads RUN, or READY while link-down is set
 *   (P4-5, D-P4e), which is also the only source of DE.
 * - dmaControl: PR/PS/TA/RA are write-1-to-clear; TE/RE/TI/RI/AI/RD/
 *   NS/LE latch as written; RX (bit 10) is the read-only reception
 *   status and AT (bit 9) a self-clearing strobe — the model never
 *   sets them, so writes are ignored and both read 0; bits 15:13 and
 *   31:17 are reserved and read 0, so the FSW's SPW_open clear write
 *   0xFFFE01E0 reads back 0 (SPW.c:531-542).
 */
#define SPW_STATUS_RUN 0xA00000u
/* D-P4e: LS = READY (2), the only value FDIR.c:711 counts as a link error */
#define SPW_STATUS_READY 0x400000u
#define SPW_STATUS_LS_FIELD \
    (STIXSPW_STATUS_LS_MASK << STIXSPW_STATUS_LS)
/* D-P4e: DE and the error IRQ repeat once a second while the link is down */
#define SPW_LINK_ERR_PERIOD_NS 1000000000LL
#define SPW_STATUS_W1C_MASK                                    \
    ((1u << STIXSPW_STATUS_TO) | (1u << STIXSPW_STATUS_CE) |   \
     (1u << STIXSPW_STATUS_ER) | (1u << STIXSPW_STATUS_DE) |   \
     (1u << STIXSPW_STATUS_PE) | (1u << STIXSPW_STATUS_WE) |   \
     (1u << STIXSPW_STATUS_IA) | (1u << STIXSPW_STATUS_EE))
#define SPW_DMACONTROL_W1C_MASK                                \
    ((1u << STIXSPW_DMACONTROL_PS) | (1u << STIXSPW_DMACONTROL_PR) | \
     (1u << STIXSPW_DMACONTROL_TA) | (1u << STIXSPW_DMACONTROL_RA))
#define SPW_DMACONTROL_LATCH_MASK                              \
    ((1u << STIXSPW_DMACONTROL_TE) | (1u << STIXSPW_DMACONTROL_RE) | \
     (1u << STIXSPW_DMACONTROL_TI) | (1u << STIXSPW_DMACONTROL_RI) | \
     (1u << STIXSPW_DMACONTROL_AI) | (1u << STIXSPW_DMACONTROL_RD) | \
     (1u << STIXSPW_DMACONTROL_NS) | (1u << STIXSPW_DMACONTROL_LE))
#define SPW_DMACONTROL_RESERVED_MASK                           \
    ((STIXSPW_DMACONTROL_RESERVED_2_MASK << STIXSPW_DMACONTROL_RESERVED_2) | \
     (STIXSPW_DMACONTROL_RESERVED_1_MASK << STIXSPW_DMACONTROL_RESERVED_1))

/*
 * D-L: the no-spill queue holds up to 64 units, like the FSW's TC
 * queue (configFSW.h:172); a unit that arrives when it is full is
 * dropped and counted in the rx-dropped property.
 */
#define SPW_RX_QUEUE_LEN 64

/* SPW_rxDsc_s is {ctrl, daddr} in big-endian words (SPW.h:291-295) */
#define SPW_RXDSC_LEN_MASK 0x1FFFFFFu

OBJECT_DECLARE_SIMPLE_TYPE(STIXSPW, GRLIB_STIXSPW)

typedef struct
{
    volatile uint32_t ctrl;
    volatile uint32_t haddr;
    volatile uint32_t dlen;
    volatile uint32_t daddr;

} spwTXdsc_s;

struct STIXSPW
{
    SysBusDevice parent_obj;

    stixSpwRegs_s reg;

    OnOffAuto tmtc_server; /* auto = on, except under qtest (hermetic tests) */
    uint32_t tmtc_port; /* 0 = dynamic: IO_TCP_SERVER_BASE_PORT + getuid() */

    /*
     * D-B direct UDP for the ground station (P2-8); the ports are 0 =
     * off and the sockets live in tmtc (StixTmtc)
     */
    char *udp_tm_host;  /* TM destination host, default 127.0.0.1 */
    uint32_t udp_tm_port; /* TM destination port (datagrams out) */
    uint32_t udp_tc_port; /* local TC bind (datagrams in) */
    bool udp_spw_header; /* keep the 4-byte SpW header in both directions */

    MemoryRegion iomem;
    qemu_irq irq;
    /*
     * D-M: an IRQ event that arrived while IE = 0; must go into a
     * vmstate if one is ever added
     */
    bool irq_latched;

    /*
     * RX side (P2-5, D-L): the descriptor selector plus the no-spill
     * queue of units waiting for a free descriptor
     */
    unsigned rx_index; /* descriptor selector, 0 on a table write/reset */
    unsigned rx_head;  /* queue head */
    unsigned rx_count; /* units pending */
    uint8_t *rx_queue[SPW_RX_QUEUE_LEN];
    size_t rx_qlen[SPW_RX_QUEUE_LEN];
    uint32_t rx_dropped; /* units dropped when the queue was full */

    /*
     * D-J: time codes generated at the SCET second rollover. The
     * scet-source link (set in leon3.c) is the CTRL device the SCET
     * comes from; timecodes is runtime-settable (qom-set).
     */
    STIXCTRL *scet_ctrl;
    bool timecodes;
    QEMUTimer *tc_timer;

    /*
     * D-PLb (PL-1): timecode-source=obt makes a platform on-board time
     * the source instead of the IDPU SCET. OBT = obt_base + (vclock -
     * obt_vclock_base) * 65536 / 1e9 in CUC 32.16 units, rebased by an
     * obt set; it survives a device reset like timecodes. The qdev
     * string is parsed at realize into obt_mode.
     */
    char *timecode_source;
    bool obt_mode;
    uint64_t obt_epoch;
    uint64_t obt_base;
    int64_t obt_vclock_base;
    /*
     * D-PLc: one correct code armed at the next rollover while
     * timecodes is false (timecode-once)
     */
    bool once_pending;
    /*
     * PL-2 (D-PLo): the last emitted code (timer, once or inject) and
     * its virtual time, -1 = none; read only by hw-state
     */
    uint32_t last_timecnt;
    int64_t last_emit_ns;

    /*
     * D-P4e (P4-5): link-down is runtime-settable (qom-set) and cleared
     * at reset. While set, LS reads READY, DE and the link-error IRQ
     * repeat on link_timer, time codes stop, RX units stay queued and
     * a TE waits (practically never reached by the FSW: SPW_send
     * checks LS first, SPW.c:692-696; only a qom-set landing between
     * SPW.c:693 and :725 gets here).
     */
    bool link_down;
    QEMUTimer *link_timer;
    /* a stixspw_rx_retry is running: a nested call returns at once */
    bool rx_retrying;

    /*
     * TMTC transport state (D-N); the fds are -1 while the server is
     * off
     */
    StixTmtc tmtc;
};

static uint8_t buffer[8192];

static void stixspw_rx_deliver(STIXSPW *s, const uint8_t *buf, int len);
static void grlib_stixspw_tx(STIXSPW *stixspw);

/*
 * The DMA paths that run outside an MMIO dispatch (the TCP/UDP fd
 * handler, rx-inject and the link-down clear, all from the main loop)
 * engage the device's re-entrancy guard, as qemu_bh_new_guarded does
 * (util/async.c, aio_bh_call). A guest descriptor address inside this
 * device's MMIO is then refused by memory.c ("Blocked re-entrant IO",
 * MEMTX_ACCESS_ERROR, the write is dropped), exactly as it is on the
 * in-MMIO paths (a dmaControl write that runs the TX or the RX retry).
 */
static bool stixspw_guard_enter(STIXSPW *s)
{
    MemReentrancyGuard *g = &DEVICE(s)->mem_reentrancy_guard;
    bool was = g->engaged_in_io;

    g->engaged_in_io = true;
    return was;
}

static void stixspw_guard_exit(STIXSPW *s, bool was)
{
    DEVICE(s)->mem_reentrancy_guard.engaged_in_io = was;
}

/*
 * A whole TC unit arrived from the transport (TCP D-K framing, or one
 * UDP datagram, D-B; BQL held): hand it to the RX DMA path (P2-5),
 * which applies truncation and the no-spill queue.
 */
static void recv_packet_callback(uint8_t *data, int length, void *opaque)
{
    STIXSPW *stixspw = opaque;
    bool was;

    trace_stixspw_recv_packet_callback(length);
    was = stixspw_guard_enter(stixspw);
    stixspw_rx_deliver(stixspw, data, length);
    stixspw_guard_exit(stixspw, was);
}

/* The RX descriptors are big-endian words (correction 16) */
static uint32_t stixspw_ld_be32(hwaddr addr)
{
    uint32_t v;

    physical_memory_read(addr, &v, sizeof(v));
    return be32_to_cpu(v);
}

static void stixspw_st_be32(hwaddr addr, uint32_t v)
{
    uint32_t b = cpu_to_be32(v);

    physical_memory_write(addr, &b, sizeof(b));
}

static void stixspw_rx_queue_clear(STIXSPW *s)
{
    int i;

    for (i = 0; i < SPW_RX_QUEUE_LEN; i++) {
        g_free(s->rx_queue[i]);
        s->rx_queue[i] = NULL;
    }
    s->rx_head = 0;
    s->rx_count = 0;
}

/*
 * What stixspw_rx_deliver_one did with a unit
 */
typedef enum {
    STIXSPW_RX_DELIVERED, /* DMA'd into a descriptor, PR set */
    STIXSPW_RX_HELD,      /* the control.IE window holds it back */
    STIXSPW_RX_NODESC,    /* no armed descriptor, or RE or RD clear */
} StixSpwRxResult;

/*
 * The descriptor-side RX work (FSW-RX §2): DMA the unit into the
 * descriptor's buffer, write the ctrl word back (the byte count
 * written, TR when truncated, WR/IE kept, EN cleared), advance the
 * selector, set PR and raise IRQ 13 when RI and the descriptor's IE
 * are set. RD is cleared on the NODESC paths because a unit is
 * pending (correction 10).
 */
static StixSpwRxResult stixspw_rx_deliver_one(STIXSPW *s,
                                              const uint8_t *buf, int len)
{
    hwaddr desc_addr = (s->reg.dmaRxDescAddr & ~0x3ffull) +
                       8ull * (s->rx_index & 127);
    uint32_t ctrl, newctrl, n, daddr;

    /*
     * M1 of the P2-5 review: an IE = 0 window is the FSW's SPW_send
     * critical section (SPW.c:707-731). PR set inside it would be
     * eaten by the FSW's own `dmaControl |= TE` read-modify-write
     * (W1C, SPW.c:725) and the deferred ISR would skip the unit, so
     * the unit waits for the IE edge instead. RD is a DMA-channel
     * condition, not a descriptor one — it is not touched here.
     */
    if (!(s->reg.control & (1u << STIXSPW_CONTROL_IE))) {
        return STIXSPW_RX_HELD;
    }
    if (!(s->reg.dmaControl & (1u << STIXSPW_DMACONTROL_RE)) ||
        !(s->reg.dmaControl & (1u << STIXSPW_DMACONTROL_RD))) {
        s->reg.dmaControl &= ~(1u << STIXSPW_DMACONTROL_RD);
        return STIXSPW_RX_NODESC;
    }

    ctrl = stixspw_ld_be32(desc_addr);
    if (!(ctrl & (1u << RXDSC_EN))) {
        s->reg.dmaControl &= ~(1u << STIXSPW_DMACONTROL_RD);
        return STIXSPW_RX_NODESC;
    }

    daddr = stixspw_ld_be32(desc_addr + 4);

    /* dmaMaxRxLength is clamped to the 25-bit LEN field on write */
    n = MIN((uint32_t)len, s->reg.dmaMaxRxLength);
    /*
     * a guest daddr at this device's own MMIO would re-enter the write
     * handler; the memory-region re-entrancy guard refuses that
     * nested access (the owner is this device)
     */
    physical_memory_write(daddr, buf, n);
    if ((uint32_t)len > n) {
        trace_stixspw_rx_truncated(len, n);
    }

    newctrl = n | (ctrl & ((1u << RXDSC_WR) | (1u << RXDSC_IE)));
    if ((uint32_t)len > n) {
        newctrl |= 1u << RXDSC_TR;
    }
    stixspw_st_be32(desc_addr, newctrl);
    trace_stixspw_rx_deliver(len, newctrl, s->rx_index,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    /*
     * the selector wraps at WR, like the FSW's re-arm loop, and at
     * the 128 descriptors of DESCSEL (StixIdpu.h:1454-1456)
     */
    s->rx_index = (ctrl & (1u << RXDSC_WR)) ? 0 : ((s->rx_index + 1) & 127);

    s->reg.dmaControl |= (1u << STIXSPW_DMACONTROL_PR);
    if ((s->reg.dmaControl & (1u << STIXSPW_DMACONTROL_RI)) &&
        (ctrl & (1u << RXDSC_IE))) {
        /* control.IE was checked up front: the pulse is direct */
        qemu_irq_pulse(s->irq);
    }
    return STIXSPW_RX_DELIVERED;
}

/*
 * A TC unit arrived. The model is always no-spill (D-L; NS=0 is not
 * honoured — the FSW always sets NS, 0x181e). If any unit is already
 * queued, this one joins the tail — the FIFO order is never jumped.
 * Otherwise it is delivered straight away if the DMA channel is
 * ready; else it waits in the no-spill queue, with RD cleared only
 * for a descriptor condition.
 */
static void stixspw_rx_deliver(STIXSPW *s, const uint8_t *buf, int len)
{
    unsigned tail;

    if (len <= 0) {
        return;
    }
    /* D-P4e: no unit crosses a down link; it waits in the queue */
    if (s->rx_count == 0 && !s->link_down) {
        StixSpwRxResult r = stixspw_rx_deliver_one(s, buf, len);

        if (r == STIXSPW_RX_DELIVERED) {
            return;
        }
        if (r == STIXSPW_RX_NODESC) {
            /*
             * correction 10: RD is cleared only while a unit is
             * pending on a descriptor condition; a unit held back by
             * the IE window leaves RD alone
             */
            s->reg.dmaControl &= ~(1u << STIXSPW_DMACONTROL_RD);
        }
    }
    if (s->rx_count == SPW_RX_QUEUE_LEN) {
        s->rx_dropped++;
        trace_stixspw_rx_dropped(len);
        return;
    }
    tail = (s->rx_head + s->rx_count) % SPW_RX_QUEUE_LEN;
    s->rx_queue[tail] = g_malloc(len);
    memcpy(s->rx_queue[tail], buf, len);
    s->rx_qlen[tail] = len;
    s->rx_count++;
    trace_stixspw_rx_queued(len, s->rx_count);
}

/*
 * The FSW's ISR writes dmaControl with RD after every re-arm
 * (SPW.c:349-350), and the IE 0->1 edge retries the units held back
 * in the IE=0 window; both come through here. The head is delivered
 * while it finds the channel ready; the first failure stops the
 * drain (deliver_one managed RD for the descriptor case).
 */
static void stixspw_rx_retry(STIXSPW *s)
{
    /*
     * The head stays queued while it is delivered, so a nested retry
     * (should the guard ever let a DMA write reach dmaControl) would
     * deliver and free it a second time: refuse to nest.
     */
    if (s->rx_count == 0 || s->link_down || s->rx_retrying) {
        return;
    }
    s->rx_retrying = true;
    trace_stixspw_rx_retry(s->rx_count);
    while (s->rx_count > 0) {
        uint8_t *buf = s->rx_queue[s->rx_head];
        int len = (int)s->rx_qlen[s->rx_head];

        if (stixspw_rx_deliver_one(s, buf, len) != STIXSPW_RX_DELIVERED) {
            break;
        }
        g_free(buf);
        s->rx_queue[s->rx_head] = NULL;
        s->rx_head = (s->rx_head + 1) % SPW_RX_QUEUE_LEN;
        s->rx_count--;
    }
    s->rx_retrying = false;
}

/* D-J: the SCET fine field counts 1/65536 s ticks */
#define STIXSPW_SUBSEC_PER_SEC 65536LL
#define STIXSPW_NSEC_PER_SEC 1000000000LL
/* D-PLb: the OBT is a 48-bit CUC (32-bit coarse, 16-bit fine) */
#define STIXSPW_OBT_MASK 0xFFFFFFFFFFFFull

/*
 * D-PLb: the OBT ticks elapsed since the base, from the virtual clock.
 * Whole seconds and the remainder are converted separately, so a long
 * run cannot overflow the 64-bit product.
 */
static uint64_t stixspw_obt_at(STIXSPW *s, int64_t now)
{
    uint64_t e = now > s->obt_vclock_base ? now - s->obt_vclock_base : 0;
    uint64_t secs = e / STIXSPW_NSEC_PER_SEC;
    uint64_t rem = e % STIXSPW_NSEC_PER_SEC;

    return s->obt_base + (secs << 16) +
           rem * STIXSPW_SUBSEC_PER_SEC / STIXSPW_NSEC_PER_SEC;
}

/*
 * D-PLb: the virtual time at which the OBT reaches the next coarse
 * rollover. The remainder is rounded up, so the OBT read at the
 * deadline is the new coarse with fine 0 (never one tick short).
 */
static int64_t stixspw_obt_next_rollover(STIXSPW *s, int64_t now)
{
    uint64_t next = ((stixspw_obt_at(s, now) >> 16) + 1) << 16;
    uint64_t d = next - s->obt_base;
    uint64_t rem = d & 0xFFFFu;

    return s->obt_vclock_base + (int64_t)((d >> 16) * STIXSPW_NSEC_PER_SEC) +
           (int64_t)DIV_ROUND_UP(rem * STIXSPW_NSEC_PER_SEC,
                                 STIXSPW_SUBSEC_PER_SEC);
}

/*
 * The time the codes follow: the OBT in obt mode, else the IDPU SCET
 * (stixctrl_scet_now, the coarse without the UNSYNCED bit).
 */
static void stixspw_tc_now(STIXSPW *s, uint32_t *coarse, uint16_t *fine)
{
    if (s->obt_mode) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t obt = stixspw_obt_at(s, now) & STIXSPW_OBT_MASK;

        *coarse = (uint32_t)(obt >> 16);
        *fine = (uint16_t)obt;
        return;
    }
    stixctrl_scet_now(s->scet_ctrl, coarse, fine);
}

/*
 * D-J: arm the time-code timer at the next coarse rollover of the
 * source (the SCET, or the OBT in obt mode, D-PLb). It also runs while
 * only a timecode-once is pending (D-PLc).
 */
static void stixspw_tc_arm(STIXSPW *s)
{
    uint32_t coarse;
    uint16_t fine;
    int64_t now, due;

    /*
     * m3: pre-realize (or timecodes=off with no once pending) there is
     * nothing to arm; D-P4e: no time codes cross a down link, the clear
     * re-arms
     */
    if (!(s->timecodes || s->once_pending) || s->link_down ||
        s->scet_ctrl == NULL || s->tc_timer == NULL) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stixspw_tc_now(s, &coarse, &fine);
    if (s->obt_mode) {
        due = stixspw_obt_next_rollover(s, now);
    } else {
        due = now + STIXSPW_NSEC_PER_SEC -
              fine * STIXSPW_NSEC_PER_SEC / STIXSPW_SUBSEC_PER_SEC;
    }
    timer_mod_ns(s->tc_timer, due);
    trace_stixspw_timecode_arm(coarse, fine, due - now);
}

/*
 * correction 6: the guest committed a new on-board time (0xCC). In obt
 * mode the codes follow the platform OBT, so the guest SCET does not
 * move them (D-PLb).
 */
static void stixspw_scet_committed(void *opaque)
{
    STIXSPW *s = opaque;

    if (!s->obt_mode) {
        stixspw_tc_arm(s);
    }
}

/*
 * One time code with this TIMECNT, when control.TR is set: it latches
 * time = TIMECNT (bits 5:0, TCTRL 7:6 = 0), sets status.TO and raises
 * IRQ 13 when control.TQ is set, gated by control.IE per D-M. Returns
 * false when TR = 0 (nothing emitted). Register and IRQ work only: no
 * DMA, so a caller outside an MMIO dispatch needs no re-entrancy guard.
 */
static bool stixspw_tc_emit(STIXSPW *s, uint32_t coarse, uint32_t timecnt)
{
    if (!(s->reg.control & (1u << STIXSPW_CONTROL_TR))) {
        return false;
    }
    s->reg.time = timecnt & 63u;
    s->reg.status |= (1u << STIXSPW_STATUS_TO);
    s->last_timecnt = s->reg.time;
    s->last_emit_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    trace_stixspw_timecode_emit(coarse, s->reg.time);
    if (s->reg.control & (1u << STIXSPW_CONTROL_TQ)) {
        if (s->reg.control & (1u << STIXSPW_CONTROL_IE)) {
            qemu_irq_pulse(s->irq);
        } else {
            s->irq_latched = true;
            trace_stixspw_irq_latched();
        }
    }
    return true;
}

/* D-PLc: a pending timecode-once is dropped (traced) */
static void stixspw_once_drop(STIXSPW *s, const char *why)
{
    if (s->once_pending) {
        s->once_pending = false;
        trace_stixspw_timecode_once_drop(why);
    }
}

/*
 * D-J: the time-code timer fired at a coarse rollover. A code is
 * emitted only in the first 1/16 s of the new second (fine < 0x1000 —
 * a mid-second code would drag the synchronised FSW clock,
 * Timing.c:199-328) and only when control.TR is set (stixspw_tc_emit).
 * A pending timecode-once is consumed by this rollover (D-PLc).
 */
static void stixspw_tc_timer(void *opaque)
{
    STIXSPW *s = opaque;
    uint32_t coarse;
    uint16_t fine;
    bool emitted;

    if (!(s->timecodes || s->once_pending) || s->link_down) {
        return; /* silenced at run time; the property re-arms */
    }

    stixspw_tc_now(s, &coarse, &fine);
    if (fine >= 0x1000) {
        /*
         * m5: the callback ran more than 62.5 ms late — possible
         * without -icount when the main loop is starved. A code
         * emitted now would drag the synchronised FSW clock
         * (Timing.c:199-328): one second is lost and the re-arm emits
         * at the next rollover (a pending once waits for it). A guest
         * SCET set cannot reach this path — every 0xCC commit re-arms
         * through the hook, and in obt mode the OBT is not guest-set.
         */
        trace_stixspw_timecode_skip(coarse, fine);
        stixspw_tc_arm(s);
        return;
    }

    emitted = stixspw_tc_emit(s, coarse, coarse & 63u);
    if (s->once_pending) {
        s->once_pending = false;
        trace_stixspw_timecode_manual("once", coarse & 63u, emitted);
    }
    stixspw_tc_arm(s);
}

/*
 * D-P4e: one link error while the link is down. DE (re-)sets — the ISR
 * clears it with a write-1 (SPW.c:227-233) and the next period brings
 * it back — and IRQ 13 is raised when control.LI is set, gated by
 * control.IE with the D-M latch like every other source.
 */
static void stixspw_link_error(STIXSPW *s)
{
    bool li = s->reg.control & (1u << STIXSPW_CONTROL_LI);
    bool ie = s->reg.control & (1u << STIXSPW_CONTROL_IE);

    s->reg.status |= (1u << STIXSPW_STATUS_DE);
    trace_stixspw_link_error(s->reg.status, li, ie);
    if (li) {
        if (ie) {
            qemu_irq_pulse(s->irq);
        } else {
            s->irq_latched = true;
            trace_stixspw_irq_latched();
        }
    }
}

static void stixspw_link_timer(void *opaque)
{
    STIXSPW *s = opaque;

    if (!s->link_down) {
        return;
    }
    stixspw_link_error(s);
    timer_mod_ns(s->link_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                SPW_LINK_ERR_PERIOD_NS);
}

/*
 * D-P4e: take the link down or bring it back (the link-down property).
 * The clear runs the TX and the RX DMA outside an MMIO dispatch (from
 * QMP), so it engages the re-entrancy guard: a guest daddr at this
 * device's MMIO is refused, as on the in-MMIO paths.
 * Down: LS = READY, a link error at once and then every second, time
 * codes stopped. Up: LS = RUN (DE stays until the guest clears it, as
 * W1C), the time codes re-arm if timecodes is true, a TE that waited
 * runs, and the queued RX units are delivered.
 */
static void stixspw_set_link(STIXSPW *s, bool down)
{
    bool was;

    if (s->link_down == down) {
        return;
    }
    s->link_down = down;
    trace_stixspw_link_down(down, s->rx_count);
    s->reg.status &= ~SPW_STATUS_LS_FIELD;
    if (down) {
        stixspw_once_drop(s, "link-down");
        s->reg.status |= SPW_STATUS_READY;
        if (s->tc_timer != NULL) {
            timer_del(s->tc_timer);
        }
        stixspw_link_timer(s);
        return;
    }
    s->reg.status |= SPW_STATUS_RUN;
    timer_del(s->link_timer);
    stixspw_tc_arm(s);
    was = stixspw_guard_enter(s);
    if (s->reg.dmaControl & (1u << STIXSPW_DMACONTROL_TE)) {
        grlib_stixspw_tx(s);
    }
    stixspw_rx_retry(s);
    stixspw_guard_exit(s, was);
}

#define STIXSPW_REG_WORDS (sizeof(stixSpwRegs_s) / sizeof(uint32_t))

static uint64_t grlib_stixspw_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXSPW *stixspw = opaque;
    uint32_t regval;

    /* security fix: the MMIO window (256 bytes) is larger than the register
     * struct (48 bytes); offsets past the struct must not index host memory.
     * The pinned baseline flows only ever access 0x00-0x2F. */
    if ((uint32_t)addr / sizeof(uint32_t) >= STIXSPW_REG_WORDS)
    {
        trace_stixspw_io_read_outside(addr);
        return 0;
    }

    switch ((uint32_t)addr / sizeof(uint32_t)) {
    case 11:
        /*
         * dmaRxDescAddr: GRSPW reads back DESCBASEADDR (bits 31:10)
         * with the core's descriptor selector in DESCSEL (bits 9:3,
         * StixIdpu.h:1451-1456). The FSW never reads the register —
         * its only access is the write at SPW.c:545.
         */
        regval = (stixspw->reg.dmaRxDescAddr & ~0x3ffu) |
                 ((stixspw->rx_index & 127) << 3);
        break;
    default:
        regval = ((volatile uint32_t *)&stixspw->reg) /* register file */
                     [(uint32_t)addr / sizeof(uint32_t)];
        break;
    }

    trace_stixspw_io_read(addr, regval);

    return regval;
}

/*
 * One TX exchange, run from a dmaControl write with TE set (the FSW's
 * dmaControl |= TE, SPW.c:725). TE always clears, even when the
 * descriptor is not armed (correction 9). The completion IRQ needs
 * TXDSC_IE and TI, and D-M defers it while control.IE = 0.
 */
static void grlib_stixspw_tx(STIXSPW *stixspw)
{
    spwTXdsc_s txdsc;
    uint32_t ctrl;

    stixspw->reg.dmaControl &= ~(1u << STIXSPW_DMACONTROL_TE);

    /* read TX descriptor from STIX */
    trace_stixspw_tx_desc_addr(stixspw->reg.dmaTxDescAddr);

    /*
     * endianness is incorrect for use in qemu; there may be a better
     * way with address_space_read, but it's not working at the moment.
     * using a small workaround
     */
    physical_memory_read(stixspw->reg.dmaTxDescAddr, &txdsc, sizeof(txdsc));

    txdsc.ctrl = bswap32(txdsc.ctrl);
    txdsc.daddr = bswap32(txdsc.daddr);
    txdsc.dlen = bswap32(txdsc.dlen);
    txdsc.haddr = bswap32(txdsc.haddr);

    trace_stixspw_tx_descriptor(txdsc.ctrl, txdsc.daddr,
                                txdsc.dlen, txdsc.haddr);

    ctrl = txdsc.ctrl;

    if (!(ctrl & (1 << TXDSC_EN))) {
        /* not armed: no send, no write-back, no PS, no IRQ */
        trace_stixspw_tx_skip_en(ctrl);
        return;
    }

    if (txdsc.dlen < sizeof(buffer)) {
        memset(buffer, 0xFF, sizeof(buffer));
        physical_memory_read(txdsc.daddr, buffer, txdsc.dlen);

        /* swap buffer dwords to fix endianness */
        uint32_t *buffer32 = (uint32_t *)buffer;
        trace_stixspw_tx_bytes(txdsc.dlen);
        for (uint32_t ix = 0; ix < DIV_ROUND_UP(txdsc.dlen, 4); ix++) {
            /* buffer32[ix] = bswap32(buffer32[ix]); */
            trace_stixspw_tx_dump_word(bswap32(buffer32[ix]));
        }

        stix_tmtc_send(&stixspw->tmtc, buffer, txdsc.dlen);
        /* D-B: a second copy goes out as one bare-SP datagram */
        stix_tmtc_udp_send(&stixspw->tmtc, buffer, txdsc.dlen);

        trace_stixspw_tx_send(txdsc.dlen);
    } else {
        trace_stixspw_tx_overflow();
    }

    /* clear desc enabled flag */
    txdsc.ctrl &= ~(1 << TXDSC_EN);

    /* before using the exchange variable, correct for endienness */
    txdsc.ctrl = bswap32(txdsc.ctrl);
    txdsc.daddr = bswap32(txdsc.daddr);
    txdsc.dlen = bswap32(txdsc.dlen);
    txdsc.haddr = bswap32(txdsc.haddr);

    /* write TX decriptor to STIX */
    physical_memory_write(stixspw->reg.dmaTxDescAddr, &txdsc, sizeof(txdsc));

    /* exchange complete */
    stixspw->reg.dmaControl |= (1u << STIXSPW_DMACONTROL_PS);

    /* generate interrupt, gated per D-M */
    if ((ctrl & (1 << TXDSC_IE)) &&
        (stixspw->reg.dmaControl & (1u << STIXSPW_DMACONTROL_TI))) {
        if (stixspw->reg.control & (1u << STIXSPW_CONTROL_IE)) {
            qemu_irq_pulse(stixspw->irq);
        } else {
            stixspw->irq_latched = true;
            trace_stixspw_irq_latched();
        }
    }
}

static void grlib_stixspw_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    STIXSPW *stixspw = opaque;
    volatile uint32_t *regs = (volatile uint32_t *)&stixspw->reg; /* re-read */
    uint32_t val = (uint32_t)value;
    uint32_t old;
    uint32_t updated;

    /* security fix: same bounds rule as the read handler. */
    if ((uint32_t)addr / sizeof(uint32_t) >= STIXSPW_REG_WORDS) {
        trace_stixspw_io_write_outside(addr);
        return;
    }

    switch ((uint32_t)addr / sizeof(uint32_t)) {
    case 0: /* control */
        old = stixspw->reg.control;

        stixspw->reg.control = val;

        /*
         * the reset flag self-clears; a core reset (SPW_close, the
         * AHB-error reinit) also drops any latched IRQ
         */
        stixspw->reg.control &= ~(1u << STIXSPW_CONTROL_RS);
        if (val & (1u << STIXSPW_CONTROL_RS)) {
            stixspw->irq_latched = false;
        }

        /*
         * D-M: an IE 0->1 write first retries the RX queue (units held
         * back by the IE=0 window, M1 of the P2-5 review), then pulses
         * the latched TX/event IRQ. The pulses coalesce in IRQMP; one
         * ISR entry drains everything.
         */
        if (!(old & (1u << STIXSPW_CONTROL_IE)) &&
            (val & (1u << STIXSPW_CONTROL_IE))) {
            stixspw_rx_retry(stixspw);
            if (stixspw->irq_latched) {
                stixspw->irq_latched = false;
                trace_stixspw_irq_deferred();
                qemu_irq_pulse(stixspw->irq);
            }
        }
        break;
    case 1: /* status: W1C, LS is read-only (RUN, or READY while down) */
        stixspw->reg.status &= ~(SPW_STATUS_W1C_MASK & val);
        break;
    case 8: /* dmaControl: W1C + latch, reserved bits read 0 */
        updated = stixspw->reg.dmaControl;

        updated &= ~(SPW_DMACONTROL_W1C_MASK & val);
        updated = (updated & ~SPW_DMACONTROL_LATCH_MASK) |
                  (val & SPW_DMACONTROL_LATCH_MASK);
        updated &= ~SPW_DMACONTROL_RESERVED_MASK;
        stixspw->reg.dmaControl = updated;

        /*
         * the exchange runs only on a dmaControl write with TE set;
         * D-P4e: while the link is down TE stays set and the exchange
         * runs when the link comes back (the GRSPW waits for Run)
         */
        if ((val & (1u << STIXSPW_DMACONTROL_TE)) && stixspw->link_down) {
            trace_stixspw_tx_link_wait();
        } else if (val & (1u << STIXSPW_DMACONTROL_TE)) {
            trace_stixspw_data_exchange();
            grlib_stixspw_tx(stixspw);
        }

        /* D-L: the ISR's post-re-arm RD write retries the queue */
        if (val & (1u << STIXSPW_DMACONTROL_RD)) {
            stixspw_rx_retry(stixspw);
        }
        break;
    case 9: /* dmaMaxRxLength: GRSPW clamps it to the 25-bit LEN field */
        stixspw->reg.dmaMaxRxLength = val & SPW_RXDSC_LEN_MASK;
        break;
    case 11:
        /*
         * dmaRxDescAddr: DESCBASEADDR is bits 31:10 (a 1 KiB-aligned
         * table) and DESCSEL the selector bits 9:3
         * (StixIdpu.h:1451-1456). The FSW writes the base once
         * (SPW.c:545) and never reads the register back.
         */
        stixspw->reg.dmaRxDescAddr = val;
        stixspw->rx_index = (val >> 3) & 127;
        break;
    default:
        /* update the register as per write request */
        regs[(uint32_t)addr / sizeof(uint32_t)] = val;
        break;
    }

    trace_stixspw_io_write(addr, regs[(uint32_t)addr / sizeof(uint32_t)]);
    trace_stixspw_dma_control(stixspw->reg.dmaControl);
}

static const MemoryRegionOps grlib_stixspw_ops = {
    .write = grlib_stixspw_write,
    .read = grlib_stixspw_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void grlib_stixspw_realize(DeviceState *dev, Error **errp)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /*
     * D-J: the time-code source is the CTRL device (the scet-source
     * link, set in leon3.c before realize). Checked first, so a failed
     * realize leaks nothing (P2-7 review m2): the TMTC server below
     * starts only after this point.
     */
    if (STIXSPW->timecodes && STIXSPW->scet_ctrl == NULL) {
        error_setg(errp,
                   "grlib-stixspw: timecodes=on needs the scet-source link");
        return;
    }
    /* D-PLb: the time-code source, scet (the default) or obt */
    if (STIXSPW->timecode_source == NULL ||
        !strcmp(STIXSPW->timecode_source, "scet")) {
        STIXSPW->obt_mode = false;
    } else if (!strcmp(STIXSPW->timecode_source, "obt")) {
        STIXSPW->obt_mode = true;
    } else {
        error_setg(errp, "grlib-stixspw: timecode-source '%s' is not "
                   "'scet' or 'obt'", STIXSPW->timecode_source);
        return;
    }
    if (STIXSPW->obt_epoch > STIXSPW_OBT_MASK) {
        error_setg(errp, "grlib-stixspw: obt-epoch 0x%" PRIx64
                   " is wider than the 48-bit CUC", STIXSPW->obt_epoch);
        return;
    }
    STIXSPW->obt_base = STIXSPW->obt_epoch;
    STIXSPW->obt_vclock_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    sysbus_init_irq(sbd, &STIXSPW->irq);

    memory_region_init_io(&STIXSPW->iomem, OBJECT(STIXSPW), &grlib_stixspw_ops, STIXSPW, "STIXSPW", STIXSPW_REG_SIZE);

    sysbus_init_mmio(sbd, &STIXSPW->iomem);

    // set link to up from the start
    // I know, it's a little cheating
    // shortcut for link running as per E_SPW_LINK_STATUS_RUN
    STIXSPW->reg.status = SPW_STATUS_RUN;

    /* tmtc-server=auto keeps the server off whenever the qtest harness
     * drives this QEMU (the -qtest chardev is connected, whatever the
     * accelerator: some qtests boot real code under TCG): no qtest attaches
     * a TM client, and the default port (20000+uid) must stay free for the
     * boot gates and a live FSW session. With the server off, TM sends
     * drop silently, as they do today with no client. */
    if (STIXSPW->tmtc_server == ON_OFF_AUTO_ON ||
        (STIXSPW->tmtc_server == ON_OFF_AUTO_AUTO && !qtest_driver()))
    {
        if (!init_tmtc_server(&STIXSPW->tmtc, recv_packet_callback, STIXSPW,
                              STIXSPW->tmtc_port, errp))
        {
            return;
        }
    }

    /*
     * D-B: the UDP transport is independent of the TCP OnOffAuto — the
     * ground station talks plain UDP, with or without the TCP server.
     */
    if (STIXSPW->udp_tm_port != 0 || STIXSPW->udp_tc_port != 0) {
        if (!stix_tmtc_udp_open(&STIXSPW->tmtc, recv_packet_callback, STIXSPW,
                                STIXSPW->udp_tm_host ? : "127.0.0.1",
                                STIXSPW->udp_tm_port, STIXSPW->udp_tc_port,
                                STIXSPW->udp_spw_header, errp))
        {
            /*
             * m1 of the P2-8 review: undo the TCP server too, so a
             * failed realize leaves nothing behind (no listening socket
             * with a dangling-opaque fd handler, no buffers). The UDP
             * sockets are already closed by stix_tmtc_udp_open.
             */
            stix_tmtc_cleanup(&STIXSPW->tmtc);
            return;
        }
    }

    STIXSPW->tc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stixspw_tc_timer,
                                     STIXSPW);
    STIXSPW->link_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stixspw_link_timer,
                                       STIXSPW);
    if (STIXSPW->timecodes) {
        stixspw_tc_arm(STIXSPW);
    }
    if (STIXSPW->scet_ctrl != NULL) {
        /*
         * m1: no link, no hook — timecodes=off with no link must not
         * dereference NULL
         */
        stixctrl_set_scet_commit_hook(STIXSPW->scet_ctrl,
                                      stixspw_scet_committed, STIXSPW);
    }
}

static void grlib_stixspw_unrealize(DeviceState *dev)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(dev);

    if (STIXSPW->scet_ctrl != NULL) {
        stixctrl_set_scet_commit_hook(STIXSPW->scet_ctrl, NULL, NULL);
    }
    if (STIXSPW->tc_timer != NULL) {
        timer_del(STIXSPW->tc_timer);
        timer_free(STIXSPW->tc_timer);
        STIXSPW->tc_timer = NULL;
    }
    if (STIXSPW->link_timer != NULL) {
        timer_del(STIXSPW->link_timer);
        timer_free(STIXSPW->link_timer);
        STIXSPW->link_timer = NULL;
    }
    stix_tmtc_cleanup(&STIXSPW->tmtc);
    stixspw_rx_queue_clear(STIXSPW);
}

static bool grlib_stixspw_get_tmtc_connected(Object *obj, Error **errp)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(obj);

    return STIXSPW->tmtc.client_fd >= 0;
}

/*
 * qtest-only TC injection (P2-5): the value is a hex string with
 * spaces between bytes; each set delivers one unit through the RX
 * path exactly as the P2-6 TCP reassembly will.
 */
static void grlib_stixspw_set_rx_inject(Object *obj, const char *value,
                                        Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);
    uint8_t buf[8192];
    const char *p;
    size_t len = 0;
    bool high = false;
    bool was;
    uint8_t cur = 0;

    if (!qtest_enabled()) {
        error_setg(errp, "rx-inject is only available under qtest");
        return;
    }

    for (p = value; *p != '\0'; p++) {
        int nib;

        if (*p == ' ') {
            continue; /* spaces separate the bytes */
        }
        nib = g_ascii_xdigit_value(*p);
        if (nib < 0) {
            error_setg(errp, "rx-inject: '%c' is not a hex digit", *p);
            return;
        }
        if (!high) {
            cur = (uint8_t)(nib << 4);
            high = true;
            continue;
        }
        if (len >= sizeof(buf)) {
            error_setg(errp, "rx-inject: unit longer than %zu bytes",
                       sizeof(buf));
            return;
        }
        buf[len++] = cur | (uint8_t)nib;
        high = false;
    }
    if (high) {
        error_setg(errp, "rx-inject: odd number of hex digits");
        return;
    }
    if (len == 0) {
        error_setg(errp, "rx-inject: no bytes to inject");
        return;
    }

    trace_stixspw_rx_inject((uint32_t)len);
    was = stixspw_guard_enter(s);
    stixspw_rx_deliver(s, buf, (int)len);
    stixspw_guard_exit(s, was);
}

static bool grlib_stixspw_get_timecodes(Object *obj, Error **errp)
{
    return GRLIB_STIXSPW(obj)->timecodes;
}

static void grlib_stixspw_set_timecodes(Object *obj, bool value, Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);

    /* D-PLc: any timecodes write drops a pending once */
    stixspw_once_drop(s, "timecodes");
    s->timecodes = value;
    if (!value) {
        if (s->tc_timer != NULL) {
            timer_del(s->tc_timer);
        }
    } else {
        /* pre-realize the timer does not exist yet; realize arms */
        stixspw_tc_arm(s);
    }
}

static bool grlib_stixspw_get_link_down(Object *obj, Error **errp)
{
    return GRLIB_STIXSPW(obj)->link_down;
}

static void grlib_stixspw_set_link_down(Object *obj, bool value, Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);

    /* runtime-only: a reset (including the one at machine start) clears it */
    if (!DEVICE(obj)->realized) {
        error_setg(errp, "link-down can only be set at run time (qom-set)");
        return;
    }
    stixspw_set_link(s, value);
}

/*
 * D-PLb: the runtime obt (uint64, CUC 32.16 in bits 47:0). The getter
 * reads the OBT in obt mode, or the IDPU SCET (coarse without UNSYNCED)
 * in scet mode; it has no side effect. The setter is an OBT jump, in
 * obt mode only: it rebases the OBT at the current virtual time and
 * re-arms the time-code timer.
 */
static void grlib_stixspw_get_obt(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);
    uint32_t coarse;
    uint16_t fine;
    uint64_t value;

    if (!DEVICE(obj)->realized || (!s->obt_mode && s->scet_ctrl == NULL)) {
        error_setg(errp, "obt is only readable at run time");
        return;
    }
    stixspw_tc_now(s, &coarse, &fine);
    value = ((uint64_t)coarse << 16) | fine;
    visit_type_uint64(v, name, &value, errp);
}

static void grlib_stixspw_set_obt(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);
    uint32_t coarse;
    uint16_t fine;
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!DEVICE(obj)->realized) {
        error_setg(errp, "obt can only be set at run time (qom-set)");
        return;
    }
    if (!s->obt_mode) {
        error_setg(errp, "obt can only be set with timecode-source=obt");
        return;
    }
    if (value > STIXSPW_OBT_MASK) {
        error_setg(errp, "obt 0x%" PRIx64 " is wider than the 48-bit CUC",
                   value);
        return;
    }
    stixspw_tc_now(s, &coarse, &fine);
    trace_stixspw_obt_set(((uint64_t)coarse << 16) | fine, value);
    s->obt_base = value;
    s->obt_vclock_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stixspw_tc_arm(s);
}

static bool grlib_stixspw_get_timecode_once(Object *obj, Error **errp)
{
    return GRLIB_STIXSPW(obj)->once_pending;
}

/*
 * D-PLc: true arms exactly one correct code at the next rollover while
 * timecodes is false (a no-op while it is true); refused while the link
 * is down or control.TR is 0, when the model would emit nothing. False
 * cancels a pending once.
 */
static void grlib_stixspw_set_timecode_once(Object *obj, bool value,
                                            Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);
    uint32_t coarse;
    uint16_t fine;

    if (!DEVICE(obj)->realized) {
        error_setg(errp, "timecode-once can only be set at run time");
        return;
    }
    if (!value) {
        stixspw_once_drop(s, "cancel");
        return;
    }
    if (s->link_down) {
        error_setg(errp, "timecode-once: the link is down");
        return;
    }
    if (!(s->reg.control & (1u << STIXSPW_CONTROL_TR))) {
        stixspw_tc_now(s, &coarse, &fine);
        trace_stixspw_timecode_manual("once", (coarse + 1) & 63u, false);
        error_setg(errp, "timecode-once: control.TR is 0, no code would "
                   "be emitted");
        return;
    }
    if (s->timecodes) {
        return; /* the next rollover emits anyway */
    }
    s->once_pending = true;
    stixspw_tc_arm(s);
}

/*
 * D-PLc: timecode-inject (write-only, 0..63) emits one code now with
 * that TIMECNT through the TR/TQ/IE path and the D-M latch: the fault
 * tool for a wrong value or a mid-second code. Refused while the link
 * is down or control.TR is 0. No DMA runs (stixspw_tc_emit).
 */
static void grlib_stixspw_set_timecode_inject(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);
    uint32_t value, coarse;
    uint16_t fine;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!DEVICE(obj)->realized) {
        error_setg(errp, "timecode-inject can only be set at run time");
        return;
    }
    if (value > 63) {
        error_setg(errp, "timecode-inject: TIMECNT %u is not 0..63", value);
        return;
    }
    if (s->link_down) {
        error_setg(errp, "timecode-inject: the link is down");
        return;
    }
    stixspw_tc_now(s, &coarse, &fine);
    if (!stixspw_tc_emit(s, coarse, value)) {
        trace_stixspw_timecode_manual("inject", value, false);
        error_setg(errp, "timecode-inject: control.TR is 0, no code "
                   "emitted");
        return;
    }
    trace_stixspw_timecode_manual("inject", value, true);
}

/* PL-2 (D-PLr): the virtual clock, the controller's time master */
static void grlib_stixspw_get_vclock_ns(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    uint64_t value = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    visit_type_uint64(v, name, &value, errp);
}

static void stixspw_put_ns_or_null(QDict *d, const char *key, int64_t ns)
{
    if (ns >= 0) {
        qdict_put_int(d, key, ns);
    } else {
        qdict_put_null(d, key);
    }
}

/*
 * PL-2 (platform plan D-PLo): the read-only SpW state as one JSON
 * object, sampled atomically on the main loop. Pure getters only: the
 * register file is read directly (never through grlib_stixspw_read),
 * and no register, timer, queue or IRQ is touched.
 */
static char *grlib_stixspw_get_hw_state(Object *obj, Error **errp)
{
    STIXSPW *s = GRLIB_STIXSPW(obj);
    g_autoptr(QDict) root = qdict_new();
    QDict *ctl = qdict_new(), *tc = qdict_new(), *cnt = qdict_new();
    uint32_t c = s->reg.control;
    int64_t due = -1;
    uint32_t coarse;
    uint16_t fine;
    GString *json;

    qdict_put_int(root, "schema", 1);
    qdict_put_int(root, "vclock_ns", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    qdict_put_int(root, "ls", (s->reg.status >> STIXSPW_STATUS_LS) &
                              STIXSPW_STATUS_LS_MASK);
    qdict_put_bool(root, "link_down", s->link_down);
    qdict_put_int(root, "status", s->reg.status);

    qdict_put_int(ctl, "raw", c);
    qdict_put_bool(ctl, "tr", (c >> STIXSPW_CONTROL_TR) & 1u);
    qdict_put_bool(ctl, "tq", (c >> STIXSPW_CONTROL_TQ) & 1u);
    qdict_put_bool(ctl, "ie", (c >> STIXSPW_CONTROL_IE) & 1u);
    qdict_put_bool(ctl, "li", (c >> STIXSPW_CONTROL_LI) & 1u);
    qdict_put(root, "control", ctl);

    qdict_put_str(tc, "source", s->obt_mode ? "obt" : "scet");
    qdict_put_bool(tc, "enabled", s->timecodes);
    qdict_put_bool(tc, "once_pending", s->once_pending);
    if (s->last_emit_ns >= 0) {
        qdict_put_int(tc, "last_timecnt", s->last_timecnt);
    } else {
        qdict_put_null(tc, "last_timecnt");
    }
    stixspw_put_ns_or_null(tc, "last_vclock_ns", s->last_emit_ns);
    if (s->tc_timer != NULL && timer_pending(s->tc_timer)) {
        due = timer_expire_time_ns(s->tc_timer);
    }
    stixspw_put_ns_or_null(tc, "next_due_vclock_ns", due);
    qdict_put(root, "timecode", tc);

    if (DEVICE(obj)->realized && (s->obt_mode || s->scet_ctrl != NULL)) {
        stixspw_tc_now(s, &coarse, &fine);
        qdict_put_int(root, "obt", ((uint64_t)coarse << 16) | fine);
    } else {
        qdict_put_null(root, "obt");
    }
    qdict_put_int(root, "rx_queued", s->rx_count);

    qdict_put_int(cnt, "tm_dropped", s->tmtc.tm_dropped);
    qdict_put_int(cnt, "rx_dropped", s->rx_dropped);
    qdict_put_int(cnt, "tc_resync", s->tmtc.tc_resync);
    qdict_put_bool(cnt, "tmtc_connected", s->tmtc.client_fd >= 0);
    qdict_put(root, "counters", cnt);

    json = qobject_to_json(QOBJECT(root));
    return g_string_free(json, false);
}

static void grlib_stixspw_instance_init(Object *obj)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(obj);

    /* no fd 0: the transport is off until realize starts it */
    STIXSPW->tmtc.listen_fd = -1;
    STIXSPW->tmtc.client_fd = -1;
    STIXSPW->tmtc.udp_tm_fd = -1;
    STIXSPW->tmtc.udp_tc_fd = -1;
    STIXSPW->timecodes = true;
    STIXSPW->last_emit_ns = -1;

    object_property_add_uint32_ptr(obj, "tm-dropped",
                                   &STIXSPW->tmtc.tm_dropped,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "tmtc-connected",
                             grlib_stixspw_get_tmtc_connected,
                             NULL);
    object_property_add_uint32_ptr(obj, "rx-dropped", &STIXSPW->rx_dropped,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "tc-resync",
                                   &STIXSPW->tmtc.tc_resync,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "rx-inject", NULL,
                            grlib_stixspw_set_rx_inject);
    /*
     * correction 8: object_property_add_bool in instance_init, so it
     * is settable at run time (qom-set), not only before realize
     */
    object_property_add_bool(obj, "timecodes",
                             grlib_stixspw_get_timecodes,
                             grlib_stixspw_set_timecodes);
    /* D-P4e: SpW link fault injection, runtime-only like timecodes */
    object_property_add_bool(obj, "link-down",
                             grlib_stixspw_get_link_down,
                             grlib_stixspw_set_link_down);
    /* D-PLb/D-PLc (PL-1): the platform time, runtime like timecodes */
    object_property_add(obj, "obt", "uint64", &uint64_type_info,
                        grlib_stixspw_get_obt, grlib_stixspw_set_obt,
                        NULL, NULL);
    object_property_add_bool(obj, "timecode-once",
                             grlib_stixspw_get_timecode_once,
                             grlib_stixspw_set_timecode_once);
    object_property_add(obj, "timecode-inject", "uint32", &uint32_type_info,
                        NULL, grlib_stixspw_set_timecode_inject, NULL, NULL);
    /* PL-2 (D-PLo, D-PLr): read-only state and the virtual clock */
    object_property_add_str(obj, "hw-state", grlib_stixspw_get_hw_state,
                            NULL);
    object_property_add(obj, "vclock-ns", "uint64", &uint64_type_info,
                        grlib_stixspw_get_vclock_ns, NULL, NULL, NULL);
}

static void grlib_stixspw_reset(DeviceState *d)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(d);

    /*
     * power-on register values; the TMTC transport (P2-1) is kept. The
     * RX queue and the descriptor selector (P2-5) clear here too; the
     * register readback shows both gone (base and DESCSEL are 0).
     */
    memset(&STIXSPW->reg, 0, sizeof(STIXSPW->reg));
    STIXSPW->reg.status = SPW_STATUS_RUN;
    STIXSPW->irq_latched = false;
    stixspw_rx_queue_clear(STIXSPW);
    STIXSPW->rx_index = 0;
    /* D-P4e: a reset brings the link back up (no TX or RX is left) */
    if (STIXSPW->link_down) {
        trace_stixspw_link_down(false, 0);
    }
    STIXSPW->link_down = false;
    if (STIXSPW->link_timer != NULL) {
        timer_del(STIXSPW->link_timer);
    }

    /*
     * D-J (correction 16): the time-code timer survives a reset — the
     * SCET keeps running (the CTRL keeps its correction), so re-arm at
     * the next rollover; the FSW re-writes control at SPW_open. D-PLb:
     * the OBT and a pending once survive too (with TR = 0 after the
     * reset the once is consumed with nothing emitted, traced).
     */
    stixspw_tc_arm(STIXSPW);
}

/*
 * udp-tm-host is a DEFINE_PROP_STRING: the property's release frees it
 * at object destruction, so there is no instance_finalize here.
 */
static const Property grlib_stixspw_properties[] = {
    DEFINE_PROP_ON_OFF_AUTO("tmtc-server", STIXSPW, tmtc_server, ON_OFF_AUTO_AUTO),
    DEFINE_PROP_UINT32("tmtc-port", STIXSPW, tmtc_port, 0),
    DEFINE_PROP_STRING("udp-tm-host", STIXSPW, udp_tm_host),
    DEFINE_PROP_UINT32("udp-tm-port", STIXSPW, udp_tm_port, 0),
    DEFINE_PROP_UINT32("udp-tc-port", STIXSPW, udp_tc_port, 0),
    DEFINE_PROP_BOOL("udp-spw-header", STIXSPW, udp_spw_header, false),
    DEFINE_PROP_LINK("scet-source", STIXSPW, scet_ctrl,
                     TYPE_GRLIB_STIXCTRL, STIXCTRL *),
    /* D-PLb (PL-1): scet (default, §20 unchanged) or obt */
    DEFINE_PROP_STRING("timecode-source", STIXSPW, timecode_source),
    /* D-PLb: the OBT start value, CUC 32.16; coarse & 63 = 0 */
    DEFINE_PROP_UINT64("obt-epoch", STIXSPW, obt_epoch,
                       0x10000000ull << 16),
};

static void grlib_stixspw_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixspw_realize;
    dc->unrealize = grlib_stixspw_unrealize;
    device_class_set_legacy_reset(dc, grlib_stixspw_reset);
    device_class_set_props(dc, grlib_stixspw_properties);
}

static const TypeInfo grlib_stixspw_info = {
    .name = TYPE_GRLIB_STIXSPW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXSPW),
    .instance_init = grlib_stixspw_instance_init,
    .class_init = grlib_stixspw_class_init,
};

static void grlib_stixspw_register_types(void)
{
    type_register_static(&grlib_stixspw_info);
}

type_init(grlib_stixspw_register_types)