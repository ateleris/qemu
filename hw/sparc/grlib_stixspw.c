#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sparc/grlib.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qemu/qemu-print.h"
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

    MemoryRegion iomem;
    qemu_irq irq;
};

static uint8_t buffer[8192];

/**  Passes TC received by the TCP server to TSIM through memory read, signalized with IRQ */
static void recv_packet_callback(uint8_t *data, int length)
{
    qemu_printf("recv_packet_callback\n");
  /*int ix;
  spwRXdsc_s rxdsc;
  uint32_t rxdsc_addr = spwRegs.dmaRxDescAddr + (sizeof(spwRXdsc_s) * rxIndex);

  // Skip header
  data += 8;
  length -= 8;

  if (length <= 0)
  {
     fprintf(stderr, "%s: Not valid packet received\n", __FUNCTION__);
     return;
  }

  if (length == 4 && data[0] == ((SPW_ESC_CHAR << 2) & SPW_TIME_CODE_PARITY_AND_CONTROL))
  {
    //
    // Receive timecode
    //  
    spwRegs.time = swap32(*(uint32_t *)data) & SPW_TIME_CODE_TIME_MASK;
    spwRegs.status |= (1 << STIXSPW_STATUS_TO);
    PRINTF("RX timecode: %d  datalen: %d\n", spwRegs.time, length);
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
    tsim_read_cpu_mem(rxdsc_addr , &rxdsc, sizeof(rxdsc)); 

    if (length > spwRegs.dmaMaxRxLength)
    {
       // Received packet was truncated 
       length = 0;
       rxdsc.ctrl |= RXDSC_TR;
    }

    PRINTF("Recv packet[%d] len: %d   rxdsc_addr: 0x%X   ctrl: 0x%X\n", rxIndex, length, rxdsc_addr,  rxdsc.ctrl);

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
    rxdsc.ctrl |= length < 8 ? 8 : length & 0x1FFFFFF;   // lenght must be >= 8
    rxdsc.ctrl &= ~(1 << RXDSC_EN);

    // Write RX descriptor
    tsim_write_cpu_mem(rxdsc_addr, &rxdsc, sizeof(rxdsc));

    // Set received packet flag
    spwRegs.dmaControl |= (1 << STIXSPW_DMACONTROL_PR);

  }

  // Generate IRQ
  ioif.set_irq(IRQ_SPW0_IOM, 0);*/
}

static uint64_t grlib_stixspw_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXSPW *stixspw = opaque;

    uint32_t regval = ((volatile uint32_t *)&stixspw->reg)[(uint32_t)addr/sizeof(uint32_t)];

    qemu_printf("SPW | Read requested: addr = %lu, regval = 0x%x\n", //, chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x, command = %u, status = 0x%x\n",
                addr, regval);                                       //, stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, (uint32_t)stixflash->ram_address, stixflash->command, stixflash->cmd_sta);

    return regval;
}

