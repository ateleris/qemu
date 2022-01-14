// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

/** @file StixIdpu.h
Hardware addresses definitions
This file contains special function register and region definitions (memory map)
for StixIdpu.
*/

#ifndef __STIXIDPU_H__
#define __STIXIDPU_H__

/* Special Constants */
#define TECHNOLOGY                                             0
#define SYSTEM_BUILD                                           1013
#define SYSTEM_RELEASE                                         34
#define USEEEPROMTYPEBIT                                       0
#define SPW0NODEADDR                                           1
#define SPW1NODEADDR                                           2
#define SPWDESTKEY                                             0
#define SYSTEM_CLOCK                                           20000000
#define NAHBMASTERS                                            5
#define NAHBSLAVES                                             7
#define NAPBSLAVES                                             10
#define PROT_CHANNEL_NUMBER                                    9
#define ROTB_MAX_BLOCKS                                        104448
#define UNLOCKING_TOKEN                                        165
#define RESET_TOKEN                                            165
#define WDOG_RELOAD                                            100000000
#define WDOG_DISABLED                                          1

/* Generic Mappings */
#define AHBCTRL_CFGADDR                                        16#FF0#
#define AHBCTRL_CFGMASK                                        16#FF0#
#define AHBCTRL_DEBUG                                          2
#define AHBCTRL_DEFMAST                                        0
#define AHBCTRL_DEVID                                          0
#define AHBCTRL_DISIRQ                                         0
#define AHBCTRL_ENBUSMON                                       0
#define AHBCTRL_FIXBRST                                        0
#define AHBCTRL_FPNPEN                                         0
#define AHBCTRL_HELPURL                                        V:\library\virtual\datasheets\grip.pdf#page=37
#define AHBCTRL_ICHECK                                         1
#define AHBCTRL_IOADDR                                         16#FFF#
#define AHBCTRL_IOEN                                           1
#define AHBCTRL_IOMASK                                         16#FFF#
#define AHBCTRL_NAHBM                                          5
#define AHBCTRL_NAHBS                                          7
#define AHBCTRL_RROBIN                                         1
#define AHBCTRL_SPLIT                                          0
#define AHBCTRL_TIMEOUT                                        0
#define AHBJTAG_AINST                                          2
#define AHBJTAG_DINST                                          3
#define AHBJTAG_HELPURL                                        V:\library\virtual\datasheets\grip.pdf#page=44
#define AHBJTAG_HINDEX                                         4
#define AHBJTAG_IDCODE                                         9
#define AHBJTAG_MANF                                           804
#define AHBJTAG_NSYNC                                          2
#define AHBJTAG_OEPOL                                          1
#define AHBJTAG_PART                                           0
#define AHBJTAG_SCANTEST                                       0
#define AHBJTAG_TCKNEN                                         0
#define AHBJTAG_TECH                                           inferred
#define AHBJTAG_VER                                            1
#define APBCTRL_ASSERTERR                                      0
#define APBCTRL_ASSERTWARN                                     0
#define APBCTRL_CCHECK                                         1
#define APBCTRL_DEBUG                                          2
#define APBCTRL_ENBUSMON                                       0
#define APBCTRL_HADDR                                          16#800#
#define APBCTRL_HELPURL                                        V:\library\virtual\datasheets\grip.pdf#page=75
#define APBCTRL_HINDEX                                         2
#define APBCTRL_HMASK                                          16#FFF#
#define APBCTRL_ICHECK                                         1
#define APBCTRL_MCHECK                                         1
#define APBCTRL_NSLAVES                                        10
#define APBCTRL_PSLVDISABLE                                    0
#define CLKDIV0_DIV                                            5
#define CLKDIV1_DIV                                            2
#define CPU_BP                                                 0
#define CPU_CACHED                                             0
#define CPU_CEINJ                                              0
#define CPU_CMFT                                               1
#define CPU_CP                                                 0
#define CPU_DCEN                                               1
#define CPU_DISAS                                              0
#define CPU_DLRAM                                              0
#define CPU_DLRAMSIZE                                          1
#define CPU_DLRAMSTART                                         16#8F#
#define CPU_DREPL                                              0
#define CPU_DSETLOCK                                           0
#define CPU_DSETS                                              1
#define CPU_DSETSIZE                                           1
#define CPU_DSNOOP                                             0
#define CPU_DSU                                                1
#define CPU_DTLBNUM                                            8
#define CPU_FABTECH                                            FABTECH
#define CPU_FPFT                                               0
#define CPU_FPU                                                0
#define CPU_HELPURL                                            V:\library\virtual\datasheets\grip.pdf#page=779
#define CPU_HINDEX                                             0
#define CPU_ICEN                                               1
#define CPU_ILINESIZE                                          4
#define CPU_ILRAM                                              0
#define CPU_ILRAMSIZE                                          1
#define CPU_ILRAMSTART                                         16#8E#
#define CPU_IREPL                                              0
#define CPU_ISETLOCK                                           0
#define CPU_ISETS                                              1
#define CPU_ISETSIZE                                           1
#define CPU_ITLBNUM                                            8
#define CPU_IUFT                                               3
#define CPU_LDDEL                                              2
#define CPU_MAC                                                0
#define CPU_MEMTECH                                            MEMTECH
#define CPU_MMUEN                                              0
#define CPU_MMUPGSZ                                            0
#define CPU_NETLIST                                            0
#define CPU_NOTAG                                              0
#define CPU_NWINDOWS                                           8
#define CPU_NWP                                                0
#define CPU_PCLOW                                              2
#define CPU_PWD                                                1
#define CPU_RSTADDR                                            0
#define CPU_SMP                                                0
#define CPU_SVT                                                0
#define CPU_TBUF                                               0
#define CPU_TLB_REP                                            0
#define CPU_TLB_TYPE                                           1
#define CPU_V8                                                 16#32#
#define DSU_HADDR                                              16#900#
#define DSU_HELPURL                                            V:\library\virtual\datasheets\grip.pdf#page=200
#define DSU_HINDEX                                             0
#define DSU_HMASK                                              16#F00#
#define DSU_KBYTES                                             0
#define DSU_NCPU                                               1
#define DSU_TBITS                                              30
#define DSU_TECH                                               MEMTECH
#define EEPROM_HADDR                                           16#000#
#define EEPROM_HINDEX                                          5
#define EEPROM_HMASK                                           16#FFF#
#define EEPROM_PADDR                                           9
#define EEPROM_PINDEX                                          9
#define EEPROM_PMASK                                           16#FFF#
#define EEPROM_USETYPEBIT                                      0
#define FLASH_HELPURL                                          V:\library\virtual\datasheets\grip.pdf#page=75
#define FLASH_HINDEX                                           1
#define FLASH_PADDR                                            8
#define FLASH_PINDEX                                           8
#define FLASH_PIRQ                                             1
#define FLASH_PMASK                                            16#FFF#
#define IRQCTRL_EIRQ                                           0
#define IRQCTRL_HELPURL                                        V:\library\virtual\datasheets\grip.pdf#page=709
#define IRQCTRL_NCPU                                           1
#define IRQCTRL_PADDR                                          2
#define IRQCTRL_PINDEX                                         2
#define IRQCTRL_PMASK                                          16#FFF#
#define RSTGEN_ACTHIGH                                         0
#define RSTGEN_HELPURL                                         V:\library\virtual\datasheets\grip.pdf#page=945
#define RSTGEN_SCANEN                                          0
#define RSTGEN_SYNCRST                                         0
#define SDRAM_HADDR                                            16#400#
#define SDRAM_HINDEX                                           3
#define SDRAM_HMASK                                            16#FC0#
#define SDRAM_MEMAVAILABLE                                     53687088
#define SDRAM_PADDR                                            12
#define SDRAM_PINDEX                                           12
#define SDRAM_PMASK                                            16#FFF#
#define SPW_DESTKEY                                            0
#define SPW_FABTECH                                            FABTECH
#define SPW_HINDEX                                             2
#define SPW_MEMTECH                                            MEMTECH
#define SPW_NODEADDR                                           1
#define SPW_PADDR                                              10
#define SPW_PINDEX                                             10
#define SPW_PIRQ                                               13
#define SPW_PMASK                                              16#FFF#
#define SPW_SYSFREQ                                            20000000
#define STIXCTRL_HADDR                                         16#E00#
#define STIXCTRL_HINDEX                                        4
#define STIXCTRL_HMASK                                         16#FF0#
#define STIXCTRL_PADDR                                         5
#define STIXCTRL_PINDEX                                        5
#define STIXCTRL_PIRQ                                          3
#define STIXCTRL_PMASK                                         16#FFF#
#define STIXCTRL_PROT_CHANNEL_NUMBER                           9
#define STIXCTRL_ROTB_MAX_BLOCKS                               104448
#define STIXCTRL_SYSTEM_BUILD                                  242
#define STIXCTRL_SYSTEM_RELEASE                                34
#define STIXCTRL_TECHNOLOGY                                    apa3e
#define STIXCTRL_UNLOCKING_TOKEN                               165
#define STIXCTRL_WDOG_DISABLED                                 1
#define STIXCTRL_WDOG_RELOAD                                   100000000
#define TIMER_EWDOGEN                                          0
#define TIMER_HELPURL                                          V:\library\virtual\datasheets\grip.pdf#page=327
#define TIMER_NBITS                                            32
#define TIMER_NTIMERS                                          2
#define TIMER_PADDR                                            3
#define TIMER_PINDEX                                           3
#define TIMER_PIRQ                                             10
#define TIMER_PMASK                                            16#FFF#
#define TIMER_SBITS                                            16
#define TIMER_SEPIRQ                                           1
#define TIMER_WDOG                                             0
#define UART_ABITS                                             8
#define UART_CONSOLE                                           0
#define UART_FIFOSIZE                                          1
#define UART_FLOW                                              0
#define UART_HELPURL                                           V:\library\virtual\datasheets\grip.pdf#page=89
#define UART_PADDR                                             1
#define UART_PARITY                                            0
#define UART_PINDEX                                            1
#define UART_PIRQ                                              2
#define UART_PMASK                                             16#FFF#
#define UART_SBITS                                             12

/* Base addresses */
#define EEPROM_AHB_BASE                                        0x00000000
#define SDRAM_AHB_BASE                                         0x40000000
#define UART_APB_BASE                                          0x80000100
#define IRQCTRL_APB_BASE                                       0x80000200
#define TIMER_APB_BASE                                         0x80000300
#define STIXCTRL_APB_BASE                                      0x80000500
#define FLASH_APB_BASE                                         0x80000800
#define EEPROM_APB_BASE                                        0x80000900
#define SPW_APB_BASE                                           0x80000A00
#define SDRAM_APB_BASE                                         0x80000C00
#define DSU_AHB_BASE                                           0x90000000
#define STIXCTRL_AHB_BASE                                      0xE0000000

/* Regions */
#define EEPROM_AHB_START                                       0x00000000
#define EEPROM_AHB_SIZE                                        0x00040000
#define EEPROM_AHB_END                                         0x0003FFFF
#define EEPROM_AHB                                             ((uint32_t*) EEPROM_AHB_START)

#define SDRAM_AVAILABLE_START                                  0x40000000
#define SDRAM_AVAILABLE_SIZE                                   0x03300000
#define SDRAM_AVAILABLE_END                                    0x432FFFFF
#define SDRAM_AVAILABLE                                        ((uint8_t*) SDRAM_AVAILABLE_START)

