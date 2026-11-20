/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that reads from a surprise-removed USB disk fail at once
 *              with a "device gone" error
 * COPYRIGHT:   ReactOS Developers
 *
 * Reads every USB disk in turn while the host unplugs one of them (the test
 * prints USBDISK-UNPLUG-NOW for the harness, see sdk/tools/usbqemu). Once
 * the hub fails the gone device's transfers, the disk class driver must not
 * retry them: usbstor used to report a failed transfer as a bus reset, so
 * every read went through a full set of class retries and device resets and
 * then failed with ERROR_IO_DEVICE, ~0.5 s per read, until the handles were
 * closed. Windows fails them with STATUS_NO_SUCH_DEVICE (ERROR_FILE_NOT_FOUND)
 * from the port driver or, once the disk saw IRP_MN_SURPRISE_REMOVAL, with
 * STATUS_DEVICE_DOES_NOT_EXIST (ERROR_DEV_NOT_EXIST) from the class driver.
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddstor.h>

#define MAX_DISKS       8
#define WAIT_UNPLUG_MS  (90 * 1000)
#define READS_AFTER     50
#define READ_SIZE       (64 * 1024)

static
BOOLEAN
IsDeviceGoneError(DWORD Error)
{
    return Error == ERROR_FILE_NOT_FOUND ||       /* STATUS_NO_SUCH_DEVICE */
           Error == ERROR_DEV_NOT_EXIST ||        /* STATUS_DEVICE_DOES_NOT_EXIST */
           Error == ERROR_DEVICE_NOT_CONNECTED;   /* STATUS_DEVICE_NOT_CONNECTED */
}

static
BOOL
ReadAt(HANDLE Device, PVOID Buffer, ULONGLONG Offset)
{
    LARGE_INTEGER Position;
    DWORD Read;

    Position.QuadPart = Offset;
    return SetFilePointerEx(Device, Position, NULL, FILE_BEGIN) &&
           ReadFile(Device, Buffer, READ_SIZE, &Read, NULL) &&
           Read == READ_SIZE;
}

START_TEST(UsbDiskRemoval)
{
    WCHAR Path[32];
    HANDLE Disks[MAX_DISKS];
    ULONGLONG Sizes[MAX_DISKS];
    ULONG Count = 0, Number, i, Pass, Gone = MAXULONG, Failed = 0;
    STORAGE_PROPERTY_QUERY Query;
    UCHAR DescriptorBuffer[512];
    PSTORAGE_DEVICE_DESCRIPTOR Descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)DescriptorBuffer;
    DISK_GEOMETRY_EX Geometry;
    DWORD Size, Start, Elapsed, Error = 0, LastError = 0, Worst = 0, t;
    PUCHAR Buffer;
    HANDLE Device;

    for (Number = 0; Number < 16 && Count < MAX_DISKS; Number++)
    {
        StringCchPrintfW(Path, _countof(Path), L"\\\\.\\PhysicalDrive%lu", Number);
        Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
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
            Geometry.DiskSize.QuadPart < 16 * 1024 * 1024)
        {
            CloseHandle(Device);
            continue;
        }

        Sizes[Count] = Geometry.DiskSize.QuadPart;
        Disks[Count++] = Device;
    }

    if (!Count)
    {
        skip("No USB mass storage disk present\n");
        return;
    }

    Buffer = VirtualAlloc(NULL, READ_SIZE, MEM_COMMIT, PAGE_READWRITE);
    ok(Buffer != NULL, "VirtualAlloc failed with %lu\n", GetLastError());
    if (!Buffer)
        goto Cleanup;

    /* For the harness: unplug a USB disk now */
    OutputDebugStringA("USBDISK-UNPLUG-NOW\n");
    trace("%lu USB disks open, waiting for one to be unplugged\n", Count);

    Start = GetTickCount();
    for (Pass = 0; Gone == MAXULONG && GetTickCount() - Start < WAIT_UNPLUG_MS; Pass++)
    {
        for (i = 0; i < Count; i++)
        {
            t = GetTickCount();
            if (!ReadAt(Disks[i], Buffer, (Pass * READ_SIZE) % (Sizes[i] - READ_SIZE)))
            {
                Error = GetLastError();
                Gone = i;
                trace("Disk %lu failed a read after %lu ms (pass %lu): error %lu, the read took %lu ms\n",
                      i, GetTickCount() - Start, Pass, Error, GetTickCount() - t);
                break;
            }
        }
    }

    if (Gone == MAXULONG)
    {
        skip("No USB disk was unplugged within %u s\n", WAIT_UNPLUG_MS / 1000);
        goto Cleanup;
    }

    /* The disk is gone: every further read must fail, and fail fast */
    Start = GetTickCount();
    for (i = 0; i < READS_AFTER; i++)
    {
        t = GetTickCount();
        if (ReadAt(Disks[Gone], Buffer, (ULONGLONG)i * READ_SIZE))
            continue;
        Failed++;
        LastError = GetLastError();
        if (!IsDeviceGoneError(LastError))
            Error = LastError;
        t = GetTickCount() - t;
        if (t > Worst)
            Worst = t;
    }
    Elapsed = GetTickCount() - Start;

    ok(IsDeviceGoneError(Error),
       "Reads of the unplugged disk failed with %lu, expected a device-gone error\n", Error);
    ok(Failed == READS_AFTER, "%lu of %u reads of the unplugged disk failed\n", Failed, READS_AFTER);
    ok(Elapsed < 3000, "%u reads of the unplugged disk took %lu ms (slowest %lu ms, last error %lu)\n",
       READS_AFTER, Elapsed, Worst, LastError);
    trace("%u reads of the unplugged disk: %lu failed in %lu ms, slowest %lu ms, last error %lu\n",
          READS_AFTER, Failed, Elapsed, Worst, LastError);

Cleanup:
    if (Buffer)
        VirtualFree(Buffer, 0, MEM_RELEASE);
    for (i = 0; i < Count; i++)
        CloseHandle(Disks[i]);
}
