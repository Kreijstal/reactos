/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that reads on several USB storage devices at once all
 *              complete (usbport must keep each endpoint's transfer list sound)
 * COPYRIGHT:   ReactOS Developers
 *
 * usbport protects an endpoint's TransferList with that endpoint's
 * EndpointSpinLock, and a miniport may only report a finished transfer from
 * its PollEndpoint for that endpoint, where USBPORT holds the lock.  usbxhci
 * reported completions from every pass over its event ring instead: from the
 * interrupt DPC and from the poll of whichever endpoint USBPORT happened to be
 * polling.  The unlink then raced the inserts USBPORT makes for the next
 * transfer, the list stopped leading back to its head, and a CPU spun at
 * DISPATCH_LEVEL in USBPORT_FlushPendingTransfers for ever -- the whole system
 * froze.  It takes transfers on two or more endpoints completing at the same
 * time, so this keeps every USB disk and CD-ROM busy at once.
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddstor.h>
#include <ntddcdrm.h>

#define MAX_DEVICES        8
#define READERS_PER_DEVICE 4
#define READS_PER_READER   60
#define WAIT_MS            (240 * 1000)

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
DWORD
WINAPI
ReaderThread(PVOID Param)
{
    READER *Reader = Param;
    HANDLE Device;
    PUCHAR Buffer;
    ULONG i, Count, MaxCount, Sector;
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

    MaxCount = 64 * 1024 / Reader->SectorSize;

    for (i = 0; i < READS_PER_READER; i++)
    {
        /* Mix small and full-sized transfers: a 64 KB read is split by
         * USBPORT and keeps several transfers of one endpoint in flight. */
        Count = 1 + NextRandom(&Reader->Seed) % MaxCount;
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
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
    CloseHandle(Device);
    return 0;
}

static
BOOL
IsUsbDevice(HANDLE Device)
{
    STORAGE_PROPERTY_QUERY Query;
    UCHAR DescriptorBuffer[512];
    PSTORAGE_DEVICE_DESCRIPTOR Descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)DescriptorBuffer;
    DWORD Size;

    ZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageDeviceProperty;
    Query.QueryType = PropertyStandardQuery;
    return DeviceIoControl(Device, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query),
                           DescriptorBuffer, sizeof(DescriptorBuffer), &Size, NULL) &&
           Descriptor->BusType == BusTypeUsb;
}

static
BOOL
AddDevice(PCWSTR Path, DWORD GeometryIoctl, READER *Readers, ULONG *DeviceCount)
{
    HANDLE Device;
    DISK_GEOMETRY_EX Geometry;
    DWORD Size;
    ULONG i;
    READER *Reader;

    if (*DeviceCount >= MAX_DEVICES)
        return FALSE;

    Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!IsUsbDevice(Device) ||
        !DeviceIoControl(Device, GeometryIoctl, NULL, 0,
                         &Geometry, sizeof(Geometry), &Size, NULL) ||
        Geometry.Geometry.BytesPerSector == 0 ||
        Geometry.Geometry.BytesPerSector > 64 * 1024 ||
        Geometry.DiskSize.QuadPart < 1024 * 1024)
    {
        CloseHandle(Device);
        return FALSE;
    }
    CloseHandle(Device);

    for (i = 0; i < READERS_PER_DEVICE; i++)
    {
        Reader = &Readers[*DeviceCount * READERS_PER_DEVICE + i];
        ZeroMemory(Reader, sizeof(*Reader));
        StringCchCopyW(Reader->Path, _countof(Reader->Path), Path);
        Reader->SectorSize = Geometry.Geometry.BytesPerSector;
        Reader->Sectors = Geometry.DiskSize.QuadPart / Reader->SectorSize;
        if (Reader->Sectors > MAXULONG)
            Reader->Sectors = MAXULONG;
        Reader->Seed = 0x42 + (*DeviceCount * READERS_PER_DEVICE + i) * 7919 + GetTickCount();
    }

    trace("%S: %I64u bytes, %lu-byte sectors\n",
          Path, Geometry.DiskSize.QuadPart, Geometry.Geometry.BytesPerSector);
    (*DeviceCount)++;
    return TRUE;
}

START_TEST(UsbConcurrentDevices)
{
    static READER Readers[MAX_DEVICES * READERS_PER_DEVICE];
    HANDLE Threads[MAX_DEVICES * READERS_PER_DEVICE];
    WCHAR Path[32];
    ULONG Number, DeviceCount = 0, i, Started = 0, Done = 0;
    DWORD Wait, Start;

    for (Number = 0; Number < 16; Number++)
    {
        StringCchPrintfW(Path, _countof(Path), L"\\\\.\\PhysicalDrive%lu", Number);
        AddDevice(Path, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, Readers, &DeviceCount);
    }
    for (Number = 0; Number < 16; Number++)
    {
        StringCchPrintfW(Path, _countof(Path), L"\\\\.\\CdRom%lu", Number);
        AddDevice(Path, IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX, Readers, &DeviceCount);
    }

    if (DeviceCount < 2)
    {
        skip("Need two USB storage devices, found %lu\n", DeviceCount);
        return;
    }

    Start = GetTickCount();
    for (i = 0; i < DeviceCount * READERS_PER_DEVICE; i++)
    {
        Threads[Started] = CreateThread(NULL, 0, ReaderThread, &Readers[i], 0, NULL);
        ok(Threads[Started] != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (Threads[Started])
            Started++;
    }

    Wait = WaitForMultipleObjects(Started, Threads, TRUE, WAIT_MS);
    for (i = 0; i < Started; i++)
    {
        Done += Readers[i].Done;
        ok(Readers[i].Failed == 0, "%S reader %lu: %ld of %ld reads failed, first error %lu\n",
           Readers[i].Path, i, Readers[i].Failed, Readers[i].Done, Readers[i].FirstError);
    }

    ok(Wait == WAIT_OBJECT_0,
       "readers still blocked after %lu ms (wait %lu), %lu of %lu reads done\n",
       GetTickCount() - Start, Wait, Done, Started * READS_PER_READER);
    trace("%lu devices, %lu reads in %lu ms\n", DeviceCount, Done, GetTickCount() - Start);

    /* A blocked reader cannot be cleaned up; leave its handle alone */
    if (Wait == WAIT_OBJECT_0)
    {
        for (i = 0; i < Started; i++)
            CloseHandle(Threads[i]);
    }
}