#define APBCTRL_AHB_START                                      0x80000000
#define APBCTRL_AHB_SIZE                                       0x00100000
#define APBCTRL_AHB_END                                        0x800FFFFF
#define APBCTRL_AHB                                            ((uint8_t*) APBCTRL_AHB_START)

#define DSU_AHB_START                                          0x90000000
#define DSU_AHB_SIZE                                           0x10000000
#define DSU_AHB_END                                            0x9FFFFFFF
#define DSU_AHB                                                ((uint8_t*) DSU_AHB_START)

#define STIXCTRL_SRAM_START                                    0xE0000000
#define STIXCTRL_SRAM_SIZE                                     0x00200000
#define STIXCTRL_SRAM_END                                      0xE01FFFFF
#define STIXCTRL_SRAM                                          ((uint8_t*) STIXCTRL_SRAM_START)

#define STIXCTRL_ELUT_START                                    0xE0000000
#define STIXCTRL_ELUT_SIZE                                     0x00180000
#define STIXCTRL_ELUT_END                                      0xE017FFFF
#define STIXCTRL_ELUT                                          ((uint8_t*) STIXCTRL_ELUT_START)

#define STIXCTRL_LACC0_START                                   0xE0180000
#define STIXCTRL_LACC0_SIZE                                    0x00006000
#define STIXCTRL_LACC0_END                                     0xE0185FFF
#define STIXCTRL_LACC0                                         ((uint16_t*) STIXCTRL_LACC0_START)

#define STIXCTRL_LACC1_START                                   0xE0188000
#define STIXCTRL_LACC1_SIZE                                    0x00006000
#define STIXCTRL_LACC1_END                                     0xE018DFFF
#define STIXCTRL_LACC1                                         ((uint16_t*) STIXCTRL_LACC1_START)

#define STIXCTRL_LACC2_START                                   0xE0190000
#define STIXCTRL_LACC2_SIZE                                    0x00006000
#define STIXCTRL_LACC2_END                                     0xE0195FFF
#define STIXCTRL_LACC2                                         ((uint16_t*) STIXCTRL_LACC2_START)

#define STIXCTRL_LACC3_START                                   0xE0198000
#define STIXCTRL_LACC3_SIZE                                    0x00006000
#define STIXCTRL_LACC3_END                                     0xE019DFFF
#define STIXCTRL_LACC3                                         ((uint16_t*) STIXCTRL_LACC3_START)

#define STIXCTRL_EACC0_START                                   0xE01A0000
#define STIXCTRL_EACC0_SIZE                                    0x00006000
#define STIXCTRL_EACC0_END                                     0xE01A5FFF
#define STIXCTRL_EACC0                                         ((uint16_t*) STIXCTRL_EACC0_START)

#define STIXCTRL_EACC1_START                                   0xE01A8000
#define STIXCTRL_EACC1_SIZE                                    0x00006000
#define STIXCTRL_EACC1_END                                     0xE01ADFFF
#define STIXCTRL_EACC1                                         ((uint16_t*) STIXCTRL_EACC1_START)

#define STIXCTRL_TLUT0_START                                   0xE01B8000
#define STIXCTRL_TLUT0_SIZE                                    0x00000600
#define STIXCTRL_TLUT0_END                                     0xE01B85FF
#define STIXCTRL_TLUT0                                         ((uint32_t*) STIXCTRL_TLUT0_START)

#define STIXCTRL_TLUT1_START                                   0xE01B9000
#define STIXCTRL_TLUT1_SIZE                                    0x00000600
#define STIXCTRL_TLUT1_END                                     0xE01B95FF
#define STIXCTRL_TLUT1                                         ((uint32_t*) STIXCTRL_TLUT1_START)

#define STIXCTRL_LVAR0_START                                   0xE0200000
#define STIXCTRL_LVAR0_SIZE                                    0x000000A0
#define STIXCTRL_LVAR0_END                                     0xE020009F
#define STIXCTRL_LVAR0                                         ((uint32_t*) STIXCTRL_LVAR0_START)

#define STIXCTRL_LVAR1_START                                   0xE0200100
#define STIXCTRL_LVAR1_SIZE                                    0x000000A0
#define STIXCTRL_LVAR1_END                                     0xE020019F
#define STIXCTRL_LVAR1                                         ((uint32_t*) STIXCTRL_LVAR1_START)

#define STIXCTRL_LVAR2_START                                   0xE0200200
#define STIXCTRL_LVAR2_SIZE                                    0x000000A0
#define STIXCTRL_LVAR2_END                                     0xE020029F
#define STIXCTRL_LVAR2                                         ((uint32_t*) STIXCTRL_LVAR2_START)

#define STIXCTRL_LVAR3_START                                   0xE0200300
#define STIXCTRL_LVAR3_SIZE                                    0x000000A0
#define STIXCTRL_LVAR3_END                                     0xE020039F
#define STIXCTRL_LVAR3                                         ((uint32_t*) STIXCTRL_LVAR3_START)

#define STIXCTRL_LTRG0_START                                   0xE0400000
#define STIXCTRL_LTRG0_SIZE                                    0x00000040
#define STIXCTRL_LTRG0_END                                     0xE040003F
#define STIXCTRL_LTRG0                                         ((uint32_t*) STIXCTRL_LTRG0_START)

#define STIXCTRL_LTRG1_START                                   0xE0400040
#define STIXCTRL_LTRG1_SIZE                                    0x00000040
#define STIXCTRL_LTRG1_END                                     0xE040007F
#define STIXCTRL_LTRG1                                         ((uint32_t*) STIXCTRL_LTRG1_START)

#define STIXCTRL_LTRG2_START                                   0xE0400080
#define STIXCTRL_LTRG2_SIZE                                    0x00000040
#define STIXCTRL_LTRG2_END                                     0xE04000BF
#define STIXCTRL_LTRG2                                         ((uint32_t*) STIXCTRL_LTRG2_START)

#define STIXCTRL_LTRG3_START                                   0xE04000C0
#define STIXCTRL_LTRG3_SIZE                                    0x00000040
#define STIXCTRL_LTRG3_END                                     0xE04000FF
#define STIXCTRL_LTRG3                                         ((uint32_t*) STIXCTRL_LTRG3_START)

#define STIXCTRL_ETRG0_START                                   0xE0600000
#define STIXCTRL_ETRG0_SIZE                                    0x00000040
#define STIXCTRL_ETRG0_END                                     0xE060003F
#define STIXCTRL_ETRG0                                         ((uint32_t*) STIXCTRL_ETRG0_START)

#define STIXCTRL_ETRG1_START                                   0xE0600040
#define STIXCTRL_ETRG1_SIZE                                    0x00000040
#define STIXCTRL_ETRG1_END                                     0xE060007F
#define STIXCTRL_ETRG1                                         ((uint32_t*) STIXCTRL_ETRG1_START)

#define STIXCTRL_BLOCK_START                                   0xE0800000
#define STIXCTRL_BLOCK_SIZE                                    0x00000200
#define STIXCTRL_BLOCK_END                                     0xE08001FF
#define STIXCTRL_BLOCK                                         ((uint32_t*) STIXCTRL_BLOCK_START)


/* Slices */
#define APBUART_CONTROL_BI                                     12
#define APBUART_CONTROL_BI_MASK                                0x1

#define APBUART_CONTROL_DB                                     11
#define APBUART_CONTROL_DB_MASK                                0x1

#define APBUART_CONTROL_DI                                     13
#define APBUART_CONTROL_DI_MASK                                0x1

#define APBUART_CONTROL_EC                                     8
#define APBUART_CONTROL_EC_MASK                                0x1

#define APBUART_CONTROL_FA                                     31
#define APBUART_CONTROL_FA_MASK                                0x1

#define APBUART_CONTROL_FL                                     6
#define APBUART_CONTROL_FL_MASK                                0x1

#define APBUART_CONTROL_LB                                     7
#define APBUART_CONTROL_LB_MASK                                0x1

#define APBUART_CONTROL_PE                                     5
#define APBUART_CONTROL_PE_MASK                                0x1

#define APBUART_CONTROL_PS                                     4
#define APBUART_CONTROL_PS_MASK                                0x1

#define APBUART_CONTROL_RE                                     0
#define APBUART_CONTROL_RE_MASK                                0x1

#define APBUART_CONTROL_RESERVED                               15
#define APBUART_CONTROL_RESERVED_MASK                          0xFFFF

#define APBUART_CONTROL_RF                                     10
#define APBUART_CONTROL_RF_MASK                                0x1

#define APBUART_CONTROL_RI                                     2
#define APBUART_CONTROL_RI_MASK                                0x1

#define APBUART_CONTROL_SI                                     14
#define APBUART_CONTROL_SI_MASK                                0x1

#define APBUART_CONTROL_TE                                     1
#define APBUART_CONTROL_TE_MASK                                0x1

#define APBUART_CONTROL_TF                                     9
#define APBUART_CONTROL_TF_MASK                                0x1

#define APBUART_CONTROL_TI                                     3
#define APBUART_CONTROL_TI_MASK                                0x1

#define APBUART_DATA_DATA                                      0
#define APBUART_DATA_DATA_MASK                                 0xFF

#define APBUART_FIFO_DATA                                      0
#define APBUART_FIFO_DATA_MASK                                 0xFF

#define APBUART_SCALER_RE                                      0
#define APBUART_SCALER_RE_MASK                                 0xFFF

#define APBUART_STATUS_BR                                      3
#define APBUART_STATUS_BR_MASK                                 0x1

#define APBUART_STATUS_DR                                      0
#define APBUART_STATUS_DR_MASK                                 0x1

#define APBUART_STATUS_FE                                      6
#define APBUART_STATUS_FE_MASK                                 0x1

#define APBUART_STATUS_OV                                      4
#define APBUART_STATUS_OV_MASK                                 0x1

#define APBUART_STATUS_PE                                      5
#define APBUART_STATUS_PE_MASK                                 0x1

#define APBUART_STATUS_RCNT                                    26
#define APBUART_STATUS_RCNT_MASK                               0x3F

#define APBUART_STATUS_RESERVED                                11
#define APBUART_STATUS_RESERVED_MASK                           0x1FF

#define APBUART_STATUS_RF                                      10
#define APBUART_STATUS_RF_MASK                                 0x1

#define APBUART_STATUS_RH                                      8
#define APBUART_STATUS_RH_MASK                                 0x1

#define APBUART_STATUS_TCNT                                    20
#define APBUART_STATUS_TCNT_MASK                               0x3F

#define APBUART_STATUS_TE                                      2
#define APBUART_STATUS_TE_MASK                                 0x1

#define APBUART_STATUS_TF                                      9
#define APBUART_STATUS_TF_MASK                                 0x1

#define APBUART_STATUS_TH                                      7
#define APBUART_STATUS_TH_MASK                                 0x1

#define APBUART_STATUS_TS                                      1
#define APBUART_STATUS_TS_MASK                                 0x1

#define FTEEPROMCTRL_STATUS_EDACON                             30
#define FTEEPROMCTRL_STATUS_EDACON_MASK                        0x1

#define FTEEPROMCTRL_STATUS_EDACRESET                          29
#define FTEEPROMCTRL_STATUS_EDACRESET_MASK                     0x1

#define FTEEPROMCTRL_STATUS_ERRORCOUNT                         0
#define FTEEPROMCTRL_STATUS_ERRORCOUNT_MASK                    0xFFFF

