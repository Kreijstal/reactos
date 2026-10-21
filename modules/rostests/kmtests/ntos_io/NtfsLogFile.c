/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (see COPYING.LIB)
 * PURPOSE:     Black-box $LogFile restart selection and read-only mount tests
 *
 * Only public volume/file requests are used. The fixture changes the first
 * two $LogFile pages, and puts back its $Volume record, on a locked,
 * dismounted throwaway volume; after the last case the restart pages of a
 * clean dismount are written back, keeping LSNs increasing. No boot sector
 * is changed.
 *
 * A dirty restart area is recovered at mount; one naming no record this
 * driver can read cannot be, and the volume then mounts read-only, marked
 * dirty for chkdsk.  An unsupported log version mounts read-only, not
 * marked: it is another system's log.  Logging is on, so a read-write
 * mount leaves both restart pages clean after its dismount.
 */
#include <kmt_test.h>
#include "NtfsKmt.h"

#define LOG_PAGE 4096
#define LOG_PAIR (2 * LOG_PAGE)

/* Native writes enforce the handle's write grant even in kernel mode.
 * Use a public kernel request for dismounted device I/O and to exercise
 * the filesystem's read-only gate independently of that native check. */
static NTSTATUS
WriteLogRequest(HANDLE Handle, PVOID Buffer, ULONG Length, PLARGE_INTEGER Offset)
{
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Device = IoGetRelatedDeviceObject(File);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, Device, Buffer, Length, Offset, &Event, &IoStatus);
    if (Irp == NULL) { ObDereferenceObject(File); return STATUS_INSUFFICIENT_RESOURCES; }
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->FileObject = File;
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    ObDereferenceObject(File);
    return Status;
}

static VOID Put16(PUCHAR p, USHORT v) { RtlCopyMemory(p, &v, sizeof(v)); }
static VOID Put32(PUCHAR p, ULONG v) { RtlCopyMemory(p, &v, sizeof(v)); }
static VOID Put64(PUCHAR p, ULONGLONG v) { RtlCopyMemory(p, &v, sizeof(v)); }

static VOID
BuildRestart(PUCHAR Page, ULONG SectorSize, ULONGLONG FileSize,
             ULONGLONG Lsn, BOOLEAN Clean, USHORT Major)
{
    ULONG i, Bits = 0;
    ULONGLONG n = FileSize;
    USHORT Tail;

    while (n) { Bits++; n >>= 1; }
    RtlZeroMemory(Page, LOG_PAGE);
    RtlCopyMemory(Page, "RSTR", 4);
    Put16(Page + 4, 0x28);
    Put16(Page + 6, LOG_PAGE / SectorSize + 1);
    Put32(Page + 0x10, LOG_PAGE);
    Put32(Page + 0x14, LOG_PAGE);
    Put16(Page + 0x18, 0x40);
    Put16(Page + 0x1a, 1);
    Put16(Page + 0x1c, Major);
    Put64(Page + 0x40, Lsn);
    Put16(Page + 0x48, 1);
    Put16(Page + 0x4a, MAXUSHORT);
    Put16(Page + 0x4c, 0);
    Put16(Page + 0x4e, Clean ? 2 : 0);
    Put32(Page + 0x50, 67 - Bits);
    Put16(Page + 0x54, 0xd0);
    Put16(Page + 0x56, 0x30);
    Put64(Page + 0x58, FileSize);
    /* A clean restart can still describe a nonempty last log record. */
    Put32(Page + 0x60, 64);
    Put16(Page + 0x64, 0x30);
    Put16(Page + 0x66, 0x40);
    Put32(Page + 0x68, 1);
    Put16(Page + 0x80, MAXUSHORT);
    Put16(Page + 0x82, MAXUSHORT);
    Put16(Page + 0x84, 1);
    Put32(Page + 0x8c, 8);
    RtlCopyMemory(Page + 0x90, L"NTFS", 8);
    Put16(Page + 0x28, 0xabcd);
    for (i = 1; i <= LOG_PAGE / SectorSize; i++)
    {
        RtlCopyMemory(&Tail, Page + i * SectorSize - 2, sizeof(Tail));
        Put16(Page + 0x28 + 2 * i, Tail);
        Put16(Page + i * SectorSize - 2, 0xabcd);
    }
}

