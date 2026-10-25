/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private object namespaces
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A private namespace is an unnamed object directory that is found by its
 * boundary descriptor instead of by a name: a namespace name plus SIDs (and
 * optionally an integrity label) that whoever creates or opens it must have
 * in its token. Objects are then created relative to the directory handle.
 * The namespace goes away with its directory, when the last handle to it is
 * closed, or earlier from NtDeletePrivateNamespace, after which it can no
 * longer be opened.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

typedef struct _OBP_PRIVATE_NAMESPACE
{
    LIST_ENTRY ListEntry;
    POBJECT_DIRECTORY Directory;
    POBJECT_BOUNDARY_DESCRIPTOR Boundary;
} OBP_PRIVATE_NAMESPACE, *POBP_PRIVATE_NAMESPACE;

#define TAG_OB_PRIVATE_NAMESPACE 'NPbO'
#define OBP_MAX_BOUNDARY_SIZE    0x10000

static LIST_ENTRY ObpPrivateNamespaceList = { &ObpPrivateNamespaceList, &ObpPrivateNamespaceList };
static KGUARDED_MUTEX ObpPrivateNamespaceLock;
static BOOLEAN ObpPrivateNamespaceLockInitialized;

/* PRIVATE FUNCTIONS *********************************************************/

static
VOID
ObpInitializePrivateNamespaceLock(VOID)
{
    /* The directory type is created early during boot, before any namespace */
    if (!ObpPrivateNamespaceLockInitialized)
    {
        KeInitializeGuardedMutex(&ObpPrivateNamespaceLock);
        ObpPrivateNamespaceLockInitialized = TRUE;
    }
}

static
POBJECT_BOUNDARY_ENTRY
ObpNextBoundaryEntry(
    _In_ POBJECT_BOUNDARY_ENTRY Entry)
{
    return (POBJECT_BOUNDARY_ENTRY)((PUCHAR)(Entry + 1) +
                                    ALIGN_UP_BY(Entry->EntryDataSize, sizeof(ULONG)));
}

/* Checks a captured boundary descriptor: one name, valid SIDs, at most one label */
static
NTSTATUS
ObpValidateBoundaryDescriptor(
    _In_ POBJECT_BOUNDARY_DESCRIPTOR Boundary)
{
    POBJECT_BOUNDARY_ENTRY Entry;
    PUCHAR End = (PUCHAR)Boundary + Boundary->TotalSize;
    ULONG i, Names = 0, Labels = 0;

    if (Boundary->Version != OBJECT_BOUNDARY_DESCRIPTOR_VERSION)
        return STATUS_INVALID_PARAMETER;

    Entry = (POBJECT_BOUNDARY_ENTRY)(Boundary + 1);
    for (i = 0; i < Boundary->Items; i++)
    {
        if ((PUCHAR)(Entry + 1) > End ||
            Entry->EntryDataSize > (ULONG)(End - (PUCHAR)(Entry + 1)))
        {
            return STATUS_INVALID_PARAMETER;
        }

        switch (Entry->EntryType)
        {
            case OBNS_Name:
                if (Entry->EntryDataSize == 0 || (Entry->EntryDataSize % sizeof(WCHAR)) != 0)
                    return STATUS_INVALID_PARAMETER;
                Names++;
                break;

            case OBNS_SID:
            case OBNS_IL:
                if (Entry->EntryDataSize < RtlLengthRequiredSid(0) ||
                    !RtlValidSid(Entry + 1) ||
                    RtlLengthSid(Entry + 1) != Entry->EntryDataSize)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (Entry->EntryType == OBNS_IL)
                    Labels++;
                break;

            default:
                return STATUS_INVALID_PARAMETER;
        }

        Entry = ObpNextBoundaryEntry(Entry);
    }

    if (Names != 1 || Labels > 1)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

/* Copies a (possibly user-mode) boundary descriptor into paged pool */
static
NTSTATUS
ObpCaptureBoundaryDescriptor(
    _In_ POBJECT_BOUNDARY_DESCRIPTOR BoundaryDescriptor,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ POBJECT_BOUNDARY_DESCRIPTOR *CapturedBoundary)
{
    POBJECT_BOUNDARY_DESCRIPTOR Boundary;
    ULONG TotalSize;
    NTSTATUS Status;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead(BoundaryDescriptor, sizeof(*BoundaryDescriptor), sizeof(ULONG));
        TotalSize = BoundaryDescriptor->TotalSize;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (TotalSize < sizeof(*Boundary) || TotalSize > OBP_MAX_BOUNDARY_SIZE)
        return STATUS_INVALID_PARAMETER;

    Boundary = ExAllocatePoolWithTag(PagedPool, TotalSize, TAG_OB_PRIVATE_NAMESPACE);
    if (!Boundary)
        return STATUS_INSUFFICIENT_RESOURCES;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead(BoundaryDescriptor, TotalSize, sizeof(ULONG));
        RtlCopyMemory(Boundary, BoundaryDescriptor, TotalSize);
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    /* The size may have changed under us: trust the captured copy only */
    if (NT_SUCCESS(Status))
    {
        Boundary->TotalSize = TotalSize;
        Status = ObpValidateBoundaryDescriptor(Boundary);
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Boundary, TAG_OB_PRIVATE_NAMESPACE);
        return Status;
    }

    *CapturedBoundary = Boundary;
    return STATUS_SUCCESS;
}