#define FTEEPROMCTRL_STATUS_FLASHMODE                          16
#define FTEEPROMCTRL_STATUS_FLASHMODE_MASK                     0x1

#define FTEEPROMCTRL_STATUS_RESERVED                           18
#define FTEEPROMCTRL_STATUS_RESERVED_MASK                      0x7FF

#define FTEEPROMCTRL_STATUS_WRITEEN                            31
#define FTEEPROMCTRL_STATUS_WRITEEN_MASK                       0x1

#define FTEEPROMCTRL_STATUS_WRITERDY                           17
#define FTEEPROMCTRL_STATUS_WRITERDY_MASK                      0x1

#define FTNANDCTRL_CMDSTA_BUSY                                 4
#define FTNANDCTRL_CMDSTA_BUSY_MASK                            0x1

#define FTNANDCTRL_CMDSTA_COMMAND                              0
#define FTNANDCTRL_CMDSTA_COMMAND_MASK                         0x7
#define FTNANDCTRL_CMDSTA_COMMAND_V_RESET                      0
#define FTNANDCTRL_CMDSTA_COMMAND_V_BLOCK_ERASE                1
#define FTNANDCTRL_CMDSTA_COMMAND_V_PAGE_READ                  2
#define FTNANDCTRL_CMDSTA_COMMAND_V_PAGE_WRITE                 3

#define FTNANDCTRL_CMDSTA_FLASHTYPE                            7
#define FTNANDCTRL_CMDSTA_FLASHTYPE_MASK                       0x1

#define FTNANDCTRL_CMDSTA_MCMADDRESS                           3
#define FTNANDCTRL_CMDSTA_MCMADDRESS_MASK                      0x1

#define FTNANDCTRL_CMDSTA_R1                                   8
#define FTNANDCTRL_CMDSTA_R1_MASK                              0xFFFF

#define FTNANDCTRL_CMDSTA_R2                                   5
#define FTNANDCTRL_CMDSTA_R2_MASK                              0x3

#define FTNANDCTRL_CMDSTA_STATUS                               24
#define FTNANDCTRL_CMDSTA_STATUS_MASK                          0xFF

#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS                     20
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_MASK                0x7
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_0            0
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_1            1
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_2            2
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_3            3
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_4            4
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_5            5
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_6            6
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_V_CHIP_7            7

#define FTNANDCTRL_PAGEADDRESS_PAGEADDRESS                     0
#define FTNANDCTRL_PAGEADDRESS_PAGEADDRESS_MASK                0x3FFFF

#define FTNANDCTRL_PAGEADDRESS_RESERVED                        18
#define FTNANDCTRL_PAGEADDRESS_RESERVED_MASK                   0x3

#define FTNANDCTRL_RAMADDRESS_RAMADDRESS                       0
#define FTNANDCTRL_RAMADDRESS_RAMADDRESS_MASK                  0xFFFFFFFF

#define FTSDCTRL16_CONFIGSTATUS_INIT                           0
#define FTSDCTRL16_CONFIGSTATUS_INIT_MASK                      0x1

#define FTSDCTRL16_CONFIGSTATUS_REFRESHRELOAD                  16
#define FTSDCTRL16_CONFIGSTATUS_REFRESHRELOAD_MASK             0xFF

#define FTSDCTRL16_CONFIGSTATUS_RESERVED_1                     24
#define FTSDCTRL16_CONFIGSTATUS_RESERVED_1_MASK                0xFF

#define FTSDCTRL16_CONFIGSTATUS_RESERVED_2                     1
#define FTSDCTRL16_CONFIGSTATUS_RESERVED_2_MASK                0x7FFF

#define FTSDCTRL16_EDAC_DEDETECTED                             31
#define FTSDCTRL16_EDAC_DEDETECTED_MASK                        0x1

#define FTSDCTRL16_EDAC_EDACON                                 12
#define FTSDCTRL16_EDAC_EDACON_MASK                            0x1

#define FTSDCTRL16_EDAC_EDACRST                                13
#define FTSDCTRL16_EDAC_EDACRST_MASK                           0x1

#define FTSDCTRL16_EDAC_RESERVED_1                             13
#define FTSDCTRL16_EDAC_RESERVED_1_MASK                        0x3FFFF

#define FTSDCTRL16_EDAC_RESERVED_2                             10
#define FTSDCTRL16_EDAC_RESERVED_2_MASK                        0x3

#define FTSDCTRL16_EDAC_SCRUBRATE                              8
#define FTSDCTRL16_EDAC_SCRUBRATE_MASK                         0x3
#define FTSDCTRL16_EDAC_SCRUBRATE_V_SCRUBOFF                   0
#define FTSDCTRL16_EDAC_SCRUBRATE_V_SCRUBSLOW                  1
#define FTSDCTRL16_EDAC_SCRUBRATE_V_SCRUBMED                   2
#define FTSDCTRL16_EDAC_SCRUBRATE_V_SCRUBFAST                  3

#define FTSDCTRL16_EDAC_SECCOUNT                               0
#define FTSDCTRL16_EDAC_SECCOUNT_MASK                          0xFF

#define GPTIMER_CONFIG_DF                                      9
#define GPTIMER_CONFIG_DF_MASK                                 0x1

#define GPTIMER_CONFIG_IRQ                                     3
#define GPTIMER_CONFIG_IRQ_MASK                                0x1F

#define GPTIMER_CONFIG_RESERVED                                10
#define GPTIMER_CONFIG_RESERVED_MASK                           0x3FFFFF

#define GPTIMER_CONFIG_SI                                      8
#define GPTIMER_CONFIG_SI_MASK                                 0x1

#define GPTIMER_CONFIG_TIMERS                                  0
#define GPTIMER_CONFIG_TIMERS_MASK                             0x7

#define GPTIMER_SCALERRELOAD_RELOADVALUE                       0
#define GPTIMER_SCALERRELOAD_RELOADVALUE_MASK                  0xFFFF

#define GPTIMER_SCALERVALUE_VALUE                              0
#define GPTIMER_SCALERVALUE_VALUE_MASK                         0xFFFF

#define GPTIMER_TIMER_CONTROL_CH                               5
#define GPTIMER_TIMER_CONTROL_CH_MASK                          0x1

#define GPTIMER_TIMER_CONTROL_DH                               6
#define GPTIMER_TIMER_CONTROL_DH_MASK                          0x1

#define GPTIMER_TIMER_CONTROL_EN                               0
#define GPTIMER_TIMER_CONTROL_EN_MASK                          0x1

#define GPTIMER_TIMER_CONTROL_IE                               3
#define GPTIMER_TIMER_CONTROL_IE_MASK                          0x1

#define GPTIMER_TIMER_CONTROL_IP                               4
#define GPTIMER_TIMER_CONTROL_IP_MASK                          0x1

#define GPTIMER_TIMER_CONTROL_LD                               2
#define GPTIMER_TIMER_CONTROL_LD_MASK                          0x1

#define GPTIMER_TIMER_CONTROL_RESERVED                         7
#define GPTIMER_TIMER_CONTROL_RESERVED_MASK                    0x1FFFFFF

#define GPTIMER_TIMER_CONTROL_RS                               1
#define GPTIMER_TIMER_CONTROL_RS_MASK                          0x1

#define GPTIMER_TIMER_COUNTER_COUNTERVAL                       0
#define GPTIMER_TIMER_COUNTER_COUNTERVAL_MASK                  0xFFFFFFFF

#define GPTIMER_TIMER_RELOAD_RELOADVAL                         0
#define GPTIMER_TIMER_RELOAD_RELOADVAL_MASK                    0xFFFFFFFF

#define IRQMP_CLEAR_EIC                                        16
#define IRQMP_CLEAR_EIC_MASK                                   0xFFFF

#define IRQMP_CLEAR_IC                                         1
#define IRQMP_CLEAR_IC_MASK                                    0x7FFF

#define IRQMP_CLEAR_R1                                         0
#define IRQMP_CLEAR_R1_MASK                                    0x1

#define IRQMP_EXTINTACK_EID                                    0
#define IRQMP_EXTINTACK_EID_MASK                               0x1F

#define IRQMP_EXTINTACK_RESERVED                               5
#define IRQMP_EXTINTACK_RESERVED_MASK                          0x7FFFFFF

#define IRQMP_FORCE_IF                                         1
#define IRQMP_FORCE_IF_MASK                                    0x7FFF

#define IRQMP_FORCE_R1                                         0
#define IRQMP_FORCE_R1_MASK                                    0x1

#define IRQMP_FORCE_RESERVED                                   16
#define IRQMP_FORCE_RESERVED_MASK                              0xFFFF

#define IRQMP_LEVEL_IL                                         1
#define IRQMP_LEVEL_IL_MASK                                    0x7FFF

#define IRQMP_LEVEL_R1                                         0
#define IRQMP_LEVEL_R1_MASK                                    0x1

#define IRQMP_LEVEL_RESERVED                                   16
#define IRQMP_LEVEL_RESERVED_MASK                              0xFFFF

#define IRQMP_MASK_EIM                                         16
#define IRQMP_MASK_EIM_MASK                                    0xFFFF

#define IRQMP_MASK_IM                                          1
#define IRQMP_MASK_IM_MASK                                     0x7FFF

#define IRQMP_MASK_R1                                          0
#define IRQMP_MASK_R1_MASK                                     0x1

#define IRQMP_PENDING_EIP                                      16
#define IRQMP_PENDING_EIP_MASK                                 0xFFFF

#define IRQMP_PENDING_IP                                       1
#define IRQMP_PENDING_IP_MASK                                  0x7FFF

#define IRQMP_PENDING_R1                                       0
#define IRQMP_PENDING_R1_MASK                                  0x1

#define STIXCTRL_ASCMDSTA_ASAON                                0
#define STIXCTRL_ASCMDSTA_ASAON_MASK                           0x1

#define STIXCTRL_ASCMDSTA_ASBON                                1
#define STIXCTRL_ASCMDSTA_ASBON_MASK                           0x1

#define STIXCTRL_ASCMDSTA_AVGSIZE                              8
#define STIXCTRL_ASCMDSTA_AVGSIZE_MASK                         0x7
#define STIXCTRL_ASCMDSTA_AVGSIZE_V_AVG1                       0
#define STIXCTRL_ASCMDSTA_AVGSIZE_V_AVG2                       1
#define STIXCTRL_ASCMDSTA_AVGSIZE_V_AVG4                       2
#define STIXCTRL_ASCMDSTA_AVGSIZE_V_AVG18                      3
#define STIXCTRL_ASCMDSTA_AVGSIZE_V_AVG116                     4

#define STIXCTRL_ASCMDSTA_RESERVED_1                           11
#define STIXCTRL_ASCMDSTA_RESERVED_1_MASK                      0x1FFFFF

#define STIXCTRL_ASCMDSTA_RESERVED_2                           2
#define STIXCTRL_ASCMDSTA_RESERVED_2_MASK                      0x3F

#define STIXCTRL_ASQLDATA_DATA                                 0
#define STIXCTRL_ASQLDATA_DATA_MASK                            0xFFFF

#define STIXCTRL_ASQLDATA_RESERVED                             16
#define STIXCTRL_ASQLDATA_RESERVED_MASK                        0xFFFF

