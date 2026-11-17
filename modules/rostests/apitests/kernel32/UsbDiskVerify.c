/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that data read from and written to a USB disk is the data
 *              on the medium, under concurrent load
 * COPYRIGHT:   ReactOS Developers
 *
 * The other USB disk tests only check that requests complete with the
 * requested length. This one needs a prepared disk: every 512-byte sector
 * holds USBV_MAGIC, its LBA and a filler derived from the LBA (see
 * FillSector). Readers check random unaligned spans of the upper half;
 * writers own disjoint slices of the lower half, write a generation-tagged
 * pattern and read it back uncached. Disks without the pattern are skipped.
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddstor.h>

#define USBV_MAGIC        0x56425355  /* 'USBV' */
#define USBW_MAGIC        0x57425355  /* 'USBW' */
#define SECTOR            512
#define MAX_SPAN          128         /* sectors = 64 KB */
#define READERS           4
#define WRITERS           2
#define OPS_PER_THREAD    200
#define WAIT_MS           (300 * 1000)

typedef struct
{
    WCHAR Path[32];
    ULONG Sectors;
    ULONG Seed;
    ULONG First;      /* first sector of this thread's range */
    ULONG Count;      /* sectors in the range */
    BOOL Write;
    LONG Done;
    LONG IoFailed;
    LONG Mismatch;
    ULONG FirstBadLba;
    DWORD FirstError;
} WORKER;

static
ULONG
NextRandom(PULONG Seed)
{
    *Seed = *Seed * 1103515245 + 12345;
    return (*Seed >> 8) & 0xFFFFFF;
}

static
void
FillSector(PULONG Words, ULONG Magic, ULONG Lba, ULONG Generation)
{
    ULONG i;

    Words[0] = Magic;
    Words[1] = Lba;
    Words[2] = Generation;
    for (i = 3; i < SECTOR / sizeof(ULONG); i++)
        Words[i] = (Lba * 2654435761u) ^ (i * 40503u) ^ Generation;
}

static
BOOL
CheckSector(const ULONG *Words, ULONG Magic, ULONG Lba, ULONG Generation)
{
    ULONG Expected[SECTOR / sizeof(ULONG)];

    FillSector(Expected, Magic, Lba, Generation);
    return memcmp(Words, Expected, SECTOR) == 0;
}

static
BOOL
DoIo(HANDLE Device, BOOL Write, ULONG Lba, ULONG Count, PVOID Buffer, PDWORD Error)
{
    LARGE_INTEGER Offset;
    DWORD Done = 0;
    BOOL Ok;

    Offset.QuadPart = (ULONGLONG)Lba * SECTOR;
    if (!SetFilePointerEx(Device, Offset, NULL, FILE_BEGIN))
    {
        *Error = GetLastError();
        return FALSE;
    }
    if (Write)
        Ok = WriteFile(Device, Buffer, Count * SECTOR, &Done, NULL);
    else
        Ok = ReadFile(Device, Buffer, Count * SECTOR, &Done, NULL);
    if (!Ok || Done != Count * SECTOR)
    {
        *Error = Ok ? ERROR_HANDLE_EOF : GetLastError();
        return FALSE;
    }
    return TRUE;
}