/* Both restart copies valid and clean, as a dismount leaves them. */
static BOOLEAN
RestartPairClean(const UCHAR *Pair, ULONG SectorSize)
{
    UCHAR Page[LOG_PAGE];
    ULONG Copy, i;
    USHORT UsaOffset, UsaCount, Offset, Flags;

    for (Copy = 0; Copy < 2; Copy++)
    {
        RtlCopyMemory(Page, Pair + Copy * LOG_PAGE, LOG_PAGE);
        RtlCopyMemory(&UsaOffset, Page + 4, 2);
        RtlCopyMemory(&UsaCount, Page + 6, 2);
        if (RtlCompareMemory(Page, "RSTR", 4) != 4 || UsaCount != LOG_PAGE / SectorSize + 1 ||
            UsaOffset + 2u * UsaCount > SectorSize)
        {
            trace("restart copy %lu: magic %.4s usa %u/%u\n", Copy, Page, UsaOffset, UsaCount);
            return FALSE;
        }
        for (i = 1; i < UsaCount; i++)
        {
            if (RtlCompareMemory(Page + i * SectorSize - 2, Page + UsaOffset, 2) != 2)
            {
                trace("restart copy %lu: sector %lu torn\n", Copy, i);
                return FALSE;
            }
            RtlCopyMemory(Page + i * SectorSize - 2, Page + UsaOffset + 2 * i, 2);
        }
        RtlCopyMemory(&Offset, Page + 0x18, 2);
        if (Offset > LOG_PAGE - 0x30)
            return FALSE;
        RtlCopyMemory(&Flags, Page + Offset + 0xE, 2);
        if (!(Flags & 2))
        {
            ULONGLONG Lsn;
            RtlCopyMemory(&Lsn, Page + Offset, 8);
            trace("restart copy %lu: flags %x lsn %I64x\n", Copy, Flags, Lsn);
            return FALSE;
        }
    }
    return TRUE;
}

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
        ok(Clusters * Geometry->BytesPerCluster >= LOG_PAIR, "$LogFile fixture is fragmented\n");
        if (Clusters * Geometry->BytesPerCluster < LOG_PAIR) break;
        ok(Lcn != 0 && Lcn < Geometry->TotalClusters.QuadPart &&
           LOG_PAIR <= (Geometry->TotalClusters.QuadPart - Lcn) * Geometry->BytesPerCluster,
           "invalid $LogFile fixture location\n");
        if (Lcn == 0 || Lcn >= Geometry->TotalClusters.QuadPart ||
            LOG_PAIR > (Geometry->TotalClusters.QuadPart - Lcn) * Geometry->BytesPerCluster) break;
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

static BOOLEAN
DismountLogVolume(HANDLE Volume)
{
    NTSTATUS Status;
    LARGE_INTEGER Delay;
    ULONG Try;

    /* A volume just mounted can be opened briefly by whoever the mount
     * notification woke up: the lock waits for that to pass. */
    Delay.QuadPart = -100 * 10000LL;
    for (Try = 0; Try < 50; Try++)
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
        if (Status != STATUS_ACCESS_DENIED)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status);
}