#define STIXCTRL_ASQLDATA_V_ASA0                               1
#define STIXCTRL_ASQLDATA_V_ASA1                               2
#define STIXCTRL_ASQLDATA_V_ASB0                               3
#define STIXCTRL_ASQLDATA_V_ASB1                               4
#define STIXCTRL_ATTNCMDSTA_COMMAND                            0
#define STIXCTRL_ATTNCMDSTA_COMMAND_MASK                       0xF
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_EMERGENCYSHUTDOWN        6
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_OPENMOTOR1               1
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_OPENMOTOR2               2
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_OPENBOTHMOTORS           3
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_CLOSEMOTOR1              4
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_CLOSEMOTOR2              8
#define STIXCTRL_ATTNCMDSTA_COMMAND_V_CLOSEBOTHMOTORS          12

#define STIXCTRL_ATTNCMDSTA_ERRORMASK                          4
#define STIXCTRL_ATTNCMDSTA_ERRORMASK_MASK                     0x1

#define STIXCTRL_ATTNCMDSTA_FAILFLAG                           11
#define STIXCTRL_ATTNCMDSTA_FAILFLAG_MASK                      0x1

#define STIXCTRL_ATTNCMDSTA_MOTOR1USED                         6
#define STIXCTRL_ATTNCMDSTA_MOTOR1USED_MASK                    0x1

#define STIXCTRL_ATTNCMDSTA_MOTOR2USED                         7
#define STIXCTRL_ATTNCMDSTA_MOTOR2USED_MASK                    0x1

#define STIXCTRL_ATTNCMDSTA_OPERATIONSTATUS                    8
#define STIXCTRL_ATTNCMDSTA_OPERATIONSTATUS_MASK               0x1

#define STIXCTRL_ATTNCMDSTA_POSITIONAB                         10
#define STIXCTRL_ATTNCMDSTA_POSITIONAB_MASK                    0x1

#define STIXCTRL_ATTNCMDSTA_POSITIONBC                         9
#define STIXCTRL_ATTNCMDSTA_POSITIONBC_MASK                    0x1

#define STIXCTRL_ATTNCMDSTA_RESERVED                           12
#define STIXCTRL_ATTNCMDSTA_RESERVED_MASK                      0xFFFFF

#define STIXCTRL_ATTNCMDSTA_TIMEOUT                            5
#define STIXCTRL_ATTNCMDSTA_TIMEOUT_MASK                       0x1

#define STIXCTRL_ATTNCONFIG1_FASTRAMPTIMEQUANT                 24
#define STIXCTRL_ATTNCONFIG1_FASTRAMPTIMEQUANT_MASK            0xFF

#define STIXCTRL_ATTNCONFIG1_RESERVED                          0
#define STIXCTRL_ATTNCONFIG1_RESERVED_MASK                     0xFFF

#define STIXCTRL_ATTNCONFIG1_SLOWRAMPTIMEQUANT                 12
#define STIXCTRL_ATTNCONFIG1_SLOWRAMPTIMEQUANT_MASK            0xFFF

#define STIXCTRL_CACHECMDSTA_DEDETECTED                        18
#define STIXCTRL_CACHECMDSTA_DEDETECTED_MASK                   0x1

#define STIXCTRL_CACHECMDSTA_EDACON                            16
#define STIXCTRL_CACHECMDSTA_EDACON_MASK                       0x1

#define STIXCTRL_CACHECMDSTA_EDACRESET                         17
#define STIXCTRL_CACHECMDSTA_EDACRESET_MASK                    0x1

#define STIXCTRL_CACHECMDSTA_OPMODE                            0
#define STIXCTRL_CACHECMDSTA_OPMODE_MASK                       0x1

#define STIXCTRL_CACHECMDSTA_RESERVED_1                        19
#define STIXCTRL_CACHECMDSTA_RESERVED_1_MASK                   0x1FFF

#define STIXCTRL_CACHECMDSTA_RESERVED_2                        6
#define STIXCTRL_CACHECMDSTA_RESERVED_2_MASK                   0x3

#define STIXCTRL_CACHECMDSTA_RESERVED_3                        1
#define STIXCTRL_CACHECMDSTA_RESERVED_3_MASK                   0x7

#define STIXCTRL_CACHECMDSTA_SCRUBRATE                         4
#define STIXCTRL_CACHECMDSTA_SCRUBRATE_MASK                    0x3
#define STIXCTRL_CACHECMDSTA_SCRUBRATE_V_SCRUBOFF              0
#define STIXCTRL_CACHECMDSTA_SCRUBRATE_V_SCRUBSLOW             1
#define STIXCTRL_CACHECMDSTA_SCRUBRATE_V_SCRUBMED              2
#define STIXCTRL_CACHECMDSTA_SCRUBRATE_V_SCRUBFAST             3

#define STIXCTRL_CACHECMDSTA_SECCOUNT                          8
#define STIXCTRL_CACHECMDSTA_SECCOUNT_MASK                     0xFF

#define STIXCTRL_FSMCALCOUNT_COUNTER                           0
#define STIXCTRL_FSMCALCOUNT_COUNTER_MASK                      0xFFFFFFFF

#define STIXCTRL_FSMCALMASK_DMASK                              0
#define STIXCTRL_FSMCALMASK_DMASK_MASK                         0xFFFFFFFF

#define STIXCTRL_FSMCALRES_RESERVED                            21
#define STIXCTRL_FSMCALRES_RESERVED_MASK                       0x7FF

#define STIXCTRL_FSMCALRES_TRUNCATE                            0
#define STIXCTRL_FSMCALRES_TRUNCATE_MASK                       0x1FFFFF

#define STIXCTRL_FSMCMD_ACTIVETLUT                             9
#define STIXCTRL_FSMCMD_ACTIVETLUT_MASK                        0x1

#define STIXCTRL_FSMCMD_CALCLEAR                               6
#define STIXCTRL_FSMCMD_CALCLEAR_MASK                          0x1

#define STIXCTRL_FSMCMD_CALIRQEN                               10
#define STIXCTRL_FSMCMD_CALIRQEN_MASK                          0x1

#define STIXCTRL_FSMCMD_CALTQ                                  16
#define STIXCTRL_FSMCMD_CALTQ_MASK                             0xFFFF

#define STIXCTRL_FSMCMD_CLEARENABLE                            1
#define STIXCTRL_FSMCMD_CLEARENABLE_MASK                       0x1

#define STIXCTRL_FSMCMD_FIFOIRQEN                              12
#define STIXCTRL_FSMCMD_FIFOIRQEN_MASK                         0x1

#define STIXCTRL_FSMCMD_OVERFLOWCLEAR                          8
#define STIXCTRL_FSMCMD_OVERFLOWCLEAR_MASK                     0x1

#define STIXCTRL_FSMCMD_QLCLEAR                                7
#define STIXCTRL_FSMCMD_QLCLEAR_MASK                           0x1

#define STIXCTRL_FSMCMD_QLIRQEN                                11
#define STIXCTRL_FSMCMD_QLIRQEN_MASK                           0x1

#define STIXCTRL_FSMCMD_RESERVED_1                             13
#define STIXCTRL_FSMCMD_RESERVED_1_MASK                        0x7

#define STIXCTRL_FSMCMD_RESERVED_2                             3
#define STIXCTRL_FSMCMD_RESERVED_2_MASK                        0x3

#define STIXCTRL_FSMCMD_RESET                                  2
#define STIXCTRL_FSMCMD_RESET_MASK                             0x1

#define STIXCTRL_FSMCMD_SETCALENABLE                           5
#define STIXCTRL_FSMCMD_SETCALENABLE_MASK                      0x1

#define STIXCTRL_FSMCMD_SETENABLE                              0
#define STIXCTRL_FSMCMD_SETENABLE_MASK                         0x1

#define STIXCTRL_FSMEVENTS_EVENTCOUNT                          0
#define STIXCTRL_FSMEVENTS_EVENTCOUNT_MASK                     0xFFFFFFFF

#define STIXCTRL_FSMINTEVENTS_INTEVENTSMAX                     0
#define STIXCTRL_FSMINTEVENTS_INTEVENTSMAX_MASK                0xFFFFFFFF

#define STIXCTRL_FSMINTTIME_INTTIMEMAX                         16
#define STIXCTRL_FSMINTTIME_INTTIMEMAX_MASK                    0xFFFF

#define STIXCTRL_FSMINTTIME_INTTIMEMIN                         0
#define STIXCTRL_FSMINTTIME_INTTIMEMIN_MASK                    0xFFFF

#define STIXCTRL_FSMQLCONFIG_QLINTTIME                         16
#define STIXCTRL_FSMQLCONFIG_QLINTTIME_MASK                    0xFFFF

#define STIXCTRL_FSMQLCONFIG_QLVARPMASK                        0
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_MASK                   0xFFF
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL0               1
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL1               2
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL2               4
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL3               8
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL4               16
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL5               32
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL6               64
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL7               128
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL8               256
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL9               512
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL10              1024
#define STIXCTRL_FSMQLCONFIG_QLVARPMASK_V_PIXEL11              2048

#define STIXCTRL_FSMQLCONFIG_RESERVED                          12
#define STIXCTRL_FSMQLCONFIG_RESERVED_MASK                     0xF

#define STIXCTRL_FSMQLVARDMASK_QLVARDMASK                      0
#define STIXCTRL_FSMQLVARDMASK_QLVARDMASK_MASK                 0xFFFFFFFF

#define STIXCTRL_FSMQLVAREMASK_QLVAREMASK                      0
#define STIXCTRL_FSMQLVAREMASK_QLVAREMASK_MASK                 0xFFFFFFFF

#define STIXCTRL_FSMRESULT1_ADCVAL                             0
#define STIXCTRL_FSMRESULT1_ADCVAL_MASK                        0xFFF

#define STIXCTRL_FSMRESULT1_ASICADDR                           27
#define STIXCTRL_FSMRESULT1_ASICADDR_MASK                      0x1

#define STIXCTRL_FSMRESULT1_ENERGY                             12
#define STIXCTRL_FSMRESULT1_ENERGY_MASK                        0x1F

#define STIXCTRL_FSMRESULT1_GROUPADDR                          28
#define STIXCTRL_FSMRESULT1_GROUPADDR_MASK                     0xF

#define STIXCTRL_FSMRESULT1_PIXELADDR                          23
#define STIXCTRL_FSMRESULT1_PIXELADDR_MASK                     0xF

#define STIXCTRL_FSMRESULT1_RESERVED                           17
#define STIXCTRL_FSMRESULT1_RESERVED_MASK                      0x3F

#define STIXCTRL_FSMRESULT2_CORVAL                             0
#define STIXCTRL_FSMRESULT2_CORVAL_MASK                        0xFFF

#define STIXCTRL_FSMRESULT2_RESERVED                           12
#define STIXCTRL_FSMRESULT2_RESERVED_MASK                      0xFFFFF

#define STIXCTRL_FSMSTA_ACTIVENOMINAL                          4
#define STIXCTRL_FSMSTA_ACTIVENOMINAL_MASK                     0x1

#define STIXCTRL_FSMSTA_ACTIVEQL                               2
#define STIXCTRL_FSMSTA_ACTIVEQL_MASK                          0x3

#define STIXCTRL_FSMSTA_CALENABLE                              7
#define STIXCTRL_FSMSTA_CALENABLE_MASK                         0x1

#define STIXCTRL_FSMSTA_CALREADY                               6
#define STIXCTRL_FSMSTA_CALREADY_MASK                          0x1

