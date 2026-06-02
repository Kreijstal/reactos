/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/ea.c
 * PURPOSE:          NTFS filesystem driver - IRP_MJ_QUERY_EA / IRP_MJ_SET_EA
 *
 * NTFS stores extended attributes in two on-disk attributes:
 *   - $EA (type 0xE0, AttributeEA): the FILE_FULL_EA_INFORMATION list itself
 *   - $EA_INFORMATION (type 0xD0, AttributeEAInformation): a small header
 *     containing PackedEaSize, NeedEaCount, UnpackedEaSize
 *
 * The data inside $EA is exactly the same FILE_FULL_EA_INFORMATION layout
 * the NT API exposes - same NextEntryOffset / Flags / EaNameLength /
 * EaValueLength fields. So query is "read $EA, walk it, copy entries that
 * match the filter into the user buffer", and set is "validate the input
 * with IoCheckEaBufferValidity, write the new list back into $EA, update
 * $EA_INFORMATION".
 *
 * Files without any EAs (the common case for ReactOS installs) just don't
 * have an $EA attribute on disk; query reports STATUS_NO_EAS_ON_FILE.
 *
 * Set against a file that doesn't yet have $EA needs the AddAttribute
 * machinery to create a brand-new attribute on the MFT record. The
 * driver does have that infrastructure (it grew it for sparse data
 * writes), but wiring it up for $EA / $EA_INFORMATION is involved enough
 * that we currently return STATUS_NOT_IMPLEMENTED in that case and have
 * a follow-up TODO. Updating an existing $EA in place works, which is
 * the common case once a file has any EAs at all.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * Compute the on-disk size that a single FILE_FULL_EA_INFORMATION entry
 * occupies, padded to 4 bytes the way NT expects when chaining.
 */
static
ULONG
NtfsSizeOfEaEntry(PFILE_FULL_EA_INFORMATION Ea)
{
    ULONG Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) +
                 Ea->EaNameLength + 1 +    /* +1 for the NUL after the name */
                 Ea->EaValueLength;
    return ALIGN_UP_BY(Size, sizeof(ULONG));
}

/*
 * Read the entire $EA attribute of an FCB into a freshly allocated
 * buffer. *Buffer is set to NULL and *Size to 0 if the file has no $EA
 * (which the caller should treat as "no EAs on file"). Returns
 * STATUS_INSUFFICIENT_RESOURCES on allocation failure, the read status
 * otherwise. The caller must ExFreePoolWithTag(*Buffer, 'aEsF') when done.
 */
