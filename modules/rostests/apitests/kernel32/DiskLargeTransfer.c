/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test reads and writes of the largest transfer a disk's adapter takes
 * COPYRIGHT:   ReactOS Developers
 *
 * A non-buffered transfer of MaximumTransferLength bytes from a page-aligned
 * buffer reaches the miniport as a single request. Its data must be the
 * same as that of the same sectors transferred in small pieces: an ATA
 * miniport has to encode the largest sector count in the command's count
 * register (as 0 for 256 sectors of a 28-bit command).
 *
 * Every disk is read; the write test only runs on a disk without any
 * partition, and puts back what was there.
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddstor.h>
#include <strsafe.h>

#define SECTOR 512
#define PIECE 4096
#define MB (1024 * 1024)

static
BOOL
TransferAt(HANDLE Disk, BOOL Write, LONGLONG Offset, PVOID Buffer, DWORD Length)
{
    LARGE_INTEGER Position;
    DWORD Done = 0;
    BOOL Ret;

    Position.QuadPart = Offset;
    if (!SetFilePointerEx(Disk, Position, NULL, FILE_BEGIN))
        return FALSE;
    if (Write)
        Ret = WriteFile(Disk, Buffer, Length, &Done, NULL);
    else
        Ret = ReadFile(Disk, Buffer, Length, &Done, NULL);
    return Ret && Done == Length;
}

/* Reads Length bytes at Offset in pieces */
static
BOOL
ReadPieces(HANDLE Disk, LONGLONG Offset, PUCHAR Buffer, DWORD Length)
{
    DWORD i;

    for (i = 0; i < Length; i += PIECE)
    {
        if (!TransferAt(Disk, FALSE, Offset + i, Buffer + i, PIECE))
            return FALSE;
    }
    return TRUE;
}

static
BOOL
IsBlankDisk(HANDLE Disk)
{
    struct
    {
        DRIVE_LAYOUT_INFORMATION_EX Layout;
        PARTITION_INFORMATION_EX More[3];
    } Layout;
    DWORD Size, i;

    if (!DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, &Layout, sizeof(Layout), &Size, NULL))
        return FALSE;
    if (Layout.Layout.PartitionStyle == PARTITION_STYLE_GPT)
        return FALSE;
    for (i = 0; i < Layout.Layout.PartitionCount && i < 4; i++)
    {
        if (Layout.Layout.PartitionEntry[i].Mbr.PartitionType != PARTITION_ENTRY_UNUSED)
            return FALSE;
    }
    return TRUE;
}