#define STIXCTRL_FSMSTA_ENABLE                                 0
#define STIXCTRL_FSMSTA_ENABLE_MASK                            0x1

#define STIXCTRL_FSMSTA_QLREADY                                5
#define STIXCTRL_FSMSTA_QLREADY_MASK                           0x1

#define STIXCTRL_FSMSTA_QUEUEOVERLOW                           1
#define STIXCTRL_FSMSTA_QUEUEOVERLOW_MASK                      0x1

#define STIXCTRL_FSMSTA_RESERVED                               8
#define STIXCTRL_FSMSTA_RESERVED_MASK                          0xFFFFFF

#define STIXCTRL_FSMSUMDMASK_SUMDMASK                          0
#define STIXCTRL_FSMSUMDMASK_SUMDMASK_MASK                     0xFFFFFFFF

#define STIXCTRL_FSMSUMEMASK_SUMEMASK                          0
#define STIXCTRL_FSMSUMEMASK_SUMEMASK_MASK                     0xFFFFFFFF

#define STIXCTRL_HKADC_ADDR                                    0
#define STIXCTRL_HKADC_ADDR_MASK                               0x7F
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_1V5_C                     0
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_2V5_C                     1
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_3V3_C                     2
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW_C                     3
#define STIXCTRL_HKADC_ADDR_V_HK_DET_C                         4
#define STIXCTRL_HKADC_ADDR_V_HK_ATT_C                         8
#define STIXCTRL_HKADC_ADDR_V_HK_ATT_V                         12
#define STIXCTRL_HKADC_ADDR_V_HK_HV_01_16_V                    20
#define STIXCTRL_HKADC_ADDR_V_HK_HV_17_32_V                    16
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_1V5_V                     24
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_2V5_V                     28
#define STIXCTRL_HKADC_ADDR_V_HK_DET_Q2_T                      32
#define STIXCTRL_HKADC_ADDR_V_HK_DET_Q1_T                      33
#define STIXCTRL_HKADC_ADDR_V_HK_DET_Q4_T                      34
#define STIXCTRL_HKADC_ADDR_V_HK_DET_Q3_T                      35
#define STIXCTRL_HKADC_ADDR_V_HK_ATT_T                         36
#define STIXCTRL_HKADC_ADDR_V_HK_PSU_T                         40
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_2V9_V                     44
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW0_V                    48
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW1_V                    52
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_PCB_T                     56
#define STIXCTRL_HKADC_ADDR_V_HK_DPU_FPGA_T                    60
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_PHOTOA0_V                 64
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_PHOTOA1_V                 65
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM05_T                   66
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM06_T                   67
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_REF_2V5A_V                68
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM01_T                   69
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM03_T                   70
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_VSENSA_V                  71
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_PHOTOB0_V                 72
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_PHOTOB1_V                 73
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM08_T                   74
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM07_T                   75
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_REF_2V5B_V                76
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM02_T                   77
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_TIM04_T                   78
#define STIXCTRL_HKADC_ADDR_V_HK_ASP_VSENSB_V                  79

#define STIXCTRL_HKADC_READY                                   7
#define STIXCTRL_HKADC_READY_MASK                              0x1u

#define STIXCTRL_HKADC_RESERVED                                8
#define STIXCTRL_HKADC_RESERVED_MASK                           0xFFFu

#define STIXCTRL_HKADC_VAL                                     20
#define STIXCTRL_HKADC_VAL_MASK                                0xFFFu

#define STIXCTRL_HKCMDSTA_HV0116EN                             1
#define STIXCTRL_HKCMDSTA_HV0116EN_MASK                        0x1u

#define STIXCTRL_HKCMDSTA_HV0116VAL                            16
#define STIXCTRL_HKCMDSTA_HV0116VAL_MASK                       0xFFu

#define STIXCTRL_HKCMDSTA_HV1732EN                             2
#define STIXCTRL_HKCMDSTA_HV1732EN_MASK                        0x1u

#define STIXCTRL_HKCMDSTA_HV1732VAL                            24
#define STIXCTRL_HKCMDSTA_HV1732VAL_MASK                       0xFFu

#define STIXCTRL_HKCMDSTA_LVEN                                 0
#define STIXCTRL_HKCMDSTA_LVEN_MASK                            0x1u

#define STIXCTRL_HKCMDSTA_POWERSTAT                            5
#define STIXCTRL_HKCMDSTA_POWERSTAT_MASK                       0x1Fu
#define STIXCTRL_HKCMDSTA_POWERSTAT_V_LVEN                     1
#define STIXCTRL_HKCMDSTA_POWERSTAT_V_HV01                     2
#define STIXCTRL_HKCMDSTA_POWERSTAT_V_HV17                     4
#define STIXCTRL_HKCMDSTA_POWERSTAT_V_SPW0                     8
#define STIXCTRL_HKCMDSTA_POWERSTAT_V_SPW1                     16

#define STIXCTRL_HKCMDSTA_RESERVED                             10
#define STIXCTRL_HKCMDSTA_RESERVED_MASK                        0x3Fu

#define STIXCTRL_HKCMDSTA_SPWEN                                3
#define STIXCTRL_HKCMDSTA_SPWEN_MASK                           0x3u

#define STIXCTRL_HKIRQSTA_HOTSAMPLE                            20
#define STIXCTRL_HKIRQSTA_HOTSAMPLE_MASK                       0xFFFu

#define STIXCTRL_HKIRQSTA_IRQEN                                0
#define STIXCTRL_HKIRQSTA_IRQEN_MASK                           0x1FFu
#define STIXCTRL_HKIRQSTA_IRQEN_V_DPU_1V5_C                    256
#define STIXCTRL_HKIRQSTA_IRQEN_V_DPU_2V5_C                    128
#define STIXCTRL_HKIRQSTA_IRQEN_V_DPU_3V3_C                    64
#define STIXCTRL_HKIRQSTA_IRQEN_V_DPU_SPW_C                    32
#define STIXCTRL_HKIRQSTA_IRQEN_V_DET_C                        16
#define STIXCTRL_HKIRQSTA_IRQEN_V_ATT_C                        8
#define STIXCTRL_HKIRQSTA_IRQEN_V_ATT_V                        4
#define STIXCTRL_HKIRQSTA_IRQEN_V_HV_17_V                      2
#define STIXCTRL_HKIRQSTA_IRQEN_V_HV_01_V                      1

#define STIXCTRL_HKIRQSTA_PROTSTAT                             9
#define STIXCTRL_HKIRQSTA_PROTSTAT_MASK                        0x1FFu
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_DPU_1V5_C                 256
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_DPU_2V5_C                 128
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_DPU_3V3_C                 64
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_DPU_SPW_C                 32
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_DET_C                     16
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_ATT_C                     8
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_ATT_V                     4
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_HV_17_V                   2
#define STIXCTRL_HKIRQSTA_PROTSTAT_V_HV_01_V                   1

#define STIXCTRL_HKIRQSTA_RESERVED                             18
#define STIXCTRL_HKIRQSTA_RESERVED_MASK                        0x3u

#define STIXCTRL_HKPROTCONFIG_PROTEN                           31
#define STIXCTRL_HKPROTCONFIG_PROTEN_MASK                      0x1u

#define STIXCTRL_HKPROTCONFIG_RESERVED                         12
#define STIXCTRL_HKPROTCONFIG_RESERVED_MASK                    0x7FFFFu

#define STIXCTRL_HKPROTCONFIG_UPTHRESHOLD                      0
#define STIXCTRL_HKPROTCONFIG_UPTHRESHOLD_MASK                 0xFFF

#define STIXCTRL_HKPROTCONFIG_V_ATT_C                          6
#define STIXCTRL_HKPROTCONFIG_V_ATT_V                          7
#define STIXCTRL_HKPROTCONFIG_V_DET_C                          5
#define STIXCTRL_HKPROTCONFIG_V_DPU_1V5_C                      1
#define STIXCTRL_HKPROTCONFIG_V_DPU_2V5_C                      2
#define STIXCTRL_HKPROTCONFIG_V_DPU_3V3_C                      3
#define STIXCTRL_HKPROTCONFIG_V_DPU_SPW_C                      4
#define STIXCTRL_HKPROTCONFIG_V_HV_01_V                        9
#define STIXCTRL_HKPROTCONFIG_V_HV_17_V                        8
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER                        16
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_MASK                   0xFu
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_RESET                0
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ADCTEMPREAD          1
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ADCBUFFREAD          2
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ADCMUXREAD           3
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_SCEND                4
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ODEVENTSEP           5
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ONCS_N               6
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_OFFCS_N              7
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_REGADDRESS           8
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_BYTEWRITE            8
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_NOTUSED              9
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_SCINITSEQ            11
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ODINITSEQ            12
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ODEVENTMASK          15
#define STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_V_ODEVENTREAD          15

#define STIXCTRL_IDEFXCMDSTA_CMDDATA                           0
#define STIXCTRL_IDEFXCMDSTA_CMDDATA_MASK                      0xFFFFu

#define STIXCTRL_IDEFXCMDSTA_CMDREADY                          31
#define STIXCTRL_IDEFXCMDSTA_CMDREADY_MASK                     0x1

#define STIXCTRL_IDEFXCMDSTA_GRPADDR                           20
#define STIXCTRL_IDEFXCMDSTA_GRPADDR_MASK                      0xF

#define STIXCTRL_IDEFXCMDSTA_RESERVED                          24
#define STIXCTRL_IDEFXCMDSTA_RESERVED_MASK                     0x7F

#define STIXCTRL_IDEFXCONFIG_DELAYMODEADC                      30
#define STIXCTRL_IDEFXCONFIG_DELAYMODEADC_MASK                 0x3
#define STIXCTRL_IDEFXCONFIG_DELAYMODEADC_V_D_DEF              0
#define STIXCTRL_IDEFXCONFIG_DELAYMODEADC_V_D_M20              1
#define STIXCTRL_IDEFXCONFIG_DELAYMODEADC_V_D_M40              16
#define STIXCTRL_IDEFXCONFIG_DELAYMODEADC_V_D_P20              17

#define STIXCTRL_IDEFXCONFIG_DELAYMODEIDEFX                    28
#define STIXCTRL_IDEFXCONFIG_DELAYMODEIDEFX_MASK               0x3
#define STIXCTRL_IDEFXCONFIG_DELAYMODEIDEFX_V_D_DEF            0
#define STIXCTRL_IDEFXCONFIG_DELAYMODEIDEFX_V_D_M20            1
#define STIXCTRL_IDEFXCONFIG_DELAYMODEIDEFX_V_D_M40            16
#define STIXCTRL_IDEFXCONFIG_DELAYMODEIDEFX_V_D_M60            17

#define STIXCTRL_IDEFXCONFIG_ENABLE                            4
#define STIXCTRL_IDEFXCONFIG_ENABLE_MASK                       0xFFFF
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q1G1                     1
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q1G2                     2
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q1G3                     4
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q1G4                     8
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q2G1                     16
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q2G2                     32
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q2G3                     64
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q2G4                     128
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q3G1                     256
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q3G2                     512
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q3G3                     1024
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q3G4                     2048
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q4G1                     4096
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q4G2                     8192
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q4G3                     16384
#define STIXCTRL_IDEFXCONFIG_ENABLE_V_Q4G4                     32768