static
BOOLEAN
ObpBoundaryHasEntry(
    _In_ POBJECT_BOUNDARY_DESCRIPTOR Boundary,
    _In_ POBJECT_BOUNDARY_ENTRY Wanted)
{
    POBJECT_BOUNDARY_ENTRY Entry = (POBJECT_BOUNDARY_ENTRY)(Boundary + 1);
    ULONG i;

    for (i = 0; i < Boundary->Items; i++)
    {
        if (Entry->EntryType == Wanted->EntryType &&
            Entry->EntryDataSize == Wanted->EntryDataSize &&
            RtlEqualMemory(Entry + 1, Wanted + 1, Entry->EntryDataSize))
        {
            return TRUE;
        }
        Entry = ObpNextBoundaryEntry(Entry);
    }
    return FALSE;
}

/* Two boundaries are the same if they have the same entries, in any order */
static
BOOLEAN
ObpBoundariesEqual(
    _In_ POBJECT_BOUNDARY_DESCRIPTOR Boundary1,
    _In_ POBJECT_BOUNDARY_DESCRIPTOR Boundary2)
{
    POBJECT_BOUNDARY_ENTRY Entry;
    ULONG i;

    if (Boundary1->Items != Boundary2->Items)
        return FALSE;

    Entry = (POBJECT_BOUNDARY_ENTRY)(Boundary1 + 1);
    for (i = 0; i < Boundary1->Items; i++)
    {
        if (!ObpBoundaryHasEntry(Boundary2, Entry))
            return FALSE;
        Entry = ObpNextBoundaryEntry(Entry);
    }
    return TRUE;
}

/* The caller must have every SID of the boundary in its token */
static
BOOLEAN
ObpCallerIsInBoundary(
    _In_ POBJECT_BOUNDARY_DESCRIPTOR Boundary)
{
    SECURITY_SUBJECT_CONTEXT SubjectContext;
    POBJECT_BOUNDARY_ENTRY Entry;
    PACCESS_TOKEN Token;
    BOOLEAN InBoundary = TRUE;
    ULONG i;

    SeCaptureSubjectContext(&SubjectContext);
    SeLockSubjectContext(&SubjectContext);
    Token = SeQuerySubjectContextToken(&SubjectContext);

    Entry = (POBJECT_BOUNDARY_ENTRY)(Boundary + 1);
    for (i = 0; i < Boundary->Items && InBoundary; i++)
    {
        if (Entry->EntryType == OBNS_SID && !SepSidInToken(Token, Entry + 1))
            InBoundary = FALSE;
        Entry = ObpNextBoundaryEntry(Entry);
    }

    SeUnlockSubjectContext(&SubjectContext);
    SeReleaseSubjectContext(&SubjectContext);
    return InBoundary;
}

