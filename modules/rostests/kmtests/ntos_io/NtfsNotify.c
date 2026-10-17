/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS directory change records, cancellation and handle lifetime
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define check_status(Expression, Expected) \
    do { \
        NTSTATUS ActualStatus = (Expression); \
        ok(ActualStatus == (Expected), "%s returned %lx, expected %lx\n", \
           #Expression, ActualStatus, (NTSTATUS)(Expected)); \
    } while (0)

typedef struct _NOTIFY_REQUEST
{
    PIRP Irp;
    PFILE_OBJECT File;
    KEVENT Event;
    NTSTATUS Status;
    ULONG_PTR Information;
    UCHAR Buffer[1024];
} NOTIFY_REQUEST, *PNOTIFY_REQUEST;

static NTSTATUS NTAPI
NotifyCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    PNOTIFY_REQUEST Request = Context;

    UNREFERENCED_PARAMETER(Device);
    Request->Status = Irp->IoStatus.Status;
    Request->Information = Irp->IoStatus.Information;
    KeSetEvent(&Request->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
BeginNotify(PFILE_OBJECT File, ULONG Filter, BOOLEAN Tree, ULONG Length,
            BOOLEAN Canceled, PNOTIFY_REQUEST *Result)
{
    PNOTIFY_REQUEST Request;
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(File);
    PIO_STACK_LOCATION Stack;

    *Result = NULL;
    ASSERT(Length <= sizeof(Request->Buffer));
    Request = NtfsKmtAlloc(sizeof(*Request));
    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Request, sizeof(*Request));
    Request->File = File;
    ObReferenceObject(File);
    Request->Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (Request->Irp == NULL)
    {
        ObDereferenceObject(File);
        NtfsKmtFree(Request);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (Length != 0)
    {
        if (IoAllocateMdl(Request->Buffer, Length, FALSE, FALSE, Request->Irp) == NULL)
        {
            IoFreeIrp(Request->Irp);
            ObDereferenceObject(File);
            NtfsKmtFree(Request);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(Request->Irp->MdlAddress);
    }
    KeInitializeEvent(&Request->Event, NotificationEvent, FALSE);
    Request->Status = STATUS_PENDING;
    Request->Irp->RequestorMode = KernelMode;
    Request->Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Request->Irp->UserBuffer = Request->Buffer;
    Request->Irp->Cancel = Canceled;
    Stack = IoGetNextIrpStackLocation(Request->Irp);
    Stack->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
    Stack->MinorFunction = IRP_MN_NOTIFY_CHANGE_DIRECTORY;
    Stack->Flags = Tree ? SL_WATCH_TREE : 0;
    Stack->FileObject = File;
    Stack->Parameters.NotifyDirectory.Length = Length;
    Stack->Parameters.NotifyDirectory.CompletionFilter = Filter;
    IoSetCompletionRoutine(Request->Irp, NotifyCompletion, Request, TRUE, TRUE, TRUE);
    *Result = Request;
    return IoCallDriver(Device, Request->Irp);
}

static NTSTATUS
StartNotify(HANDLE Handle, ULONG Filter, BOOLEAN Tree, ULONG Length,
            BOOLEAN Canceled, PNOTIFY_REQUEST *Result)
{
    PFILE_OBJECT File;
    NTSTATUS Status;

    *Result = NULL;
    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = BeginNotify(File, Filter, Tree, Length, Canceled, Result);
    ObDereferenceObject(File);
    return Status;
}

static BOOLEAN
WaitNotify(PNOTIFY_REQUEST Request, NTSTATUS Expected)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (Request == NULL)
    {
        ok(FALSE, "notification allocation failed\n");
        return FALSE;
    }
    Timeout.QuadPart = -3 * 10000000LL;
    Status = KeWaitForSingleObject(&Request->Event, Executive, KernelMode, FALSE, &Timeout);
    check_status(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
    {
        IoCancelIrp(Request->Irp);
        return FALSE;
    }
    check_status(Request->Status, Expected);
    return Request->Status == Expected;
}

static VOID
FreeNotify(PNOTIFY_REQUEST Request)
{
    if (Request == NULL)
        return;
    if (!KeReadStateEvent(&Request->Event) && !WaitNotify(Request, STATUS_CANCELLED))
        return; /* Keep the request and its file alive if completion is late. */
    if (Request->Irp->MdlAddress != NULL)
        IoFreeMdl(Request->Irp->MdlAddress);
    IoFreeIrp(Request->Irp);
    ObDereferenceObject(Request->File);
    NtfsKmtFree(Request);
}

static PFILE_NOTIFY_INFORMATION
CheckRecord(PNOTIFY_REQUEST Request, ULONG Offset, ULONG Action, PCWSTR Expected)
{
    PFILE_NOTIFY_INFORMATION Info;
    ULONG NameLength = (ULONG)wcslen(Expected) * sizeof(WCHAR);

    ok(Request->Information <= sizeof(Request->Buffer), "oversized notification: %Iu\n", Request->Information);
    if (Request->Information > sizeof(Request->Buffer) || Offset > Request->Information ||
        Request->Information - Offset < FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName))
    {
        ok(FALSE, "missing notification record at %lu (%Iu bytes)\n", Offset, Request->Information);
        return NULL;
    }
    Info = (PFILE_NOTIFY_INFORMATION)(Request->Buffer + Offset);
    ok_eq_ulong(Info->Action, Action);
    ok_eq_ulong(Info->FileNameLength, NameLength);
    ok(Info->FileNameLength <= Request->Information - Offset - FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName),
       "record name extends past completion length\n");
    if (Info->FileNameLength == NameLength &&
        NameLength <= Request->Information - Offset - FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName))
        ok(RtlEqualMemory(Info->FileName, Expected, NameLength), "wrong name, expected %ls\n", Expected);
    return Info;
}