static
void
TestDisk(HANDLE Disk, PCWSTR Name)
{
    STORAGE_PROPERTY_QUERY Query;
    STORAGE_ADAPTER_DESCRIPTOR Adapter;
    GET_LENGTH_INFORMATION DiskLength;
    PUCHAR Large, Pieces, Saved;
    LONGLONG Offsets[2];
    DWORD Length, Size, i, j;

    ZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageAdapterProperty;
    Query.QueryType = PropertyStandardQuery;
    if (!DeviceIoControl(Disk, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query),
                         &Adapter, sizeof(Adapter), &Size, NULL) ||
        !DeviceIoControl(Disk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                         &DiskLength, sizeof(DiskLength), &Size, NULL))
    {
        skip("%ls: no adapter descriptor or length (%lu)\n", Name, GetLastError());
        return;
    }

    /* One request: no more than the adapter's transfer length and pages */
    Length = Adapter.MaximumTransferLength;
    if (Adapter.MaximumPhysicalPages && Length / PIECE > Adapter.MaximumPhysicalPages)
        Length = Adapter.MaximumPhysicalPages * PIECE;
    Length = min(Length, 1 * MB) & ~(PIECE - 1);
    if (Length < PIECE || DiskLength.Length.QuadPart < 4 * (LONGLONG)Length)
    {
        skip("%ls: transfer length %lu, disk length 0x%I64x\n", Name, Length, DiskLength.Length.QuadPart);
        return;
    }
    trace("%ls: %lu-byte transfers (MaximumTransferLength %lu, MaximumPhysicalPages %lu)\n",
          Name, Length, Adapter.MaximumTransferLength, Adapter.MaximumPhysicalPages);

    Large = VirtualAlloc(NULL, Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Pieces = VirtualAlloc(NULL, Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Saved = VirtualAlloc(NULL, Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Large || !Pieces || !Saved)
    {
        skip("%ls: out of memory\n", Name);
        goto Done;
    }

    /* The start of the disk and its end, past the 28-bit LBA bits of a large disk */
    Offsets[0] = 0;
    Offsets[1] = (DiskLength.Length.QuadPart - Length) & ~(LONGLONG)(PIECE - 1);

    for (i = 0; i < _countof(Offsets); i++)
    {
        memset(Large, 0xCC, Length);
        ok(TransferAt(Disk, FALSE, Offsets[i], Large, Length),
           "%ls: reading %lu bytes at 0x%I64x failed with %lu\n", Name, Length, Offsets[i], GetLastError());
        ok(ReadPieces(Disk, Offsets[i], Pieces, Length),
           "%ls: reading in pieces at 0x%I64x failed with %lu\n", Name, Offsets[i], GetLastError());
        ok(memcmp(Large, Pieces, Length) == 0,
           "%ls: large read at 0x%I64x differs from the read in pieces\n", Name, Offsets[i]);
    }

    if (!IsBlankDisk(Disk))
    {
        skip("%ls: not blank, no write test\n", Name);
        goto Done;
    }

    /* Write every sector its own number, in one request, and read it back in pieces */
    for (i = 0; i < _countof(Offsets); i++)
    {
        if (!TransferAt(Disk, FALSE, Offsets[i], Saved, Length))
        {
            skip("%ls: cannot save the data at 0x%I64x (%lu)\n", Name, Offsets[i], GetLastError());
            continue;
        }

        for (j = 0; j < Length; j += sizeof(ULONG))
            *(PULONG)(Large + j) = (ULONG)((Offsets[i] + j) / SECTOR) ^ (j % SECTOR);
        ok(TransferAt(Disk, TRUE, Offsets[i], Large, Length),
           "%ls: writing %lu bytes at 0x%I64x failed with %lu\n", Name, Length, Offsets[i], GetLastError());
        memset(Pieces, 0xCC, Length);
        ok(ReadPieces(Disk, Offsets[i], Pieces, Length),
           "%ls: reading in pieces at 0x%I64x failed with %lu\n", Name, Offsets[i], GetLastError());
        for (j = 0; j < Length; j += SECTOR)
        {
            if (memcmp(Large + j, Pieces + j, SECTOR))
                break;
        }
        ok(j == Length, "%ls: sector %lu of the large write at 0x%I64x reads back wrong\n",
           Name, j / SECTOR, Offsets[i]);

        ok(TransferAt(Disk, TRUE, Offsets[i], Saved, Length),
           "%ls: restoring the data at 0x%I64x failed with %lu\n", Name, Offsets[i], GetLastError());
    }

Done:
    if (Large)
        VirtualFree(Large, 0, MEM_RELEASE);
    if (Pieces)
        VirtualFree(Pieces, 0, MEM_RELEASE);
    if (Saved)
        VirtualFree(Saved, 0, MEM_RELEASE);
}

START_TEST(DiskLargeTransfer)
{
    WCHAR Name[32];
    HANDLE Disk;
    ULONG i, Tested = 0;

    for (i = 0; i < 16; i++)
    {
        StringCchPrintfW(Name, _countof(Name), L"\\\\.\\PhysicalDrive%lu", i);
        Disk = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
        if (Disk == INVALID_HANDLE_VALUE)
            continue;
        TestDisk(Disk, Name);
        CloseHandle(Disk);
        Tested++;
    }
    if (!Tested)
        skip("No disk\n");
}
