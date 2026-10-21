/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (see COPYING.LIB)
 * PURPOSE:     Black-box $LogFile restart recovery: redo and undo at mount
 *
 * NTFS logs metadata before writing it and, at mount, replays the log of a
 * volume that went away dirty.  Only public requests are used, on the
 * throwaway fixture volume:
 *
 *   - the volume is locked and dismounted, and the raw bytes of a file's
 *     MFT record are kept;
 *   - mounted again, the file's data grows (a logged metadata change) and
 *     is flushed; the raw $LogFile is copied while the volume is mounted,
 *     its restart area dirty;
 *   - dismounted, the kept record bytes and the copied log are written
 *     back: the change is in the log, its record write never reached the
 *     disk;
 *   - the next mount must recover: read-write, not dirty, the log clean,
 *     the change there (redo);
 *   - then the copied log is written back once more, ending after the
 *     change and before the record that commits it (that record's LSN is
 *     wiped): the next mount must roll the change back (undo).
 *
 * Every raw transfer goes through a handle that bypasses NTFS, and writes
 * happen only while the volume is dismounted.  The kept bytes, and a clean
 * log, are written back if the volume is left read-only or dirty.
 */
#include <kmt_test.h>
#include "NtfsKmt.h"

#define LOG_PAGE        4096
#define MAX_LOG_BYTES   (4 * 1024 * 1024)
#define DATA_BYTES      300

typedef struct _RUN
{
    ULONGLONG Vcn;
    ULONGLONG Lcn;
    ULONGLONG Count;
} RUN;

static NTSTATUS
RawIo(HANDLE Handle, UCHAR Major, PVOID Buffer, ULONG Length, LONGLONG Offset)
{
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Position;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Device = IoGetRelatedDeviceObject(File);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Position.QuadPart = Offset;
    Irp = IoBuildSynchronousFsdRequest(Major, Device, Buffer, Length, &Position, &Event, &IoStatus);
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

static ULONGLONG Get64(const UCHAR *p) { ULONGLONG v; RtlCopyMemory(&v, p, sizeof(v)); return v; }
static ULONG Get32(const UCHAR *p) { ULONG v; RtlCopyMemory(&v, p, sizeof(v)); return v; }
static USHORT Get16(const UCHAR *p) { USHORT v; RtlCopyMemory(&v, p, sizeof(v)); return v; }
static VOID Put64(PUCHAR p, ULONGLONG v) { RtlCopyMemory(p, &v, sizeof(v)); }

/* The unnamed $DATA runs and data size of a file, from its file record. */
static ULONG
GetDataRuns(HANDLE Volume, PNTFS_VOLUME_DATA_BUFFER Geometry, ULONGLONG FileRef,
            RUN *Runs, ULONG MaxRuns, PULONGLONG DataSize)
{
    NTFS_FILE_RECORD_INPUT_BUFFER Input;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER Output;
    PUCHAR Record, Attr, Run;
    ULONG OutputSize, Position, Length, Count = 0, i, LengthBytes, OffsetBytes;
    ULONGLONG Vcn = 0;
    LONGLONG Lcn = 0, Delta;
    ULONGLONG Clusters;

    OutputSize = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) +
                 Geometry->BytesPerFileRecordSegment;
    Output = NtfsKmtAlloc(OutputSize);
    if (Output == NULL) return 0;
    Input.FileReferenceNumber.QuadPart = FileRef;
    if (!NT_SUCCESS(NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input),
                                 Output, OutputSize, NULL)) ||
        Output->FileRecordLength < 24 || Output->FileRecordLength > Geometry->BytesPerFileRecordSegment)
        goto Cleanup;
    Record = Output->FileRecordBuffer;
    for (Position = Get16(Record + 20); Position + 16 <= Output->FileRecordLength; Position += Length)
    {
        Attr = Record + Position;
        Length = Get32(Attr + 4);
        if (Get32(Attr) == MAXULONG || Length < 16 || Length > Output->FileRecordLength - Position)
            break;
        if (Get32(Attr) != 0x80 || Attr[9] != 0 || !Attr[8] || Length < 64)
            continue;
        *DataSize = Get64(Attr + 48);
        for (Run = Attr + Get16(Attr + 32); Run < Attr + Length && *Run != 0 && Count < MaxRuns;
             Run += 1 + LengthBytes + OffsetBytes)
        {
            LengthBytes = Run[0] & 15;
            OffsetBytes = Run[0] >> 4;
            if (LengthBytes == 0 || LengthBytes > 8 || OffsetBytes > 8 ||
                Run + 1 + LengthBytes + OffsetBytes > Attr + Length)
                break;
            for (i = 0, Clusters = 0; i < LengthBytes; i++)
                Clusters |= (ULONGLONG)Run[1 + i] << (8 * i);
            for (i = 0, Delta = 0; i < OffsetBytes; i++)
                Delta |= (LONGLONG)Run[1 + LengthBytes + i] << (8 * i);
            if (OffsetBytes != 0 && (Run[LengthBytes + OffsetBytes] & 0x80))
                Delta -= (LONGLONG)1 << (8 * OffsetBytes);
            if (OffsetBytes == 0)
                break;  /* sparse: not expected in these files */
            Lcn += Delta;
            Runs[Count].Vcn = Vcn;
            Runs[Count].Lcn = (ULONGLONG)Lcn;
            Runs[Count].Count = Clusters;
            Count++;
            Vcn += Clusters;
        }
        break;
    }