static VOID
CheckSingle(PNOTIFY_REQUEST Request, ULONG Action, PCWSTR Name)
{
    PFILE_NOTIFY_INFORMATION Info;

    if (WaitNotify(Request, STATUS_SUCCESS))
    {
        Info = CheckRecord(Request, 0, Action, Name);
        if (Info != NULL)
            ok_eq_ulong(Info->NextEntryOffset, 0);
    }
    FreeNotify(Request);
}

static HANDLE
OpenDirectory(PCWSTR Path, BOOLEAN DeleteAccess)
{
    HANDLE Handle = NULL;

    check_status(NtfsKmtOpen(&Handle, Path, FILE_LIST_DIRECTORY | (DeleteAccess ? DELETE : 0), FILE_DIRECTORY_FILE), STATUS_SUCCESS);
    return Handle;
}

static VOID
SetDelete(HANDLE Handle, BOOLEAN Delete)
{
    FILE_DISPOSITION_INFORMATION Info;
    IO_STATUS_BLOCK IoStatus;

    Info.DeleteFile = Delete;
    check_status(ZwSetInformationFile(Handle, &IoStatus, &Info, sizeof(Info), FileDispositionInformation), STATUS_SUCCESS);
}

static VOID
TestReplacementNotify(PCWSTR Dir, BOOLEAN ChangeCase)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH], NewPath[MAX_PATH];
    HANDLE File = NULL, Watch = NULL, Attributes = NULL;
    PNOTIFY_REQUEST Request = NULL, AttributeRequest = NULL;
    PFILE_NOTIFY_INFORMATION Info;
    NTSTATUS Status;
    ULONG i, Offset = 0;
    const ULONG Actions[] = {FILE_ACTION_REMOVED, FILE_ACTION_RENAMED_OLD_NAME, FILE_ACTION_RENAMED_NEW_NAME};
    const PCWSTR Names[] = {L"MiXeD.bin", L"notify-src.bin", L"mixed.bin"};

    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Dir, L"notify-src.bin");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, ChangeCase ? L"MiXeD.bin" : L"notify-dst.bin");
    NtfsKmtJoin(NewPath, RTL_NUMBER_OF(NewPath), Dir, ChangeCase ? L"mixed.bin" : L"notify-dst.bin");
    check_status(NtfsKmtCreateFileWithData(Source, "data", 4), STATUS_SUCCESS);
    check_status(NtfsKmtCreateFileWithData(Target, "old!", 4), STATUS_SUCCESS);
    check_status(NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE), STATUS_SUCCESS);
    Watch = OpenDirectory(Dir, FALSE);
    if (File == NULL || Watch == NULL) goto CleanupReplace;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    if (!ChangeCase)
    {
        Attributes = OpenDirectory(Dir, FALSE);
        if (Attributes == NULL) goto CleanupReplace;
        check_status(StartNotify(Attributes, FILE_NOTIFY_CHANGE_ATTRIBUTES, FALSE, 1024, FALSE,
                                 &AttributeRequest), STATUS_PENDING);
    }
    check_status(NtfsKmtRename(File, NewPath, TRUE), STATUS_SUCCESS);
    if (!ChangeCase)
    {
        /* An exact-case replacement removes the source name and modifies
         * the destination. It is not an old/new rename-name pair. */
        CheckSingle(Request, FILE_ACTION_REMOVED, L"notify-src.bin"); Request = NULL;
        CheckSingle(AttributeRequest, FILE_ACTION_MODIFIED, L"notify-dst.bin"); AttributeRequest = NULL;
    }
    else
    {
        /* Preserve ordering across completions: FsRtl can complete the first
         * removal separately and buffer the following old/new name pair. */
        for (i = 0; i < RTL_NUMBER_OF(Actions); i++)
        {
            if (Request == NULL)
            {
                Status = StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request);
                ok(Status == STATUS_PENDING || Status == STATUS_SUCCESS, "notify status %lx\n", Status);
                Offset = 0;
            }
            if (!WaitNotify(Request, STATUS_SUCCESS)) break;
            Info = CheckRecord(Request, Offset, Actions[i], Names[i]);
            if (Info == NULL) break;
            if (Info->NextEntryOffset != 0)
            {
                ok(i + 1 < RTL_NUMBER_OF(Actions), "unexpected extra replacement notification\n");
                Offset += Info->NextEntryOffset;
            }
            else
            {
                FreeNotify(Request); Request = NULL;
            }
        }
    }
