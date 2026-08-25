/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Device family enumeration and the PCI-ID -> configuration
 *              table.
 *
 * This table is the whole point of the driver's breadth: one transport
 * implementation covers every Intel Wireless part whose bring-up sequence
 * is the same, and adding a part is a table row plus a firmware pin.
 *
 * A device ID that is NOT in this table is never claimed.  We do not fall
 * back to "probably close enough" - an unrecognised part gets
 * NDIS_STATUS_ADAPTER_NOT_FOUND rather than a speculative power-on
 * sequence, for the same reason ar9485 refuses an unexpected AR_SREV.
 */

#ifndef _IWLWIFI_DEVICES_H_
#define _IWLWIFI_DEVICES_H_

#include <ndis.h>

/*
 * Device families, in the order Linux's enum iwl_device_family declares
 * them.  The ordering is load-bearing: several bring-up decisions are
 * written as ">= IWL_DEVICE_FAMILY_xxx" comparisons.
 */
typedef enum _IWL_DEVICE_FAMILY
{
    IWL_DEVICE_FAMILY_UNDEFINED = 0,
    IWL_DEVICE_FAMILY_1000,
    IWL_DEVICE_FAMILY_100,
    IWL_DEVICE_FAMILY_2000,
    IWL_DEVICE_FAMILY_2030,
    IWL_DEVICE_FAMILY_105,
    IWL_DEVICE_FAMILY_135,
    IWL_DEVICE_FAMILY_5000,
    IWL_DEVICE_FAMILY_5150,
    IWL_DEVICE_FAMILY_6000,
    IWL_DEVICE_FAMILY_6000i,
    IWL_DEVICE_FAMILY_6005,
    IWL_DEVICE_FAMILY_6030,
    IWL_DEVICE_FAMILY_6050,
    IWL_DEVICE_FAMILY_6150,
    IWL_DEVICE_FAMILY_7000,
    IWL_DEVICE_FAMILY_8000,
    IWL_DEVICE_FAMILY_9000,
    IWL_DEVICE_FAMILY_22000,
    IWL_DEVICE_FAMILY_AX210,
    IWL_DEVICE_FAMILY_BZ,
    IWL_DEVICE_FAMILY_SC
} IWL_DEVICE_FAMILY;

/* Per-device configuration flags. */

/* Part needs CSR_ANA_PLL_CFG programmed before leaving D0U. */
#define IWL_CFG_PLL_CFG                 0x00000001
/* Part's firmware name is composed from the MAC/RF pair discovered at
 * runtime (family 9000 and later) rather than being fixed in this table.
 * FwNamePre then holds the MAC half only. */
#define IWL_CFG_RF_COMPOSED_FW_NAME     0x00000002
/* Integrated CNVi part: the PCI function is only the MAC, the radio lives
 * on a separate CRF module across the CNVio link. */
#define IWL_CFG_INTEGRATED              0x00000004
/* Part needs a Platform NVM blob (iwlwifi-<pre>.pnvm) in addition to the
 * .ucode container.  AX210 and later only - the earlier families carry the
 * equivalent data on-die.  The blob is pushed AFTER the firmware reports
 * ALIVE, because the SKU ID that selects the right section out of it comes
 * from that ALIVE response. */
#define IWL_CFG_NEEDS_PNVM              0x00000008

typedef struct _IWL_DEVICE_CFG
{
    USHORT              DeviceId;
    /* 0xFFFF matches any subsystem ID.  Several device IDs are shared by
     * parts that differ only in their subsystem ID; the more specific rows
     * must precede the wildcard row. */
    USHORT              SubsystemId;
    PCSTR               Name;
    IWL_DEVICE_FAMILY   Family;
    /* Firmware base name, i.e. the "X" in iwlwifi-X-<api>.ucode. */
    PCSTR               FwNamePre;
    /* Highest and lowest firmware API revision this driver understands for
     * the part.  Probing walks down from Max to Min, which is exactly what
     * iwl_request_firmware() does. */
    UCHAR               UcodeApiMax;
    UCHAR               UcodeApiMin;
    ULONG               Flags;
} IWL_DEVICE_CFG, *PIWL_DEVICE_CFG;

/*
 * Look up a PCI device by ID.  Returns NULL when the part is not one this
 * driver knows how to bring up.
 */
const IWL_DEVICE_CFG *
IwlLookupDevice(_In_ USHORT DeviceId, _In_ USHORT SubsystemId);

/* Human-readable family name, for logging. */
PCSTR
IwlFamilyName(_In_ IWL_DEVICE_FAMILY Family);

/*
 * Compose the firmware file name for a given API revision:
 *   iwlwifi-<FwNamePre>-<Api>.ucode
 * Returns FALSE if the buffer is too small.
 */
BOOLEAN
IwlBuildFirmwareName(
    _In_ const IWL_DEVICE_CFG *Cfg,
    _In_ UCHAR Api,
    _Out_writes_z_(BufferChars) PSTR Buffer,
    _In_ SIZE_T BufferChars);

/*
 * Compose the Platform NVM file name:
 *   iwlwifi-<FwNamePre>.pnvm
 * Same base name as the ucode container but with no API revision - the
 * PNVM is not versioned against the host API.
 */
BOOLEAN
IwlBuildPnvmName(
    _In_ const IWL_DEVICE_CFG *Cfg,
    _Out_writes_z_(BufferChars) PSTR Buffer,
    _In_ SIZE_T BufferChars);

#endif /* _IWLWIFI_DEVICES_H_ */