Cleanup:
    NtfsKmtFree(Output);
    return Count;
}

/* Volume byte offset of a byte of a file mapped by Runs, or -1. */
static LONGLONG
MapByte(const RUN *Runs, ULONG Count, ULONG BytesPerCluster, ULONGLONG Byte)
{
    ULONGLONG Vcn = Byte / BytesPerCluster;
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        if (Vcn >= Runs[i].Vcn && Vcn < Runs[i].Vcn + Runs[i].Count)
            return (LONGLONG)((Runs[i].Lcn + Vcn - Runs[i].Vcn) * BytesPerCluster + Byte % BytesPerCluster);
    }
    return -1;
}

static BOOLEAN
LockAndDismount(WCHAR Letter)
{
    HANDLE Volume;
    NTSTATUS Status;
    LARGE_INTEGER Delay;
    ULONG Try;
    BOOLEAN Done = FALSE;

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = NtfsKmtFlush(Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
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
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Done = NT_SUCCESS(Status);
    }
    ZwClose(Volume);
    return Done;
}

/* Undo/redo the update sequence array of a multi-sector page in place. */
static BOOLEAN
Unfix(PUCHAR Page, ULONG Size, ULONG Sector)
{
    USHORT UsaOffset = Get16(Page + 4), UsaCount = Get16(Page + 6), i;

    if (UsaCount != Size / Sector + 1 || UsaOffset + 2u * UsaCount > Sector)
        return FALSE;
    for (i = 1; i < UsaCount; i++)
    {
        if (RtlCompareMemory(Page + i * Sector - 2, Page + UsaOffset, 2) != 2)
            return FALSE;
        RtlCopyMemory(Page + i * Sector - 2, Page + UsaOffset + 2 * i, 2);
    }
    return TRUE;
}

static VOID
Refix(PUCHAR Page, ULONG Sector)
{
    USHORT UsaOffset = Get16(Page + 4), UsaCount = Get16(Page + 6), i;

    for (i = 1; i < UsaCount; i++)
    {
        RtlCopyMemory(Page + UsaOffset + 2 * i, Page + i * Sector - 2, 2);
        RtlCopyMemory(Page + i * Sector - 2, Page + UsaOffset, 2);
    }
}

typedef struct _LOG_VIEW
{
    PUCHAR Log;             /* raw copy of the first LogLength bytes */
    ULONG LogLength;
    ULONG LogSize;
    ULONG Sector;
    ULONG SeqBits;
    ULONGLONG CurrentLsn;   /* newest restart copy */
    ULONGLONG OldestLsn;    /* of its client: where recovery would start */
    ULONG CurrentCopy;
    BOOLEAN Clean;
    UCHAR Page[LOG_PAGE];
    UCHAR Record[3 * LOG_PAGE];
} LOG_VIEW, *PLOG_VIEW;

