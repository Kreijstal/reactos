/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS RTL8168/8111 driver
 * FILE:        rtlhw.h
 * PURPOSE:     Realtek 8168/8111 GbE register / descriptor definitions
 *
 * Register layout ported from the Linux r8169 driver
 * (drivers/net/ethernet/realtek/r8169_main.c), licensed GPLv2.
 */

#pragma once

#define MAXIMUM_MULTICAST_ADDRESSES 32

/* Standard registers (8169/8168/8101 common block) */
#define R_MAC0              0x00    /* MAC address bytes 0..3 */
#define R_MAC4              0x04    /* MAC address bytes 4..5 */
#define R_MAR0              0x08    /* Multicast filter, 8 bytes */
#define R_TXDESC_LO         0x20
#define R_TXDESC_HI         0x24
#define R_TXHDESC_LO        0x28
#define R_TXHDESC_HI        0x2C
#define R_FLASH             0x30
#define R_ERSR              0x36
#define R_CMD               0x37    /* ChipCmd */
#define R_TXPOLL            0x38
#define R_IM                0x3C    /* IntrMask */
#define R_IS                0x3E    /* IntrStatus */
#define R_TC                0x40    /* TxConfig (also chip version field) */
#define R_RC                0x44    /* RxConfig */
#define R_CFG9346           0x50
#define R_CFG0              0x51
#define R_CFG1              0x52
#define R_CFG2              0x53
#define R_CFG3              0x54
#define R_CFG4              0x55
#define R_CFG5              0x56
#define R_PHYAR             0x60
#define R_PHYSTS            0x6C
#define R_PMCH              0x6F    /* Power-management clock control */
#define D3_NO_PLL_DOWN      0xC0    /* Bits 6+7 -- keep PLL up in D3 */
#define R_EEE_LED           0xDB    /* EEE LED frequency control */
#define R_ERIDR             0x70    /* ERI data register */
#define R_ERIAR             0x74    /* ERI address register */
#define R_EPHYAR            0x80    /* EPHY (PCIe analog) indirect register */
#define R_OCPDR             0xB0    /* MAC OCP data register */
#define R_OCPAR             0xB4
#define R_GPHY_OCP          0xB8    /* GPHY OCP register (8168G+ MDIO path) */
#define R_DLLPR             0xD0
#define R_MCU               0xD3
#define R_RXMAXSIZE         0xDA
#define R_CPLUSCMD          0xE0
#define R_IMITIGATE         0xE2
#define R_RXDESC_LO         0xE4
#define R_RXDESC_HI         0xE8
#define R_MAXTXPKTSIZE      0xEC    /* 8101/8168 unit of 128 bytes */
#define R_MISC              0xF0    /* 8168E and later */
#define R_MISC1             0xF2    /* MISC_1, 8168H and later */
#define R_DBGREG            0xD1    /* DBG_REG */
#define R_IBCR0             0xF8    /* 8168EP CMAC */
#define R_IBCR2             0xF9    /* 8168EP CMAC */
#define R_IBISR0            0xFB    /* 8168EP CMAC */

/* MaxTxPacketSize values (unit of 128 bytes) -- Linux TxPacketMax/EarlySize */
#define TXPKT_MAX           (8064 >> 7)     /* pre-8168E-VL */
#define TXPKT_EARLY_SIZE    0x27            /* 8168E-VL and later */

/* DBG_REG bits */
#define DBGREG_FIX_NAK_1    0x10    /* BIT(4) */
#define DBGREG_FIX_NAK_2    0x08    /* BIT(3) */

/* ERI (Extended Register Indirect) -- indirect access to chip-internal regs
 * 0x00..0x2FF.  Used for FIFO sizing, pause thresholds, ASPM, EEE. */
#define ERIAR_FLAG          0x80000000UL
#define ERIAR_WRITE_CMD     0x80000000UL
#define ERIAR_READ_CMD      0x00000000UL
#define ERIAR_TYPE_SHIFT    16
#define ERIAR_EXGMAC        (0x00U << ERIAR_TYPE_SHIFT)
#define ERIAR_MSIX          (0x01U << ERIAR_TYPE_SHIFT)
#define ERIAR_OOB           (0x02U << ERIAR_TYPE_SHIFT)
#define ERIAR_MASK_SHIFT    12
#define ERIAR_MASK_0001     (0x1U << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_0011     (0x3U << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_0100     (0x4U << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_0101     (0x5U << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_1111     (0xFU << ERIAR_MASK_SHIFT)

/* EPHY (PCIe analog PHY) indirect access */
#define EPHYAR_FLAG         0x80000000UL
#define EPHYAR_WRITE_CMD    0x80000000UL
#define EPHYAR_REG_MASK     0x1F
#define EPHYAR_REG_SHIFT    16
#define EPHYAR_DATA_MASK    0xFFFFU

/* GPHY OCP (used by 8168G+ to reach the MDIO bus) */
#define OCPAR_FLAG          0x80000000UL
#define OCP_STD_PHY_BASE    0xA400

