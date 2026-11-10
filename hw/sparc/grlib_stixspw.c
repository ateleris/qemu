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

OBJECT_DECLARE_SIMPLE_TYPE(STIXSPW, GRLIB_STIXSPW)

typedef struct
{
    volatile uint32_t ctrl;
    volatile uint32_t haddr;
    volatile uint32_t dlen;
    volatile uint32_t daddr;

} spwTXdsc_s;

/** Definition of receive descriptor */
typedef struct
{
    volatile uint32_t ctrl;
    volatile uint32_t daddr;

} spwRXdsc_s;

struct STIXSPW
{
    SysBusDevice parent_obj;

    stixSpwRegs_s reg;

    OnOffAuto tmtc_server; /* auto = on, except under qtest (hermetic tests) */
    uint32_t tmtc_port; /* 0 = dynamic: IO_TCP_SERVER_BASE_PORT + getuid() */
    MemoryRegion iomem;
    qemu_irq irq;
};

static uint8_t buffer[8192];

/**  Passes TC received by the TCP server to TSIM through memory read, signalized with IRQ */
static void recv_packet_callback(uint8_t *data, int length, void *opaque)
{
    trace_stixspw_recv_packet_callback();

    /*STIXSPW *stixspw = opaque;

    int ix;
    spwRXdsc_s rxdsc;
    uint32_t rxdsc_addr = stixspw->reg.dmaRxDescAddr + (sizeof(spwRXdsc_s) * rxIndex);

    // Skip header
    data += 8;
    length -= 8;

    if (length <= 0)
    {
        qemu_printf("recv_packet_callback: Not valid packet received\n");
        return;
    }

    if (length == 4 && data[0] == ((SPW_ESC_CHAR << 2) & SPW_TIME_CODE_PARITY_AND_CONTROL))
    {
        //
        // Receive timecode
        //
        stixspw->reg.time = swap32(*(uint32_t *)data) & SPW_TIME_CODE_TIME_MASK;
        stixspw->reg.status |= (1 << STIXSPW_STATUS_TO);
        qemu_printf("RX timecode: %d  datalen: %d\n", stixspw->reg.time, length);
    }
    else
    {
        //
        // Receive data
        //

        // Swap received dwords
        uint32_t *data32 = (uint32_t *)data;
        for (ix = 0; ix < length / 4 + length % 4; ix++)
        {
            data32[ix] = swap32(data32[ix]);
        }

        // Read RX descriptor
        tsim_read_cpu_mem(rxdsc_addr, &rxdsc, sizeof(rxdsc));

        if (length > stixspw->reg.dmaMaxRxLength)
        {
            // Received packet was truncated
            length = 0;
            rxdsc.ctrl |= RXDSC_TR;
        }

        qemu_printf("Recv packet[%d] len: %d   rxdsc_addr: 0x%X   ctrl: 0x%X\n", rxIndex, length, rxdsc_addr, rxdsc.ctrl);

        // Write received data to RX buffer
        tsim_write_cpu_mem(rxdsc.daddr, data, length);

        // Increment RX index
        if (rxdsc.ctrl & (1 << RXDSC_WR))
        {
            rxIndex = 0;
        }
        else
        {
            rxIndex++;
        }

        // Save received packet length to control register
        rxdsc.ctrl |= length < 8 ? 8 : length & 0x1FFFFFF; // lenght must be >= 8
        rxdsc.ctrl &= ~(1 << RXDSC_EN);

        // Write RX descriptor
        tsim_write_cpu_mem(rxdsc_addr, &rxdsc, sizeof(rxdsc));

        // Set received packet flag
        stixspw->reg.dmaControl |= (1 << STIXSPW_DMACONTROL_PR);
    }

    // generate interrupt
    qemu_irq_pulse(stixspw->irq);*/
}

#define STIXSPW_REG_WORDS (sizeof(stixSpwRegs_s) / sizeof(uint32_t))

static uint64_t grlib_stixspw_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXSPW *stixspw = opaque;

    /* security fix: the MMIO window (256 bytes) is larger than the register
     * struct (48 bytes); offsets past the struct must not index host memory.
     * The pinned baseline flows only ever access 0x00-0x2F. */
    if ((uint32_t)addr / sizeof(uint32_t) >= STIXSPW_REG_WORDS)
    {
        trace_stixspw_io_read_outside(addr);
        return 0;
    }

    uint32_t regval = ((volatile uint32_t *)&stixspw->reg)[(uint32_t)addr / sizeof(uint32_t)];

    trace_stixspw_io_read(addr, regval);

    return regval;
}

