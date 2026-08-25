/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Control-and-Status Register definitions for Intel Wireless
 *              PCIe devices.
 *
 * Names and values are kept VERBATIM from Linux drivers/net/wireless/intel/
 * iwlwifi/iwl-csr.h so this file diffs cleanly against upstream.  Do not
 * rename anything here to house style - the whole point is that a future
 * resync is a mechanical diff.
 *
 * The CSR block is the first 1 KiB of BAR0 and is the only register space
 * reachable before the device is powered up.  Everything else (PRPH, direct
 * memory) requires the APM to be in D0A first; see hw/trans.c.
 */

#ifndef _IWLWIFI_CSR_H_
#define _IWLWIFI_CSR_H_

/* ------------------------------------------------------------------ */
/* CSR register offsets from the start of BAR0                        */
/* ------------------------------------------------------------------ */

#define CSR_HW_IF_CONFIG_REG        0x000
#define CSR_INT_COALESCING          0x004
#define CSR_INT                     0x008
#define CSR_INT_MASK                0x00c
#define CSR_FH_INT_STATUS           0x010
#define CSR_GPIO_IN                 0x018
#define CSR_RESET                   0x020
#define CSR_GP_CNTRL                0x024
#define CSR_HW_REV                  0x028
#define CSR_EEPROM_REG              0x02c
#define CSR_EEPROM_GP               0x030
#define CSR_OTP_GP_REG              0x034
#define CSR_GIO_REG                 0x03C
#define CSR_GP_UCODE_REG            0x048
#define CSR_GP_DRIVER_REG           0x050
#define CSR_UCODE_DRV_GP1           0x054
#define CSR_UCODE_DRV_GP1_SET       0x058
#define CSR_UCODE_DRV_GP1_CLR       0x05c
#define CSR_UCODE_DRV_GP2           0x060
#define CSR_MBOX_SET_REG            0x088
#define CSR_LED_REG                 0x094
#define CSR_DRAM_INT_TBL_REG        0x0A0
#define CSR_MAC_SHADOW_REG_CTRL     0x0A8

/* GIO Chicken Bits (PCI Express bus link power management) */
#define CSR_GIO_CHICKEN_BITS        0x100

/* Analog phase-lock-loop configuration */
#define CSR_ANA_PLL_CFG             0x20c

/*
 * Hardware revision workaround register.  Indicates the "dash" value of a
 * revision the main CSR_HW_REV cannot express on some parts.
 */
#define CSR_HW_REV_WA_REG           0x22C

#define CSR_DBG_HPET_MEM_REG        0x240
#define CSR_DBG_LINK_PWR_MGMT_REG   0x250

/* RF identification, present from family 9000 on. */
#define CSR_HW_RF_ID                0x09c

/* ------------------------------------------------------------------ */
/* CSR_HW_IF_CONFIG_REG bits                                          */
/* ------------------------------------------------------------------ */

#define CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH       (0x00000003)
#define CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP       (0x0000000C)
#define CSR_HW_IF_CONFIG_REG_MSK_BOARD_VER      (0x000000C0)
#define CSR_HW_IF_CONFIG_REG_BIT_MAC_SI         (0x00000100)
#define CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI       (0x00000200)
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE       (0x00000C00)
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH       (0x00003000)
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP       (0x0000C000)

#define CSR_HW_IF_CONFIG_REG_POS_PHY_DASH       (12)
#define CSR_HW_IF_CONFIG_REG_POS_PHY_STEP       (14)

#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A   (0x00080000)
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY      (0x00400000)
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE (0x02000000)
#define CSR_HW_IF_CONFIG_REG_PREPARE            (0x08000000)
#define CSR_HW_IF_CONFIG_REG_ENABLE_PME         (0x10000000)
#define CSR_HW_IF_CONFIG_REG_PERSIST_MODE       (0x40000000)

/* ------------------------------------------------------------------ */
/* CSR_INT / CSR_INT_MASK bits                                        */
/* ------------------------------------------------------------------ */

#define CSR_INT_BIT_FH_RX           (1 << 31)   /* Rx DMA, cmd responses */
#define CSR_INT_BIT_HW_ERR          (1 << 29)   /* DMA hardware error */
#define CSR_INT_BIT_RX_PERIODIC     (1 << 28)   /* Rx periodic */
#define CSR_INT_BIT_FH_TX           (1 << 27)   /* Tx DMA FH_INT[1:0] */
#define CSR_INT_BIT_SCD             (1 << 26)
#define CSR_INT_BIT_SW_ERR          (1 << 25)   /* uCode error */
#define CSR_INT_BIT_PAGING          (1 << 24)   /* SDIO PAGING */
#define CSR_INT_BIT_RF_KILL         (1 << 7)    /* HW RFKILL switch toggled */
#define CSR_INT_BIT_CT_KILL         (1 << 6)    /* Critical temp reached */
#define CSR_INT_BIT_SW_RX           (1 << 3)    /* Rx, command responses */
#define CSR_INT_BIT_WAKEUP          (1 << 1)    /* NIC controller waking up */
#define CSR_INT_BIT_ALIVE           (1 << 0)    /* uCode interrupts once ready */