/* The newest valid restart copy (ntfs-3g logfile.h RESTART_AREA). */
static BOOLEAN
ReadRestart(PLOG_VIEW View, const UCHAR *Log)
{
    ULONG Copy;
    BOOLEAN Found = FALSE;

    for (Copy = 0; Copy < 2; Copy++)
    {
        PUCHAR Area;
        RtlCopyMemory(View->Page, Log + Copy * LOG_PAGE, LOG_PAGE);
        if (RtlCompareMemory(View->Page, "RSTR", 4) != 4 || !Unfix(View->Page, LOG_PAGE, View->Sector))
        {
            trace("restart copy %lu: magic %02x%02x%02x%02x usa %x/%x tail %x usn %x\n", Copy,
                  View->Page[0], View->Page[1], View->Page[2], View->Page[3], Get16(View->Page + 4),
                  Get16(View->Page + 6), Get16(View->Page + View->Sector - 2), Get16(View->Page + Get16(View->Page + 4) % LOG_PAGE));
            continue;
        }
        Area = View->Page + Get16(View->Page + 0x18);
        if (!Found || Get64(Area) > View->CurrentLsn)
        {
            View->CurrentLsn = Get64(Area);
            View->Clean = (Get16(Area + 0xE) & 2) != 0;
            /* LOG_CLIENT_RECORD of the in-use client (ClientArrayOffset). */
            View->OldestLsn = Get64(Area + Get16(Area + 0x16) + Get16(Area + 0xC) * 0xA0);
            View->SeqBits = Get32(Area + 0x10);
            View->CurrentCopy = Copy;
            Found = TRUE;
        }
    }
    return Found;
}

/* The log record at Lsn, reassembled across pages into View->Record. */
static BOOLEAN
ReadRecord(PLOG_VIEW View, ULONGLONG Lsn)
{
    ULONG Offset = (ULONG)((Lsn & (((ULONGLONG)1 << (64 - View->SeqBits)) - 1)) << 3);
    ULONG Page = Offset - Offset % LOG_PAGE, Within = Offset % LOG_PAGE, Length = 0x30, Done = 0, Part;

    if (View->SeqBits == 0 || View->SeqBits >= 64 || Offset >= View->LogLength || Within < 0x40)
        return FALSE;
    for (;;)
    {
        RtlCopyMemory(View->Page, View->Log + Page, LOG_PAGE);
        if (RtlCompareMemory(View->Page, "RCRD", 4) != 4 || !Unfix(View->Page, LOG_PAGE, View->Sector))
            return FALSE;
        if (Done == 0)
        {
            if (LOG_PAGE - Within < 0x30)
                return FALSE;
            Length = 0x30 + Get32(View->Page + Within + 0x18);
            if (Length > sizeof(View->Record))
                return FALSE;
        }
        Part = min(Length - Done, LOG_PAGE - Within);
        RtlCopyMemory(View->Record + Done, View->Page + Within, Part);
        Done += Part;
        if (Done == Length)
            break;
        Page += LOG_PAGE;
        if (Page >= View->LogLength)
            return FALSE;
        Within = 0x40;
    }
    return Get64(View->Record) == Lsn;
}

static BOOLEAN
HasMarker(const UCHAR *Image, ULONG Length, const UCHAR *Marker, ULONG MarkerLength)
{
    ULONG i;

    for (i = 0; i + MarkerLength <= Length; i++)
        if (RtlCompareMemory(Image + i, Marker, MarkerLength) == MarkerLength)
            return TRUE;
    return FALSE;
}

/* Walks the record pages of the copied log (each batch starts on a page,
 * records follow each other) and finds the record that put the new data in
 * the file's record: an InitializeFileRecordSegment (2) whose redo image
 * holds it and whose undo image does not.  *CommitLsn receives the record
 * committing its transaction (ForgetTransaction, 0x1B). */