CleanupReplace:
    if (Request != NULL) { IoCancelIrp(Request->Irp); FreeNotify(Request); }
    if (AttributeRequest != NULL) { IoCancelIrp(AttributeRequest->Irp); FreeNotify(AttributeRequest); }
    if (Attributes != NULL) ZwClose(Attributes);
    if (Watch != NULL) ZwClose(Watch);
    if (File != NULL) ZwClose(File);
}

START_TEST(NtfsNotify)
{
    WCHAR Root[MAX_PATH], Dir[MAX_PATH], Path[MAX_PATH], Target[MAX_PATH];
    WCHAR Subdir[MAX_PATH], Child[MAX_PATH], Moved[MAX_PATH];
    HANDLE Watch = NULL, Other = NULL, File = NULL;
    PNOTIFY_REQUEST Request = NULL, Second = NULL;
    PFILE_NOTIFY_INFORMATION Info;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL)) return;
    Status = NtfsKmtPrepareDirectory(Root, L"KMT_NTFS_NOTIFY", Dir);
    check_status(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    TestReplacementNotify(Dir, FALSE);
    TestReplacementNotify(Dir, TRUE);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"alpha.txt");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, L"beta.txt");
    NtfsKmtJoin(Subdir, RTL_NUMBER_OF(Subdir), Dir, L"child");
    NtfsKmtJoin(Child, RTL_NUMBER_OF(Child), Subdir, L"one.txt");
    NtfsKmtJoin(Moved, RTL_NUMBER_OF(Moved), Dir, L"moved");
    Watch = OpenDirectory(Dir, TRUE);
    if (Watch == NULL) goto Cleanup;

    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    ok(Request != NULL && KeReadStateEvent(&Request->Event) == 0, "notify did not remain pending\n");
    check_status(NtfsKmtCreateFileWithData(Path, "a", 1), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_ADDED, L"alpha.txt");
    Request = NULL;
    check_status(NtfsKmtOpen(&File, Path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_NON_DIRECTORY_FILE), STATUS_SUCCESS);
    if (File == NULL) goto Cleanup;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    /* MS-FSA Appendix A <185>: the I/O manager's target-parent open
     * conflicts with a parent handle granted DELETE, even with share-delete. */
    check_status(NtfsKmtRename(File, Target, FALSE), STATUS_SHARING_VIOLATION);
    if (Request != NULL)
    {
        ok_eq_long(KeReadStateEvent(&Request->Event), 0);
        IoCancelIrp(Request->Irp);
        WaitNotify(Request, STATUS_CANCELLED);
    }
    FreeNotify(Request); Request = NULL;
    ZwClose(Watch); Watch = OpenDirectory(Dir, FALSE);
    if (Watch == NULL) goto Cleanup;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtRename(File, Target, FALSE), STATUS_SUCCESS);
    if (WaitNotify(Request, STATUS_SUCCESS))
    {
        Info = CheckRecord(Request, 0, FILE_ACTION_RENAMED_OLD_NAME, L"alpha.txt");
        if (Info != NULL)
        {
            ok(Info->NextEntryOffset != 0 && (Info->NextEntryOffset % sizeof(ULONG)) == 0,
               "missing or unaligned second rename record\n");
            if (Info->NextEntryOffset != 0)
            {
                Info = CheckRecord(Request, Info->NextEntryOffset, FILE_ACTION_RENAMED_NEW_NAME, L"beta.txt");
                if (Info != NULL) ok_eq_ulong(Info->NextEntryOffset, 0);
            }
        }
    }
    FreeNotify(Request); Request = NULL;
    ZwClose(File); File = NULL;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtDelete(Target), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_REMOVED, L"beta.txt"); Request = NULL;

    /* Directory changes do not satisfy a file-name-only watch. */
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtCreateDirectory(Subdir), STATUS_SUCCESS);
    if (Request != NULL) ok_eq_long(KeReadStateEvent(&Request->Event), 0);
    check_status(NtfsKmtCreateFileWithData(Child, "b", 1), STATUS_SUCCESS);
    if (Request != NULL)
    {
        ok_eq_long(KeReadStateEvent(&Request->Event), 0);
        IoCancelIrp(Request->Irp);
        WaitNotify(Request, STATUS_CANCELLED);
    }
    FreeNotify(Request); Request = NULL;
    ZwClose(Watch); Watch = OpenDirectory(Dir, FALSE);
    if (Watch == NULL) goto Cleanup;

    /* Recursive records are relative to the watched directory. */
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, TRUE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtDelete(Child), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_REMOVED, L"child\\one.txt"); Request = NULL;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, TRUE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtCreateFileWithData(Child, "c", 1), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_ADDED, L"child\\one.txt"); Request = NULL;
    ZwClose(Watch); Watch = OpenDirectory(Subdir, TRUE);
    if (Watch == NULL) goto Cleanup;

    /* A directory watch follows that directory when its own path is renamed. */
    check_status(NtfsKmtDelete(Child), STATUS_SUCCESS);
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtRename(Watch, Moved, FALSE), STATUS_SUCCESS);
    NtfsKmtJoin(Child, RTL_NUMBER_OF(Child), Moved, L"two.txt");
    check_status(NtfsKmtCreateFileWithData(Child, "d", 1), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_ADDED, L"two.txt"); Request = NULL;
    ZwClose(Watch); Watch = NULL;
    check_status(NtfsKmtDelete(Child), STATUS_SUCCESS);
    check_status(NtfsKmtDelete(Moved), STATUS_SUCCESS);

    /* Cancellation is valid before dispatch and from DISPATCH_LEVEL. */
    Watch = OpenDirectory(Dir, FALSE);
    if (Watch == NULL) goto Cleanup;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, TRUE, &Request), STATUS_PENDING);
    WaitNotify(Request, STATUS_CANCELLED); FreeNotify(Request); Request = NULL;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    if (Request != NULL)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        IoCancelIrp(Request->Irp);
        KeLowerIrql(OldIrql);
        WaitNotify(Request, STATUS_CANCELLED);
    }
    FreeNotify(Request); Request = NULL;

    /* Canceling one request on an open does not lose a record destined for
     * another request on the same open, including a cancel/report race. */
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Second), STATUS_PENDING);
    if (Request != NULL)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        IoCancelIrp(Request->Irp);
        KeLowerIrql(OldIrql);
    }
    check_status(NtfsKmtCreateFileWithData(Path, "multi", 5), STATUS_SUCCESS);
    WaitNotify(Request, STATUS_CANCELLED); FreeNotify(Request); Request = NULL;
    CheckSingle(Second, FILE_ACTION_ADDED, L"alpha.txt"); Second = NULL;
    ZwClose(Watch); Watch = NULL;
    check_status(NtfsKmtDelete(Path), STATUS_SUCCESS);
    Watch = OpenDirectory(Dir, FALSE);
    if (Watch == NULL) goto Cleanup;

    /* Cleanup completes a pending watch, even with a reference retaining the
     * cleaned file object; a new IRP on it completes with NOTIFY_CLEANUP too. */
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    ZwClose(Watch); Watch = NULL;
    if (Request != NULL)
    {
        WaitNotify(Request, STATUS_NOTIFY_CLEANUP);
        check_status(BeginNotify(Request->File, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Second), STATUS_PENDING);
        WaitNotify(Second, STATUS_NOTIFY_CLEANUP);
        FreeNotify(Second); Second = NULL;
    }
    FreeNotify(Request); Request = NULL;

    /* Small output buffers request a rescan instead of a truncated record. */
    Watch = OpenDirectory(Dir, FALSE);
    if (Watch == NULL) goto Cleanup;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 8, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtCreateFileWithData(Path, "e", 1), STATUS_SUCCESS);
    if (WaitNotify(Request, STATUS_NOTIFY_ENUM_DIR)) ok_eq_ulongptr(Request->Information, 0);
    FreeNotify(Request); Request = NULL;
    ZwClose(Watch); Watch = NULL;
    check_status(NtfsKmtDelete(Path), STATUS_SUCCESS);

    /* A root watch keeps root-relative paths after its first completion. */
    Watch = OpenDirectory(Root, FALSE);
    if (Watch == NULL) goto Cleanup;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, TRUE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtCreateFileWithData(Path, "root", 4), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_ADDED, L"KMT_NTFS_NOTIFY\\alpha.txt"); Request = NULL;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, TRUE, 1024, FALSE, &Request), STATUS_PENDING);
    check_status(NtfsKmtDelete(Path), STATUS_SUCCESS);
    CheckSingle(Request, FILE_ACTION_REMOVED, L"KMT_NTFS_NOTIFY\\alpha.txt"); Request = NULL;
    ZwClose(Watch); Watch = NULL;

    /* Deletion marking is sticky for an existing notification open, even
     * after the directory's delete disposition is canceled (MS-FSA per Open). */
    check_status(NtfsKmtCreateDirectory(Subdir), STATUS_SUCCESS);
    Watch = OpenDirectory(Subdir, FALSE); Other = OpenDirectory(Subdir, TRUE);
    if (Watch == NULL || Other == NULL) goto Cleanup;
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    SetDelete(Other, TRUE);
    WaitNotify(Request, STATUS_DELETE_PENDING); FreeNotify(Request); Request = NULL;
    SetDelete(Other, FALSE);
    check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
    WaitNotify(Request, STATUS_DELETE_PENDING); FreeNotify(Request); Request = NULL;
    ZwClose(Other); Other = NULL;
    ZwClose(Watch); Watch = NULL;

    /* Reject notifications on ordinary files and invalid filter masks. */
    check_status(NtfsKmtCreateFileWithData(Path, "f", 1), STATUS_SUCCESS);
    check_status(NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE), STATUS_SUCCESS);
    if (File != NULL)
    {
        check_status(StartNotify(File, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_INVALID_PARAMETER);
        WaitNotify(Request, STATUS_INVALID_PARAMETER); FreeNotify(Request); Request = NULL;
    }
    Watch = OpenDirectory(Dir, FALSE);
    if (Watch != NULL)
    {
        check_status(StartNotify(Watch, 0, FALSE, 1024, FALSE, &Request), STATUS_INVALID_PARAMETER);
        WaitNotify(Request, STATUS_INVALID_PARAMETER); FreeNotify(Request); Request = NULL;
        check_status(StartNotify(Watch, 0x80000000, FALSE, 1024, FALSE, &Request), STATUS_INVALID_PARAMETER);
        WaitNotify(Request, STATUS_INVALID_PARAMETER); FreeNotify(Request); Request = NULL;
    }

Cleanup:
    if (Request != NULL) { IoCancelIrp(Request->Irp); FreeNotify(Request); }
    if (Second != NULL) { IoCancelIrp(Second->Irp); FreeNotify(Second); }
    if (File != NULL) ZwClose(File);
    if (Other != NULL) ZwClose(Other);
    if (Watch != NULL) ZwClose(Watch);
    check_status(NtfsKmtDeleteTree(Dir), STATUS_SUCCESS);
}