/* CONFIG1 bits */
#define CFG1_SPEED_DOWN     0x10    /* BIT(4) */

/* CONFIG2 bits */
#define CFG2_PMSTS_EN       0x20
#define CFG2_CLKREQEN       0x80    /* Clock Request Enable */

/* CONFIG3 bits */
#define CFG3_BEACON_EN      0x01    /* 8168 only. Reserved in the 8168b */
#define CFG3_RDY_TO_L23     0x02    /* L23 Enable */
#define CFG3_JUMBO_EN0      0x04    /* 8168 only. Reserved in the 8168b */

/* CONFIG4 bits */
#define CFG4_JUMBO_EN1      0x02    /* 8168 only. Reserved in the 8168b */

/* CONFIG5 bits */
#define CFG5_ASPM_EN        0x01    /* ASPM enable */
#define CFG5_LANWAKE        0x02
#define CFG5_SPI_EN         0x08
#define CFG5_PME_STS        0x40

/* MII basic mode control register bits (PHY register 0) */
#define MII_BMCR            0x00
#define BMCR_RESET          0x8000
#define BMCR_ANENABLE       0x1000
#define BMCR_PDOWN          0x0800
#define BMCR_ANRESTART      0x0200

/* DLLPR bits (8168E+) */
#define DLLPR_PFM_EN        0x40    /* BIT(6) */
#define DLLPR_TX_10M_PS_EN  0x80    /* BIT(7) */

/* MISC_1 bits */
#define MISC1_PFM_D3COLD_EN 0x40    /* BIT(6) */

/* MCU bits */
#define MCU_NOW_IS_OOB      0x80    /* Out-of-band (management) mode */
#define MCU_TX_EMPTY        0x20
#define MCU_RX_EMPTY        0x10
#define MCU_RXTX_EMPTY      (MCU_TX_EMPTY | MCU_RX_EMPTY)
#define MCU_LINK_LIST_RDY   0x02    /* BIT(1) */

/* MISC bits (8168E+, 32-bit) */
#define MISC_RXDV_GATED_EN  0x00080000  /* Gates RX-data-valid -- chip drops all RX while set */
#define MISC_PWM_EN         0x00400000
#define MISC_TXPLA_RST      0x20000000  /* TX FIFO pointer reset */
#define MISC_DISABLE_LAN_EN 0x80000000  /* 8168FP-only OOB disable */

/* ChipCmd bits (R_CMD) */
#define B_CMD_STOPREQ       0x80
#define B_CMD_RESET         0x10
#define B_CMD_RXENB         0x08
#define B_CMD_TXENB         0x04
#define B_CMD_RXBUFEMPTY    0x01

/* TXPoll bits */
#define B_TXP_HPQ           0x80
#define B_TXP_NPQ           0x40
#define B_TXP_FSWINT        0x01

/* IntrMask / IntrStatus bits */
#define R_I_SYSERR          0x8000
#define R_I_PCSTMOUT        0x4000
#define R_I_SWINT           0x0100
#define R_I_TXDESCUNAVAIL   0x0080
#define R_I_RXFIFOOVR       0x0040
#define R_I_LINKCHG         0x0020
#define R_I_RXOVRFLW        0x0010
#define R_I_TXERR           0x0008
#define R_I_TXOK            0x0004
#define R_I_RXERR           0x0002
#define R_I_RXOK            0x0001

#define DEFAULT_INTERRUPT_MASK  (R_I_RXOK | R_I_RXERR | R_I_TXOK | R_I_TXERR | \
                                 R_I_RXOVRFLW | R_I_LINKCHG |                 \
                                 R_I_RXFIFOOVR | R_I_PCSTMOUT | R_I_SYSERR)

/* Cfg9346 */
#define CFG9346_LOCK        0x00
#define CFG9346_UNLOCK      0xC0

/* RxConfig bits */
#define RX_RX128_INT_EN     (1 <<15)    /* 8111c and later */
#define RX_MULTI_EN         (1 << 14)   /* 8111c only */
#define RX_FIFO_THRESH      (7 << 13)   /* no FIFO threshold before first PCI xfer */
#define RX_EARLY_OFF        (1 << 11)
#define RX_DMA_BURST        (7 <<  8)   /* unlimited burst */
#define RX_ACCEPT_ERR       0x20
#define RX_ACCEPT_RUNT      0x10
#define RX_ACCEPT_BCAST     0x08
#define RX_ACCEPT_MCAST     0x04
#define RX_ACCEPT_MYPHYS    0x02
#define RX_ACCEPT_ALLPHYS   0x01
#define RX_ACCEPT_OK_MASK   0x0F
#define RX_ACCEPT_MASK      0x3F

