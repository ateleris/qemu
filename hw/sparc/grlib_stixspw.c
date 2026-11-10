#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qapi/qapi-types-common.h"
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
 *   stays at RUN and nothing in the model sets the error bits.
 * - dmaControl: PR/PS/TA/RA are write-1-to-clear; TE/RE/TI/RI/AI/RD/
 *   NS/LE latch as written; RX (bit 10) is the read-only reception
 *   status and AT (bit 9) a self-clearing strobe — the model never
 *   sets them, so writes are ignored and both read 0; bits 15:13 and
 *   31:17 are reserved and read 0, so the FSW's SPW_open clear write
 *   0xFFFE01E0 reads back 0 (SPW.c:531-542).
 */
#define SPW_STATUS_RUN 0xA00000u
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
     * TMTC transport state (D-N); the fds are -1 while the server is
     * off
     */
    StixTmtc tmtc;
};

static uint8_t buffer[8192];

/*
 * TC bytes from the TCP transport reach the device here (BQL held).
 * Unit reassembly across recv chunks is P2-6; until then the bytes
 * are traced and dropped, as in the baseline.
 */
static void recv_packet_callback(uint8_t *data, int length, void *opaque)
{
    trace_stixspw_recv_packet_callback();
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
    trace_stixspw_rx_deliver(len, newctrl, s->rx_index);

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
    if (s->rx_count == 0) {
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
    if (s->rx_count == 0) {
        return;
    }
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
    case 1: /* status: W1C, LS stays RUN */
        stixspw->reg.status &= ~(SPW_STATUS_W1C_MASK & val);
        break;
    case 8: /* dmaControl: W1C + latch, reserved bits read 0 */
        updated = stixspw->reg.dmaControl;

        updated &= ~(SPW_DMACONTROL_W1C_MASK & val);
        updated = (updated & ~SPW_DMACONTROL_LATCH_MASK) |
                  (val & SPW_DMACONTROL_LATCH_MASK);
        updated &= ~SPW_DMACONTROL_RESERVED_MASK;
        stixspw->reg.dmaControl = updated;

        /* the exchange runs only on a dmaControl write with TE set */
        if (val & (1u << STIXSPW_DMACONTROL_TE)) {
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
}

static void grlib_stixspw_unrealize(DeviceState *dev)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(dev);

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
    stixspw_rx_deliver(s, buf, (int)len);
}

static void grlib_stixspw_instance_init(Object *obj)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(obj);

    /* no fd 0: the transport is off until realize starts it */
    STIXSPW->tmtc.listen_fd = -1;
    STIXSPW->tmtc.client_fd = -1;

    object_property_add_uint32_ptr(obj, "tm-dropped",
                                   &STIXSPW->tmtc.tm_dropped,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "tmtc-connected",
                             grlib_stixspw_get_tmtc_connected,
                             NULL);
    object_property_add_uint32_ptr(obj, "rx-dropped", &STIXSPW->rx_dropped,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "rx-inject", NULL,
                            grlib_stixspw_set_rx_inject);
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
}

static const Property grlib_stixspw_properties[] = {
    DEFINE_PROP_ON_OFF_AUTO("tmtc-server", STIXSPW, tmtc_server, ON_OFF_AUTO_AUTO),
    DEFINE_PROP_UINT32("tmtc-port", STIXSPW, tmtc_port, 0),
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