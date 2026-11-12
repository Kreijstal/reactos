/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that every CD-ROM reports its media state consistently
 *              and promptly (TEST UNIT READY, CHECK_VERIFY, GET_GEOMETRY)
 * COPYRIGHT:   ReactOS Developers
 *
 * A port driver that answers TEST UNIT READY itself instead of passing it to
 * an ATAPI device makes an empty drive look ready, and one that drops the
 * CHECK CONDITION sense data makes the class driver retry for seconds before
 * it reports "no media" (storahci did both; opening an empty DVD on AHCI took
 * ~20 s of file system mount attempts).
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddscsi.h>
#include <strsafe.h>

/* Each request should be answered by the device in well under a second;
 * retries without sense data take 2 s and more. */
#define MAX_MS 1500

typedef struct
{
    SCSI_PASS_THROUGH Spt;
    ULONG Filler;
    UCHAR Sense[32];
} SPT_TUR;

static
void
TestCdRom(HANDLE Device, int Number)
{
    SPT_TUR Tur;
    DISK_GEOMETRY Geometry;
    DWORD Size, Start, Elapsed, Error;
    BOOL Ret, HaveTur, TurReady = FALSE, Verified, HaveGeometry;

    /* TEST UNIT READY straight to the device */
    ZeroMemory(&Tur, sizeof(Tur));
    Tur.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    Tur.Spt.CdbLength = 6;
    Tur.Spt.SenseInfoLength = sizeof(Tur.Sense);
    Tur.Spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    Tur.Spt.TimeOutValue = 10;
    Tur.Spt.SenseInfoOffset = FIELD_OFFSET(SPT_TUR, Sense);
    Tur.Spt.Cdb[0] = 0x00; /* SCSIOP_TEST_UNIT_READY */
    Start = GetTickCount();
    Ret = DeviceIoControl(Device, IOCTL_SCSI_PASS_THROUGH, &Tur, sizeof(Tur), &Tur, sizeof(Tur), &Size, NULL);
    Elapsed = GetTickCount() - Start;
    HaveTur = Ret;
    /* Every Windows port driver (scsiport, storport, ataport) supports it */
    ok(HaveTur, "CdRom%d: IOCTL_SCSI_PASS_THROUGH failed with %lu\n", Number, GetLastError());
    if (HaveTur)
    {
        ok(Elapsed < MAX_MS, "CdRom%d: TEST UNIT READY took %lu ms\n", Number, Elapsed);
        TurReady = (Tur.Spt.ScsiStatus == 0);
    }
    if (HaveTur && !TurReady)
    {
        ok(Tur.Spt.ScsiStatus == 2, "CdRom%d: TEST UNIT READY ScsiStatus 0x%x\n", Number, Tur.Spt.ScsiStatus);
        /* CHECK CONDITION must come with sense data (response code 70h/71h) */
        ok((Tur.Sense[0] & 0x7E) == 0x70, "CdRom%d: no sense data with CHECK CONDITION (%02x %02x)\n",
           Number, Tur.Sense[0], Tur.Sense[2]);
        ok((Tur.Sense[2] & 0x0F) != 0, "CdRom%d: sense key 0 with CHECK CONDITION\n", Number);
        trace("CdRom%d: TUR sense %x/%02x/%02x\n", Number, Tur.Sense[2] & 0x0F, Tur.Sense[12], Tur.Sense[13]);
    }

    /* CHECK_VERIFY: success means media present */
    Start = GetTickCount();
    Verified = DeviceIoControl(Device, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &Size, NULL);
    Error = GetLastError();
    Elapsed = GetTickCount() - Start;
    ok(Elapsed < MAX_MS, "CdRom%d: CHECK_VERIFY took %lu ms\n", Number, Elapsed);
    if (!Verified)
    {
        /* A media change notification may come first, then the real state */
        if (Error == ERROR_MEDIA_CHANGED)
        {
            Verified = DeviceIoControl(Device, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &Size, NULL);
            Error = GetLastError();
        }
        if (!Verified)
            ok(Error == ERROR_NOT_READY, "CdRom%d: CHECK_VERIFY error %lu\n", Number, Error);
    }

    /* GET_DRIVE_GEOMETRY needs the medium */
    Start = GetTickCount();
    HaveGeometry = DeviceIoControl(Device, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Geometry, sizeof(Geometry), &Size, NULL);
    Elapsed = GetTickCount() - Start;
    ok(Elapsed < MAX_MS, "CdRom%d: GET_DRIVE_GEOMETRY took %lu ms\n", Number, Elapsed);

    trace("CdRom%d: TUR %s, CHECK_VERIFY %s, geometry %s\n", Number,
          !HaveTur ? "n/a" : TurReady ? "ready" : "not ready", Verified ? "ok" : "failed", HaveGeometry ? "ok" : "failed");

    /* All three must agree on whether there is a medium */
    if (HaveTur)
        ok(TurReady == HaveGeometry, "CdRom%d: TEST UNIT READY says %s but GET_DRIVE_GEOMETRY %s\n",
           Number, TurReady ? "ready" : "not ready", HaveGeometry ? "succeeded" : "failed");
    ok(Verified == HaveGeometry, "CdRom%d: CHECK_VERIFY %s but GET_DRIVE_GEOMETRY %s\n",
       Number, Verified ? "succeeded" : "failed", HaveGeometry ? "succeeded" : "failed");
}

START_TEST(CdRomMedia)
{
    int Number, Found = 0;
    WCHAR Name[32];
    HANDLE Device;
    DWORD Start;

    for (Number = 0; Number < 8; Number++)
    {
        StringCchPrintfW(Name, _countof(Name), L"\\\\.\\CdRom%d", Number);
        Start = GetTickCount();
        Device = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (Device == INVALID_HANDLE_VALUE)
            continue;
        Found++;
        /* Opening the device must not sit in file system mount attempts */
        ok(GetTickCount() - Start < 2 * MAX_MS, "CdRom%d: open took %lu ms\n", Number, GetTickCount() - Start);
        TestCdRom(Device, Number);
        CloseHandle(Device);
    }

    if (!Found)
        skip("No CD-ROM drive found\n");
}
