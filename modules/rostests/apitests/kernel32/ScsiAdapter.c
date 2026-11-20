/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that every storage unit's adapter can be opened as \\.\ScsiN:
 *              and takes IOCTL_SCSI_PASS_THROUGH addressed by PathId/TargetId/Lun
 * COPYRIGHT:   ReactOS Developers
 *
 * Every SCSI port driver (scsiport, storport, ataport) gives its adapter a
 * \DosDevices\ScsiN: link, N being the PortNumber IOCTL_SCSI_GET_ADDRESS reports
 * for its units. A pass-through sent there from user mode goes to the unit it
 * addresses; for a unit a class driver has claimed, storport fails it with
 * STATUS_INVALID_DEVICE_REQUEST (see IOCTL_SCSI_PASS_THROUGH on MSDN).
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddscsi.h>
#include <strsafe.h>

typedef struct
{
    SCSI_PASS_THROUGH Spt;
    ULONG Filler;
    UCHAR Sense[32];
    UCHAR Data[96];
} SPT_INQUIRY;

static
BOOL
SendInquiry(HANDLE Handle, const SCSI_ADDRESS *Address, SPT_INQUIRY *Inq, DWORD *Error)
{
    DWORD Size;
    BOOL Ret;

    ZeroMemory(Inq, sizeof(*Inq));
    Inq->Spt.Length = sizeof(SCSI_PASS_THROUGH);
    if (Address)
    {
        Inq->Spt.PathId = Address->PathId;
        Inq->Spt.TargetId = Address->TargetId;
        Inq->Spt.Lun = Address->Lun;
    }
    Inq->Spt.CdbLength = 6;
    Inq->Spt.SenseInfoLength = sizeof(Inq->Sense);
    Inq->Spt.DataIn = SCSI_IOCTL_DATA_IN;
    Inq->Spt.DataTransferLength = sizeof(Inq->Data);
    Inq->Spt.TimeOutValue = 10;
    Inq->Spt.DataBufferOffset = FIELD_OFFSET(SPT_INQUIRY, Data);
    Inq->Spt.SenseInfoOffset = FIELD_OFFSET(SPT_INQUIRY, Sense);
    Inq->Spt.Cdb[0] = 0x12; /* SCSIOP_INQUIRY */
    Inq->Spt.Cdb[4] = sizeof(Inq->Data);
    SetLastError(0xdeadbeef);
    Ret = DeviceIoControl(Handle, IOCTL_SCSI_PASS_THROUGH, Inq, sizeof(*Inq), Inq, sizeof(*Inq), &Size, NULL);
    *Error = GetLastError();
    return Ret;
}

static
void
TestUnit(HANDLE Device, PCWSTR Name)
{
    SCSI_ADDRESS Address, Absent;
    SPT_INQUIRY Inq;
    WCHAR AdapterName[32];
    HANDLE Adapter;
    DWORD Size, Error;
    UCHAR DeviceType;
    BOOL Ret;

    Ret = DeviceIoControl(Device, IOCTL_SCSI_GET_ADDRESS, NULL, 0, &Address, sizeof(Address), &Size, NULL);
    if (!Ret)
    {
        skip("%ls: no SCSI address (%lu)\n", Name, GetLastError());
        return;
    }
    trace("%ls: port %u path %u target %u lun %u\n", Name,
          Address.PortNumber, Address.PathId, Address.TargetId, Address.Lun);

    /* Through the unit, as its class driver lets it */
    Ret = SendInquiry(Device, NULL, &Inq, &Error);
    ok(Ret, "%ls: INQUIRY failed with %lu\n", Name, Error);
    if (!Ret)
        return;
    ok(Inq.Spt.ScsiStatus == 0, "%ls: INQUIRY ScsiStatus 0x%x\n", Name, Inq.Spt.ScsiStatus);
    ok(Inq.Spt.DataTransferLength >= 36, "%ls: INQUIRY returned %lu bytes\n", Name, Inq.Spt.DataTransferLength);
    DeviceType = Inq.Data[0] & 0x1F;

    /* The adapter of the unit */
    StringCchPrintfW(AdapterName, _countof(AdapterName), L"\\\\.\\Scsi%u:", Address.PortNumber);
    Adapter = CreateFileW(AdapterName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    ok(Adapter != INVALID_HANDLE_VALUE, "%ls: cannot open %ls (%lu)\n", Name, AdapterName, GetLastError());
    if (Adapter == INVALID_HANDLE_VALUE)
        return;

    /* A claimed unit refuses it (storport); a port that takes it must reach this very unit */
    Ret = SendInquiry(Adapter, &Address, &Inq, &Error);
    if (Ret)
    {
        ok(Inq.Spt.ScsiStatus == 0, "%ls: INQUIRY via %ls ScsiStatus 0x%x\n", Name, AdapterName, Inq.Spt.ScsiStatus);
        ok(Inq.Spt.DataTransferLength >= 36, "%ls: INQUIRY via %ls returned %lu bytes\n",
           Name, AdapterName, Inq.Spt.DataTransferLength);
        ok((Inq.Data[0] & 0x1F) == DeviceType, "%ls: INQUIRY via %ls device type %u, expected %u\n",
           Name, AdapterName, Inq.Data[0] & 0x1F, DeviceType);
    }
    else
    {
        ok(Error == ERROR_INVALID_FUNCTION, "%ls: INQUIRY via %ls failed with %lu\n", Name, AdapterName, Error);
    }

    /* No unit answers at 255:255:255 */
    Absent = Address;
    Absent.PathId = 0xFF;
    Absent.TargetId = 0xFF;
    Absent.Lun = 0xFF;
    Ret = SendInquiry(Adapter, &Absent, &Inq, &Error);
    ok(!Ret, "%ls: INQUIRY via %ls to %u:%u:%u succeeded\n", Name, AdapterName,
       Absent.PathId, Absent.TargetId, Absent.Lun);

    /* Too short to hold the address */
    Ret = DeviceIoControl(Adapter, IOCTL_SCSI_PASS_THROUGH, &Inq, 2, &Inq, sizeof(Inq), &Size, NULL);
    ok(!Ret, "%ls: 2-byte pass-through via %ls succeeded\n", Name, AdapterName);

    CloseHandle(Adapter);
}

START_TEST(ScsiAdapter)
{
    static const PCWSTR Formats[] = { L"\\\\.\\PhysicalDrive%d", L"\\\\.\\CdRom%d" };
    int Format, Number, Found = 0;
    WCHAR Name[32];
    HANDLE Device;

    for (Format = 0; Format < _countof(Formats); Format++)
    {
        for (Number = 0; Number < 16; Number++)
        {
            StringCchPrintfW(Name, _countof(Name), Formats[Format], Number);
            Device = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
            if (Device == INVALID_HANDLE_VALUE)
                continue;
            Found++;
            TestUnit(Device, Name);
            CloseHandle(Device);
        }
    }

    if (!Found)
        skip("No storage unit found\n");
}
