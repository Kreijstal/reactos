/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/security.c
 * PURPOSE:          NTFS filesystem driver - per-file $SECURITY_DESCRIPTOR
 *                   round-trip workers + thin IRP wrappers
 *
 * Two on-disk forms exist and the read path has to understand both:
 *
 *   - NT4 / NTFS 1.2: each MFT record owns a serialized
 *     SECURITY_DESCRIPTOR_RELATIVE in attribute type
 *     AttributeSecurityDescriptor (0x50).  This is what ReactOS writes.
 *
 *   - NTFS 3.0+: the descriptor is stored once in \$Secure:$SDS and every
 *     file references it by a 32-bit SecurityId held in the NTFS 3.0
 *     extension of $STANDARD_INFORMATION.  $SII indexes $SDS by that id
 *     ($SDH indexes it by hash, which only the write/dedup path needs).
 *     Windows-formatted volumes use this form exclusively, so ignoring it
 *     means ignoring every ACL Windows ever wrote on the volume.
 *
 * NtfsGetSecurityFromRecord therefore resolves in this order: explicit
 * $SECURITY_DESCRIPTOR attribute, then \$Secure via SecurityId, then the
 * synthesised default below.
 *
 * Out of scope here:
 *   - writing through to \$Secure: NtfsSetSecurityOnRecord still creates a
 *     per-record $SECURITY_DESCRIPTOR attribute.  That is a legal NTFS
 *     descriptor and, thanks to the precedence above, takes effect over any
 *     stale SecurityId, so a set-then-get round-trips.  Sharing/deduplicating
 *     into $SDS additionally requires $SDH maintenance and hash collision
 *     chaining on the write side.
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

/* Default SD surfaced when a file has no $SECURITY_DESCRIPTOR attribute yet
 * and a caller asks for one via IRP_MJ_QUERY_SECURITY.
 *
 * This MUST be a descriptor AccessCheck() can evaluate.  An SD with all four
 * offsets zero is not: AccessCheck() rejects an ownerless descriptor with
 * STATUS_INVALID_SECURITY_DESCR (win32 error 1338), so every caller that asks
 * "may I read/execute this file?" through the documented
 * GetFileSecurity + AccessCheck sequence gets a hard failure rather than a
 * grant.  GNAT does exactly that in __gnat_check_OWNER_ACL (gcc/ada/adaint.c),
 * which made gnatmake report "unable to locate gcc" for a gcc.exe sitting in
 * plain sight on PATH.
 *
 * A volume whose files carry no security information behaves like one with no
 * access control, so hand back the equivalent: owner and group BUILTIN\
 * Administrators (S-1-5-32-544) plus a DACL granting FILE_ALL_ACCESS to
 * Everyone (S-1-1-0).  Built as a byte blob rather than with the Rtl and Se
 * security helpers so this TU keeps compiling against the userspace test
 * harness's mock ntfs.h.
 *
 * Layout (self-relative, 80 bytes):
 *   0x00 header (20)   0x14 DACL (28)   0x30 owner SID (16)   0x40 group SID (16)
 */
static const UCHAR NtfsDefaultSelfRelativeSd[] =
{
    /* --- SECURITY_DESCRIPTOR_RELATIVE header --- */
    0x01,                   /* Revision = SECURITY_DESCRIPTOR_REVISION */
    0x00,                   /* Sbz1 */
    0x04, 0x80,             /* Control = SE_DACL_PRESENT | SE_SELF_RELATIVE */
    0x30, 0x00, 0x00, 0x00, /* Owner offset = 0x30 */
    0x40, 0x00, 0x00, 0x00, /* Group offset = 0x40 */
    0x00, 0x00, 0x00, 0x00, /* Sacl offset  = 0 (not present) */
    0x14, 0x00, 0x00, 0x00, /* Dacl offset  = 0x14 */

    /* --- ACL header @0x14: revision 2, size 28, one ACE --- */
    0x02, 0x00, 0x1C, 0x00,
    0x01, 0x00, 0x00, 0x00,

    /* --- ACCESS_ALLOWED_ACE @0x1C: size 20, inheritable --- */
    0x00,                   /* AceType = ACCESS_ALLOWED_ACE_TYPE */
    0x03,                   /* AceFlags = OBJECT_INHERIT | CONTAINER_INHERIT */
    0x14, 0x00,             /* AceSize = 20 */
    0xFF, 0x01, 0x1F, 0x00, /* AccessMask = FILE_ALL_ACCESS (0x001F01FF) */
    /* SID S-1-1-0 (Everyone), 12 bytes */
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00,

    /* --- Owner SID @0x30: S-1-5-32-544 (BUILTIN\Administrators) --- */
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,

    /* --- Group SID @0x40: S-1-5-32-544 --- */
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,
};