#define STIXCTRL_IDEFXCONFIG_Q1ON                              0
#define STIXCTRL_IDEFXCONFIG_Q1ON_MASK                         0x1

#define STIXCTRL_IDEFXCONFIG_Q2ON                              1
#define STIXCTRL_IDEFXCONFIG_Q2ON_MASK                         0x1

#define STIXCTRL_IDEFXCONFIG_Q3ON                              2
#define STIXCTRL_IDEFXCONFIG_Q3ON_MASK                         0x1

#define STIXCTRL_IDEFXCONFIG_Q4ON                              3
#define STIXCTRL_IDEFXCONFIG_Q4ON_MASK                         0x1

#define STIXCTRL_IDEFXCONFIG_SEUIRQEN                          20
#define STIXCTRL_IDEFXCONFIG_SEUIRQEN_MASK                     0xF
#define STIXCTRL_IDEFXCONFIG_SEUIRQEN_V_Q1                     1
#define STIXCTRL_IDEFXCONFIG_SEUIRQEN_V_Q2                     2
#define STIXCTRL_IDEFXCONFIG_SEUIRQEN_V_Q3                     256
#define STIXCTRL_IDEFXCONFIG_SEUIRQEN_V_Q4                     4096

#define STIXCTRL_IDEFXCONFIG_TLATENCY                          24
#define STIXCTRL_IDEFXCONFIG_TLATENCY_MASK                     0xF

#define STIXCTRL_IDEFXMODEDATA_READMODE                        16
#define STIXCTRL_IDEFXMODEDATA_READMODE_MASK                   0xFFFFu
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q1G1                 1
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q1G2                 2
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q1G3                 4
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q1G4                 8
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q2G1                 16
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q2G2                 32
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q2G3                 64
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q2G4                 128
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q3G1                 256
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q3G2                 512
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q3G3                 1024
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q3G4                 2048
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q4G1                 4096
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q4G2                 8192
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q4G3                 16384
#define STIXCTRL_IDEFXMODEDATA_READMODE_V_Q4G4                 32768

#define STIXCTRL_IDEFXMODEDATA_RESPONSEDATA                    0
#define STIXCTRL_IDEFXMODEDATA_RESPONSEDATA_MASK               0xFFFF

#define STIXCTRL_IDEFXWRITEMASK_RESERVED                       0
#define STIXCTRL_IDEFXWRITEMASK_RESERVED_MASK                  0xFFFF

#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK                      16
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_MASK                 0xFFFF
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q1G1               1
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q1G2               2
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q1G3               4
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q1G4               8
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q2G1               16
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q2G2               32
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q2G3               64
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q2G4               128
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q3G1               256
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q3G2               512
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q3G3               1024
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q3G4               2048
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q4G1               4096
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q4G2               8192
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q4G3               16384
#define STIXCTRL_IDEFXWRITEMASK_WRITEMASK_V_Q4G4               32768

#define STIXCTRL_LONGTIMERSEC_SECS                             0
#define STIXCTRL_LONGTIMERSEC_SECS_MASK                        0xFFFFFFFF

#define STIXCTRL_LONGTIMERSUBSEC_RESERVED                      16
#define STIXCTRL_LONGTIMERSUBSEC_RESERVED_MASK                 0xFFFF

#define STIXCTRL_LONGTIMERSUBSEC_SUBSEC                        0
#define STIXCTRL_LONGTIMERSUBSEC_SUBSEC_MASK                   0xFFFF

#define STIXCTRL_MAGICREG_MAGICVAL                             0
#define STIXCTRL_MAGICREG_MAGICVAL_MASK                        0xFFFFFFFF

#define STIXCTRL_ROTBCMDSTA_BLOCKCOUNT                         7
#define STIXCTRL_ROTBCMDSTA_BLOCKCOUNT_MASK                    0x1FFFF

#define STIXCTRL_ROTBCMDSTA_COMMAND                            0
#define STIXCTRL_ROTBCMDSTA_COMMAND_MASK                       0x3
#define STIXCTRL_ROTBCMDSTA_COMMAND_V_NOTHING                  0
#define STIXCTRL_ROTBCMDSTA_COMMAND_V_GETBLOCK                 1
#define STIXCTRL_ROTBCMDSTA_COMMAND_V_PUTBLOCK                 2
#define STIXCTRL_ROTBCMDSTA_COMMAND_V_RESETFIFO                3

#define STIXCTRL_ROTBCMDSTA_DEDETECTED                         4
#define STIXCTRL_ROTBCMDSTA_DEDETECTED_MASK                    0x1

#define STIXCTRL_ROTBCMDSTA_EDACRESET                          2
#define STIXCTRL_ROTBCMDSTA_EDACRESET_MASK                     0x1

#define STIXCTRL_ROTBCMDSTA_FULL                               5
#define STIXCTRL_ROTBCMDSTA_FULL_MASK                          0x1

#define STIXCTRL_ROTBCMDSTA_READY                              3
#define STIXCTRL_ROTBCMDSTA_READY_MASK                         0x1

#define STIXCTRL_ROTBCMDSTA_RESERVED                           6
#define STIXCTRL_ROTBCMDSTA_RESERVED_MASK                      0x1

#define STIXCTRL_ROTBCMDSTA_SECCOUNT                           24
#define STIXCTRL_ROTBCMDSTA_SECCOUNT_MASK                      0xFF

#define STIXCTRL_ROTBCONFIG_EDACON                             12
#define STIXCTRL_ROTBCONFIG_EDACON_MASK                        0x1

#define STIXCTRL_ROTBCONFIG_ENABLE                             31
#define STIXCTRL_ROTBCONFIG_ENABLE_MASK                        0x1

#define STIXCTRL_ROTBCONFIG_REFRESHRATE                        4
#define STIXCTRL_ROTBCONFIG_REFRESHRATE_MASK                   0xFF

#define STIXCTRL_ROTBCONFIG_RESERVED_1                         14
#define STIXCTRL_ROTBCONFIG_RESERVED_1_MASK                    0x1FFFF

#define STIXCTRL_ROTBCONFIG_RESERVED_2                         2
#define STIXCTRL_ROTBCONFIG_RESERVED_2_MASK                    0x3

#define STIXCTRL_ROTBCONFIG_TESTMODE                           13
#define STIXCTRL_ROTBCONFIG_TESTMODE_MASK                      0x1

#define STIXCTRL_STIXSTATUS_CLEANSHUTDOWN                      30
#define STIXCTRL_STIXSTATUS_CLEANSHUTDOWN_MASK                 0x1

#define STIXCTRL_STIXSTATUS_CLEAR                              31
#define STIXCTRL_STIXSTATUS_CLEAR_MASK                         0x1u

#define STIXCTRL_STIXSTATUS_HWBUILD                            8
#define STIXCTRL_STIXSTATUS_HWBUILD_MASK                       0xFFFF

#define STIXCTRL_STIXSTATUS_HWRELEASE                          24
#define STIXCTRL_STIXSTATUS_HWRELEASE_MASK                     0x3F

#define STIXCTRL_STIXSTATUS_OTHERISON                          6
#define STIXCTRL_STIXSTATUS_OTHERISON_MASK                     0x1

#define STIXCTRL_STIXSTATUS_RESERVED                           2
#define STIXCTRL_STIXSTATUS_RESERVED_MASK                      0x7

#define STIXCTRL_STIXSTATUS_ROTBENABLED                        0
#define STIXCTRL_STIXSTATUS_ROTBENABLED_MASK                   0x1

#define STIXCTRL_STIXSTATUS_THISIDPU                           7
#define STIXCTRL_STIXSTATUS_THISIDPU_MASK                      0x1
#define STIXCTRL_STIXSTATUS_THISIDPU_V_MAIN                    1
#define STIXCTRL_STIXSTATUS_THISIDPU_V_RED                     0

#define STIXCTRL_STIXSTATUS_TICKENABLED                        1
#define STIXCTRL_STIXSTATUS_TICKENABLED_MASK                   0x1

#define STIXCTRL_STIXSTATUS_WATCHDOG                           5
#define STIXCTRL_STIXSTATUS_WATCHDOG_MASK                      0x1

#define STIXCTRL_TESTPULSECMDSTA_BUSY                          16
#define STIXCTRL_TESTPULSECMDSTA_BUSY_MASK                     0x1

#define STIXCTRL_TESTPULSECMDSTA_DACVALUE                      0
#define STIXCTRL_TESTPULSECMDSTA_DACVALUE_MASK                 0xFFF

#define STIXCTRL_TESTPULSECMDSTA_QUARTERMASK                   12
#define STIXCTRL_TESTPULSECMDSTA_QUARTERMASK_MASK              0xF

#define STIXCTRL_TESTPULSECMDSTA_QUARTERSEU                    28
#define STIXCTRL_TESTPULSECMDSTA_QUARTERSEU_MASK               0xF

#define STIXCTRL_TESTPULSECMDSTA_RESERVED                      17
#define STIXCTRL_TESTPULSECMDSTA_RESERVED_MASK                 0x7FF

#define STIXCTRL_UNLOCKINGREG_RESERVED                         8
#define STIXCTRL_UNLOCKINGREG_RESERVED_MASK                    0xFFFFFF

#define STIXCTRL_UNLOCKINGREG_TOKEN                            0
#define STIXCTRL_UNLOCKINGREG_TOKEN_MASK                       0xFF

#define STIXCTRL_WATCHDOG_COMMAND                              29
#define STIXCTRL_WATCHDOG_COMMAND_MASK                         0x7
#define STIXCTRL_WATCHDOG_COMMAND_V_SAFE                       0
#define STIXCTRL_WATCHDOG_COMMAND_V_RELOAD                     1
#define STIXCTRL_WATCHDOG_COMMAND_V_FORCE                      2
#define STIXCTRL_WATCHDOG_COMMAND_V_FORCE1                     3
#define STIXCTRL_WATCHDOG_COMMAND_V_FORCE2                     4
#define STIXCTRL_WATCHDOG_COMMAND_V_DISABLE                    5
#define STIXCTRL_WATCHDOG_COMMAND_V_ENABLE                     6
#define STIXCTRL_WATCHDOG_COMMAND_V_CHANGE                     7

#define STIXCTRL_WATCHDOG_COUNT                                0
#define STIXCTRL_WATCHDOG_COUNT_MASK                           0x3F

#define STIXCTRL_WATCHDOG_RELOADVAL                            11
#define STIXCTRL_WATCHDOG_RELOADVAL_MASK                       0x3F

#define STIXCTRL_WATCHDOG_RESERVED_1                           17
#define STIXCTRL_WATCHDOG_RESERVED_1_MASK                      0xFFF

#define STIXCTRL_WATCHDOG_RESERVED_2                           6
#define STIXCTRL_WATCHDOG_RESERVED_2_MASK                      0x1F

#define STIXSPW_CLOCKDIVISOR_CLKDIVRUN                         0
#define STIXSPW_CLOCKDIVISOR_CLKDIVRUN_MASK                    0xFF

#define STIXSPW_CLOCKDIVISOR_CLKDIVSTART                       8
#define STIXSPW_CLOCKDIVISOR_CLKDIVSTART_MASK                  0xFF

#define STIXSPW_CLOCKDIVISOR_RESERVED                          16
#define STIXSPW_CLOCKDIVISOR_RESERVED_MASK                     0xFFFF

#define STIXSPW_CONTROL_AS                                     2
#define STIXSPW_CONTROL_AS_MASK                                0x1