/* ------------------------------------------------------------------ */
/* CSR_RESET bits                                                     */
/* ------------------------------------------------------------------ */

#define CSR_RESET_REG_FLAG_NEVO_RESET       (0x00000001)
#define CSR_RESET_REG_FLAG_FORCE_NMI        (0x00000002)
#define CSR_RESET_REG_FLAG_SW_RESET         (0x00000080)
#define CSR_RESET_REG_FLAG_MASTER_DISABLED  (0x00000100)
#define CSR_RESET_REG_FLAG_STOP_MASTER      (0x00000200)
#define CSR_RESET_LINK_PWR_MGMT_DISABLED    (0x80000000)

/* ------------------------------------------------------------------ */
/* CSR_GP_CNTRL bits                                                  */
/* ------------------------------------------------------------------ */

#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY   (0x00000001)
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE         (0x00000004)
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ    (0x00000008)
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP    (0x00000010)
#define CSR_GP_CNTRL_REG_FLAG_XTAL_ON           (0x00000400)

#define CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN      (0x00000001)

#define CSR_GP_CNTRL_REG_MSK_POWER_SAVE_TYPE    (0x07000000)
#define CSR_GP_CNTRL_REG_FLAG_MAC_POWER_SAVE    (0x04000000)
#define CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW     (0x08000000)

/*
 * From AX210 on the bring-up bits moved.  iwl_finish_nic_init() asserts
 * MAC_CLOCK_READY first on those parts, then INIT_DONE.
 */
#define CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ (0x00000020)

/* ------------------------------------------------------------------ */
/* CSR_HW_REV decoding                                                */
/* ------------------------------------------------------------------ */

#define CSR_HW_REV_TYPE_MSK         (0x000FFF0)
#define CSR_HW_REV_DASH(_val)       (((_val) & 0x0000003) >> 0)
#define CSR_HW_REV_STEP(_val)       (((_val) & 0x000000C) >> 2)
#define CSR_HW_REV_TYPE(_val)       (((_val) & 0x000FFF0) >> 4)

#define SILICON_A_STEP              0
#define SILICON_B_STEP              1
#define SILICON_C_STEP              2
#define SILICON_D_STEP              3
#define SILICON_Z_STEP              0xf

/* CSR_HW_RF_ID decoding */
#define CSR_HW_RF_ID_TYPE_CHIP_ID(_val)     (((_val) >> 12) & 0xFFF)
#define CSR_HW_RF_ID_TYPE_STEP(_val)        (((_val) >> 8) & 0xF)
#define CSR_HW_RF_ID_TYPE_DASH(_val)        (((_val) >> 4) & 0xF)

/* ------------------------------------------------------------------ */
/* CSR_GIO_CHICKEN_BITS bits                                          */
/* ------------------------------------------------------------------ */

#define CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX      (0x00800000)
#define CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER (0x20000000)

/* CSR_GIO_REG */
#define CSR_GIO_REG_VAL_L0S_ENABLED     (0x00000002)

/* CSR_MBOX_SET_REG */
#define CSR_MBOX_SET_REG_OS_ALIVE       0x20

/* CSR_DBG_HPET_MEM_REG: FH wait threshold, max value (HW-error W/A) */
#define CSR_DBG_HPET_MEM_REG_VAL        (0xFFFF0000)

/* CSR_ANA_PLL_CFG value for parts whose config asks for it */
#define CSR50_ANA_PLL_CFG_VAL           (0x00880300)

/* CSR_UCODE_DRV_GP1 bits */
#define CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP     (0x00000001)
#define CSR_UCODE_SW_BIT_RFKILL             (0x00000002)
#define CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED   (0x00000004)
#define CSR_GP1_MSK_SW_RESET_ACTIVE         (0x00000008)

/* ------------------------------------------------------------------ */
/* Timeouts (microseconds unless stated)                              */
/* ------------------------------------------------------------------ */

/* iwl_pcie_set_hw_ready() polls NIC_READY for this long per attempt. */
#define IWL_HW_READY_TIMEOUT_US         50
/* iwl_pcie_prepare_card_hw() inner poll budget after asserting PREPARE. */
#define IWL_PREPARE_POLL_BUDGET_US      150000
#define IWL_PREPARE_POLL_STEP_US        200
/* ...and how many outer PREPARE attempts before giving up. */
#define IWL_PREPARE_ATTEMPTS            10
#define IWL_PREPARE_BACKOFF_US          25000
/* iwl_finish_nic_init() clock-stabilisation poll. */
#define IWL_CLOCK_READY_TIMEOUT_US      25000
/* iwl_pcie_apm_stop_master() master-disable poll. */
#define IWL_MASTER_DISABLE_TIMEOUT_US   100

#endif /* _IWLWIFI_CSR_H_ */