static
DWORD
WINAPI
WorkerThread(PVOID Param)
{
    WORKER *W = Param;
    HANDLE Device;
    PUCHAR Buffer;
    ULONG Op, Count, Lba, i, Generation;
    DWORD Error = 0;

    Device = CreateFileW(W->Path, GENERIC_READ | (W->Write ? GENERIC_WRITE : 0),
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        W->FirstError = GetLastError();
        W->IoFailed = OPS_PER_THREAD;
        return 0;
    }

    Buffer = VirtualAlloc(NULL, MAX_SPAN * SECTOR, MEM_COMMIT, PAGE_READWRITE);
    if (!Buffer)
    {
        W->FirstError = GetLastError();
        W->IoFailed = OPS_PER_THREAD;
        CloseHandle(Device);
        return 0;
    }

    for (Op = 0; Op < OPS_PER_THREAD; Op++)
    {
        Count = 1 + NextRandom(&W->Seed) % MAX_SPAN;
        Lba = W->First + NextRandom(&W->Seed) % (W->Count - Count);

        if (W->Write)
        {
            Generation = 1 + Op;
            for (i = 0; i < Count; i++)
                FillSector((PULONG)(Buffer + i * SECTOR), USBW_MAGIC, Lba + i, Generation);
            if (!DoIo(Device, TRUE, Lba, Count, Buffer, &Error))
                goto IoFail;
            FillMemory(Buffer, Count * SECTOR, 0xCC);
        }
        else
        {
            Generation = 0;
            FillMemory(Buffer, Count * SECTOR, 0xCC);
        }

        if (!DoIo(Device, FALSE, Lba, Count, Buffer, &Error))
            goto IoFail;

        for (i = 0; i < Count; i++)
        {
            if (!CheckSector((PULONG)(Buffer + i * SECTOR),
                             W->Write ? USBW_MAGIC : USBV_MAGIC, Lba + i, Generation))
            {
                if (!W->Mismatch++)
                    W->FirstBadLba = Lba + i;
                break;
            }
        }
        InterlockedIncrement(&W->Done);
        continue;

IoFail:
        if (!W->IoFailed++)
            W->FirstError = Error;
        InterlockedIncrement(&W->Done);
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
    CloseHandle(Device);
    return 0;
}

static
BOOL
IsPatternDisk(PCWSTR Path, PULONG Sectors)
{
    HANDLE Device;
    STORAGE_PROPERTY_QUERY Query;
    UCHAR DescriptorBuffer[512];
    PSTORAGE_DEVICE_DESCRIPTOR Descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)DescriptorBuffer;
    DISK_GEOMETRY_EX Geometry;
    PULONG Sector;
    DWORD Size, Error;
    BOOL Result = FALSE;

    Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;

    ZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageDeviceProperty;
    Query.QueryType = PropertyStandardQuery;
    Sector = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (Sector &&
        DeviceIoControl(Device, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query),
                        DescriptorBuffer, sizeof(DescriptorBuffer), &Size, NULL) &&
        Descriptor->BusType == BusTypeUsb &&
        DeviceIoControl(Device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                        &Geometry, sizeof(Geometry), &Size, NULL) &&
        Geometry.Geometry.BytesPerSector == SECTOR &&
        DoIo(Device, FALSE, 1, 1, Sector, &Error) &&
        CheckSector(Sector, USBV_MAGIC, 1, 0))
    {
        *Sectors = (ULONG)min(Geometry.DiskSize.QuadPart / SECTOR, MAXULONG);
        Result = TRUE;
    }

    if (Sector)
        VirtualFree(Sector, 0, MEM_RELEASE);
    CloseHandle(Device);
    return Result;
}

START_TEST(UsbDiskVerify)
{
    static WORKER Workers[16 * (READERS + WRITERS)];
    HANDLE Threads[RTL_NUMBER_OF(Workers)];
    WCHAR Path[32];
    ULONG Number, Sectors, Disks = 0, Started = 0, i, Half, Slice;
    DWORD Wait, Start;

    for (Number = 0; Number < 16; Number++)
    {
        StringCchPrintfW(Path, _countof(Path), L"\\\\.\\PhysicalDrive%lu", Number);
        if (!IsPatternDisk(Path, &Sectors))
            continue;

        trace("%S: pattern disk, %lu sectors\n", Path, Sectors);
        Half = Sectors / 2;
        Slice = (Half - 2) / WRITERS;
        for (i = 0; i < READERS + WRITERS; i++)
        {
            WORKER *W = &Workers[Started];

            ZeroMemory(W, sizeof(*W));
            StringCchCopyW(W->Path, _countof(W->Path), Path);
            W->Sectors = Sectors;
            W->Seed = 0x5a + Started * 7919 + GetTickCount();
            W->Write = (i >= READERS);
            if (W->Write)
            {
                /* Sectors 0 and 1 stay intact: they identify the disk */
                W->First = 2 + (i - READERS) * Slice;
                W->Count = Slice;
            }
            else
            {
                W->First = Half;
                W->Count = Sectors - Half;
            }
            Started++;
        }
        Disks++;
    }

    if (!Disks)
    {
        skip("No USB disk carrying the verification pattern\n");
        return;
    }

    Start = GetTickCount();
    for (i = 0; i < Started; i++)
    {
        Threads[i] = CreateThread(NULL, 0, WorkerThread, &Workers[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (!Threads[i])
        {
            Started = i;
            break;
        }
    }

    Wait = WaitForMultipleObjects(Started, Threads, TRUE, WAIT_MS);
    for (i = 0; i < Started; i++)
    {
        WORKER *W = &Workers[i];

        ok(W->IoFailed == 0, "%S %s %lu: %ld of %ld I/Os failed, first error %lu\n",
           W->Path, W->Write ? "writer" : "reader", i, W->IoFailed, W->Done, W->FirstError);
        ok(W->Mismatch == 0, "%S %s %lu: %ld of %ld I/Os returned wrong data, first at LBA %lu\n",
           W->Path, W->Write ? "writer" : "reader", i, W->Mismatch, W->Done, W->FirstBadLba);
    }
    ok(Wait == WAIT_OBJECT_0, "workers still blocked after %lu ms (wait %lu)\n",
       GetTickCount() - Start, Wait);
    trace("%lu disks, %lu workers, %lu ms\n", Disks, Started, GetTickCount() - Start);

    if (Wait == WAIT_OBJECT_0)
    {
        for (i = 0; i < Started; i++)
            CloseHandle(Threads[i]);
    }
}