#define STIXSPW_CONTROL_IE                                     3
#define STIXSPW_CONTROL_IE_MASK                                0x1

#define STIXSPW_CONTROL_LD                                     0
#define STIXSPW_CONTROL_LD_MASK                                0x1

#define STIXSPW_CONTROL_LI                                     9
#define STIXSPW_CONTROL_LI_MASK                                0x1

#define STIXSPW_CONTROL_LS                                     1
#define STIXSPW_CONTROL_LS_MASK                                0x1

#define STIXSPW_CONTROL_NP                                     20
#define STIXSPW_CONTROL_NP_MASK                                0x1

#define STIXSPW_CONTROL_PM                                     5
#define STIXSPW_CONTROL_PM_MASK                                0x1

#define STIXSPW_CONTROL_PO                                     26
#define STIXSPW_CONTROL_PO_MASK                                0x1

#define STIXSPW_CONTROL_PS                                     21
#define STIXSPW_CONTROL_PS_MASK                                0x1

#define STIXSPW_CONTROL_RA                                     31
#define STIXSPW_CONTROL_RA_MASK                                0x1

#define STIXSPW_CONTROL_RC                                     29
#define STIXSPW_CONTROL_RC_MASK                                0x1

#define STIXSPW_CONTROL_RD                                     17
#define STIXSPW_CONTROL_RD_MASK                                0x1

#define STIXSPW_CONTROL_RE                                     16
#define STIXSPW_CONTROL_RE_MASK                                0x1

#define STIXSPW_CONTROL_RESERVED_1                             27
#define STIXSPW_CONTROL_RESERVED_1_MASK                        0x3

#define STIXSPW_CONTROL_RESERVED_2                             22
#define STIXSPW_CONTROL_RESERVED_2_MASK                        0xF

#define STIXSPW_CONTROL_RESERVED_3                             18
#define STIXSPW_CONTROL_RESERVED_3_MASK                        0x3

#define STIXSPW_CONTROL_RESERVED_4                             12
#define STIXSPW_CONTROL_RESERVED_4_MASK                        0xF

#define STIXSPW_CONTROL_RESERVED_5                             7
#define STIXSPW_CONTROL_RESERVED_5_MASK                        0x1

#define STIXSPW_CONTROL_RS                                     6
#define STIXSPW_CONTROL_RS_MASK                                0x1

#define STIXSPW_CONTROL_RX                                     30
#define STIXSPW_CONTROL_RX_MASK                                0x1

#define STIXSPW_CONTROL_TI                                     4
#define STIXSPW_CONTROL_TI_MASK                                0x1

#define STIXSPW_CONTROL_TQ                                     8
#define STIXSPW_CONTROL_TQ_MASK                                0x1

#define STIXSPW_CONTROL_TR                                     11
#define STIXSPW_CONTROL_TR_MASK                                0x1

#define STIXSPW_CONTROL_TT                                     10
#define STIXSPW_CONTROL_TT_MASK                                0x1

#define STIXSPW_DESTKEY_DESTKEY                                0
#define STIXSPW_DESTKEY_DESTKEY_MASK                           0xFF

#define STIXSPW_DESTKEY_RESERVED                               8
#define STIXSPW_DESTKEY_RESERVED_MASK                          0xFFFFFF

#define STIXSPW_DMACONTROL_AI                                  4
#define STIXSPW_DMACONTROL_AI_MASK                             0x1

#define STIXSPW_DMACONTROL_AT                                  9
#define STIXSPW_DMACONTROL_AT_MASK                             0x1

#define STIXSPW_DMACONTROL_LE                                  16
#define STIXSPW_DMACONTROL_LE_MASK                             0x1

#define STIXSPW_DMACONTROL_NS                                  12
#define STIXSPW_DMACONTROL_NS_MASK                             0x1

#define STIXSPW_DMACONTROL_PR                                  6
#define STIXSPW_DMACONTROL_PR_MASK                             0x1

#define STIXSPW_DMACONTROL_PS                                  5
#define STIXSPW_DMACONTROL_PS_MASK                             0x1

#define STIXSPW_DMACONTROL_RA                                  8
#define STIXSPW_DMACONTROL_RA_MASK                             0x1

#define STIXSPW_DMACONTROL_RD                                  11
#define STIXSPW_DMACONTROL_RD_MASK                             0x1

#define STIXSPW_DMACONTROL_RE                                  1
#define STIXSPW_DMACONTROL_RE_MASK                             0x1

#define STIXSPW_DMACONTROL_RESERVED_1                          17
#define STIXSPW_DMACONTROL_RESERVED_1_MASK                     0x7FFF

#define STIXSPW_DMACONTROL_RESERVED_2                          13
#define STIXSPW_DMACONTROL_RESERVED_2_MASK                     0x7

#define STIXSPW_DMACONTROL_RI                                  3
#define STIXSPW_DMACONTROL_RI_MASK                             0x1

#define STIXSPW_DMACONTROL_RX                                  10
#define STIXSPW_DMACONTROL_RX_MASK                             0x1

#define STIXSPW_DMACONTROL_TA                                  7
#define STIXSPW_DMACONTROL_TA_MASK                             0x1

#define STIXSPW_DMACONTROL_TE                                  0
#define STIXSPW_DMACONTROL_TE_MASK                             0x1

#define STIXSPW_DMACONTROL_TI                                  2
#define STIXSPW_DMACONTROL_TI_MASK                             0x1

#define STIXSPW_DMAMAXRXLENGTH_RESERVED                        25
#define STIXSPW_DMAMAXRXLENGTH_RESERVED_MASK                   0x7F

#define STIXSPW_DMAMAXRXLENGTH_RXMAXLEN                        0
#define STIXSPW_DMAMAXRXLENGTH_RXMAXLEN_MASK                   0x1FFFFFF

#define STIXSPW_DMARXDESCADDR_DESCBASEADDR                     10
#define STIXSPW_DMARXDESCADDR_DESCBASEADDR_MASK                0x3FFFFF

#define STIXSPW_DMARXDESCADDR_DESCSEL                          3
#define STIXSPW_DMARXDESCADDR_DESCSEL_MASK                     0x7F

#define STIXSPW_DMARXDESCADDR_RESERVED                         0
#define STIXSPW_DMARXDESCADDR_RESERVED_MASK                    0x7

#define STIXSPW_DMATXDESCADDR_DESCBASEADDR                     10
#define STIXSPW_DMATXDESCADDR_DESCBASEADDR_MASK                0x3FFFFF

#define STIXSPW_DMATXDESCADDR_DESCSEL                          4
#define STIXSPW_DMATXDESCADDR_DESCSEL_MASK                     0x3F

#define STIXSPW_DMATXDESCADDR_RESERVED                         0
#define STIXSPW_DMATXDESCADDR_RESERVED_MASK                    0xF

#define STIXSPW_NODEADDRESS_NODEADDR                           0
#define STIXSPW_NODEADDRESS_NODEADDR_MASK                      0xFF

#define STIXSPW_NODEADDRESS_RESERVED                           8
#define STIXSPW_NODEADDRESS_RESERVED_MASK                      0xFFFFFF

#define STIXSPW_STATUS_AP                                      9
#define STIXSPW_STATUS_AP_MASK                                 0x1

#define STIXSPW_STATUS_CE                                      1
#define STIXSPW_STATUS_CE_MASK                                 0x1

#define STIXSPW_STATUS_DE                                      3
#define STIXSPW_STATUS_DE_MASK                                 0x1

#define STIXSPW_STATUS_EE                                      8
#define STIXSPW_STATUS_EE_MASK                                 0x1

#define STIXSPW_STATUS_ER                                      2
#define STIXSPW_STATUS_ER_MASK                                 0x1

#define STIXSPW_STATUS_IA                                      7
#define STIXSPW_STATUS_IA_MASK                                 0x1

#define STIXSPW_STATUS_LS                                      21
#define STIXSPW_STATUS_LS_MASK                                 0x7

#define STIXSPW_STATUS_PE                                      4
#define STIXSPW_STATUS_PE_MASK                                 0x1

#define STIXSPW_STATUS_RESERVED_1                              24
#define STIXSPW_STATUS_RESERVED_1_MASK                         0xFF

#define STIXSPW_STATUS_RESERVED_2                              10
#define STIXSPW_STATUS_RESERVED_2_MASK                         0x7FF

#define STIXSPW_STATUS_RESERVED_3                              5
#define STIXSPW_STATUS_RESERVED_3_MASK                         0x1

#define STIXSPW_STATUS_TO                                      0
#define STIXSPW_STATUS_TO_MASK                                 0x1

#define STIXSPW_STATUS_WE                                      6
#define STIXSPW_STATUS_WE_MASK                                 0x1

#define STIXSPW_TIME_RESERVED                                  8
#define STIXSPW_TIME_RESERVED_MASK                             0xFFFFFF

#define STIXSPW_TIME_TCTRL                                     6
#define STIXSPW_TIME_TCTRL_MASK                                0x3

#define STIXSPW_TIME_TIMECNT                                   0
#define STIXSPW_TIME_TIMECNT_MASK                              0x3F

#define STIXSPW_TIMERDISC_DISCONNECT                           12
#define STIXSPW_TIMERDISC_DISCONNECT_MASK                      0x3FF

#define STIXSPW_TIMERDISC_RESERVED                             22
#define STIXSPW_TIMERDISC_RESERVED_MASK                        0x3FF

#define STIXSPW_TIMERDISC_TIMER64                              0
#define STIXSPW_TIMERDISC_TIMER64_MASK                         0xFFF


/* Register Structures entry points */
#define EEPROM_REGISTERS                                       ((ftEepromCtrlRegs_s *) EEPROM_APB_BASE)
#define FLASH_REGISTERS                                        ((ftNandCtrlRegs_s *) FLASH_APB_BASE)
#define IRQCTRL_REGISTERS                                      ((irqmpRegs_s *) IRQCTRL_APB_BASE)
#define SDRAM_REGISTERS                                        ((ftSdCtrl16Regs_s *) SDRAM_APB_BASE)
#define SPW_REGISTERS                                          ((stixSpwRegs_s *) SPW_APB_BASE)
#define STIXCTRL_REGISTERS                                     ((stixCtrlRegs_s *) STIXCTRL_APB_BASE)
#define TIMER_REGISTERS                                        ((gptimerRegs_s *) TIMER_APB_BASE)
#define UART_REGISTERS                                         ((apbuartRegs_s *) UART_APB_BASE)

/* Interrupt definitions */
#define IRQ_FLASH                                               1 /* Interrupt indicating that last operation was finished. */
#define IRQ_SPW                                                 13 /* Default Interrupt for GRSPW controller. */
#define IRQ_STIXCTRL_DETCTRL                                    3 /* Detector Controller interrupt. (Internal FIFO overflow, SEU in Detectors, Calibration report ready, Quick Look buffers switch)  */
#define IRQ_STIXCTRL_EDAC                                       6 /* Errors (DED) in Detector's Cache Memory or in Rotating Buffer's SDRAM memory. Cache EDAC Interrrupt is reported if OpMode of Cache memory is set to 0. */
#define IRQ_STIXCTRL_HK                                         4 /* Housekeeping Monitored Value exceeded threshold. */
#define IRQ_STIXCTRL_OTHERISON                                  5 /* Interrupt triggered when Other IDPU becomes On. */
#define IRQ_STIXCTRL_ROTB                                       7 /* Rotating Buffer overflow interrupt. This memory is reportet whenever HW writes new block and Rotating Buffer memory is full. */
#define IRQ_TIMER_TIMER0                                        10 /* Timer0 overflow interrupt. */
#define IRQ_TIMER_TIMER1                                        11 /* Timer1 overflow interrupt. */
#define IRQ_UART                                                2 /* Interrupt indicating reception/ transmission of character or FIFO empty/full status. */