/* Fixed size of the self-relative SD header (revision + sbz1 + control +
 * four DWORD offsets for Owner, Group, Sacl, Dacl).  Matches
 * sizeof(SECURITY_DESCRIPTOR_RELATIVE) in <xdk/setypes.h> but referenced
 * by a literal here so this file compiles cleanly against the userspace
 * test harness's mock ntfs.h which doesn't pull in SDK security types. */
#define NTFS_MIN_SELF_RELATIVE_SD_BYTES 20

/* Copy Sd out under the IRP_MJ_QUERY_SECURITY contract: LenOut always
 * receives the required size, even when the buffer is too small. */
static NTSTATUS
NtfsReturnSecurityDescriptor(const VOID *Sd,
                             ULONG SdLength,
                             PVOID Buf,
                             ULONG BufLen,
                             PULONG LenOut)
{
    if (LenOut != NULL)
        *LenOut = SdLength;

    if (BufLen < SdLength)
        return STATUS_BUFFER_OVERFLOW;

    if (Buf != NULL && SdLength != 0)
        RtlCopyMemory(Buf, Sd, SdLength);

    return STATUS_SUCCESS;
}

/**
 * @name NtfsSecureProbe
 * @implemented
 *
 * Mount-time bootstrap for the shared descriptor store.  Locates \$Secure
 * and caches its MFT index so per-file lookups don't re-walk the root
 * directory.  Both the $SDS data stream and the $SII index must be present:
 * without $SII an id lookup would have to linearly scan $SDS, and without
 * $SDS there is nothing to point at.
 *
 * \$Secure is read at its architectural MFT index rather than by resolving
 * the path "\\$Secure", because NtfsFindMftRecord - which every path lookup
 * goes through - only ever returns index entries whose target is >=
 * NTFS_FILE_FIRST_USER_FILE.  Metadata files are deliberately invisible to
 * it, so a path lookup for \$Secure (record 9) can only ever fail, leaving
 * SecurePresent permanently FALSE.  Records 0..11 are fixed by the format -
 * this driver already reads the root directory, $MFT and $Bitmap that way -
 * so the index is not something that needs discovering.
 *
 * Record 9 held \$Quota on NTFS 1.2 and holds \$Secure from NTFS 3.0 on.
 * Demanding both $SDS and $SII tells the two apart without version sniffing:
 * a \$Quota record carries $Q and $O and no $SDS.
 *
 * A volume with no \$Secure is not an error - that's every NTFS 1.2 volume
 * and every volume ReactOS's own mkntfs produces.  Such volumes simply keep
 * using the per-record $SECURITY_DESCRIPTOR attribute.  Returns
 * STATUS_SUCCESS either way so mount never fails on this.
 */
NTSTATUS
NtfsSecureProbe(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER FileRecord = NULL;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    NTSTATUS Status;

    if (Vcb == NULL)
        return STATUS_INVALID_PARAMETER;

    Vcb->SecurePresent = FALSE;
    Vcb->SecureFileMft = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_SUCCESS;

    Status = ReadFileRecord(Vcb, NTFS_FILE_SECURE, FileRecord);
    if (!NT_SUCCESS(Status) || !(FileRecord->Flags & FRH_IN_USE))
        goto Cleanup;

    Status = FindAttribute(Vcb, FileRecord, AttributeData, L"$SDS", 4,
                           &AttrCtx, NULL);
    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(AttrCtx);
        AttrCtx = NULL;

        Status = FindAttribute(Vcb, FileRecord, AttributeIndexRoot, L"$SII", 4,
                               &AttrCtx, NULL);
        if (NT_SUCCESS(Status))
        {
            ReleaseAttributeContext(AttrCtx);
            Vcb->SecurePresent = TRUE;
            Vcb->SecureFileMft = NTFS_FILE_SECURE;
        }
    }