static ULONGLONG
FindChange(PLOG_VIEW View, ULONGLONG MftIndex, PULONGLONG CommitLsn)
{
    static const UCHAR Marker[16] = "RRRRRRRRRRRRRRRR";
    ULONGLONG Change = 0, Lsn;
    ULONG Transaction = 0, Page, At, Length;
    UCHAR *Header = View->Page;

    *CommitLsn = 0;
    for (Page = 4 * LOG_PAGE; Page + LOG_PAGE <= View->LogLength && *CommitLsn == 0; Page += LOG_PAGE)
    {
        RtlCopyMemory(View->Page, View->Log + Page, LOG_PAGE);
        if (RtlCompareMemory(View->Page, "RCRD", 4) != 4 || !Unfix(View->Page, LOG_PAGE, View->Sector))
            continue;
        for (At = 0x40; At + 0x50 <= LOG_PAGE; At += Length)
        {
            Lsn = Get64(Header + At);
            Length = (0x30 + Get32(Header + At + 0x18) + 7) & ~7;
            if (Lsn == 0 || (ULONG)((Lsn & (((ULONGLONG)1 << (64 - View->SeqBits)) - 1)) << 3) != Page + At)
                break;
            if (Change != 0)
            {
                if (Get16(Header + At + 0x30) == 0x1B && Get32(Header + At + 0x24) == Transaction)
                {
                    *CommitLsn = Lsn;
                    break;
                }
            }
            else if (Get16(Header + At + 0x30) == 2 && ReadRecord(View, Lsn))
            {
                PUCHAR Rec = View->Record;
                PUCHAR Redo = Rec + 0x30 + Get16(Rec + 0x34), Undo = Rec + 0x30 + Get16(Rec + 0x38);
                ULONG RedoLength = Get16(Rec + 0x36), UndoLength = Get16(Rec + 0x3A);

                /* MFTRecordNumber (XP header) of the image names the record. */
                if (RedoLength >= 0x30 && Get32(Redo + 0x2C) == (ULONG)MftIndex &&
                    HasMarker(Redo, RedoLength, Marker, sizeof(Marker)) &&
                    !HasMarker(Undo, UndoLength, Marker, sizeof(Marker)))
                {
                    Change = Lsn;
                    Transaction = Get32(Rec + 0x24);
                }
                /* ReadRecord used the page buffer. */
                RtlCopyMemory(View->Page, View->Log + Page, LOG_PAGE);
                Unfix(View->Page, LOG_PAGE, View->Sector);
            }
            if (Length > LOG_PAGE - At)
                break;
        }
    }
    return Change;
}

/* The commit never reached the disk: the log ends before it (its LSN field
 * is gone), and both restart copies name nothing past the change. */
static VOID
CutLog(PLOG_VIEW View, ULONGLONG Change, ULONGLONG Commit)
{
    ULONG Offset = (ULONG)((Commit & (((ULONGLONG)1 << (64 - View->SeqBits)) - 1)) << 3);
    PUCHAR Page = View->Log + Offset - Offset % LOG_PAGE;
    ULONG Copy;

    if (Unfix(Page, LOG_PAGE, View->Sector))
    {
        Put64(Page + Offset % LOG_PAGE, 0);
        Refix(Page, View->Sector);
    }
    for (Copy = 0; Copy < 2; Copy++)
    {
        Page = View->Log + Copy * LOG_PAGE;
        if (RtlCompareMemory(Page, "RSTR", 4) != 4 || !Unfix(Page, LOG_PAGE, View->Sector))
            continue;
        if (Get64(Page + Get16(Page + 0x18)) > Change)
            Put64(Page + Get16(Page + 0x18), Change);
        Refix(Page, View->Sector);
    }
}

/* The mount after a raw rewrite: recovered, writable, not dirty, the log
 * clean again. */