/* Use a fresh open for each mask: the notify package remembers the first
 * completion filter for an open, including between successful completions. */
#define expect_change(Filter, Operation, Action, Name) \
    do { \
        Watch = OpenDirectory(Dir, FALSE); \
        if (Watch != NULL) \
        { \
            check_status(StartNotify(Watch, Filter, FALSE, 1024, FALSE, &Request), STATUS_PENDING); \
            check_status((Operation), STATUS_SUCCESS); \
            CheckSingle(Request, Action, Name); Request = NULL; \
            ZwClose(Watch); Watch = NULL; \
        } \
    } while (0)

static VOID
CancelUnchanged(PNOTIFY_REQUEST Request)
{
    if (Request != NULL)
    {
        ok_eq_long(KeReadStateEvent(&Request->Event), 0);
        IoCancelIrp(Request->Irp);
        WaitNotify(Request, STATUS_CANCELLED);
        FreeNotify(Request);
    }
}

START_TEST(NtfsChangeData)
{
    WCHAR Root[MAX_PATH], Dir[MAX_PATH], Path[MAX_PATH], StreamPath[MAX_PATH], Implicit[MAX_PATH];
    HANDLE File = NULL, Stream = NULL, Watch = NULL;
    PNOTIFY_REQUEST Request = NULL;
    FILE_BASIC_INFORMATION Basic;
    FILE_END_OF_FILE_INFORMATION End;
    FILE_ZERO_DATA_INFORMATION Zero;
    IO_STATUS_BLOCK IoStatus;
    SECURITY_DESCRIPTOR Sd;
    UCHAR EaBuffer[32];
    PFILE_FULL_EA_INFORMATION Ea = (PFILE_FULL_EA_INFORMATION)EaBuffer;
    USHORT Compression = COMPRESSION_FORMAT_LZNT1;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL)) return;
    Status = NtfsKmtPrepareDirectory(Root, L"KMT_NTFS_CHANGE_DATA", Dir);
    check_status(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"data.bin");
    NtfsKmtJoin(StreamPath, RTL_NUMBER_OF(StreamPath), Dir, L"data.bin:extra");
    check_status(NtfsKmtCreateFileWithData(Path, "abc", 3), STATUS_SUCCESS);
    check_status(NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE), STATUS_SUCCESS);
    if (File == NULL) goto DataCleanup;

    /* A create naming a stream on a missing file also creates its base name. */
    NtfsKmtJoin(Implicit, RTL_NUMBER_OF(Implicit), Dir, L"new.bin:extra");
    expect_change(FILE_NOTIFY_CHANGE_FILE_NAME, NtfsKmtCreateFileWithData(Implicit, "new", 3),
                  FILE_ACTION_ADDED, L"new.bin");

    expect_change(FILE_NOTIFY_CHANGE_LAST_WRITE, NtfsKmtWrite(File, 0, "d", 1, NULL),
                  FILE_ACTION_MODIFIED, L"data.bin");
    expect_change(FILE_NOTIFY_CHANGE_SIZE, NtfsKmtWrite(File, 3, "e", 1, NULL),
                  FILE_ACTION_MODIFIED, L"data.bin");
    End.EndOfFile.QuadPart = 2;
    expect_change(FILE_NOTIFY_CHANGE_SIZE,
                  ZwSetInformationFile(File, &IoStatus, &End, sizeof(End), FileEndOfFileInformation),
                  FILE_ACTION_MODIFIED, L"data.bin");

    /* Overwrites and zero-byte writes do not change the stream's size. */
    Watch = OpenDirectory(Dir, FALSE);
    if (Watch != NULL)
    {
        check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_SIZE, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
        check_status(NtfsKmtWrite(File, 0, "f", 1, NULL), STATUS_SUCCESS);
        check_status(NtfsKmtWrite(File, 0, "", 0, NULL), STATUS_SUCCESS);
        CancelUnchanged(Request); Request = NULL;
        ZwClose(Watch); Watch = NULL;
    }

    RtlZeroMemory(&Basic, sizeof(Basic));
    Basic.FileAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE;
    expect_change(FILE_NOTIFY_CHANGE_ATTRIBUTES,
                  ZwSetInformationFile(File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation),
                  FILE_ACTION_MODIFIED, L"data.bin");
    Watch = OpenDirectory(Dir, FALSE);
    if (Watch != NULL)
    {
        check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_ATTRIBUTES, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
        check_status(ZwSetInformationFile(File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation), STATUS_SUCCESS);
        CancelUnchanged(Request); Request = NULL;
        ZwClose(Watch); Watch = NULL;
    }
    RtlZeroMemory(&Basic, sizeof(Basic));
    KeQuerySystemTime(&Basic.CreationTime);
    expect_change(FILE_NOTIFY_CHANGE_CREATION,
                  ZwSetInformationFile(File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation),
                  FILE_ACTION_MODIFIED, L"data.bin");
    RtlZeroMemory(&Basic, sizeof(Basic));
    KeQuerySystemTime(&Basic.LastAccessTime);
    expect_change(FILE_NOTIFY_CHANGE_LAST_ACCESS,
                  ZwSetInformationFile(File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation),
                  FILE_ACTION_MODIFIED, L"data.bin");
    RtlZeroMemory(&Basic, sizeof(Basic));
    KeQuerySystemTime(&Basic.LastWriteTime);
    expect_change(FILE_NOTIFY_CHANGE_LAST_WRITE,
                  ZwSetInformationFile(File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation),
                  FILE_ACTION_MODIFIED, L"data.bin");

    RtlZeroMemory(EaBuffer, sizeof(EaBuffer));
    Ea->EaNameLength = 1;
    Ea->EaValueLength = 1;
    Ea->EaName[0] = 'N'; Ea->EaName[1] = 0; Ea->EaName[2] = 'V';
    expect_change(FILE_NOTIFY_CHANGE_EA,
                  ZwSetEaFile(File, &IoStatus, Ea, FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + 3),
                  FILE_ACTION_MODIFIED, L"data.bin");
    Ea->EaName[2] = 'W';
    expect_change(FILE_NOTIFY_CHANGE_ATTRIBUTES,
                  ZwSetEaFile(File, &IoStatus, Ea, FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + 3),
                  FILE_ACTION_MODIFIED, L"data.bin");
    check_status(RtlCreateSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION), STATUS_SUCCESS);
    check_status(RtlSetDaclSecurityDescriptor(&Sd, TRUE, NULL, FALSE), STATUS_SUCCESS);
    expect_change(FILE_NOTIFY_CHANGE_SECURITY, ZwSetSecurityObject(File, DACL_SECURITY_INFORMATION, &Sd),
                  FILE_ACTION_MODIFIED, L"data.bin");

    expect_change(FILE_NOTIFY_CHANGE_ATTRIBUTES,
                  NtfsKmtFsctl(File, FSCTL_SET_COMPRESSION, &Compression, sizeof(Compression), NULL, 0, NULL),
                  FILE_ACTION_MODIFIED, L"data.bin");
    Zero.FileOffset.QuadPart = 0; Zero.BeyondFinalZero.QuadPart = 1;
    expect_change(FILE_NOTIFY_CHANGE_LAST_WRITE,
                  NtfsKmtFsctl(File, FSCTL_SET_ZERO_DATA, &Zero, sizeof(Zero), NULL, 0, NULL),
                  FILE_ACTION_MODIFIED, L"data.bin");

    /* Named-stream records carry file:stream and the separate stream action. */
    expect_change(FILE_NOTIFY_CHANGE_STREAM_NAME,
                  NtfsKmtCreate(&Stream, NULL, StreamPath, FILE_ALL_ACCESS, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL),
                  FILE_ACTION_ADDED_STREAM, L"data.bin:extra");
    if (Stream != NULL)
    {
        expect_change(FILE_NOTIFY_CHANGE_STREAM_SIZE, NtfsKmtWrite(Stream, 0, "xyz", 3, NULL),
                      FILE_ACTION_MODIFIED_STREAM, L"data.bin:extra");
        expect_change(FILE_NOTIFY_CHANGE_STREAM_WRITE, NtfsKmtWrite(Stream, 0, "q", 1, NULL),
                      FILE_ACTION_MODIFIED_STREAM, L"data.bin:extra");
        End.EndOfFile.QuadPart = 1;
        expect_change(FILE_NOTIFY_CHANGE_STREAM_SIZE,
                      ZwSetInformationFile(Stream, &IoStatus, &End, sizeof(End), FileEndOfFileInformation),
                      FILE_ACTION_MODIFIED_STREAM, L"data.bin:extra");
        /* The ordinary FILE_NAME filter ignores stream writes. */
        Watch = OpenDirectory(Dir, FALSE);
        if (Watch != NULL)
        {
            check_status(StartNotify(Watch, FILE_NOTIFY_CHANGE_FILE_NAME, FALSE, 1024, FALSE, &Request), STATUS_PENDING);
            check_status(NtfsKmtWrite(Stream, 0, "r", 1, NULL), STATUS_SUCCESS);
            CancelUnchanged(Request); Request = NULL;
            ZwClose(Watch); Watch = NULL;
        }
        ZwClose(Stream); Stream = NULL;
    }
    expect_change(FILE_NOTIFY_CHANGE_STREAM_NAME, NtfsKmtDelete(StreamPath),
                  FILE_ACTION_REMOVED_STREAM, L"data.bin:extra");
    ZwClose(File); File = NULL;
    expect_change(FILE_NOTIFY_CHANGE_SIZE,
                  NtfsKmtCreate(&File, NULL, Path, FILE_ALL_ACCESS, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OVERWRITE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL),
                  FILE_ACTION_MODIFIED, L"data.bin");