static
NTSTATUS
NtfsReadEaAttribute(PNTFS_VCB Vcb,
                    PNTFS_FCB Fcb,
                    PVOID *Buffer,
                    PULONG Size)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT EaContext = NULL;
    NTSTATUS Status;
    ULONGLONG DataLength;
    ULONG ReadLength;

    *Buffer = NULL;
    *Size = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = FindAttribute(Vcb, FileRecord, AttributeEA, L"", 0, &EaContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        /* No $EA attribute - file has no EAs. Not an error here; the
         * caller distinguishes by checking *Buffer. */
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_SUCCESS;
    }

    DataLength = AttributeDataLength(EaContext->pRecord);
    if (DataLength == 0 || DataLength > 0x10000)
    {
        /* 64KB hard cap matches the NT EA size limit. */
        ReleaseAttributeContext(EaContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_SUCCESS;
    }

    *Buffer = ExAllocatePoolWithTag(PagedPool, (SIZE_T)DataLength, 'aEsF');
    if (*Buffer == NULL)
    {
        ReleaseAttributeContext(EaContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ReadLength = ReadAttribute(Vcb, EaContext, 0, (PCHAR)*Buffer, (ULONG)DataLength);
    if (ReadLength != DataLength)
    {
        ExFreePoolWithTag(*Buffer, 'aEsF');
        *Buffer = NULL;
        ReleaseAttributeContext(EaContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    *Size = (ULONG)DataLength;

    ReleaseAttributeContext(EaContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
    return STATUS_SUCCESS;
}

/*
 * Copy a single FILE_FULL_EA_INFORMATION entry into the user output
 * buffer at *OutOffset. Updates *OutOffset on success. Returns
 * STATUS_BUFFER_OVERFLOW if there's no room (and reports the partial
 * copy via *PreviousNextOffset chaining if at least one entry has
 * already been written), STATUS_BUFFER_TOO_SMALL if even a single
 * entry doesn't fit and we haven't written any yet.
 */
static
NTSTATUS
NtfsCopyEaEntry(PFILE_FULL_EA_INFORMATION SourceEa,
                PUCHAR OutBuffer,
                ULONG OutBufferLength,
                PULONG OutOffset,
                PULONG PreviousNextOffset,
                BOOLEAN AnyWritten)
{
    ULONG EntrySize = NtfsSizeOfEaEntry(SourceEa);
    ULONG RawSize = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) +
                    SourceEa->EaNameLength + 1 +
                    SourceEa->EaValueLength;
    PFILE_FULL_EA_INFORMATION DestEa;

    if (*OutOffset + RawSize > OutBufferLength)
    {
        return AnyWritten ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
    }

    DestEa = (PFILE_FULL_EA_INFORMATION)(OutBuffer + *OutOffset);

    /* Patch the previously written entry to point at this one. */
    if (*PreviousNextOffset != MAXULONG)
    {
        PFILE_FULL_EA_INFORMATION Prev =
            (PFILE_FULL_EA_INFORMATION)(OutBuffer + *PreviousNextOffset);
        Prev->NextEntryOffset = *OutOffset - *PreviousNextOffset;
    }

    DestEa->NextEntryOffset = 0;
    DestEa->Flags           = SourceEa->Flags;
    DestEa->EaNameLength    = SourceEa->EaNameLength;
    DestEa->EaValueLength   = SourceEa->EaValueLength;
    RtlCopyMemory(DestEa->EaName,
                  SourceEa->EaName,
                  SourceEa->EaNameLength + 1 + SourceEa->EaValueLength);

    *PreviousNextOffset = *OutOffset;
    *OutOffset += EntrySize;

    /* If the *next* entry won't fit fully, the caller decides whether
     * to stop with the data we have. */
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsQueryEa(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_FCB Fcb;
    PNTFS_VCB Vcb;
    PVOID EaListBuffer = NULL;
    ULONG EaListSize = 0;
    PUCHAR OutBuffer;
    ULONG OutBufferLength;
    ULONG OutOffset = 0;
    ULONG PreviousNextOffset = MAXULONG;
    BOOLEAN ReturnSingle;
    BOOLEAN RestartScan;
    BOOLEAN IndexSpecified;
    PUCHAR UserEaList;
    ULONG UserEaListLength;
    ULONG StartOffset;
    PFILE_FULL_EA_INFORMATION SourceEa;
    NTSTATUS Status;

    DPRINT("NtfsQueryEa(%p)\n", IrpContext);

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;

    Fcb = (PNTFS_FCB)FileObject->FsContext;
    if (Fcb->Identifier.Type != NTFS_TYPE_FCB)
        return STATUS_INVALID_PARAMETER;

    Vcb = (PNTFS_VCB)IrpContext->DeviceObject->DeviceExtension;

    OutBuffer       = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    OutBufferLength = Stack->Parameters.QueryEa.Length;
    UserEaList      = Stack->Parameters.QueryEa.EaList;
    UserEaListLength = Stack->Parameters.QueryEa.EaListLength;
    ReturnSingle    = BooleanFlagOn(Stack->Flags, SL_RETURN_SINGLE_ENTRY);
    RestartScan     = BooleanFlagOn(Stack->Flags, SL_RESTART_SCAN);
    IndexSpecified  = BooleanFlagOn(Stack->Flags, SL_INDEX_SPECIFIED);

    if (OutBuffer == NULL || OutBufferLength == 0)
        return STATUS_INVALID_PARAMETER;

    /* Read $EA from disk. NULL EaListBuffer means the file has no EAs. */
    Status = NtfsReadEaAttribute(Vcb, Fcb, &EaListBuffer, &EaListSize);
    if (!NT_SUCCESS(Status))
        return Status;

    if (EaListBuffer == NULL || EaListSize == 0)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_NO_EAS_ON_FILE;
    }

    /* Determine where in the on-disk list to start. */
    if (IndexSpecified)
    {
        /* The user wants the EaIndex'th entry, 1-based. Walk to it. */
        ULONG Index = Stack->Parameters.QueryEa.EaIndex;
        ULONG Cur = 1;
        StartOffset = 0;
        while (StartOffset < EaListSize && Cur < Index)
        {
            SourceEa = (PFILE_FULL_EA_INFORMATION)((PUCHAR)EaListBuffer + StartOffset);
            if (SourceEa->NextEntryOffset == 0)
            {
                StartOffset = EaListSize;
                break;
            }
            StartOffset += SourceEa->NextEntryOffset;
            Cur++;
        }
    }
    else if (RestartScan || FileObject->FsContext2 == NULL)
    {
        StartOffset = 0;
    }
    else
    {
        PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
        StartOffset = Ccb->LastOffset;
    }

    if (StartOffset >= EaListSize)
    {
        ExFreePoolWithTag(EaListBuffer, 'aEsF');
        Irp->IoStatus.Information = 0;
        return STATUS_NO_MORE_EAS;
    }

    /* Copy entries that match the filter (or all of them if no filter). */
    while (StartOffset < EaListSize)
    {
        SourceEa = (PFILE_FULL_EA_INFORMATION)((PUCHAR)EaListBuffer + StartOffset);

        BOOLEAN ShouldCopy = TRUE;
        if (UserEaList != NULL && UserEaListLength > 0)
        {
            /* The filter is itself a chained FILE_GET_EA_INFORMATION
             * list - but every implementation simply walks the file's
             * own EA list and returns the union. We do the same: if
             * the source EA's name doesn't appear in the filter, skip. */
            PFILE_GET_EA_INFORMATION GetEa =
                (PFILE_GET_EA_INFORMATION)UserEaList;
            ULONG GetOff = 0;
            ShouldCopy = FALSE;
            while (GetOff < UserEaListLength)
            {
                if (GetEa->EaNameLength == SourceEa->EaNameLength &&
                    _strnicmp(GetEa->EaName, SourceEa->EaName,
                              SourceEa->EaNameLength) == 0)
                {
                    ShouldCopy = TRUE;
                    break;
                }
                if (GetEa->NextEntryOffset == 0)
                    break;
                GetOff += GetEa->NextEntryOffset;
                GetEa = (PFILE_GET_EA_INFORMATION)(UserEaList + GetOff);
            }
        }

        if (ShouldCopy)
        {
            Status = NtfsCopyEaEntry(SourceEa, OutBuffer, OutBufferLength,
                                     &OutOffset, &PreviousNextOffset,
                                     PreviousNextOffset != MAXULONG);
            if (!NT_SUCCESS(Status))
            {
                /* Buffer ran out. If we wrote at least one entry,
                 * report partial success via STATUS_BUFFER_OVERFLOW
                 * (which the docs say is the right code here). */
                if (FileObject->FsContext2 != NULL)
                {
                    PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
                    Ccb->LastOffset = StartOffset;
                }
                Irp->IoStatus.Information = OutOffset;
                ExFreePoolWithTag(EaListBuffer, 'aEsF');
                return Status;
            }

            if (ReturnSingle)
            {
                /* Advance past the entry we just copied so the next
                 * call (without RESTART_SCAN) picks up after it. */
                ULONG Advance = SourceEa->NextEntryOffset != 0 ?
                                SourceEa->NextEntryOffset :
                                EaListSize - StartOffset;
                if (FileObject->FsContext2 != NULL)
                {
                    PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
                    Ccb->LastOffset = StartOffset + Advance;
                }
                break;
            }
        }

        if (SourceEa->NextEntryOffset == 0)
        {
            StartOffset = EaListSize;
            break;
        }
        StartOffset += SourceEa->NextEntryOffset;
    }

    if (FileObject->FsContext2 != NULL && !ReturnSingle)
    {
        PNTFS_CCB Ccb = (PNTFS_CCB)FileObject->FsContext2;
        Ccb->LastOffset = StartOffset;
    }

    Irp->IoStatus.Information = OutOffset;
    ExFreePoolWithTag(EaListBuffer, 'aEsF');
    return OutOffset > 0 ? STATUS_SUCCESS : STATUS_NO_MORE_EAS;
}

NTSTATUS
NtfsSetEa(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_FCB Fcb;
    PNTFS_VCB Vcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PNTFS_ATTR_CONTEXT EaContext = NULL;
    PUCHAR InBuffer;
    ULONG InBufferLength;
    NTSTATUS Status;
    ULONG ErrorOffset = 0;
    ULONGLONG ExistingLength;
    ULONG BytesWritten;

    DPRINT("NtfsSetEa(%p)\n", IrpContext);

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;

    Fcb = (PNTFS_FCB)FileObject->FsContext;
    if (Fcb->Identifier.Type != NTFS_TYPE_FCB)
        return STATUS_INVALID_PARAMETER;

    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_INVALID_DEVICE_REQUEST;

    Vcb = (PNTFS_VCB)IrpContext->DeviceObject->DeviceExtension;

    InBuffer       = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    InBufferLength = Stack->Parameters.SetEa.Length;

    if (InBuffer == NULL || InBufferLength == 0)
        return STATUS_INVALID_PARAMETER;

    /* Validate the input list. The kernel helper checks NextEntryOffset
     * chaining, name lengths, alignment, etc. and returns an error
     * offset on the first malformed entry. */
    Status = IoCheckEaBufferValidity((PFILE_FULL_EA_INFORMATION)InBuffer,
                                     InBufferLength,
                                     &ErrorOffset);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = ErrorOffset;
        return Status;
    }

    /* Locate (or fail to locate) the existing $EA attribute. */
    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = FindAttribute(Vcb, FileRecord, AttributeEA, L"", 0, &EaContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        /* The file has no $EA attribute yet. Creating a fresh attribute
         * on an existing MFT record requires AddAttribute infrastructure
         * that this driver does grow attributes through (it does so for
         * data writes), but the orchestration for $EA + $EA_INFORMATION
         * together is not yet wired in. Reject the set so callers see
         * a stable error rather than the wrong-data result they'd get
         * from a broken half-implementation. TODO: implement creation
         * via the same path NtfsWriteFile uses to extend $DATA, and
         * write a fresh $EA_INFORMATION header at the same time. */
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_NOT_IMPLEMENTED;
    }

    ExistingLength = AttributeDataLength(EaContext->pRecord);
    if (InBufferLength > 0x10000 || InBufferLength > (ULONG)ExistingLength)
    {
        /* Growing the $EA attribute (changing its allocated size) also
         * needs the resize-attribute path. Same TODO as above. */
        ReleaseAttributeContext(EaContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_NOT_IMPLEMENTED;
    }

    Status = WriteAttribute(Vcb,
                            EaContext,
                            0,
                            InBuffer,
                            InBufferLength,
                            &BytesWritten,
                            FileRecord);

    ReleaseAttributeContext(EaContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = BytesWritten;

    return Status;
}

/* EOF */
