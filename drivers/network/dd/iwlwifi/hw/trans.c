/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     PCIe transport power-up: take the device away from
 *              firmware, bring the APM to D0A, and make the register file
 *              past the CSR block reachable.
 *
 * Mirrors iwl_pcie_prepare_card_hw() / iwl_pcie_apm_init() /
 * iwl_finish_nic_init() / iwl_pcie_apm_stop() from Linux's
 * drivers/net/wireless/intel/iwlwifi/pcie/trans.c.
 *
 * NOTHING here touches PRPH or device memory.  Until the APM reports
 * MAC_CLOCK_READY the only reachable window is the 1 KiB CSR block, and
 * reads past it return garbage rather than faulting - which is exactly the
 * kind of silent-zero trap that eats a debugging session.  Every step
 * below therefore checks its own completion bit instead of assuming the
 * previous write took effect.
 */

#include "../iwlwifi.h"

#define NDEBUG
#include <debug.h>

/*
 * Busy-waiting is only acceptable for the short device-timing delays the
 * datasheet asks for.  Anything longer yields, which MiniportInitializeEx
 * (PASSIVE_LEVEL) is allowed to do.
 */
#define IWL_STALL_MAX_US    100

VOID
IwlDelayUs(_In_ ULONG Microseconds)
{
    LARGE_INTEGER Interval;

    if (Microseconds <= IWL_STALL_MAX_US || KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        KeStallExecutionProcessor(Microseconds);
        return;
    }

    /* Relative interval, 100 ns units. */
    Interval.QuadPart = -((LONGLONG)Microseconds * 10);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

BOOLEAN
IwlPollBit(
    _In_ PIWL_ADAPTER Adapter,
    _In_ ULONG Offset,
    _In_ ULONG Expected,
    _In_ ULONG Mask,
    _In_ ULONG TimeoutUs)
{
    ULONG Elapsed = 0;

    do
    {
        if ((IwlRead32(Adapter, Offset) & Mask) == Expected)
            return TRUE;

        IwlDelayUs(IWL_PREPARE_POLL_STEP_US);
        Elapsed += IWL_PREPARE_POLL_STEP_US;
    } while (Elapsed < TimeoutUs);

    return FALSE;
}

/*
 * iwl_pcie_set_hw_ready(): one short poll for NIC_READY.  Success also
 * publishes "the driver is alive" to the management engine, which is what
 * stops it from taking the device back.
 */
static BOOLEAN
IwlSetHwReady(_In_ PIWL_ADAPTER Adapter)
{
    ULONG Elapsed = 0;

    do
    {
        if (IwlRead32(Adapter, CSR_HW_IF_CONFIG_REG) &
            CSR_HW_IF_CONFIG_REG_BIT_NIC_READY)
        {
            IwlSetBit(Adapter, CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
            return TRUE;
        }

        KeStallExecutionProcessor(5);
        Elapsed += 5;
    } while (Elapsed < IWL_HW_READY_TIMEOUT_US);

    return FALSE;
}

NDIS_STATUS
IwlPrepareCardHw(_In_ PIWL_ADAPTER Adapter)
{
    ULONG Attempt;

    /* Common case: nobody else holds the device. */
    if (IwlSetHwReady(Adapter))
    {
        DPRINT1("iwlwifi: device was already ready\n");
        return NDIS_STATUS_SUCCESS;
    }

    /* Otherwise the link power-management state can keep the device from
     * answering; disable it before asking again. */
    IwlSetBit(Adapter, CSR_DBG_LINK_PWR_MGMT_REG,
              CSR_RESET_LINK_PWR_MGMT_DISABLED);
    IwlDelayUs(2000);

    for (Attempt = 0; Attempt < IWL_PREPARE_ATTEMPTS; Attempt++)
    {
        ULONG Elapsed = 0;

        IwlSetBit(Adapter, CSR_HW_IF_CONFIG_REG,
                  CSR_HW_IF_CONFIG_REG_PREPARE);

        do
        {
            if (IwlSetHwReady(Adapter))
            {
                DPRINT1("iwlwifi: device ready after %u PREPARE attempt(s)\n",
                        Attempt + 1);
                return NDIS_STATUS_SUCCESS;
            }

            IwlDelayUs(IWL_PREPARE_POLL_STEP_US);
            Elapsed += IWL_PREPARE_POLL_STEP_US;
        } while (Elapsed < IWL_PREPARE_POLL_BUDGET_US);

        IwlDelayUs(IWL_PREPARE_BACKOFF_US);
    }

    DPRINT1("iwlwifi: device never reported NIC_READY (HW_IF_CONFIG=0x%08x); "
            "the platform firmware or management engine still owns it\n",
            IwlRead32(Adapter, CSR_HW_IF_CONFIG_REG));
    return NDIS_STATUS_ADAPTER_NOT_READY;
}

/*
 * iwl_finish_nic_init(): move the adapter from D0U* to D0A* and wait for
 * the MAC clock.  Past this point the register file beyond the CSR block
 * is reachable.
 */
static NDIS_STATUS
IwlFinishNicInit(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->Cfg->Family >= IWL_DEVICE_FAMILY_AX210)
    {
        /* AX210 and later gate the whole bring-up on the clock request
         * being raised first; INIT_DONE alone is not enough. */
        IwlSetBit(Adapter, CSR_GP_CNTRL,
                  CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY);
        IwlDelayUs(2);
    }

    IwlSetBit(Adapter, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);

    if (!IwlPollBit(Adapter,
                    CSR_GP_CNTRL,
                    CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                    CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                    IWL_CLOCK_READY_TIMEOUT_US))
    {
        DPRINT1("iwlwifi: MAC clock never stabilised (GP_CNTRL=0x%08x)\n",
                IwlRead32(Adapter, CSR_GP_CNTRL));
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
IwlApmInit(_In_ PIWL_ADAPTER Adapter)
{
    NDIS_STATUS Status;

    DPRINT1("iwlwifi: init APM\n");

    /* Disable the L0S exit timer.  Platform NMI work-around; the timer was
     * removed on AX210 and later, where the bit means something else. */
    if (Adapter->Cfg->Family < IWL_DEVICE_FAMILY_AX210)
    {
        IwlSetBit(Adapter, CSR_GIO_CHICKEN_BITS,
                  CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER);
    }

    /* Disable L0s without touching L1 - do not wait for the ICH's L0s
     * handshake, which some chipsets never complete. */
    IwlSetBit(Adapter, CSR_GIO_CHICKEN_BITS,
              CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);

    /* Push the FH wait threshold to maximum; work-around for a hardware
     * error seen under sustained DMA load. */
    IwlSetBit(Adapter, CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);

    /* Let an interrupt from the management bus wake the device. */
    IwlSetBit(Adapter, CSR_HW_IF_CONFIG_REG,
              CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);

    /* Program the analog PLL before activating to D0A on the parts whose
     * configuration asks for it. */
    if (Adapter->Cfg->Flags & IWL_CFG_PLL_CFG)
        IwlSetBit(Adapter, CSR_ANA_PLL_CFG, CSR50_ANA_PLL_CFG_VAL);

    Status = IwlFinishNicInit(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    InterlockedOr(&Adapter->Flags, IWL_FLAG_APM_UP);
    return NDIS_STATUS_SUCCESS;
}

/* iwl_pcie_apm_stop_master(): stop DMA before anything is torn down. */
static VOID
IwlApmStopMaster(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->Cfg->Family >= IWL_DEVICE_FAMILY_9000)
    {
        IwlSetBit(Adapter, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_POWER_SAVE);

        if (!IwlPollBit(Adapter,
                        CSR_GP_CNTRL,
                        0,
                        CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                        IWL_MASTER_DISABLE_TIMEOUT_US))
        {
            DPRINT1("iwlwifi: timeout entering MAC power save\n");
        }
        return;
    }

    IwlSetBit(Adapter, CSR_RESET, CSR_RESET_REG_FLAG_STOP_MASTER);

    if (!IwlPollBit(Adapter,
                    CSR_RESET,
                    CSR_RESET_REG_FLAG_MASTER_DISABLED,
                    CSR_RESET_REG_FLAG_MASTER_DISABLED,
                    IWL_MASTER_DISABLE_TIMEOUT_US))
    {
        DPRINT1("iwlwifi: timeout waiting for DMA master to stop\n");
    }
}

VOID
IwlApmStop(_In_ PIWL_ADAPTER Adapter)
{
    if (!(Adapter->Flags & IWL_FLAG_APM_UP))
        return;

    DPRINT1("iwlwifi: stop APM\n");

    IwlApmStopMaster(Adapter);

    /* Clear "initialization complete", returning the device to D0U. */
    IwlClearBit(Adapter, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);

    InterlockedAnd(&Adapter->Flags, ~IWL_FLAG_APM_UP);
}

VOID
IwlSwReset(_In_ PIWL_ADAPTER Adapter)
{
    IwlSetBit(Adapter, CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);

    /* The reset needs settling time before the register file answers
     * sanely again; upstream uses 5 ms on pre-AX210 parts and 2 ms from
     * AX210 on. */
    if (Adapter->Cfg->Family >= IWL_DEVICE_FAMILY_AX210)
        IwlDelayUs(2000);
    else
        IwlDelayUs(5000);
}

VOID
IwlDisableInterrupts(_In_ PIWL_ADAPTER Adapter)
{
    /* Mask everything, then acknowledge whatever was already latched so a
     * shared line is not left asserted. */
    IwlWrite32(Adapter, CSR_INT_MASK, 0);
    IwlWrite32(Adapter, CSR_INT, 0xFFFFFFFF);
    IwlWrite32(Adapter, CSR_FH_INT_STATUS, 0xFFFFFFFF);
}