/* Register Blocks definitions */
typedef struct
{
    __volatile__ uint32_t counter; /* 0x00-0x03 Counter value register*/
    __volatile__ uint32_t reload; /* 0x04-0x07 Reload value register*/
    __volatile__ uint32_t control; /* 0x08-0x0B Control register*/
    __volatile__ uint32_t notUsed0; /* 0x0C-0x0F */
} timer_s;


/* Register Structures definitions */
typedef struct
{
    __volatile__ uint32_t data; /* 0x00-0x03 UART Data register*/
    __volatile__ uint32_t status; /* 0x04-0x07 UART Status register*/
    __volatile__ uint32_t control; /* 0x08-0x0B UART Control register*/
    __volatile__ uint32_t scaler; /* 0x0C-0x0F UART Scaler register*/
    __volatile__ uint32_t fifo; /* 0x10-0x13 UART FIFO debug register*/
} apbuartRegs_s;

typedef struct
{
    __volatile__ uint32_t status; /* 0x00-0x03 EepromCtrl Commands and Status  Register*/
} ftEepromCtrlRegs_s;

typedef struct
{
    __volatile__ uint32_t cmdSta; /* 0x00-0x03 Command/Status register.*/
    __volatile__ uint32_t pageAddress; /* 0x04-0x07 Page Address in NAND FLASH for operation.*/
    __volatile__ uint32_t ramAddress; /* 0x08-0x0B AHB Address for operation.*/
} ftNandCtrlRegs_s;

typedef struct
{
    __volatile__ uint32_t configStatus; /* 0x00-0x03 Control/Status Register of FtSdCtrl16*/
    __volatile__ uint32_t edac; /* 0x04-0x07 EDAC Configuration/Status Register*/
} ftSdCtrl16Regs_s;

typedef struct
{
    __volatile__ uint32_t scalerValue; /* 0x00-0x03 Scaler configuration register*/
    __volatile__ uint32_t scalerReload; /* 0x04-0x07 Scaler Reload register*/
    __volatile__ uint32_t config; /* 0x08-0x0B Configuration register*/
    __volatile__ uint32_t notUsed0; /* 0x0C-0x0F */
    timer_s timer[2]; /* 0x10-0x30 */
} gptimerRegs_s;

typedef struct
{
    __volatile__ uint32_t level; /* 0x00-0x03 Interrupt level register*/
    __volatile__ uint32_t pending; /* 0x04-0x07 Interrupt pending register*/
    __volatile__ uint32_t force; /* 0x08-0x0B Interrupt force register (NCPU = 1)*/
    __volatile__ uint32_t clear; /* 0x0C-0x0F Interrupt clear register*/
    __volatile__ uint32_t notUsed0[12]; /* 0x10-0x3F */
    __volatile__ uint32_t mask; /* 0x40-0x43 Processor interrupt mask register*/
    __volatile__ uint32_t notUsed1[31]; /* 0x44-0xBF */
    __volatile__ uint32_t extIntAck; /* 0xC0-0xC3 Processor extended interrupt acknowledge register*/
} irqmpRegs_s;

typedef struct
{
    __volatile__ uint32_t ideFxCmdSta; /* 0x00-0x03 Detector Controller Command/Status Register*/
    __volatile__ uint32_t idefxModeData; /* 0x04-0x07 Idefx Read Mode Switch On/Off bits (16 MSB bits) and Response Data (16 LSB bits) of last command. */
    __volatile__ uint32_t idefxWriteMask; /* 0x08-0x0B Idefx Group Mask for configuration.*/
    __volatile__ uint32_t idefxConfig; /* 0x0C-0x0F IdeFX configuration register*/
    __volatile__ uint32_t testPulseCmdSta; /* 0x10-0x13 Test Pulse Generation register.*/
    __volatile__ uint32_t fsmCmd; /* 0x14-0x17 Readout FSM Control Register.*/
    __volatile__ uint32_t fsmSta; /* 0x18-0x1B Readout FSM Control/Status Register.*/
    __volatile__ uint32_t fsmIntTime; /* 0x1C-0x1F Integration Time for FSM*/
    __volatile__ uint32_t fsmQlConfig; /* 0x20-0x23 Integration Time for FSM for QL accumulators.*/
    __volatile__ uint32_t fsmQlVarDMask; /* 0x24-0x27 Detector Mask for QLVAR (VARDMASK).*/
    __volatile__ uint32_t fsmQlVarEMask; /* 0x28-0x2B Energy Mask for QLVAR (VAREMASK).*/
    __volatile__ uint32_t fsmIntEvents; /* 0x2C-0x2F Maximum events to integrate for FSM*/
    __volatile__ uint32_t fsmEvents; /* 0x30-0x33 FSM event counter*/
    __volatile__ uint32_t fsmSumDmask; /* 0x34-0x37 Detector Mask for EACC and SUMCNT (SUMDMASK).*/
    __volatile__ uint32_t fsmSumEmask; /* 0x38-0x3B Energy Level Mask for SUMCNT (SUMEMASK).*/
    __volatile__ uint32_t fsmResult1; /* 0x3C-0x3F Readout FSM result register 1*/
    __volatile__ uint32_t fsmResult2; /* 0x40-0x43 Readout FSM result register 2*/
    __volatile__ uint32_t fsmCalMask; /* 0x44-0x47 Readout FSM Calibration Detector Mask*/
    __volatile__ uint32_t fsmCalCount; /* 0x48-0x4B Calibration Counter*/
    __volatile__ uint32_t fsmCalRes; /* 0x4C-0x4F Calibration Event Data*/
    __volatile__ uint32_t cacheCmdSta; /* 0x50-0x53 Cache Controller Command/Status register.*/
    __volatile__ uint32_t rotbCmdSta; /* 0x54-0x57 Circular buffer (FIFO) controller Control/Status register.*/
    __volatile__ uint32_t rotbConfig; /* 0x58-0x5B Circular buffer (FIFO) controller Configuration register.*/
    __volatile__ uint32_t attnCmdSta; /* 0x5C-0x5F Attenuator Controller Command Status Register*/
    __volatile__ uint32_t attnConfig1; /* 0x60-0x63 Attenuator Controller PWM Configuration Register 1*/
    __volatile__ uint32_t notUsed0; /* 0x64-0x67 */
    __volatile__ uint32_t asCmdSta; /* 0x68-0x6B Aspect System Control/Status Register.*/
    __volatile__ uint32_t asQlData[4]; /* 0x6C-0x7B Aspect System Quick look register.*/
    __volatile__ uint32_t notUsed1[3]; /* 0x7C-0x87 */
    __volatile__ uint32_t hkCmdSta; /* 0x88-0x8B Housekeeping Control/Status Register*/
    __volatile__ uint32_t hkAdc; /* 0x8C-0x8F Housekeeping On Demand*/
    __volatile__ uint32_t hkIrqSta; /* 0x90-0x93 Hardware protection status and irq mask*/
    __volatile__ uint32_t hkProtConfig[9]; /* 0x94-0xB7 HouseKeeping Automatic protection thresholds {n}.*/
    __volatile__ uint32_t notUsed2[4]; /* 0xB8-0xC7 */
    __volatile__ uint32_t longTimerSec; /* 0xC8-0xCB Long Timer Seconds.*/
    __volatile__ uint32_t longTimerSubSec; /* 0xCC-0xCF Long Timer SubSecond and Control/Status register.*/
    __volatile__ uint32_t unlockingReg; /* 0xD0-0xD3 Protection unlocking register.*/
    __volatile__ uint32_t watchDog; /* 0xD4-0xD7 WatchDog Control/Status Register.*/
    __volatile__ uint32_t magicReg; /* 0xD8-0xDB Start-up magic register.*/
    __volatile__ uint32_t notUsed3[5]; /* 0xDC-0xEF */
    __volatile__ uint32_t stixStatus; /* 0xF0-0xF3 General STIX Controller status register.*/
} stixCtrlRegs_s;

typedef struct
{
    __volatile__ uint32_t control; /* 0x00-0x03 Control register.*/
    __volatile__ uint32_t status; /* 0x04-0x07 GRSPW status register.*/
    __volatile__ uint32_t nodeAddress; /* 0x08-0x0B GRSPW node address register.*/
    __volatile__ uint32_t clockDivisor; /* 0x0C-0x0F GRSPW clock divisor register.*/
    __volatile__ uint32_t destKey; /* 0x10-0x13 GRSPW destination key.*/
    __volatile__ uint32_t time; /* 0x14-0x17 GRSPW time register.*/
    __volatile__ uint32_t timerDisc; /* 0x18-0x1B GRSPW timer and disconnect register.*/
    __volatile__ uint32_t notUsed0; /* 0x1C-0x1F */
    __volatile__ uint32_t dmaControl; /* 0x20-0x23 GRSPW DMA control register*/
    __volatile__ uint32_t dmaMaxRxLength; /* 0x24-0x27 DMA channel 1 rx maximum length*/
    __volatile__ uint32_t dmaTxDescAddr; /* 0x28-0x2B DMA channel 1 transmit descriptor table address.*/
    __volatile__ uint32_t dmaRxDescAddr; /* 0x2C-0x2F DMA channel 1 receive descriptor table address.*/
} stixSpwRegs_s;


/* Interrupt structure */
typedef enum 
{
    E_FLASH_IRQ = IRQ_FLASH /* Interrupt indicating that last operation was finished. */,
    E_UART_IRQ = IRQ_UART /* Interrupt indicating reception/ transmission of character or FIFO empty/full status. */,
    E_STIXCTRL_DETCTRL_IRQ = IRQ_STIXCTRL_DETCTRL /* Detector Controller interrupt. (Internal FIFO overflow, SEU in Detectors, Calibration report ready, Quick Look buffers switch)  */,
    E_STIXCTRL_HK_IRQ = IRQ_STIXCTRL_HK /* Housekeeping Monitored Value exceeded threshold. */,
    E_STIXCTRL_OTHERISON_IRQ = IRQ_STIXCTRL_OTHERISON /* Interrupt triggered when Other IDPU becomes On. */,
    E_STIXCTRL_EDAC_IRQ = IRQ_STIXCTRL_EDAC /* Errors (DED) in Detector's Cache Memory or in Rotating Buffer's SDRAM memory. Cache EDAC Interrrupt is reported if OpMode of Cache memory is set to 0. */,
    E_STIXCTRL_ROTB_IRQ = IRQ_STIXCTRL_ROTB /* Rotating Buffer overflow interrupt. This memory is reportet whenever HW writes new block and Rotating Buffer memory is full. */,
    E_TIMER_TIMER0_IRQ = IRQ_TIMER_TIMER0 /* Timer0 overflow interrupt. */,
    E_TIMER_TIMER1_IRQ = IRQ_TIMER_TIMER1 /* Timer1 overflow interrupt. */,
    E_SPW_IRQ = IRQ_SPW /* Default Interrupt for GRSPW controller. */} stixIdpuIrqs_e;
#endif /*__STIXIDPU_H__*/
