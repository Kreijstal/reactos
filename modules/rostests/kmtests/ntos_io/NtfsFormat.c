/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (see COPYING.LIB)
 * PURPOSE:     Inspect volumes created through the public formatter API
 */
#include <kmt_test.h>
#include "NtfsKmt.h"

static BOOLEAN
FindLogLocation(HANDLE Volume, PNTFS_VOLUME_DATA_BUFFER Geometry,
                PLARGE_INTEGER Offset, PULONGLONG FileSize)
{
    NTFS_FILE_RECORD_INPUT_BUFFER Input;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER Output;
    PUCHAR Record, Attr, Run;
    ULONG OutputSize, Position, Type, Length, i, LengthBytes, OffsetBytes;
    USHORT AttributeOffset, MappingOffset;
    ULONGLONG Clusters = 0, Lcn = 0;
    NTSTATUS Status;
    BOOLEAN Found = FALSE;

    OutputSize = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) +
                 Geometry->BytesPerFileRecordSegment;
    Output = NtfsKmtAlloc(OutputSize);
    if (Output == NULL) return FALSE;
    Input.FileReferenceNumber.QuadPart = 2;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input),
                          Output, OutputSize, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ok_eq_longlong(Output->FileReferenceNumber.QuadPart & 0x0000ffffffffffffULL, 2);
    if (Output->FileRecordLength < 24 ||
        Output->FileRecordLength > Geometry->BytesPerFileRecordSegment) goto Cleanup;
    Record = Output->FileRecordBuffer;
    RtlCopyMemory(&AttributeOffset, Record + 20, sizeof(AttributeOffset));
    for (Position = AttributeOffset; Position <= Output->FileRecordLength - 8; Position += Length)
    {
        Attr = Record + Position;
        RtlCopyMemory(&Type, Attr, sizeof(Type));
        RtlCopyMemory(&Length, Attr + 4, sizeof(Length));
        if (Type == MAXULONG) break;
        if (Length < 16 || Length > Output->FileRecordLength - Position) break;
        if (Type != 0x80 || Attr[9] != 0) continue;
        if (!Attr[8] || Length < 64) break;
        RtlCopyMemory(&MappingOffset, Attr + 32, sizeof(MappingOffset));
        if (MappingOffset >= Length) break;
        Run = Attr + MappingOffset;
        LengthBytes = Run[0] & 15;
        OffsetBytes = Run[0] >> 4;
        if (!LengthBytes || LengthBytes > 8 || !OffsetBytes || OffsetBytes > 8 ||
            1 + LengthBytes + OffsetBytes > Length - MappingOffset) break;
        for (i = 0; i < LengthBytes; i++) Clusters |= (ULONGLONG)Run[1 + i] << (8 * i);
        for (i = 0; i < OffsetBytes; i++) Lcn |= (ULONGLONG)Run[1 + LengthBytes + i] << (8 * i);
        if (Run[LengthBytes + OffsetBytes] & 0x80) break;
        ok(Clusters * Geometry->BytesPerCluster >= 8192, "$LogFile fixture is fragmented\n");
        if (Clusters * Geometry->BytesPerCluster < 8192) break;
        ok(Lcn != 0 && Lcn < Geometry->TotalClusters.QuadPart &&
           8192 <= (Geometry->TotalClusters.QuadPart - Lcn) * Geometry->BytesPerCluster,
           "invalid $LogFile fixture location\n");
        if (Lcn == 0 || Lcn >= Geometry->TotalClusters.QuadPart ||
            8192 > (Geometry->TotalClusters.QuadPart - Lcn) * Geometry->BytesPerCluster) break;
        Offset->QuadPart = Lcn * Geometry->BytesPerCluster;
        RtlCopyMemory(FileSize, Attr + 48, sizeof(*FileSize));
        Found = TRUE;
        break;
    }
    ok(Found, "cannot locate $LogFile fixture pages\n");
Cleanup:
    NtfsKmtFree(Output);
    return Found;
}

static USHORT Read16(PUCHAR Bytes)
{
    USHORT Value;
    RtlCopyMemory(&Value, Bytes, sizeof(Value));
    return Value;
}