Cleanup:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return STATUS_SUCCESS;
}

/**
 * @name NtfsLookupSecurityDescriptorById
 * @implemented
 *
 * Resolves an NTFS 3.x SecurityId to the self-relative descriptor bytes held
 * in \$Secure:$SDS.
 *
 * The id is looked up through the $SII index rather than by scanning $SDS.
 * $SII is exactly the id -> descriptor-location map NTFS maintains for this
 * purpose (COLLATION_NTOFS_ULONG over the 4-byte id, value = a copy of the
 * 20-byte $SDS entry header), and btree.c already walks arbitrary view
 * indexes with lazy child loading, so a lookup costs one descent instead of
 * a linear read of a stream that is 256 KiB even on a fresh volume.  A scan
 * would also have to reimplement the mirror/alignment rules just to know
 * where the next entry starts.
 *
 * The header found in $SDS is cross-checked against the one $SII handed us;
 * a disagreement means the index and the stream have diverged, which is
 * reported as corruption rather than served as an ACL.
 *
 * @param Vcb        Source volume.
 * @param SecurityId Id from $STANDARD_INFORMATION.  Zero is "none".
 * @param SdOut      Out: pool block (TAG_NTFS) the caller must free.
 * @param SdLenOut   Out: size of *SdOut in bytes.
 *
 * @return STATUS_SUCCESS, STATUS_OBJECT_NAME_NOT_FOUND when the volume has
 *         no \$Secure or the id isn't in $SII, STATUS_FILE_CORRUPT_ERROR on
 *         an inconsistent entry, or a propagated allocation / read failure.
 */