DataCleanup:
    if (Request != NULL) { IoCancelIrp(Request->Irp); FreeNotify(Request); }
    if (Watch != NULL) ZwClose(Watch);
    if (Stream != NULL) ZwClose(Stream);
    if (File != NULL) ZwClose(File);
    check_status(NtfsKmtDeleteTree(Dir), STATUS_SUCCESS);
}

static NTSTATUS NTAPI
RemoveCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

START_TEST(NtfsWatchRemove)
{
    WCHAR Root[MAX_PATH];
    HANDLE A = NULL, B = NULL;
    PNOTIFY_REQUEST One = NULL, Two = NULL;
    PDEVICE_OBJECT Device;
    PIRP Irp;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL)) return;
    A = OpenDirectory(Root, FALSE); B = OpenDirectory(Root, FALSE);
    if (A == NULL || B == NULL) goto CleanupRemove;
    check_status(StartNotify(A, FILE_NOTIFY_CHANGE_FILE_NAME, TRUE, 1024, FALSE, &One), STATUS_PENDING);
    check_status(StartNotify(B, FILE_NOTIFY_CHANGE_FILE_NAME, TRUE, 1024, FALSE, &Two), STATUS_PENDING);
    if (One == NULL || Two == NULL) goto CleanupRemove;
    Device = IoGetRelatedDeviceObject(One->File);
    ObReferenceObject(Device);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    ok(Irp != NULL, "could not allocate removal IRP\n");
    if (Irp != NULL)
    {
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        Irp->RequestorMode = KernelMode;
        Irp->Tail.Overlay.Thread = PsGetCurrentThread();
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->MajorFunction = IRP_MJ_PNP;
        Stack->MinorFunction = IRP_MN_SURPRISE_REMOVAL;
        IoSetCompletionRoutine(Irp, RemoveCompletion, &Event, TRUE, TRUE, TRUE);
        Status = IoCallDriver(Device, Irp);
        if (Status == STATUS_PENDING)
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        check_status(Irp->IoStatus.Status, STATUS_SUCCESS);
        IoFreeIrp(Irp);
        WaitNotify(One, STATUS_NOTIFY_CLEANUP);
        WaitNotify(Two, STATUS_NOTIFY_CLEANUP);
    }
    ObDereferenceObject(Device);
CleanupRemove:
    if (One != NULL && !KeReadStateEvent(&One->Event)) IoCancelIrp(One->Irp);
    if (Two != NULL && !KeReadStateEvent(&Two->Event)) IoCancelIrp(Two->Irp);
    FreeNotify(One); FreeNotify(Two);
    if (B != NULL) check_status(ZwClose(B), STATUS_SUCCESS);
    if (A != NULL) check_status(ZwClose(A), STATUS_SUCCESS);
}