/* Called with the lock held */
static
POBP_PRIVATE_NAMESPACE
ObpFindPrivateNamespace(
    _In_ POBJECT_BOUNDARY_DESCRIPTOR Boundary)
{
    PLIST_ENTRY ListEntry;
    POBP_PRIVATE_NAMESPACE Namespace;

    for (ListEntry = ObpPrivateNamespaceList.Flink;
         ListEntry != &ObpPrivateNamespaceList;
         ListEntry = ListEntry->Flink)
    {
        Namespace = CONTAINING_RECORD(ListEntry, OBP_PRIVATE_NAMESPACE, ListEntry);
        if (ObpBoundariesEqual(Namespace->Boundary, Boundary))
            return Namespace;
    }
    return NULL;
}

static
VOID
ObpFreePrivateNamespace(
    _In_ POBP_PRIVATE_NAMESPACE Namespace)
{
    ExFreePoolWithTag(Namespace->Boundary, TAG_OB_PRIVATE_NAMESPACE);
    ExFreePoolWithTag(Namespace, TAG_OB_PRIVATE_NAMESPACE);
}

/* Unlinks the namespace of Directory, if it is one; returns whether it was */
static
BOOLEAN
ObpRemovePrivateNamespace(
    _In_ POBJECT_DIRECTORY Directory)
{
    PLIST_ENTRY ListEntry;
    POBP_PRIVATE_NAMESPACE Namespace = NULL;

    KeAcquireGuardedMutex(&ObpPrivateNamespaceLock);
    for (ListEntry = ObpPrivateNamespaceList.Flink;
         ListEntry != &ObpPrivateNamespaceList;
         ListEntry = ListEntry->Flink)
    {
        POBP_PRIVATE_NAMESPACE Candidate =
            CONTAINING_RECORD(ListEntry, OBP_PRIVATE_NAMESPACE, ListEntry);
        if (Candidate->Directory == Directory)
        {
            RemoveEntryList(&Candidate->ListEntry);
            Namespace = Candidate;
            break;
        }
    }
    KeReleaseGuardedMutex(&ObpPrivateNamespaceLock);

    if (!Namespace)
        return FALSE;

    ObpFreePrivateNamespace(Namespace);
    return TRUE;
}

/*
 * Delete procedure of the directory object type: a private namespace ends
 * with its directory.
 */