NTSTATUS
NtfsLookupSecurityDescriptorById(PNTFS_VCB Vcb,
                                 ULONG SecurityId,
                                 PVOID *SdOut,
                                 PULONG SdLenOut)
{
    PFILE_RECORD_HEADER FileRecord = NULL;
    PNTFS_ATTR_CONTEXT IndexRootCtx = NULL;
    PNTFS_ATTR_CONTEXT SdsCtx = NULL;
    PINDEX_ROOT_ATTRIBUTE IndexRoot = NULL;
    PINDEX_ENTRY_ATTRIBUTE Entry = NULL;
    PB_TREE Tree = NULL;
    PVOID Sd = NULL;
    PUCHAR Value;
    UCHAR Header[NTFS_SDS_ENTRY_HEADER_SIZE];
    ULONGLONG EntryOffset = 0;
    ULONGLONG SdsLength;
    ULONG IndexRootLength;
    ULONG EntryLength = 0;
    ULONG StoredLength = 0;
    ULONG StoredId = 0;
    ULONG SdLength;
    NTSTATUS Status;

    if (SdOut != NULL)
        *SdOut = NULL;
    if (SdLenOut != NULL)
        *SdLenOut = 0;

    if (Vcb == NULL || SdOut == NULL || SdLenOut == NULL)
        return STATUS_INVALID_PARAMETER;

    if (!Vcb->SecurePresent || SecurityId == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Vcb->SecureFileMft, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* --- $SII: SecurityId -> $SDS entry header --- */

    Status = FindAttribute(Vcb, FileRecord, AttributeIndexRoot, L"$SII", 4,
                           &IndexRootCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    IndexRootLength = (ULONG)AttributeDataLength(IndexRootCtx->pRecord);
    if (IndexRootLength < sizeof(INDEX_ROOT_ATTRIBUTE))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    IndexRoot = ExAllocatePoolWithTag(NonPagedPool, IndexRootLength, TAG_NTFS);
    if (IndexRoot == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    if (ReadAttribute(Vcb, IndexRootCtx, 0, (PCHAR)IndexRoot, IndexRootLength)
            != IndexRootLength)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    Status = CreateBTreeFromIndexEx(Vcb, FileRecord, L"$SII", 4,
                                    IndexRootCtx, IndexRoot, &Tree);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtfsBTreeFindBlob(Tree, &SecurityId, sizeof(SecurityId), &Entry);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (Entry->Data.ViewIndex.DataLength < NTFS_SDS_ENTRY_HEADER_SIZE)
    {
        DPRINT1("$SII entry for id 0x%lx is too short (%u bytes)\n",
                SecurityId, Entry->Data.ViewIndex.DataLength);
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    Value = (PUCHAR)Entry + Entry->Data.ViewIndex.DataOffset;
    RtlCopyMemory(&EntryOffset, Value + NTFS_SDS_ENTRY_STREAM_OFFSET_OFFSET,
                  sizeof(EntryOffset));
    RtlCopyMemory(&EntryLength, Value + NTFS_SDS_ENTRY_LENGTH_OFFSET,
                  sizeof(EntryLength));

    if (EntryLength < NTFS_SDS_ENTRY_HEADER_SIZE + NTFS_MIN_SELF_RELATIVE_SD_BYTES ||
        EntryLength > NTFS_SDS_ENTRY_MAX_LENGTH)
    {
        DPRINT1("$SII entry for id 0x%lx has implausible length 0x%lx\n",
                SecurityId, EntryLength);
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    /* --- $SDS: read the entry the index pointed at --- */

    Status = FindAttribute(Vcb, FileRecord, AttributeData, L"$SDS", 4,
                           &SdsCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    SdsLength = AttributeDataLength(SdsCtx->pRecord);
    if (EntryOffset > SdsLength || EntryLength > SdsLength - EntryOffset)
    {
        DPRINT1("$SII entry for id 0x%lx points outside $SDS\n", SecurityId);
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    if (ReadAttribute(Vcb, SdsCtx, EntryOffset, (PCHAR)Header, sizeof(Header))
            != sizeof(Header))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    RtlCopyMemory(&StoredId, Header + NTFS_SDS_ENTRY_SECURITY_ID_OFFSET,
                  sizeof(StoredId));
    RtlCopyMemory(&StoredLength, Header + NTFS_SDS_ENTRY_LENGTH_OFFSET,
                  sizeof(StoredLength));

    if (StoredId != SecurityId || StoredLength != EntryLength)
    {
        DPRINT1("$SDS entry at 0x%I64x disagrees with $SII (id 0x%lx/0x%lx, "
                "length 0x%lx/0x%lx)\n",
                EntryOffset, StoredId, SecurityId, StoredLength, EntryLength);
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    SdLength = EntryLength - NTFS_SDS_ENTRY_HEADER_SIZE;

    Sd = ExAllocatePoolWithTag(NonPagedPool, SdLength, TAG_NTFS);
    if (Sd == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    if (ReadAttribute(Vcb, SdsCtx, EntryOffset + NTFS_SDS_ENTRY_HEADER_SIZE,
                      (PCHAR)Sd, SdLength) != SdLength)
    {
        ExFreePoolWithTag(Sd, TAG_NTFS);
        Sd = NULL;
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    *SdOut = Sd;
    *SdLenOut = SdLength;
    Status = STATUS_SUCCESS;

Cleanup:
    if (Tree != NULL)
        DestroyBTree(Tree);
    if (SdsCtx != NULL)
        ReleaseAttributeContext(SdsCtx);
    if (IndexRoot != NULL)
        ExFreePoolWithTag(IndexRoot, TAG_NTFS);
    if (IndexRootCtx != NULL)
        ReleaseAttributeContext(IndexRootCtx);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
    return Status;
}

/* Read $STANDARD_INFORMATION.SecurityId, or 0 when the record predates the
 * NTFS 3.0 extension (attribute shorter than 0x48 bytes) or has no
 * $STANDARD_INFORMATION at all. */
static ULONG
NtfsGetRecordSecurityId(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord)
{
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONG SecurityId = 0;
    NTSTATUS Status;

    Status = FindAttribute(Vcb, FileRecord, AttributeStandardInformation,
                           L"", 0, &AttrCtx, NULL);
    if (!NT_SUCCESS(Status))
        return 0;

    if (AttributeDataLength(AttrCtx->pRecord) >= STANDARD_INFORMATION_V3_LENGTH)
    {
        if (ReadAttribute(Vcb, AttrCtx, STANDARD_INFORMATION_SECURITY_ID_OFFSET,
                          (PCHAR)&SecurityId, sizeof(SecurityId)) != sizeof(SecurityId))
        {
            SecurityId = 0;
        }
    }

    ReleaseAttributeContext(AttrCtx);
    return SecurityId;
}

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
     * beyond that we don't try to walk the ACL - that's the upper layer's
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
 * Reads the file's security descriptor into the caller's buffer, resolving
 * the three places it can live, in order:
 *
 *   1. the record's own $SECURITY_DESCRIPTOR attribute (NT4 layout, and what
 *      NtfsSetSecurityOnRecord writes, so an explicit set always wins);
 *   2. \$Secure:$SDS, located by $STANDARD_INFORMATION.SecurityId - the only
 *      place a Windows-formatted volume keeps ACLs;
 *   3. the synthesised default self-relative SD above, so callers always get
 *      something valid back on a volume that carries no security information
 *      at all - mirrors how Windows synthesises a default SD from the
 *      Objects-Directory template when a file has no explicit one.
 *
 * A malformed entry at any step degrades to the next one rather than failing
 * the query: a caller asking "may I open this file?" needs a parseable
 * answer more than it needs to know which store was unreadable.
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
 *                                required size - matches IRP_MJ_QUERY_SECURITY
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
    PVOID SharedSd = NULL;
    ULONGLONG AttrLength;
    ULONG SharedSdLength = 0;
    ULONG SecurityId;
    ULONG CopyLength;

    /* 1. Explicit per-record $SECURITY_DESCRIPTOR. */
    Status = FindAttribute(Vcb, FileRecord, AttributeSecurityDescriptor,
                           L"", 0, &AttrCtx, NULL);
    if (NT_SUCCESS(Status))
    {
        AttrLength = AttributeDataLength(AttrCtx->pRecord);
        if (AttrLength != 0 && AttrLength <= MAXULONG)
        {
            CopyLength = (ULONG)AttrLength;

            if (LenOut != NULL)
                *LenOut = CopyLength;

            if (BufLen < CopyLength)
            {
                ReleaseAttributeContext(AttrCtx);
                return STATUS_BUFFER_OVERFLOW;
            }

            if (Buf != NULL)
                ReadAttribute(Vcb, AttrCtx, 0, (PCHAR)Buf, CopyLength);

            ReleaseAttributeContext(AttrCtx);
            return STATUS_SUCCESS;
        }

        /* Corrupted / bogus $SECURITY_DESCRIPTOR attribute - length makes
         * no sense.  Fall through to the shared store and then the default
         * rather than crashing the caller; this gives IRP consumers
         * something parseable while an operator can rewrite a real SD via
         * IRP_MJ_SET_SECURITY. */
        ReleaseAttributeContext(AttrCtx);
    }

    /* 2. Shared descriptor in \$Secure, keyed by $STANDARD_INFORMATION's
     *    NTFS 3.0 SecurityId.  This is where every ACL written by Windows
     *    actually lives; skipping it would silently substitute the default
     *    below for real on-disk access control. */
    SecurityId = NtfsGetRecordSecurityId(Vcb, FileRecord);
    if (SecurityId != 0)
    {
        Status = NtfsLookupSecurityDescriptorById(Vcb, SecurityId,
                                                  &SharedSd, &SharedSdLength);
        if (NT_SUCCESS(Status))
        {
            Status = NtfsReturnSecurityDescriptor(SharedSd, SharedSdLength,
                                                  Buf, BufLen, LenOut);
            ExFreePoolWithTag(SharedSd, TAG_NTFS);
            return Status;
        }
    }

    /* 3. No security information on this file at all - hand back the
     *    canonical default so callers (SRM, security propagation, user-mode
     *    query) always observe something parseable. */
    return NtfsReturnSecurityDescriptor(NtfsDefaultSelfRelativeSd,
                                        sizeof(NtfsDefaultSelfRelativeSd),
                                        Buf, BufLen, LenOut);
}

/**
 * @name NtfsDeleteSecurityFromRecord
 * @implemented
 *
 * Removes the $SECURITY_DESCRIPTOR attribute from FileRecord (if present).
 * Analogous to NtfsDelete{ObjectId,ReparsePoint}FromRecord - the attribute
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
 * harness-friendly - the IRP types are `{int dummy;}` under the userspace
 * mock and would not compile against the wrappers. */

/* EOF */
