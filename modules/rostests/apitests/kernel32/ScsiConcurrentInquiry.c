/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that concurrent SCSI pass-through requests to one unit all complete
 * COPYRIGHT:   ReactOS Developers
 *
 * Several threads send INQUIRY to the same disk or CD-ROM at once. Every request must
 * come back, promptly and successfully. storahci completed some SRBs (INQUIRY among them)
 * from a DPC that took one SRB per run: when two completed before it ran, the DPC was
 * queued once and the second SRB stayed behind until another one came along, if ever.
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddscsi.h>
#include <strsafe.h>

#define THREADS     4
#define ITERATIONS  250
#define MAX_MS      5000
#define WAIT_MS     60000

static BOOL Stuck;

typedef struct
{
    SCSI_PASS_THROUGH Spt;
    ULONG Filler;
    UCHAR Sense[32];
    UCHAR Data[96];
} SPT_INQUIRY;

typedef struct
{
    HANDLE Device;
    volatile LONG Done;
    LONG Failed;
    LONG BadStatus;
    DWORD LastError;
    DWORD MaxMs;
} WORKER;

static
DWORD
WINAPI
InquiryWorker(PVOID Parameter)
{
    WORKER *Worker = Parameter;
    SPT_INQUIRY Inq;
    DWORD Size, Start, Elapsed;
    int i;

    for (i = 0; i < ITERATIONS; i++)
    {
        ZeroMemory(&Inq, sizeof(Inq));
        Inq.Spt.Length = sizeof(SCSI_PASS_THROUGH);
        Inq.Spt.CdbLength = 6;
        Inq.Spt.SenseInfoLength = sizeof(Inq.Sense);
        Inq.Spt.DataIn = SCSI_IOCTL_DATA_IN;
        Inq.Spt.DataTransferLength = sizeof(Inq.Data);
        Inq.Spt.TimeOutValue = 10;
        Inq.Spt.DataBufferOffset = FIELD_OFFSET(SPT_INQUIRY, Data);
        Inq.Spt.SenseInfoOffset = FIELD_OFFSET(SPT_INQUIRY, Sense);
        Inq.Spt.Cdb[0] = 0x12; /* SCSIOP_INQUIRY */
        Inq.Spt.Cdb[4] = sizeof(Inq.Data);

        Start = GetTickCount();
        if (!DeviceIoControl(Worker->Device, IOCTL_SCSI_PASS_THROUGH, &Inq, sizeof(Inq),
                             &Inq, sizeof(Inq), &Size, NULL))
        {
            Worker->Failed++;
            Worker->LastError = GetLastError();
        }
        else if (Inq.Spt.ScsiStatus != 0 || Inq.Spt.DataTransferLength < 36)
        {
            Worker->BadStatus++;
        }
        Elapsed = GetTickCount() - Start;
        if (Elapsed > Worker->MaxMs)
            Worker->MaxMs = Elapsed;
        InterlockedIncrement(&Worker->Done);
    }

    return 0;
}

static
BOOL
TestUnit(PCWSTR Name)
{
    static WORKER Workers[THREADS];
    HANDLE Threads[THREADS];
    SPT_INQUIRY Probe;
    DWORD Size, Wait;
    LONG Done = 0;
    int i, Count;

    ZeroMemory(Workers, sizeof(Workers));
    for (Count = 0; Count < THREADS; Count++)
    {
        Workers[Count].Device = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            NULL, OPEN_EXISTING, 0, NULL);
        if (Workers[Count].Device == INVALID_HANDLE_VALUE)
            break;
    }
    if (Count == 0)
        return FALSE;

    /* Only units that take a pass-through at all */
    ZeroMemory(&Probe, sizeof(Probe));
    Probe.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    Probe.Spt.CdbLength = 6;
    Probe.Spt.DataIn = SCSI_IOCTL_DATA_IN;
    Probe.Spt.DataTransferLength = sizeof(Probe.Data);
    Probe.Spt.TimeOutValue = 10;
    Probe.Spt.DataBufferOffset = FIELD_OFFSET(SPT_INQUIRY, Data);
    Probe.Spt.Cdb[0] = 0x12;
    Probe.Spt.Cdb[4] = sizeof(Probe.Data);
    if (!DeviceIoControl(Workers[0].Device, IOCTL_SCSI_PASS_THROUGH, &Probe, sizeof(Probe),
                         &Probe, sizeof(Probe), &Size, NULL))
    {
        skip("%ls: no pass-through (%lu)\n", Name, GetLastError());
        for (i = 0; i < Count; i++)
            CloseHandle(Workers[i].Device);
        return TRUE;
    }

    for (i = 0; i < Count; i++)
    {
        Threads[i] = CreateThread(NULL, 0, InquiryWorker, &Workers[i], 0, NULL);
        ok(Threads[i] != NULL, "%ls: CreateThread failed with %lu\n", Name, GetLastError());
        if (!Threads[i])
        {
            Count = i;
            break;
        }
    }

    Wait = WaitForMultipleObjects(Count, Threads, TRUE, WAIT_MS);
    for (i = 0; i < Count; i++)
        Done += Workers[i].Done;
    ok(Wait == WAIT_OBJECT_0, "%ls: %ld of %d INQUIRYs came back within %u s\n",
       Name, Done, Count * ITERATIONS, WAIT_MS / 1000);
    if (Wait != WAIT_OBJECT_0)
    {
        /* Requests are stuck in the driver: their threads and handles cannot go, nor Workers */
        Stuck = TRUE;
        return TRUE;
    }

    for (i = 0; i < Count; i++)
    {
        ok(Workers[i].Failed == 0, "%ls: thread %d: %ld INQUIRYs failed, last error %lu\n",
           Name, i, Workers[i].Failed, Workers[i].LastError);
        ok(Workers[i].BadStatus == 0, "%ls: thread %d: %ld INQUIRYs without good status and data\n",
           Name, i, Workers[i].BadStatus);
        ok(Workers[i].MaxMs < MAX_MS, "%ls: thread %d: an INQUIRY took %lu ms\n", Name, i, Workers[i].MaxMs);
        trace("%ls: thread %d: %ld done, slowest %lu ms\n", Name, i, Workers[i].Done, Workers[i].MaxMs);
        CloseHandle(Threads[i]);
        CloseHandle(Workers[i].Device);
    }

    return TRUE;
}

START_TEST(ScsiConcurrentInquiry)
{
    static const PCWSTR Formats[] = { L"\\\\.\\PhysicalDrive%d", L"\\\\.\\CdRom%d" };
    int Format, Number, Found = 0;
    WCHAR Name[32];

    for (Format = 0; Format < _countof(Formats); Format++)
    {
        for (Number = 0; Number < 16 && !Stuck; Number++)
        {
            StringCchPrintfW(Name, _countof(Name), Formats[Format], Number);
            if (TestUnit(Name))
                Found++;
        }
    }

    if (!Found)
        skip("No storage unit found\n");
}