/* TxConfig (TxDMAShift=8, TxIFGShift=24).  Chip version in bits 30..23. */
#define TXCFG_VER_MASK      0x7CF00000U /* per Linux rtl_get_mac_version */
#define TXCFG_DMA_UNLIMITED (7 <<  8)
#define TXCFG_IFG_NORMAL    (3 << 24)
#define TXCFG_AUTO_FIFO     (1 <<  7)   /* 8111e-vl */
#define TXCFG_EMPTY         (1 << 11)   /* 8111e-vl */

/* CPlusCmd bits */
#define CPCMD_RXCHKSUM      (1 << 5)
#define CPCMD_PCIDAC        (1 << 4)
#define CPCMD_PCI_MRW       (1 << 3)
#define CPCMD_NORMAL        (1 << 13)

/* PHYSTS */
#define PHYSTS_TBI_ENABLE   0x80
#define PHYSTS_TXFLOWCTRL   0x40
#define PHYSTS_RXFLOWCTRL   0x20
#define PHYSTS_1000_FD      0x10
#define PHYSTS_100          0x08
#define PHYSTS_10           0x04
#define PHYSTS_LINK_UP      0x02
#define PHYSTS_FULL_DUPLEX  0x01

/* PHYAR (MDIO indirect) */
#define PHYAR_FLAG          0x80000000U
#define PHYAR_DATA_MASK     0x0000FFFFU
#define PHYAR_REG_SHIFT     16

/* Descriptor bits (opts1) */
#define DESC_OWN            (1U << 31)
#define DESC_EOR            (1U << 30)  /* End of ring */
#define DESC_FS             (1U << 29)  /* First segment */
#define DESC_LS             (1U << 28)  /* Last segment */
#define DESC_LEN_MASK       0x0000FFFFU

/* TX desc opts1 bits (chip-class 1; 8168c and later) */
#define TXD1_LSO            (1U << 27)
#define TXD2_IPv4_CS        (1U << 29)
#define TXD2_TCP_CS         (1U << 30)
#define TXD2_UDP_CS         (1U << 31)

#define RXD_FAE             (1U << 27)  /* Frame alignment error */
#define RXD_CRC             (1U << 19)  /* CRC error */
#define RXD_RUNT            (1U << 20)
#define RXD_RES             (1U << 21)
#define RXD_RWT             (1U << 22)

#define MAXIMUM_FRAME_SIZE  1514
#define MINIMUM_FRAME_SIZE  60
#define RX_BUF_SIZE         2048

#define TX_DESC_COUNT       64
#define RX_DESC_COUNT       64

/* Each descriptor is 16 bytes (32-bit opts1, 32-bit opts2, 64-bit addr).
 * Ring must be 256-byte aligned per Realtek datasheet.  */
typedef struct _RTL_DESC {
    ULONG opts1;
    ULONG opts2;
    ULONGLONG addr;
} RTL_DESC, *PRTL_DESC;

#define IEEE_802_ADDR_LENGTH 6

typedef struct _ETH_HEADER {
    UCHAR Destination[IEEE_802_ADDR_LENGTH];
    UCHAR Source[IEEE_802_ADDR_LENGTH];
    USHORT PayloadType;
} ETH_HEADER, *PETH_HEADER;

/* Chip versions we accept and report (subset of Linux RTL_GIGA_MAC_VER_*).
 * Unknown silicon fails MiniportInitialize, matching Linux which refuses
 * to probe an unrecognized XID. */
typedef enum _RTL_MAC_VER {
    RTL_MAC_VER_UNKNOWN = 0,
    RTL_MAC_VER_11,     /* RTL8168B / RTL8111B */
    RTL_MAC_VER_17,     /* RTL8168B / RTL8111B */
    RTL_MAC_VER_18,     /* RTL8168CP/8111CP */
    RTL_MAC_VER_19,     /* RTL8168C/8111C */
    RTL_MAC_VER_20,
    RTL_MAC_VER_21,
    RTL_MAC_VER_22,
    RTL_MAC_VER_23,
    RTL_MAC_VER_24,
    RTL_MAC_VER_25,     /* RTL8168D/8111D */
    RTL_MAC_VER_26,
    RTL_MAC_VER_28,     /* RTL8168DP/8111DP */
    RTL_MAC_VER_29,     /* RTL8105E */
    RTL_MAC_VER_30,
    RTL_MAC_VER_31,
    RTL_MAC_VER_32,     /* RTL8168E/8111E -- baseline target (rainb) */
    RTL_MAC_VER_33,
    RTL_MAC_VER_34,
    RTL_MAC_VER_35,     /* RTL8168F/8111F */
    RTL_MAC_VER_36,
    RTL_MAC_VER_37,
    RTL_MAC_VER_38,
    RTL_MAC_VER_39,
    RTL_MAC_VER_40,     /* RTL8168G/8111G */
    RTL_MAC_VER_42,
    RTL_MAC_VER_43,
    RTL_MAC_VER_44,
    RTL_MAC_VER_46,     /* RTL8168H/8111H */
    RTL_MAC_VER_48,
    RTL_MAC_VER_51,
} RTL_MAC_VER;