static VOID
CheckRecoveredMount(WCHAR Letter, HANDLE Raw, PLOG_VIEW View, LONGLONG LogOffset, ULONGLONG RecoveredLsn,
                    PCSTR Label)
{
    HANDLE Volume;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    UCHAR Attributes[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32];
    PFILE_FS_ATTRIBUTE_INFORMATION Info = (PVOID)Attributes;
    PUCHAR Pages;
    LOG_VIEW *Check;
    ULONG Dirty = 0;

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, Attributes, sizeof(Attributes),
                                          FileFsAttributeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(!(Info->FileSystemAttributes & FILE_READ_ONLY_VOLUME),
           "%s: the volume was mounted read-only: its log was not recovered\n", Label);
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &Dirty, sizeof(Dirty), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(!(Dirty & VOLUME_IS_DIRTY), "%s: recovery left the volume dirty\n", Label);
    /* Nothing has changed since the mount: the restart area recovery wrote
     * is the newest, and it is clean. */
    Check = NtfsKmtAlloc(sizeof(*Check));
    Pages = NtfsKmtAlloc(2 * LOG_PAGE);
    if (Check != NULL && Pages != NULL)
    {
        Status = RawIo(Raw, IRP_MJ_READ, Pages, 2 * LOG_PAGE, LogOffset);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Check->Sector = View->Sector;
        /* Recovered: no record of the log it found is needed any more (the
         * restart is clean, or names only records logged since). */
        ok(NT_SUCCESS(Status) && ReadRestart(Check, Pages) &&
           (Check->Clean || Check->OldestLsn > RecoveredLsn) && Check->CurrentLsn >= RecoveredLsn,
           "%s: the recovered records are still needed (restart %I64x clean %u oldest %I64x, recovered up to %I64x)\n",
           Label, Check->CurrentLsn, Check->Clean, Check->OldestLsn, RecoveredLsn);
    }
    NtfsKmtFree(Pages);
    NtfsKmtFree(Check);
    ZwClose(Volume);
}