static void grlib_stixspw_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXSPW *stixspw = opaque;

    /* security fix: same bounds rule as the read handler. */
    if ((uint32_t)addr / sizeof(uint32_t) >= STIXSPW_REG_WORDS)
    {
        trace_stixspw_io_write_outside(addr);
        return;
    }

    // update the register as per write request
    ((volatile uint32_t *)&stixspw->reg)[(uint32_t)addr / sizeof(uint32_t)] = (uint32_t)value;

    volatile uint32_t *regs = (volatile uint32_t *)&stixspw->reg; /* re-read */

    trace_stixspw_io_write(addr, regs[(uint32_t)addr / sizeof(uint32_t)]);
    trace_stixspw_dma_control(stixspw->reg.dmaControl);

    // shortcut for link running as per E_SPW_LINK_STATUS_RUN
    stixspw->reg.status = 0xA00000;

    // clear the reset flag
    stixspw->reg.control &= ~(1 << STIXSPW_CONTROL_RS);

    // used later e.g., to signal transmit OK
    uint32_t dma_control_modifier_mask = 0;

    // clear the DMA control register
    if (stixspw->reg.dmaControl != 0)
    {
        trace_stixspw_data_exchange();

        // is transmit enabled?
        if (stixspw->reg.dmaControl & (1 << STIXSPW_DMACONTROL_TE))
        {
            spwTXdsc_s txdsc;

            // read TX descriptor from STIX
            trace_stixspw_tx_desc_addr(stixspw->reg.dmaTxDescAddr);

            // endianness is incorrect for use in qemu; there may be a better way with address_space_read, but it's not working at the moment.
            // using a small workaround
            physical_memory_read(stixspw->reg.dmaTxDescAddr, &txdsc, sizeof(txdsc));

            txdsc.ctrl = bswap32(txdsc.ctrl);
            txdsc.daddr = bswap32(txdsc.daddr);
            txdsc.dlen = bswap32(txdsc.dlen);
            txdsc.haddr = bswap32(txdsc.haddr);

            trace_stixspw_tx_descriptor(txdsc.ctrl, txdsc.daddr,
                                        txdsc.dlen, txdsc.haddr);

            if (txdsc.dlen < sizeof(buffer))
            {
                memset(buffer, 0xFF, sizeof(buffer));
                physical_memory_read(txdsc.daddr, buffer, txdsc.dlen);

                // swap buffer dwords to fix endianness
                uint32_t *buffer32 = (uint32_t *)buffer;
                trace_stixspw_tx_bytes(txdsc.dlen);
                for (uint32_t ix = 0; ix < txdsc.dlen / 4 + ((txdsc.dlen % 4) ? 1 : 0); ix++)
                {
                    // buffer32[ix] = bswap32(buffer32[ix]);
                    trace_stixspw_tx_dump_word(bswap32(buffer32[ix]));
                }

                send_tm_packet(buffer, txdsc.dlen);

                trace_stixspw_tx_send(txdsc.dlen);
            }
            else
            {
                trace_stixspw_tx_overflow();
            }

            // clear desc enabled flag
            txdsc.ctrl &= ~(1 << TXDSC_EN);

            // before using the exchange variable, correct for endienness
            txdsc.ctrl = bswap32(txdsc.ctrl);
            txdsc.daddr = bswap32(txdsc.daddr);
            txdsc.dlen = bswap32(txdsc.dlen);
            txdsc.haddr = bswap32(txdsc.haddr);

            // write TX decriptor to STIX
            physical_memory_write(stixspw->reg.dmaTxDescAddr, &txdsc, sizeof(txdsc));

            dma_control_modifier_mask = (1 << STIXSPW_DMACONTROL_PS);

            // generate interrupt
            qemu_irq_pulse(stixspw->irq);
        }

        // clear DMA control register
        stixspw->reg.dmaControl = dma_control_modifier_mask;
    }
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
    STIXSPW->reg.status = 0xA00000;

    /* tmtc-server=auto keeps the server off whenever the qtest harness
     * drives this QEMU (the -qtest chardev is connected, whatever the
     * accelerator: some qtests boot real code under TCG): no qtest attaches
     * a TM client, and the default port (20000+uid) must stay free for the
     * boot gates and a live FSW session. With the server off, TM sends
     * drop silently, as they do today with no client. */
    if (STIXSPW->tmtc_server == ON_OFF_AUTO_ON ||
        (STIXSPW->tmtc_server == ON_OFF_AUTO_AUTO && !qtest_driver()))
    {
        if (!init_tmtc_server(recv_packet_callback, STIXSPW, STIXSPW->tmtc_port, errp))
        {
            return;
        }
    }
}

static void grlib_stixspw_reset(DeviceState *d)
{
    // STIXSPW *STIXSPW = grlib_stixspw(d);
}

static const Property grlib_stixspw_properties[] = {
    DEFINE_PROP_ON_OFF_AUTO("tmtc-server", STIXSPW, tmtc_server, ON_OFF_AUTO_AUTO),
    DEFINE_PROP_UINT32("tmtc-port", STIXSPW, tmtc_port, 0),
};

static void grlib_stixspw_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixspw_realize;
    device_class_set_legacy_reset(dc, grlib_stixspw_reset);
    device_class_set_props(dc, grlib_stixspw_properties);
}

static const TypeInfo grlib_stixspw_info = {
    .name = TYPE_GRLIB_STIXSPW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXSPW),
    .class_init = grlib_stixspw_class_init,
};

static void grlib_stixspw_register_types(void)
{
    type_register_static(&grlib_stixspw_info);
}

type_init(grlib_stixspw_register_types)