static ULONG Read32(PUCHAR Bytes)
{
    ULONG Value;
    RtlCopyMemory(&Value, Bytes, sizeof(Value));
    return Value;
}

static VOID
CheckFormattedVolume(VOID)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], Drive;
    HANDLE Volume = NULL, File = NULL;
    NTFS_VOLUME_DATA_BUFFER Geometry;
    LARGE_INTEGER LogOffset;
    ULONGLONG LogSize;
    PUCHAR Pages;
    ULONG i, SectorSize, Flags;
    ULONG_PTR Bytes;
    NTSTATUS Status;
    const CHAR Data[] = "Freshly formatted NTFS supports ordinary file I/O";
    CHAR Readback[sizeof(Data)];

    if (!NtfsKmtFindVolume(Root, &Drive))
    {
        /* The user part already selected and formatted this fixture. */
        ok(FALSE, "Formatted volume is unavailable or still read-only\n");
        return;
    }
    Status = NtfsKmtOpenVolume(Drive, FILE_READ_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                          &Geometry, sizeof(Geometry), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    SectorSize = Geometry.BytesPerSector;
    ok_eq_ulong(SectorSize, 512);
    if (SectorSize != 512) goto Cleanup;
    ok(Geometry.BytesPerCluster == 512 || Geometry.BytesPerCluster == 4096,
       "Unexpected cluster size %lu\n", Geometry.BytesPerCluster);
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0,
                          &Flags, sizeof(Flags), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ok_eq_ulong(Flags, 0);
    if (!FindLogLocation(Volume, &Geometry, &LogOffset, &LogSize)) goto Cleanup;
    Pages = NtfsKmtAlloc(8192);
    ok(Pages != NULL, "Cannot allocate restart-page buffer\n");
    if (Pages == NULL) goto Cleanup;
    Status = NtfsKmtRead(Volume, LogOffset.QuadPart, Pages, 8192, &Bytes);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_ulong(Bytes, 8192);
        ok(RtlCompareMemory(Pages, "RSTR", 4) == 4, "Missing restart magic\n");
        ok(RtlCompareMemory(Pages, Pages + 4096, 4096) == 4096,
           "Fresh restart copies differ\n");
        ok_eq_ulong(Read32(Pages + 0x10), 4096);
        ok_eq_ulong(Read32(Pages + 0x14), 4096);
        ok_eq_uint(Read16(Pages + 0x18), 0x40);
        ok_eq_uint(Read16(Pages + 0x1A), 1);
        ok_eq_uint(Read16(Pages + 0x1C), 1);
        ok_eq_uint(Read16(Pages + 0x4E), 2);
        ok_eq_uint(Read16(Pages + 0x54), 0xD0);
        ok_eq_uint(Read16(Pages + 0x56), 0x30);
        ok_eq_uint(Read16(Pages + 0x48), 1);
        ok_eq_uint(Read16(Pages + 0x4A), MAXUSHORT);
        ok_eq_uint(Read16(Pages + 0x4C), 0);
        ok_eq_uint(Read16(Pages + 0x84), 1);
        ok_eq_ulong(Read32(Pages + 0x8C), 8);
        ok(RtlCompareMemory(Pages + 0x90, L"NTFS", 8) == 8, "Missing NTFS client\n");
        for (i = 1; i <= 4096 / SectorSize; ++i)
            ok_eq_uint(Read16(Pages + i * SectorSize - 2), Read16(Pages + 0x28));
    }
    NtfsKmtFree(Pages);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_FORMAT_DATA.bin");
    Status = NtfsKmtCreateFileWithData(Path, Data, sizeof(Data));
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtOpen(&File, Path, FILE_READ_DATA, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtRead(File, 0, Readback, sizeof(Readback), &Bytes);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Bytes, sizeof(Readback));
    ok(RtlCompareMemory(Readback, Data, sizeof(Data)) == sizeof(Data), "File data differs\n");
    ZwClose(File); File = NULL;
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
Cleanup:
    if (File != NULL) ZwClose(File);
    ZwClose(Volume);
}

START_TEST(NtfsFormat)
{
    CheckFormattedVolume();
}

START_TEST(NtfsChkdsk)
{
    CheckFormattedVolume();
}