START_TEST(NtfsLogReplay)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], RawPath[16], Letter;
    HANDLE Volume = NULL, Raw = NULL, File = NULL;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Name;
    NTFS_VOLUME_DATA_BUFFER Geometry;
    RUN MftRuns[64], LogRuns[8];
    ULONG MftRunCount, LogRunCount, RecordSize = 0;
    ULONGLONG MftIndex, MftSize = 0, LogSize = 0, ChangeLsn, CommitLsn = 0;
    LONGLONG RecordOffset, LogOffset = 0, VolumeOffset = -1, MirrorOffset;
    PUCHAR PreRecord = NULL, PostRecord = NULL, VolumeRecord = NULL, MirrorRecord = NULL;
    PUCHAR CleanLog = NULL, Data = NULL;
    PLOG_VIEW View = NULL;
    ULONG_PTR Done;
    ULONG i;
    BOOLEAN Modified = FALSE, Restore = FALSE;

    if (!NtfsKmtFindVolume(Root, &Letter)) { ok(FALSE, "no NTFS fixture volume\n"); return; }
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"kmt_logreplay.dat");
    (VOID)NtfsKmtDelete(Path);
    Status = NtfsKmtCreateFileWithData(Path, "A", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtOpen(&File, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    MftIndex = NtfsKmtQueryFileId(File) & 0x0000ffffffffffffULL;
    ZwClose(File); File = NULL;
    ok(MftIndex >= 16, "unexpected file record %I64u\n", MftIndex);

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Geometry, sizeof(Geometry), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    RecordSize = Geometry.BytesPerFileRecordSegment;
    MftRunCount = GetDataRuns(Volume, &Geometry, 0, MftRuns, RTL_NUMBER_OF(MftRuns), &MftSize);
    LogRunCount = GetDataRuns(Volume, &Geometry, 2, LogRuns, RTL_NUMBER_OF(LogRuns), &LogSize);
    RecordOffset = MapByte(MftRuns, MftRunCount, Geometry.BytesPerCluster, MftIndex * RecordSize);
    VolumeOffset = MapByte(MftRuns, MftRunCount, Geometry.BytesPerCluster, 3ULL * RecordSize);
    MirrorOffset = Geometry.Mft2StartLcn.QuadPart * Geometry.BytesPerCluster + 3LL * RecordSize;
    ok(MftRunCount != 0 && RecordOffset > 0 && VolumeOffset > 0, "cannot locate the file record\n");
    ok(LogRunCount != 0 && LogSize >= 6 * LOG_PAGE, "cannot locate $LogFile\n");
    if (MftRunCount == 0 || RecordOffset <= 0 || VolumeOffset <= 0 || LogRunCount == 0 ||
        LogSize < 6 * LOG_PAGE || RecordSize == 0 || RecordSize > LOG_PAGE ||
        Geometry.BytesPerSector == 0 || Geometry.BytesPerSector > LOG_PAGE) goto Cleanup;
    LogOffset = (LONGLONG)LogRuns[0].Lcn * Geometry.BytesPerCluster;

    View = NtfsKmtAlloc(sizeof(*View));
    PreRecord = NtfsKmtAlloc(RecordSize);
    PostRecord = NtfsKmtAlloc(RecordSize);
    VolumeRecord = NtfsKmtAlloc(RecordSize);
    MirrorRecord = NtfsKmtAlloc(RecordSize);
    Data = NtfsKmtAlloc(DATA_BYTES);
    if (View == NULL || PreRecord == NULL || PostRecord == NULL || VolumeRecord == NULL ||
        MirrorRecord == NULL || Data == NULL) goto Cleanup;
    View->Sector = Geometry.BytesPerSector;
    View->LogSize = (ULONG)min(LogSize, (ULONGLONG)MAXULONG);
    View->LogLength = (ULONG)min(LogSize, (ULONGLONG)MAX_LOG_BYTES);
    ok(LogRuns[0].Count * Geometry.BytesPerCluster >= View->LogLength, "$LogFile fixture is fragmented\n");
    if (LogRuns[0].Count * Geometry.BytesPerCluster < View->LogLength) goto Cleanup;
    View->Log = NtfsKmtAlloc(View->LogLength);
    CleanLog = NtfsKmtAlloc(View->LogLength);
    if (View->Log == NULL || CleanLog == NULL) goto Cleanup;

    RtlStringCchPrintfW(RawPath, RTL_NUMBER_OF(RawPath), L"\\??\\%c:", Letter);
    RtlInitUnicodeString(&Name, RawPath);
    /* An attribute-only open of the device bypasses NTFS. */
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&Raw, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &Attributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ZwClose(Volume); Volume = NULL;

    /* 1. The file record and $Volume's, as a clean dismount leaves them. */
    if (!LockAndDismount(Letter)) goto Cleanup;
    Status = RawIo(Raw, IRP_MJ_READ, PreRecord, RecordSize, RecordOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_READ, VolumeRecord, RecordSize, VolumeOffset);
    if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_READ, MirrorRecord, RecordSize, MirrorOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;

    /* 2. A logged metadata change, flushed; the log as it is now. */
    Status = NtfsKmtOpen(&File, Path, FILE_WRITE_DATA, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    RtlFillMemory(Data, DATA_BYTES, 'R');
    Status = NtfsKmtWrite(File, 0, Data, DATA_BYTES, &Done);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFlush(File);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File); File = NULL;
    Status = RawIo(Raw, IRP_MJ_READ, View->Log, View->LogLength, LogOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ok(ReadRestart(View, View->Log) && !View->Clean,
       "the restart area is clean after a metadata change: nothing was logged\n");

    /* 3. Dismounted; the record write never reached the disk, the log is
     * as it was. */
    if (!LockAndDismount(Letter)) goto Cleanup;
    Status = RawIo(Raw, IRP_MJ_READ, PostRecord, RecordSize, RecordOffset);
    if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_READ, CleanLog, View->LogLength, LogOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Modified = TRUE;
    Restore = TRUE;
    Status = RawIo(Raw, IRP_MJ_WRITE, PreRecord, RecordSize, RecordOffset);
    if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_WRITE, View->Log, View->LogLength, LogOffset);
    if (NT_SUCCESS(Status)) Status = NtfsKmtFlush(Raw);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;

    /* 4. Redo. */
    CheckRecoveredMount(Letter, Raw, View, LogOffset, View->CurrentLsn, "redo");
    Status = NtfsKmtOpen(&File, Path, FILE_READ_DATA, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Data, DATA_BYTES);
        Status = NtfsKmtRead(File, 0, Data, DATA_BYTES, &Done);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Done, DATA_BYTES);
        for (i = 0; i < DATA_BYTES && Data[i] == 'R'; i++);
        ok(i == DATA_BYTES, "redo: the logged change is not there (byte %lu is %x)\n", i,
           i < DATA_BYTES ? Data[i] : 0);
        ZwClose(File); File = NULL;
    }

    /* 5. Undo: the same log cut off after the change, before the request
     * that made it committed. */
    ChangeLsn = FindChange(View, MftIndex, &CommitLsn);
    ok(ChangeLsn != 0, "no logged record carries the change\n");
    ok(CommitLsn != 0, "no logged record commits the change\n");
    if (ChangeLsn != 0 && CommitLsn != 0 && LockAndDismount(Letter))
    {
        CutLog(View, ChangeLsn, CommitLsn);
        Status = RawIo(Raw, IRP_MJ_WRITE, PreRecord, RecordSize, RecordOffset);
        if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_WRITE, View->Log, View->LogLength, LogOffset);
        if (NT_SUCCESS(Status)) Status = NtfsKmtFlush(Raw);
        ok_eq_hex(Status, STATUS_SUCCESS);
        CheckRecoveredMount(Letter, Raw, View, LogOffset, ChangeLsn, "undo");
        Status = NtfsKmtOpen(&File, Path, FILE_READ_DATA, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            RtlZeroMemory(Data, DATA_BYTES);
            Status = NtfsKmtRead(File, 0, Data, DATA_BYTES, &Done);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(Data[0] == 'A', "undo: the change was not rolled back (first byte %x)\n", Data[0]);
            for (i = 1; i < Done && Data[i] != 'R'; i++);
            ok(i >= Done, "undo: byte %lu still holds the uncommitted change\n", i);
            ZwClose(File); File = NULL;
        }
    }

    /* The volume is writable and clean again: nothing to put back. */
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA, &Volume);
    if (NT_SUCCESS(Status))
    {
        UCHAR Buffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32];
        PFILE_FS_ATTRIBUTE_INFORMATION Info = (PVOID)Buffer;
        ULONG Dirty = 0;
        if (NT_SUCCESS(ZwQueryVolumeInformationFile(Volume, &IoStatus, Buffer, sizeof(Buffer),
                                                    FileFsAttributeInformation)) &&
            !(Info->FileSystemAttributes & FILE_READ_ONLY_VOLUME) &&
            NT_SUCCESS(NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &Dirty, sizeof(Dirty), NULL)) &&
            !(Dirty & VOLUME_IS_DIRTY))
            Restore = FALSE;
        ZwClose(Volume); Volume = NULL;
    }

