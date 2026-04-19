/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/security.c
 * PURPOSE:          NTFS filesystem driver — per-file $SECURITY_DESCRIPTOR
 *                   round-trip workers + thin IRP wrappers
 *
 * This slice implements the legacy (NT4-era) per-file SD form: each MFT
 * record owns a serialized SECURITY_DESCRIPTOR_RELATIVE in attribute type
 * AttributeSecurityDescriptor (0x50).  Real Windows post-NT4 stores the SD
 * once in \$Secure:$SDS and references it via $STANDARD_INFORMATION.SecurityId
 * (plus $SDH / $SII B-tree indexes).  \$Secure is explicitly a follow-up slice
 * (see Kreijstal/reactos#36).
 *
 * Out of scope for this slice:
 *   - \$Secure:$SDS, $SDH (hash-indexed SD), $SII (id-indexed SD),
 *   - non-resident $SECURITY_DESCRIPTOR (currently rejected with
 *     STATUS_NOT_IMPLEMENTED; fits for the ACLs we'd see on files created
 *     by ReactOS itself and is bounded at ~700 bytes by the resident cap),
 *   - merging OWNER/GROUP/DACL/SACL sub-fields of SECURITY_INFORMATION
 *     against an existing SD; the worker overwrites wholesale.  Callers
 *     that need partial-update semantics should read, merge in memory,
 *     then write.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* World-readable default SD surfaced when a file has no $SECURITY_DESCRIPTOR
 * attribute yet and a caller asks for one via IRP_MJ_QUERY_SECURITY.  This is
 * a minimal self-relative SECURITY_DESCRIPTOR_RELATIVE with no Owner / Group /
 * DACL / SACL — Windows clients treat that as "no access control information
 * present, inherit or use defaults".  SECURITY_DESCRIPTOR_RELATIVE::Control
 * has SE_SELF_RELATIVE set (0x8000, low-endian 00 80). */
static const UCHAR NtfsDefaultSelfRelativeSd[] =
{
    0x01,                   /* Revision = SECURITY_DESCRIPTOR_REVISION */
    0x00,                   /* Sbz1 */
    0x00, 0x80,             /* Control = SE_SELF_RELATIVE */
    0x00, 0x00, 0x00, 0x00, /* Owner offset (0 = not present) */
    0x00, 0x00, 0x00, 0x00, /* Group offset */
    0x00, 0x00, 0x00, 0x00, /* Sacl offset  */
    0x00, 0x00, 0x00, 0x00, /* Dacl offset  */
};

/* Fixed size of the self-relative SD header (revision + sbz1 + control +
 * four DWORD offsets for Owner, Group, Sacl, Dacl).  Matches
 * sizeof(SECURITY_DESCRIPTOR_RELATIVE) in <xdk/setypes.h> but referenced
 * by a literal here so this file compiles cleanly against the userspace
 * test harness's mock ntfs.h which doesn't pull in SDK security types. */
#define NTFS_MIN_SELF_RELATIVE_SD_BYTES 20

/**
 * @name NtfsSetSecurityOnRecord
 * @implemented
 *
 * Writes the caller-supplied self-relative security descriptor into the
 * file's $SECURITY_DESCRIPTOR attribute.  Creates the attribute (resident)
 * if it doesn't already exist; overwrites in-place otherwise.
 *
 * @param Vcb         Target volume.
 * @param FileRecord  The file record to mutate.  Caller owns the buffer and
 *                    is responsible for committing with UpdateFileRecord.
 * @param SD          Serialized SECURITY_DESCRIPTOR_RELATIVE bytes.
 * @param SdLength    Size of SD in bytes.
 *
 * @return STATUS_SUCCESS on success,
 *         STATUS_INVALID_PARAMETER if SD/SdLength is bogus,
 *         STATUS_NOT_IMPLEMENTED if growing / migrating to non-resident,
 *         or a propagated error from WriteAttribute / AddResidentAttribute.
 */
NTSTATUS
NtfsSetSecurityOnRecord(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord,
                        const VOID *SD,
                        ULONG SdLength)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;

    if (SD == NULL || SdLength == 0)
        return STATUS_INVALID_PARAMETER;

    /* Basic SECURITY_DESCRIPTOR_RELATIVE sanity.  The buffer must at least
     * hold the fixed-size header (revision + control + four DWORD offsets);
     * beyond that we don't try to walk the ACL — that's the upper layer's
     * concern (and it was presumably produced by RtlCreateSecurityDescriptor
     * or similar). */
    if (SdLength < NTFS_MIN_SELF_RELATIVE_SD_BYTES)
        return STATUS_INVALID_PARAMETER;

    Status = FindAttribute(Vcb, FileRecord, AttributeSecurityDescriptor,
                           L"", 0, &AttrCtx, NULL);
    if (NT_SUCCESS(Status))
    {
        ULONGLONG ExistingLength = AttributeDataLength(AttrCtx->pRecord);
        ULONG BytesWritten = 0;

        if ((ULONG)ExistingLength != SdLength)
        {
            /* Growing/shrinking a resident $SECURITY_DESCRIPTOR requires
             * InternalSetResidentAttributeLength plus slot-packing against
             * the caller-owned FileRecord buffer.  Keep the worker simple
             * for now: callers that need to resize should delete + re-set. */
            ReleaseAttributeContext(AttrCtx);
            return STATUS_NOT_IMPLEMENTED;
        }

        Status = WriteAttribute(Vcb, AttrCtx, 0, (PUCHAR)SD, SdLength,
                                &BytesWritten, FileRecord);
        ReleaseAttributeContext(AttrCtx);
        return Status;
    }

    /* Bounds check resident attribute creation.  Same cap AddResidentAttribute
     * enforces on the slot itself (file record byte limit).  Allocating a
     * non-resident $SECURITY_DESCRIPTOR is a follow-up; reject clearly. */
    if (SdLength > Vcb->NtfsInfo.BytesPerFileRecord)
        return STATUS_NOT_IMPLEMENTED;

    return AddResidentAttribute(Vcb, FileRecord, AttributeSecurityDescriptor,
                                L"", 0, SD, SdLength);
}

