/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     PCI-ID -> device configuration table and firmware-name
 *              composition.
 */

#include "devices.h"
#include <ntstrsafe.h>

#define NDEBUG
#include <debug.h>

/*
 * The table below is transcribed from the upstream iwlwifi PCI ID table
 * plus the per-family cfg structures that supply the firmware base name
 * and API range.
 *
 * VERIFICATION STATUS.  Exactly one row has been exercised against real
 * silicon so far - 8086:51F1, the Raptor Lake CNVi part in the development
 * host.  Every other row is transcribed, not tested.  That distinction is
 * recorded per row so nobody later mistakes "it is in the table" for "it
 * has been brought up".  Rows are cheap; claiming support is not.
 *
 * ANY = wildcard subsystem ID.  Rows are searched in order, so a row that
 * narrows on a subsystem ID must appear before the wildcard row for the
 * same device ID.
 */

#define ANY 0xFFFF

static const IWL_DEVICE_CFG IwlDeviceTable[] =
{
/*  DevId   SubId  Name                          Family                    FwNamePre                   ApiMax ApiMin Flags */

/* --- Legacy MVM-less parts (5000/6000/1000/2000).  These predate the ---
 * --- modern transport by a wide margin; they are listed so the driver ---
 * --- can NAME them in the log instead of silently ignoring them.     --- */
{ 0x4232, ANY, "WiFi Link 5100",                  IWL_DEVICE_FAMILY_5000,  "5000",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x4235, ANY, "Ultimate N WiFi Link 5300",       IWL_DEVICE_FAMILY_5000,  "5000",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x4236, ANY, "Ultimate N WiFi Link 5300",       IWL_DEVICE_FAMILY_5000,  "5000",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x4237, ANY, "WiFi Link 5100",                  IWL_DEVICE_FAMILY_5000,  "5000",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x423A, ANY, "WiMAX/WiFi Link 5150",            IWL_DEVICE_FAMILY_5150,  "5150",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x423B, ANY, "WiMAX/WiFi Link 5150",            IWL_DEVICE_FAMILY_5150,  "5150",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x0083, ANY, "Centrino Wireless-N 1000",        IWL_DEVICE_FAMILY_1000,  "1000",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x0084, ANY, "Centrino Wireless-N 1000",        IWL_DEVICE_FAMILY_1000,  "1000",                      5,  1, IWL_CFG_PLL_CFG },
{ 0x08AE, ANY, "Centrino Wireless-N 100",         IWL_DEVICE_FAMILY_100,   "100",                       5,  1, 0 },
{ 0x08AF, ANY, "Centrino Wireless-N 100",         IWL_DEVICE_FAMILY_100,   "100",                       5,  1, 0 },
{ 0x0890, ANY, "Centrino Wireless-N 2200",        IWL_DEVICE_FAMILY_2000,  "2000",                      6,  1, 0 },
{ 0x0891, ANY, "Centrino Wireless-N 2200",        IWL_DEVICE_FAMILY_2000,  "2000",                      6,  1, 0 },
{ 0x0887, ANY, "Centrino Wireless-N 2230",        IWL_DEVICE_FAMILY_2030,  "2030",                      6,  1, 0 },
{ 0x0888, ANY, "Centrino Wireless-N 2230",        IWL_DEVICE_FAMILY_2030,  "2030",                      6,  1, 0 },
{ 0x0894, ANY, "Centrino Wireless-N 105",         IWL_DEVICE_FAMILY_105,   "105",                       6,  1, 0 },
{ 0x0895, ANY, "Centrino Wireless-N 105",         IWL_DEVICE_FAMILY_105,   "105",                       6,  1, 0 },
{ 0x0892, ANY, "Centrino Wireless-N 135",         IWL_DEVICE_FAMILY_135,   "135",                       6,  1, 0 },
{ 0x0893, ANY, "Centrino Wireless-N 135",         IWL_DEVICE_FAMILY_135,   "135",                       6,  1, 0 },
{ 0x422B, ANY, "Centrino Ultimate-N 6300",        IWL_DEVICE_FAMILY_6000,  "6000",                      6,  4, 0 },
{ 0x422C, ANY, "Centrino Advanced-N 6200",        IWL_DEVICE_FAMILY_6000i, "6000",                      6,  4, 0 },
{ 0x4238, ANY, "Centrino Ultimate-N 6300",        IWL_DEVICE_FAMILY_6000,  "6000",                      6,  4, 0 },
{ 0x4239, ANY, "Centrino Advanced-N 6200",        IWL_DEVICE_FAMILY_6000i, "6000",                      6,  4, 0 },
{ 0x0082, ANY, "Centrino Advanced-N 6205",        IWL_DEVICE_FAMILY_6005,  "6000g2a",                   6,  4, 0 },
{ 0x0085, ANY, "Centrino Advanced-N 6205",        IWL_DEVICE_FAMILY_6005,  "6000g2a",                   6,  4, 0 },
{ 0x008A, ANY, "Centrino Wireless-N 6230",        IWL_DEVICE_FAMILY_6030,  "6000g2b",                   6,  4, 0 },
{ 0x008B, ANY, "Centrino Wireless-N 6230",        IWL_DEVICE_FAMILY_6030,  "6000g2b",                   6,  4, 0 },
{ 0x0090, ANY, "Centrino Advanced-N 6235",        IWL_DEVICE_FAMILY_6030,  "6000g2b",                   6,  4, 0 },
{ 0x0091, ANY, "Centrino Advanced-N 6235",        IWL_DEVICE_FAMILY_6030,  "6000g2b",                   6,  4, 0 },
{ 0x0087, ANY, "Centrino Advanced-N + WiMAX 6250",IWL_DEVICE_FAMILY_6050,  "6050",                      5,  4, 0 },
{ 0x0089, ANY, "Centrino Advanced-N + WiMAX 6250",IWL_DEVICE_FAMILY_6050,  "6050",                      5,  4, 0 },
{ 0x0885, ANY, "Centrino Wireless-N + WiMAX 6150",IWL_DEVICE_FAMILY_6150,  "6050",                      5,  4, 0 },
{ 0x0886, ANY, "Centrino Wireless-N + WiMAX 6150",IWL_DEVICE_FAMILY_6150,  "6050",                      5,  4, 0 },

/* --- Family 7000: first generation with the MVM firmware API. --- */
{ 0x08B1, ANY, "Wireless-AC 7260",                IWL_DEVICE_FAMILY_7000,  "7260",                     17, 17, 0 },
{ 0x08B2, ANY, "Wireless-AC 7260",                IWL_DEVICE_FAMILY_7000,  "7260",                     17, 17, 0 },
{ 0x08B3, ANY, "Wireless-N 3160",                 IWL_DEVICE_FAMILY_7000,  "3160",                     17, 17, 0 },
{ 0x08B4, ANY, "Wireless-N 3160",                 IWL_DEVICE_FAMILY_7000,  "3160",                     17, 17, 0 },
{ 0x095A, ANY, "Wireless-AC 7265",                IWL_DEVICE_FAMILY_7000,  "7265D",                    29, 17, 0 },
{ 0x095B, ANY, "Wireless-AC 7265",                IWL_DEVICE_FAMILY_7000,  "7265D",                    29, 17, 0 },
{ 0x3165, ANY, "Wireless-AC 3165",                IWL_DEVICE_FAMILY_7000,  "7265D",                    29, 17, 0 },
{ 0x3166, ANY, "Wireless-AC 3165",                IWL_DEVICE_FAMILY_7000,  "7265D",                    29, 17, 0 },
{ 0x3167, ANY, "Wireless-AC 3168",                IWL_DEVICE_FAMILY_7000,  "3168",                     29, 22, 0 },
{ 0x3168, ANY, "Wireless-AC 3168",                IWL_DEVICE_FAMILY_7000,  "3168",                     29, 22, 0 },

/* --- Family 8000. --- */
{ 0x24F3, ANY, "Wireless-AC 8260",                IWL_DEVICE_FAMILY_8000,  "8000C",                    36, 22, 0 },
{ 0x24F4, ANY, "Wireless-AC 8260",                IWL_DEVICE_FAMILY_8000,  "8000C",                    36, 22, 0 },
{ 0x24F5, ANY, "Wireless-AC 4165",                IWL_DEVICE_FAMILY_8000,  "8000C",                    36, 22, 0 },
{ 0x24F6, ANY, "Wireless-AC 4165",                IWL_DEVICE_FAMILY_8000,  "8000C",                    36, 22, 0 },
{ 0x24FD, ANY, "Wireless-AC 8265",                IWL_DEVICE_FAMILY_8000,  "8265",                     36, 22, 0 },

/* --- Family 9000.  Firmware name is MAC+RF composed upstream; the pre
 *     recorded here is the full pair for the common configuration.  Parts
 *     whose CRF differs will need the composed path (see
 *     IWL_CFG_RF_COMPOSED_FW_NAME) once RF-ID decoding lands. --- */
{ 0x2526, ANY, "Wireless-AC 9260",                IWL_DEVICE_FAMILY_9000,  "9260-th-b0-jf-b0",         46, 30, 0 },
{ 0x271B, ANY, "Wireless-AC 9160",                IWL_DEVICE_FAMILY_9000,  "9260-th-b0-jf-b0",         46, 30, 0 },
{ 0x271C, ANY, "Wireless-AC 9160",                IWL_DEVICE_FAMILY_9000,  "9260-th-b0-jf-b0",         46, 30, 0 },
{ 0x30DC, ANY, "Wireless-AC 9560",                IWL_DEVICE_FAMILY_9000,  "9000-pu-b0-jf-b0",         46, 30, IWL_CFG_INTEGRATED },
{ 0x31DC, ANY, "Wireless-AC 9560",                IWL_DEVICE_FAMILY_9000,  "9000-pu-b0-jf-b0",         46, 30, IWL_CFG_INTEGRATED },
{ 0x9DF0, ANY, "Wireless-AC 9560",                IWL_DEVICE_FAMILY_9000,  "9000-pu-b0-jf-b0",         46, 30, IWL_CFG_INTEGRATED },
{ 0xA370, ANY, "Wireless-AC 9560",                IWL_DEVICE_FAMILY_9000,  "9000-pu-b0-jf-b0",         46, 30, IWL_CFG_INTEGRATED },
{ 0x2720, ANY, "Wireless-AC 9560",                IWL_DEVICE_FAMILY_9000,  "9000-pu-b0-jf-b0",         46, 30, IWL_CFG_INTEGRATED },

/* --- Family 22000: AX200 discrete and AX201 integrated (Qu/QuZ). --- */
{ 0x2723, ANY, "Wi-Fi 6 AX200",                   IWL_DEVICE_FAMILY_22000, "cc-a0",                    77, 39, 0 },
{ 0x02F0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "Qu-b0-hr-b0",              77, 39, IWL_CFG_INTEGRATED },
{ 0x06F0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "QuZ-a0-hr-b0",             77, 39, IWL_CFG_INTEGRATED },
{ 0x34F0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "QuZ-a0-hr-b0",             77, 39, IWL_CFG_INTEGRATED },
{ 0x3DF0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "Qu-b0-hr-b0",              77, 39, IWL_CFG_INTEGRATED },
{ 0x43F0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "Qu-b0-hr-b0",              77, 39, IWL_CFG_INTEGRATED },
{ 0x4DF0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "Qu-b0-hr-b0",              77, 39, IWL_CFG_INTEGRATED },
{ 0xA0F0, ANY, "Wi-Fi 6 AX201",                   IWL_DEVICE_FAMILY_22000, "Qu-b0-hr-b0",              77, 39, IWL_CFG_INTEGRATED },

/* --- Family AX210. --- */
{ 0x2725, ANY, "Wi-Fi 6E AX210",                  IWL_DEVICE_FAMILY_AX210, "ty-a0-gf-a0",              89, 59, IWL_CFG_NEEDS_PNVM },
{ 0x2726, ANY, "Wi-Fi 6E AX211",                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_NEEDS_PNVM },
{ 0x7A70, ANY, "Wi-Fi 6E AX211",                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
{ 0x7AF0, ANY, "Wi-Fi 6E AX211",                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
{ 0x7E40, ANY, "Wi-Fi 6E AX211",                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
{ 0x54F0, ANY, "Wi-Fi 6E AX211",                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
{ 0x51F0, ANY, "Wi-Fi 6E AX211",                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
/* The development host's own radio - the one row with hardware behind it. */
{ 0x51F1, ANY, "Wi-Fi 6E AX211 (Raptor Lake CNVi)",
                                                  IWL_DEVICE_FAMILY_AX210, "so-a0-gf-a0",              89, 59, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },

/* --- Family BZ (Wi-Fi 7). --- */
{ 0x272B, ANY, "Wi-Fi 7 BE200",                   IWL_DEVICE_FAMILY_BZ,    "gl-c0-fm-c0",             101, 83, IWL_CFG_NEEDS_PNVM },
{ 0xA840, ANY, "Wi-Fi 7 BE201",                   IWL_DEVICE_FAMILY_BZ,    "bz-b0-fm-c0",             101, 92, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
{ 0x7740, ANY, "Wi-Fi 7 BE201",                   IWL_DEVICE_FAMILY_BZ,    "bz-b0-fm-c0",             101, 92, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
{ 0x4D40, ANY, "Wi-Fi 7 BE201",                   IWL_DEVICE_FAMILY_BZ,    "bz-b0-fm-c0",             101, 92, IWL_CFG_INTEGRATED | IWL_CFG_NEEDS_PNVM },
};

const IWL_DEVICE_CFG *
IwlLookupDevice(_In_ USHORT DeviceId, _In_ USHORT SubsystemId)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(IwlDeviceTable); i++)
    {
        if (IwlDeviceTable[i].DeviceId != DeviceId)
            continue;
        if (IwlDeviceTable[i].SubsystemId != ANY &&
            IwlDeviceTable[i].SubsystemId != SubsystemId)
            continue;
        return &IwlDeviceTable[i];
    }

    return NULL;
}

PCSTR
IwlFamilyName(_In_ IWL_DEVICE_FAMILY Family)
{
    switch (Family)
    {
        case IWL_DEVICE_FAMILY_1000:  return "1000";
        case IWL_DEVICE_FAMILY_100:   return "100";
        case IWL_DEVICE_FAMILY_2000:  return "2000";
        case IWL_DEVICE_FAMILY_2030:  return "2030";
        case IWL_DEVICE_FAMILY_105:   return "105";
        case IWL_DEVICE_FAMILY_135:   return "135";
        case IWL_DEVICE_FAMILY_5000:  return "5000";
        case IWL_DEVICE_FAMILY_5150:  return "5150";
        case IWL_DEVICE_FAMILY_6000:  return "6000";
        case IWL_DEVICE_FAMILY_6000i: return "6000i";
        case IWL_DEVICE_FAMILY_6005:  return "6005";
        case IWL_DEVICE_FAMILY_6030:  return "6030";
        case IWL_DEVICE_FAMILY_6050:  return "6050";
        case IWL_DEVICE_FAMILY_6150:  return "6150";
        case IWL_DEVICE_FAMILY_7000:  return "7000";
        case IWL_DEVICE_FAMILY_8000:  return "8000";
        case IWL_DEVICE_FAMILY_9000:  return "9000";
        case IWL_DEVICE_FAMILY_22000: return "22000";
        case IWL_DEVICE_FAMILY_AX210: return "AX210";
        case IWL_DEVICE_FAMILY_BZ:    return "BZ";
        case IWL_DEVICE_FAMILY_SC:    return "SC";
        default:                      return "undefined";
    }
}

BOOLEAN
IwlBuildPnvmName(
    _In_ const IWL_DEVICE_CFG *Cfg,
    _Out_writes_z_(BufferChars) PSTR Buffer,
    _In_ SIZE_T BufferChars)
{
    NTSTATUS Status;

    Status = RtlStringCbPrintfA(Buffer,
                                BufferChars,
                                "iwlwifi-%s.pnvm",
                                Cfg->FwNamePre);
    return NT_SUCCESS(Status);
}

BOOLEAN
IwlBuildFirmwareName(
    _In_ const IWL_DEVICE_CFG *Cfg,
    _In_ UCHAR Api,
    _Out_writes_z_(BufferChars) PSTR Buffer,
    _In_ SIZE_T BufferChars)
{
    NTSTATUS Status;

    Status = RtlStringCbPrintfA(Buffer,
                                BufferChars,
                                "iwlwifi-%s-%u.ucode",
                                Cfg->FwNamePre,
                                Api);
    return NT_SUCCESS(Status);
}