Cleanup:
    if (File) ZwClose(File);
    if (Volume) { ZwClose(Volume); Volume = NULL; }
    if (Restore && Raw != NULL && LockAndDismount(Letter))
    {
        /* Left read-only or dirty: the state of a clean dismount back. */
        trace("putting the fixture's clean state back\n");
        Status = RawIo(Raw, IRP_MJ_WRITE, PostRecord, RecordSize, RecordOffset);
        if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_WRITE, CleanLog, View->LogLength, LogOffset);
        if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_WRITE, VolumeRecord, RecordSize, VolumeOffset);
        if (NT_SUCCESS(Status)) Status = RawIo(Raw, IRP_MJ_WRITE, MirrorRecord, RecordSize, MirrorOffset);
        if (NT_SUCCESS(Status)) Status = NtfsKmtFlush(Raw);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Raw) ZwClose(Raw);
    UNREFERENCED_PARAMETER(Modified);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (View != NULL) NtfsKmtFree(View->Log);
    NtfsKmtFree(CleanLog);
    NtfsKmtFree(View);
    NtfsKmtFree(Data);
    NtfsKmtFree(MirrorRecord);
    NtfsKmtFree(VolumeRecord);
    NtfsKmtFree(PostRecord);
    NtfsKmtFree(PreRecord);
}