static void grlib_stixspw_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXSPW *stixspw = opaque;

    // update the register as per write request
    ((volatile uint32_t *)&stixspw->reg)[(uint32_t)addr/sizeof(uint32_t)] = (uint32_t)value;

    qemu_printf("SPW | Write completed addr = %lu, new regval = 0x%x\n", addr, ((volatile uint32_t *)&stixspw->reg)[(uint32_t)addr/sizeof(uint32_t)]);
    qemu_printf("dma control %u\n", stixspw->reg.dmaControl);

    // shortcut for link running as per E_SPW_LINK_STATUS_RUN
    stixspw->reg.status = 0xA00000;

    // clear the reset flag
    stixspw->reg.control &= ~(1 << STIXSPW_CONTROL_RS);

    // used later e.g., to signal transmit OK
    uint32_t dma_control_modifier_mask = 0;

    // clear the DMA control register
    if (stixspw->reg.dmaControl != 0)
    {
        qemu_printf("STIX SPW: Data exchange requested\n");

        // is transmit enabled?
        if (stixspw->reg.dmaControl & (1 << STIXSPW_DMACONTROL_TE))
        {
            spwTXdsc_s txdsc;

            // read TX descriptor from STIX
            qemu_printf("stixspw->reg.dmaTxDescAddr = 0x%x\n", stixspw->reg.dmaTxDescAddr);

            // endianness is incorrect for use in qemu; there may be a better way with address_space_read, but it's not working at the moment.
            // using a small workaround           
            cpu_physical_memory_read(stixspw->reg.dmaTxDescAddr, &txdsc, sizeof(txdsc));

            txdsc.ctrl = bswap32(txdsc.ctrl);
            txdsc.daddr = bswap32(txdsc.daddr);
            txdsc.dlen = bswap32(txdsc.dlen);
            txdsc.haddr = bswap32(txdsc.haddr);

            qemu_printf("txdsc.ctrl = %u, txdsc.daddr = 0x%x, txdsc.dlen = %u, txdsc.haddr = 0x%x\n", txdsc.ctrl, txdsc.daddr, txdsc.dlen, txdsc.haddr);

            if (txdsc.dlen < sizeof(buffer))
            {
                memset(buffer, 0xFF, sizeof(buffer));
                cpu_physical_memory_read(txdsc.daddr, buffer, txdsc.dlen);

                // swap buffer dwords to fix endianness
                uint32_t *buffer32 = (uint32_t *)buffer;
                qemu_printf("\n\n\nUse %u bytes\n\n", txdsc.dlen);
                for (uint32_t ix = 0; ix < txdsc.dlen / 4 + ((txdsc.dlen % 4) ? 1 : 0); ix++)
                {
                    //buffer32[ix] = bswap32(buffer32[ix]);
                    qemu_printf("%08x ", bswap32(buffer32[ix]));
                }
                qemu_printf("\n\n\n");

                send_tm_packet(buffer, txdsc.dlen);

                qemu_printf("Send packet len: %d\n", txdsc.dlen);
            }
            else
            {
                qemu_printf("error: buffer overwflow\n");
            }

            // clear desc enabled flag
            txdsc.ctrl &= ~(1 << TXDSC_EN);

            // before using the exchange variable, correct for endienness
            txdsc.ctrl = bswap32(txdsc.ctrl);
            txdsc.daddr = bswap32(txdsc.daddr);
            txdsc.dlen = bswap32(txdsc.dlen);
            txdsc.haddr = bswap32(txdsc.haddr);

            // write TX decriptor to STIX
            cpu_physical_memory_write(stixspw->reg.dmaTxDescAddr, &txdsc, sizeof(txdsc));

            dma_control_modifier_mask = (1 << STIXSPW_DMACONTROL_PS);

            // generate interrupt
            qemu_irq_pulse(stixspw->irq);
        }

        // clear DMA control register
        stixspw->reg.dmaControl = dma_control_modifier_mask;
    }

    /*
        switch (addr)
        {
        case 0: // control
            if(value & (1 << STIXSPW_CONTROL_RS)) // request reset
            {
                // reset to zero to indicate correct reset, could also be faked to take a while if need be by letting it repeat a few times
                *((uint32_t *)(&stixspw->reg + (uint32_t)addr)) &= ~(1 << STIXSPW_CONTROL_RS);
            }
            break;
        case 8: // nodeAddress
            // noop
            break;
        case 12: // clockDivisor
            // noop
            break;
        case 24: // timerDisc
            // noop
            break;
        case 32: // dmaControl
            break;
        case 36: // dmaMaxRxLength
            // noop
            break;
        default:
            qemu_printf("SPW | ADDRESS UNKNOWN = %lu\n", addr);
            assert(0);
        }*/

    //qemu_printf("SPW | Write completed addr = %lu, new regval = 0x%x\n", addr, (uint32_t)value); //: chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x command = %u, status = 0x%x\n",
                                                                                                 // stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, (uint32_t)stixflash->ram_address, stixflash->command, stixflash->cmd_sta);
}

static const MemoryRegionOps grlib_stixspw_ops = {
    .write = grlib_stixspw_write,
    .read = grlib_stixspw_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
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

    init_tmtc_server(recv_packet_callback);
}

static void grlib_stixspw_reset(DeviceState *d)
{
    // STIXSPW *STIXSPW = grlib_stixspw(d);
}

static void grlib_stixspw_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixspw_realize;
    dc->reset = grlib_stixspw_reset;
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