static BOOLEAN
CheckLogMount(WCHAR Letter, PCWSTR Path, BOOLEAN ReadOnly, BOOLEAN MarkedDirty, PBOOLEAN Dismounted,
              PLARGE_INTEGER LogOffset, PUCHAR Pages)
{
    HANDLE Volume, File = NULL;
    WCHAR NewPath[MAX_PATH];
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    UCHAR Attributes[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32];
    PFILE_FS_ATTRIBUTE_INFORMATION Info = (PVOID)Attributes;
    FILE_END_OF_FILE_INFORMATION Eof;
    FILE_POSITION_INFORMATION Position;
    FILE_ACCESS_INFORMATION Access;
    CREATE_USN_JOURNAL_DATA Journal = {0};
    ULONG Dirty;
    LARGE_INTEGER Zero = {{0}};
    CHAR Value = 0;

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) { *Dismounted = FALSE; return FALSE; }
    *Dismounted = FALSE;
    Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, Attributes, sizeof(Attributes),
                                          FileFsAttributeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(!!(Info->FileSystemAttributes & FILE_READ_ONLY_VOLUME) == ReadOnly,
           "read-only mount: got %lx, expected %u\n", Info->FileSystemAttributes, ReadOnly);
    if (ReadOnly)
    {
        Status = ZwWriteFile(Volume, NULL, NULL, NULL, &IoStatus, Pages, LOG_PAIR, LogOffset, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
    }
    Status = NtfsKmtOpen(&File, Path, FILE_READ_DATA, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ZwReadFile(File, NULL, NULL, NULL, &IoStatus, &Value, 1, &Zero, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_uint(Value, 'L');
        if (ReadOnly)
        {
            Status = NtfsKmtFlush(File);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = WriteLogRequest(File, &Value, 1, &Zero);
            ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
            Status = NtfsKmtFlush(File);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Eof.EndOfFile.QuadPart = 0;
            Status = ZwSetInformationFile(File, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
            ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
            Status = NtfsKmtFlush(File);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        ZwClose(File);
        File = NULL;
    }
    if (ReadOnly)
    {
        Status = NtfsKmtFlush(Volume);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    Status = NtfsKmtCreate(&File, NULL, Path, FILE_READ_DATA, FILE_ATTRIBUTE_NORMAL,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Position.CurrentByteOffset.QuadPart = 0;
        Status = ZwSetInformationFile(File, &IoStatus, &Position, sizeof(Position), FilePositionInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File); File = NULL;
    }
    Status = NtfsKmtOpen(&File, Path, MAXIMUM_ALLOWED, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ZwQueryInformationFile(File, &IoStatus, &Access, sizeof(Access), FileAccessInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status) && ReadOnly)
            ok(!(Access.AccessFlags & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
                                       FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER)),
               "MAXIMUM_ALLOWED granted mutation access on a read-only mount: %lx\n", Access.AccessFlags);
        ZwClose(File); File = NULL;
    }
    Status = NtfsKmtOpen(&File, Path, FILE_WRITE_DATA, 0);
    ok_eq_hex(Status, ReadOnly ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        if (!ReadOnly)
        {
            /* A logged data and MFT update. */
            Status = NtfsKmtWrite(File, 0, "L", 1, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = NtfsKmtFlush(File);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        ZwClose(File); File = NULL;
    }
    if (ReadOnly)
    {
        RtlStringCchPrintfW(NewPath, RTL_NUMBER_OF(NewPath), L"%ls.new", Path);
        Status = NtfsKmtCreate(&File, NULL, NewPath, FILE_WRITE_DATA, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
        if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
        Status = NtfsKmtCreate(&File, NULL, NewPath, FILE_READ_DATA, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
        if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
        RtlStringCchPrintfW(NewPath, RTL_NUMBER_OF(NewPath), L"%ls:newstream", Path);
        Status = NtfsKmtCreate(&File, NULL, NewPath, FILE_READ_DATA, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
        if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
        RtlStringCchPrintfW(NewPath, RTL_NUMBER_OF(NewPath), L"%ls.new:newstream", Path);
        Status = NtfsKmtCreate(&File, NULL, NewPath, FILE_READ_DATA, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
        if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
        Status = NtfsKmtFsctl(Volume, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
        Status = NtfsKmtFsctl(Volume, FSCTL_CREATE_USN_JOURNAL, &Journal, sizeof(Journal), NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_MEDIA_WRITE_PROTECTED);
        Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &Dirty, sizeof(Dirty), NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(!!(Dirty & VOLUME_IS_DIRTY) == MarkedDirty, "dirty flag %lx, expected %u\n", Dirty, MarkedDirty);
    }
    *Dismounted = DismountLogVolume(Volume);
    /* A read-write mount logged and left its own clean restart pages: those
     * are checked as they are. */
    if (*Dismounted && ReadOnly)
    {
        /* Identical log bytes, written through the same volume handle only
         * after the filesystem is dismounted. No boot sector is touched. */
        Status = ZwWriteFile(Volume, NULL, NULL, NULL, &IoStatus, Pages, LOG_PAIR, LogOffset, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtFlush(Volume);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(Volume);
    return *Dismounted;
}

START_TEST(NtfsLogFile)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], RawPath[16], Letter;
    HANDLE Volume = NULL, Raw = NULL, File = NULL;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Name;
    NTFS_VOLUME_DATA_BUFFER Geometry;
    LARGE_INTEGER Offset;
    ULONGLONG LogSize;
    PUCHAR Saved = NULL, Pages = NULL, Verify = NULL, Latest = NULL, VolumeRecords = NULL;
    LARGE_INTEGER VolumeOffset, MirrorOffset;
    ULONG Case;
    BOOLEAN Dismounted = FALSE, Modified = FALSE, HaveLatest = FALSE, ReadOnly, MarkedDirty;

    if (!NtfsKmtFindVolume(Root, &Letter)) { ok(FALSE, "no NTFS fixture volume\n"); return; }
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"kmt_logfile.dat");
    Status = NtfsKmtCreateFileWithData(Path, "L", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Geometry, sizeof(Geometry), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || Geometry.BytesPerSector == 0 || Geometry.BytesPerSector > LOG_PAGE ||
        !FindLogLocation(Volume, &Geometry, &Offset, &LogSize)) goto Cleanup;
    RtlStringCchPrintfW(RawPath, RTL_NUMBER_OF(RawPath), L"\\??\\%c:", Letter);
    RtlInitUnicodeString(&Name, RawPath);
    /* A bare-device attribute-only open deliberately bypasses NTFS. It
     * remains open across remounts, and is used only while dismounted. */
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&Raw, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &Attributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Saved = NtfsKmtAlloc(LOG_PAIR);
    Pages = NtfsKmtAlloc(LOG_PAIR);
    Verify = NtfsKmtAlloc(LOG_PAIR);
    Latest = NtfsKmtAlloc(LOG_PAIR);
    VolumeRecords = NtfsKmtAlloc(2 * Geometry.BytesPerFileRecordSegment);
    if (Saved == NULL || Pages == NULL || Verify == NULL || Latest == NULL || VolumeRecords == NULL ||
        Geometry.BytesPerFileRecordSegment == 0) goto Cleanup;
    /* $Volume's record and its mirror: the first MFT extent holds it. */
    VolumeOffset.QuadPart = Geometry.MftStartLcn.QuadPart * Geometry.BytesPerCluster +
                            3LL * Geometry.BytesPerFileRecordSegment;
    MirrorOffset.QuadPart = Geometry.Mft2StartLcn.QuadPart * Geometry.BytesPerCluster +
                            3LL * Geometry.BytesPerFileRecordSegment;
    Status = NtfsKmtFlush(Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Dismounted = DismountLogVolume(Volume);
    ZwClose(Volume); Volume = NULL;
    if (!Dismounted) goto Cleanup;
    Status = ZwReadFile(Raw, NULL, NULL, NULL, &IoStatus, Saved, LOG_PAIR, &Offset, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = ZwReadFile(Raw, NULL, NULL, NULL, &IoStatus, VolumeRecords,
                        Geometry.BytesPerFileRecordSegment, &VolumeOffset, NULL);
    if (NT_SUCCESS(Status))
        Status = ZwReadFile(Raw, NULL, NULL, NULL, &IoStatus, VolumeRecords + Geometry.BytesPerFileRecordSegment,
                            Geometry.BytesPerFileRecordSegment, &MirrorOffset, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    for (Case = 0; Case < 9; Case++)
    {
        trace("restart fixture %lu\n", Case);
        BuildRestart(Pages, Geometry.BytesPerSector, LogSize, 200, TRUE, 1);
        BuildRestart(Pages + LOG_PAGE, Geometry.BytesPerSector, LogSize, 100, TRUE, 1);
        switch (Case)
        {
            case 1: BuildRestart(Pages, Geometry.BytesPerSector, LogSize, 200, FALSE, 1); break;
            case 2: BuildRestart(Pages + LOG_PAGE, Geometry.BytesPerSector, LogSize, 300, FALSE, 1); break;
            case 3: Pages[Geometry.BytesPerSector - 1] ^= 1; break;
            case 4:
                BuildRestart(Pages, Geometry.BytesPerSector, LogSize, 200, TRUE, 2);
                BuildRestart(Pages + LOG_PAGE, Geometry.BytesPerSector, LogSize, 100, TRUE, 2);
                break;
            case 5: RtlFillMemory(Pages, LOG_PAIR, 'X'); break;
            case 6: RtlFillMemory(Pages, LOG_PAIR, 0xff); break;
            case 7: Put16(Pages + 6, MAXUSHORT); Put16(Pages + LOG_PAGE + 6, MAXUSHORT); break;
            case 8: Put16(Pages + 0x56, 0xfff8); Put16(Pages + LOG_PAGE + 0x56, 0xfff8); break;
        }
        Modified = TRUE;
        Status = WriteLogRequest(Raw, Pages, LOG_PAIR, &Offset);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        /* Each case starts from a volume not marked dirty. */
        Status = WriteLogRequest(Raw, VolumeRecords, Geometry.BytesPerFileRecordSegment, &VolumeOffset);
        if (NT_SUCCESS(Status))
            Status = WriteLogRequest(Raw, VolumeRecords + Geometry.BytesPerFileRecordSegment,
                                     Geometry.BytesPerFileRecordSegment, &MirrorOffset);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        Status = NtfsKmtFlush(Raw);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        ReadOnly = Case == 1 || Case == 2 || Case == 4 || Case == 5 || Case == 7 || Case == 8;
        /* Unrecoverable logs are chkdsk's; another version is not ours. */
        MarkedDirty = ReadOnly && Case != 4;
        if (!CheckLogMount(Letter, Path, ReadOnly, MarkedDirty, &Dismounted, &Offset, Pages)) break;
        Status = ZwReadFile(Raw, NULL, NULL, NULL, &IoStatus, Verify, LOG_PAIR, &Offset, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status) && ReadOnly)
            ok(RtlEqualMemory(Pages, Verify, LOG_PAIR), "a read-only mount changed $LogFile\n");
        else if (NT_SUCCESS(Status))
        {
            ok(RestartPairClean(Verify, Geometry.BytesPerSector),
               "a read-write mount did not leave both restart pages clean\n");
            RtlCopyMemory(Latest, Verify, LOG_PAIR);
            HaveLatest = TRUE;
        }
    }
Cleanup:
    if (Modified && Dismounted)
    {
        /* The newest clean restart the driver wrote: the records carry LSNs
         * up to it. */
        Status = WriteLogRequest(Raw, HaveLatest ? Latest : Saved, LOG_PAIR, &Offset);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = WriteLogRequest(Raw, VolumeRecords, Geometry.BytesPerFileRecordSegment, &VolumeOffset);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = WriteLogRequest(Raw, VolumeRecords + Geometry.BytesPerFileRecordSegment,
                                 Geometry.BytesPerFileRecordSegment, &MirrorOffset);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtFlush(Raw);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Raw) ZwClose(Raw);
    if (Volume) ZwClose(Volume);
    if (File) ZwClose(File);
    if (!Modified || Dismounted)
    {
        Status = NtfsKmtDelete(Path);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    NtfsKmtFree(VolumeRecords);
    NtfsKmtFree(Latest);
    NtfsKmtFree(Verify);
    NtfsKmtFree(Pages);
    NtfsKmtFree(Saved);
}