/**
 * @name NtfsGetSecurityFromRecord
 * @implemented
 *
 * Reads the file's $SECURITY_DESCRIPTOR attribute into the caller's buffer.
 * If the attribute is absent, synthesises the minimal default self-relative
 * SD above so callers always get something valid back — mirrors how Windows
 * synthesises a default SD from the Objects-Directory template when a file
 * has no explicit one.
 *
 * @param Vcb         Source volume.
 * @param FileRecord  File record to query.
 * @param Buf         Output buffer (may be NULL iff BufLen == 0).
 * @param BufLen      Size of Buf in bytes.
 * @param LenOut      Out: required length (always populated when non-NULL,
 *                    even on STATUS_BUFFER_OVERFLOW).
 *
 * @return STATUS_SUCCESS on full copy,
 *         STATUS_BUFFER_OVERFLOW if BufLen is too small (LenOut set to the
 *                                required size — matches IRP_MJ_QUERY_SECURITY
 *                                contract).
 */
NTSTATUS
NtfsGetSecurityFromRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          PVOID Buf,
                          ULONG BufLen,
                          PULONG LenOut)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONGLONG AttrLength;
    ULONG CopyLength;

    Status = FindAttribute(Vcb, FileRecord, AttributeSecurityDescriptor,
                           L"", 0, &AttrCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        /* File has no $SECURITY_DESCRIPTOR yet.  Hand back the canonical
         * default so callers (SRM, security propagation, user-mode query)
         * always observe something parseable.  The Vcb argument is retained
         * for future per-volume defaults (e.g. derived from \$Secure once
         * that's implemented); ignored today. */
        UNREFERENCED_PARAMETER(Vcb);

        if (LenOut != NULL)
            *LenOut = sizeof(NtfsDefaultSelfRelativeSd);

        if (BufLen < sizeof(NtfsDefaultSelfRelativeSd))
            return STATUS_BUFFER_OVERFLOW;

        if (Buf != NULL)
            RtlCopyMemory(Buf, NtfsDefaultSelfRelativeSd,
                          sizeof(NtfsDefaultSelfRelativeSd));
        return STATUS_SUCCESS;
    }

    AttrLength = AttributeDataLength(AttrCtx->pRecord);
    if (AttrLength == 0 || AttrLength > MAXULONG)
    {
        /* Corrupted / bogus $SECURITY_DESCRIPTOR attribute — length makes
         * no sense.  Fall back to the default rather than crashing the
         * caller; this gives IRP consumers something parseable while an
         * operator can rewrite a real SD via IRP_MJ_SET_SECURITY. */
        ReleaseAttributeContext(AttrCtx);
        if (LenOut != NULL)
            *LenOut = sizeof(NtfsDefaultSelfRelativeSd);
        if (BufLen < sizeof(NtfsDefaultSelfRelativeSd))
            return STATUS_BUFFER_OVERFLOW;
        if (Buf != NULL)
            RtlCopyMemory(Buf, NtfsDefaultSelfRelativeSd,
                          sizeof(NtfsDefaultSelfRelativeSd));
        return STATUS_SUCCESS;
    }

    CopyLength = (ULONG)AttrLength;

    if (LenOut != NULL)
        *LenOut = CopyLength;

    if (BufLen < CopyLength)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_BUFFER_OVERFLOW;
    }

    if (Buf != NULL && CopyLength != 0)
        ReadAttribute(Vcb, AttrCtx, 0, (PCHAR)Buf, CopyLength);

    ReleaseAttributeContext(AttrCtx);
    return STATUS_SUCCESS;
}

/**
 * @name NtfsDeleteSecurityFromRecord
 * @implemented
 *
 * Removes the $SECURITY_DESCRIPTOR attribute from FileRecord (if present).
 * Analogous to NtfsDelete{ObjectId,ReparsePoint}FromRecord — the attribute
 * must be resident and live in the base record; non-resident / migrated
 * cases are explicitly out of scope for this slice and return
 * STATUS_NOT_IMPLEMENTED so callers see a stable error instead of silent
 * partial deletes.
 *
 * @return STATUS_SUCCESS, STATUS_NOT_FOUND if the attribute is already
 *         absent, STATUS_NOT_IMPLEMENTED for non-resident / attribute-list
 *         cases.
 */
NTSTATUS
NtfsDeleteSecurityFromRecord(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONG AttrOffset = 0;
    PNTFS_ATTR_RECORD Slot;

    Status = FindAttribute(Vcb, FileRecord, AttributeSecurityDescriptor,
                           L"", 0, &AttrCtx, &AttrOffset);
    if (!NT_SUCCESS(Status))
        return STATUS_NOT_FOUND;

    if (AttrCtx->pRecord->IsNonResident || AttrCtx->MigratedToMFTIndex != 0)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_NOT_IMPLEMENTED;
    }

    Slot = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    Status = RemoveResidentAttribute(Vcb, FileRecord, Slot);
    ReleaseAttributeContext(AttrCtx);
    return Status;
}

/* IRP wrappers for IRP_MJ_QUERY_SECURITY / IRP_MJ_SET_SECURITY live in
 * dispatch.c (alongside NtfsQueryEa / NtfsSetEa) so the worker TU stays
 * harness-friendly — the IRP types are `{int dummy;}` under the userspace
 * mock and would not compile against the wrappers. */

/* EOF */