VOID
NTAPI
ObpDeleteDirectory(
    _In_ PVOID ObjectBody)
{
    if (ObpPrivateNamespaceLockInitialized)
        ObpRemovePrivateNamespace(ObjectBody);
}

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
NtCreatePrivateNamespace(
    _Out_ PHANDLE NamespaceHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ POBJECT_BOUNDARY_DESCRIPTOR BoundaryDescriptor)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    POBJECT_BOUNDARY_DESCRIPTOR Boundary;
    POBP_PRIVATE_NAMESPACE Namespace;
    POBJECT_DIRECTORY Directory;
    HANDLE NewHandle;
    NTSTATUS Status;
    PAGED_CODE();

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForWriteHandle(NamespaceHandle);

        /* The namespace is found by its boundary, never by a name */
        if (ObjectAttributes)
        {
            if (PreviousMode != KernelMode)
                ProbeForRead(ObjectAttributes, sizeof(*ObjectAttributes), sizeof(ULONG));
            if (ObjectAttributes->ObjectName || ObjectAttributes->RootDirectory)
                _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    Status = ObpCaptureBoundaryDescriptor(BoundaryDescriptor, PreviousMode, &Boundary);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!ObpCallerIsInBoundary(Boundary))
    {
        ExFreePoolWithTag(Boundary, TAG_OB_PRIVATE_NAMESPACE);
        return STATUS_ACCESS_DENIED;
    }

    Namespace = ExAllocatePoolWithTag(PagedPool, sizeof(*Namespace), TAG_OB_PRIVATE_NAMESPACE);
    if (!Namespace)
    {
        ExFreePoolWithTag(Boundary, TAG_OB_PRIVATE_NAMESPACE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Namespace->Boundary = Boundary;

    Status = ObCreateObject(PreviousMode,
                            ObpDirectoryObjectType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(OBJECT_DIRECTORY),
                            0,
                            0,
                            (PVOID*)&Directory);
    if (!NT_SUCCESS(Status))
    {
        ObpFreePrivateNamespace(Namespace);
        return Status;
    }

    RtlZeroMemory(Directory, sizeof(OBJECT_DIRECTORY));
    ExInitializePushLock(&Directory->Lock);
    Directory->SessionId = -1;
    Namespace->Directory = Directory;

    /* Publish it, unless the boundary is taken already */
    KeAcquireGuardedMutex(&ObpPrivateNamespaceLock);
    if (ObpFindPrivateNamespace(Boundary))
    {
        KeReleaseGuardedMutex(&ObpPrivateNamespaceLock);
        ObpFreePrivateNamespace(Namespace);
        ObDereferenceObject(Directory);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    InsertTailList(&ObpPrivateNamespaceList, &Namespace->ListEntry);
    KeReleaseGuardedMutex(&ObpPrivateNamespaceLock);

    /* On failure this dereferences the directory, which unlinks the namespace */
    Status = ObInsertObject(Directory, NULL, DesiredAccess, 0, NULL, &NewHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *NamespaceHandle = NewHandle;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

NTSTATUS
NTAPI
NtOpenPrivateNamespace(
    _Out_ PHANDLE NamespaceHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ POBJECT_BOUNDARY_DESCRIPTOR BoundaryDescriptor)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    POBJECT_BOUNDARY_DESCRIPTOR Boundary;
    POBP_PRIVATE_NAMESPACE Namespace;
    POBJECT_DIRECTORY Directory = NULL;
    ULONG HandleAttributes = 0;
    HANDLE NewHandle;
    NTSTATUS Status;
    PAGED_CODE();

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForWriteHandle(NamespaceHandle);

        if (ObjectAttributes)
        {
            if (PreviousMode != KernelMode)
                ProbeForRead(ObjectAttributes, sizeof(*ObjectAttributes), sizeof(ULONG));
            if (ObjectAttributes->ObjectName || ObjectAttributes->RootDirectory)
                _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
            HandleAttributes = ObjectAttributes->Attributes;
        }
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    Status = ObpCaptureBoundaryDescriptor(BoundaryDescriptor, PreviousMode, &Boundary);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!ObpCallerIsInBoundary(Boundary))
    {
        ExFreePoolWithTag(Boundary, TAG_OB_PRIVATE_NAMESPACE);
        return STATUS_ACCESS_DENIED;
    }

    /* Keep the directory alive while we open it */
    KeAcquireGuardedMutex(&ObpPrivateNamespaceLock);
    Namespace = ObpFindPrivateNamespace(Boundary);
    if (Namespace && ObReferenceObjectSafe(Namespace->Directory))
        Directory = Namespace->Directory;
    KeReleaseGuardedMutex(&ObpPrivateNamespaceLock);
    ExFreePoolWithTag(Boundary, TAG_OB_PRIVATE_NAMESPACE);

    if (!Directory)
        return STATUS_OBJECT_PATH_NOT_FOUND;

    /* This does the access check against the namespace's security */
    Status = ObOpenObjectByPointer(Directory,
                                   HandleAttributes & OBJ_INHERIT,
                                   NULL,
                                   DesiredAccess,
                                   ObpDirectoryObjectType,
                                   PreviousMode,
                                   &NewHandle);
    ObDereferenceObject(Directory);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *NamespaceHandle = NewHandle;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

NTSTATUS
NTAPI
NtDeletePrivateNamespace(
    _In_ HANDLE NamespaceHandle)
{
    POBJECT_DIRECTORY Directory;
    NTSTATUS Status;
    PAGED_CODE();

    Status = ObReferenceObjectByHandle(NamespaceHandle,
                                       DELETE,
                                       ObpDirectoryObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID*)&Directory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Existing handles keep the directory, but nobody can open it again */
    if (!ObpRemovePrivateNamespace(Directory))
        Status = STATUS_INVALID_PARAMETER;

    ObDereferenceObject(Directory);
    return Status;
}

VOID
NTAPI
ObpInitPrivateNamespaces(VOID)
{
    ObpInitializePrivateNamespaceLock();
}

/* EOF */
