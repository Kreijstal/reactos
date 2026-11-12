/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that concurrent reads from a USB mass storage disk all
 *              complete (usbstor must never lose a queued request)
 * COPYRIGHT:   ReactOS Developers
 *
 * usbstor keeps its own request list next to the I/O manager's device queue.
 * A request that arrived while the previous one was finishing -- after its
 * completion routine released ActiveSrb, before it released the device
 * queue -- claimed ActiveSrb, was inserted into the still-busy device queue
 * and never reached StartIo: every later request waited behind it for good.
 * That is how the ASUS X550DP lost its USB boot disk.  The window only opens
 * for requests that arrive independently of the completions, so the readers
 * here wait a random time between their reads.
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddstor.h>

#define READERS          8
#define READS_PER_READER 150
#define WAIT_MS          (180 * 1000)

typedef struct
{
    WCHAR Path[32];
    ULONGLONG Sectors;
    ULONG SectorSize;
    ULONG Seed;
    LONG Done;
    LONG Failed;
    DWORD FirstError;
} READER;

static
ULONG
NextRandom(PULONG Seed)
{
    *Seed = *Seed * 1103515245 + 12345;
    return (*Seed >> 8) & 0xFFFFFF;
}

static
void
SpinMs(ULONG Ms)
{
    LARGE_INTEGER Frequency, Start, Now;

    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Start);
    do
    {
        YieldProcessor();
        QueryPerformanceCounter(&Now);
    } while ((Now.QuadPart - Start.QuadPart) * 1000 < (LONGLONG)Ms * Frequency.QuadPart);
}

static
DWORD
WINAPI
ReaderThread(PVOID Param)
{
    READER *Reader = Param;
    HANDLE Device;
    PUCHAR Buffer;
    ULONG i, Count, Sector;
    LARGE_INTEGER Offset;
    DWORD Read;

    Device = CreateFileW(Reader->Path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        Reader->FirstError = GetLastError();
        Reader->Failed = READS_PER_READER;
        return 0;
    }

    Buffer = VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT, PAGE_READWRITE);
    if (!Buffer)
    {
        Reader->FirstError = GetLastError();
        Reader->Failed = READS_PER_READER;
        CloseHandle(Device);
        return 0;
    }

    for (i = 0; i < READS_PER_READER; i++)
    {
        Count = 1 + NextRandom(&Reader->Seed) % (64 * 1024 / Reader->SectorSize);
        Sector = NextRandom(&Reader->Seed) % (ULONG)(Reader->Sectors - Count);
        Offset.QuadPart = (ULONGLONG)Sector * Reader->SectorSize;

        if (!SetFilePointerEx(Device, Offset, NULL, FILE_BEGIN) ||
            !ReadFile(Device, Buffer, Count * Reader->SectorSize, &Read, NULL) ||
            Read != Count * Reader->SectorSize)
        {
            if (!Reader->Failed++)
                Reader->FirstError = GetLastError();
        }
        InterlockedIncrement(&Reader->Done);

        /* Arrive out of step with the completions.  The pause must be able to
         * outlast a whole request (tens of ms on an emulated device), or every
         * arrival lands a fixed time after its own completion and never in the
         * window of the next one.  Spin rather than Sleep: timer expiry runs
         * in a DPC, and the processor completing the USB transfers is busy at
         * DISPATCH_LEVEL exactly then, so sleepers wake only after it. */
        SpinMs(NextRandom(&Reader->Seed) % 160);
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
    CloseHandle(Device);
    return 0;
}

static
void
TestUsbDisk(PCWSTR Path, const DISK_GEOMETRY_EX *Geometry)
{
    static READER Readers[READERS];
    HANDLE Threads[READERS];
    ULONG i, Started = 0, Done = 0, Failed = 0;
    DWORD Wait, Start;

    Start = GetTickCount();
    for (i = 0; i < READERS; i++)
    {
        ZeroMemory(&Readers[i], sizeof(Readers[i]));
        StringCchCopyW(Readers[i].Path, _countof(Readers[i].Path), Path);
        Readers[i].SectorSize = Geometry->Geometry.BytesPerSector;
        Readers[i].Sectors = Geometry->DiskSize.QuadPart / Readers[i].SectorSize;
        if (Readers[i].Sectors > MAXULONG)
            Readers[i].Sectors = MAXULONG;
        Readers[i].Seed = 0x38 + i * 7919 + GetTickCount();

        Threads[i] = CreateThread(NULL, 0, ReaderThread, &Readers[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (!Threads[i])
            break;
        Started++;
    }

    Wait = WaitForMultipleObjects(Started, Threads, TRUE, WAIT_MS);
    for (i = 0; i < Started; i++)
    {
        Done += Readers[i].Done;
        Failed += Readers[i].Failed;
        ok(Readers[i].Failed == 0, "%S reader %lu: %ld of %ld reads failed, first error %lu\n",
           Path, i, Readers[i].Failed, Readers[i].Done, Readers[i].FirstError);
    }

    ok(Wait == WAIT_OBJECT_0,
       "%S: readers still blocked after %lu ms (wait %lu), %lu of %lu reads done -- a request was lost\n",
       Path, GetTickCount() - Start, Wait, Done, Started * READS_PER_READER);
    trace("%S: %lu reads, %lu failed, %lu ms\n", Path, Done, Failed, GetTickCount() - Start);

    /* A blocked reader cannot be cleaned up; leave its handle alone */
    if (Wait == WAIT_OBJECT_0)
    {
        for (i = 0; i < Started; i++)
            CloseHandle(Threads[i]);
    }
}

START_TEST(UsbDiskQueue)
{
    WCHAR Path[32];
    HANDLE Device;
    STORAGE_PROPERTY_QUERY Query;
    UCHAR DescriptorBuffer[512];
    PSTORAGE_DEVICE_DESCRIPTOR Descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)DescriptorBuffer;
    DISK_GEOMETRY_EX Geometry;
    DWORD Size;
    ULONG Number, Tested = 0;

    for (Number = 0; Number < 16; Number++)
    {
        StringCchPrintfW(Path, _countof(Path), L"\\\\.\\PhysicalDrive%lu", Number);
        Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (Device == INVALID_HANDLE_VALUE)
            continue;

        ZeroMemory(&Query, sizeof(Query));
        Query.PropertyId = StorageDeviceProperty;
        Query.QueryType = PropertyStandardQuery;
        if (!DeviceIoControl(Device, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query),
                             DescriptorBuffer, sizeof(DescriptorBuffer), &Size, NULL) ||
            Descriptor->BusType != BusTypeUsb ||
            !DeviceIoControl(Device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                             &Geometry, sizeof(Geometry), &Size, NULL) ||
            Geometry.Geometry.BytesPerSector == 0 ||
            Geometry.DiskSize.QuadPart < 1024 * 1024)
        {
            CloseHandle(Device);
            continue;
        }
        CloseHandle(Device);

        TestUsbDisk(Path, &Geometry);
        Tested++;
    }

    if (!Tested)
        skip("No USB mass storage disk present\n");
}
