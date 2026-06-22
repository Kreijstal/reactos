/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/config/cmse.c
 * PURPOSE:         Configuration Manager - Security Subsystem Interface
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"
#define NDEBUG
#include "debug.h"

/* GLOBALS *******************************************************************/

/* FUNCTIONS *****************************************************************/

PSECURITY_DESCRIPTOR
NTAPI
CmpHiveRootSecurityDescriptor(VOID)
{
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    PACL Acl, AclCopy;
    PSID Sid[5];
    SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
#if (NTDDI_VERSION >= NTDDI_WIN8)
    SID_IDENTIFIER_AUTHORITY AppPackageAuthority = {SECURITY_APP_PACKAGE_AUTHORITY};
    const ULONG SidCount = 5;
#else
    const ULONG SidCount = 4;
#endif
    ULONG AceLength, AclLength, SidLength;
    PACE_HEADER AceHeader;
    ULONG i;
    PAGED_CODE();

    /* Phase 1: Allocate SIDs */
    SidLength = RtlLengthRequiredSid(1);
    Sid[0] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    Sid[1] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    Sid[2] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
    SidLength = RtlLengthRequiredSid(2);
    Sid[3] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
#if (NTDDI_VERSION >= NTDDI_WIN8)
    Sid[4] = ExAllocatePoolWithTag(PagedPool, SidLength, TAG_CMSD);
#endif

    /* Make sure all SIDs were allocated */
    for (i = 0; i < SidCount; i++)
    {
        if (!Sid[i]) KeBugCheckEx(REGISTRY_ERROR, 11, 1, 0, 0);
    }

    /* Phase 2: Initialize all SIDs */
    Status = RtlInitializeSid(Sid[0], &WorldAuthority, 1);
    Status |= RtlInitializeSid(Sid[1], &NtAuthority, 1);
    Status |= RtlInitializeSid(Sid[2], &NtAuthority, 1);
    Status |= RtlInitializeSid(Sid[3], &NtAuthority, 2);
#if (NTDDI_VERSION >= NTDDI_WIN8)
    Status |= RtlInitializeSid(Sid[4], &AppPackageAuthority, 2);
#endif
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 2, 0, 0);

    /* Phase 2: Setup SID Sub Authorities */
    *RtlSubAuthoritySid(Sid[0], 0) = SECURITY_WORLD_RID;
    *RtlSubAuthoritySid(Sid[1], 0) = SECURITY_RESTRICTED_CODE_RID;
    *RtlSubAuthoritySid(Sid[2], 0) = SECURITY_LOCAL_SYSTEM_RID;
    *RtlSubAuthoritySid(Sid[3], 0) = SECURITY_BUILTIN_DOMAIN_RID;
    *RtlSubAuthoritySid(Sid[3], 1) = DOMAIN_ALIAS_RID_ADMINS;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    *RtlSubAuthoritySid(Sid[4], 0) = SECURITY_APP_PACKAGE_BASE_RID;
    *RtlSubAuthoritySid(Sid[4], 1) = SECURITY_BUILTIN_PACKAGE_ANY_PACKAGE;
#endif

    /* Make sure all SIDs are valid */
    for (i = 0; i < SidCount; i++)
    {
        ASSERT(RtlValidSid(Sid[i]));
    }

    /* Phase 3: Calculate ACL Length */
    AclLength = sizeof(ACL);
    for (i = 0; i < SidCount; i++)
    {
        /* This is what MSDN says to do */
        AceLength = FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart);
        AceLength += SeLengthSid(Sid[i]);
        AclLength += AceLength;
    }

    /* Phase 3: Allocate the ACL */
    Acl = ExAllocatePoolWithTag(PagedPool, AclLength, TAG_CMSD);
    if (!Acl) KeBugCheckEx(REGISTRY_ERROR, 11, 3, 0, 0);

    /* Phase 4: Create the ACL */
    Status = RtlCreateAcl(Acl, AclLength, ACL_REVISION);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 4, Status, 0);

    /* Phase 5: Build the ACL */
    Status = RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_ALL_ACCESS, Sid[2]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_ALL_ACCESS, Sid[3]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_READ, Sid[0]);
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_READ, Sid[1]);
#if (NTDDI_VERSION >= NTDDI_WIN8)
    Status |= RtlAddAccessAllowedAce(Acl, ACL_REVISION, KEY_READ, Sid[4]);
#endif
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 5, Status, 0);

    /* Phase 5: Make the ACEs inheritable */
    for (i = 0; i < SidCount; i++)
    {
        Status = RtlGetAce(Acl, i, (PVOID*)&AceHeader);
        ASSERT(NT_SUCCESS(Status));
        AceHeader->AceFlags |= CONTAINER_INHERIT_ACE;
    }

    /* Phase 6: Allocate the security descriptor and make space for the ACL */
    SecurityDescriptor = ExAllocatePoolWithTag(PagedPool,
                                               sizeof(SECURITY_DESCRIPTOR) +
                                               AclLength,
                                               TAG_CMSD);
    if (!SecurityDescriptor) KeBugCheckEx(REGISTRY_ERROR, 11, 6, 0, 0);

    /* Phase 6: Make a copy of the ACL */
    AclCopy = (PACL)((PISECURITY_DESCRIPTOR)SecurityDescriptor + 1);
    RtlCopyMemory(AclCopy, Acl, AclLength);

    /* Phase 7: Create the security descriptor */
    Status = RtlCreateSecurityDescriptor(SecurityDescriptor,
                                         SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 7, Status, 0);

    /* Phase 8: Set the ACL as a DACL */
    Status = RtlSetDaclSecurityDescriptor(SecurityDescriptor,
                                          TRUE,
                                          AclCopy,
                                          FALSE);
    if (!NT_SUCCESS(Status)) KeBugCheckEx(REGISTRY_ERROR, 11, 8, Status, 0);

    /* Free the SIDs and original ACL */
    for (i = 0; i < SidCount; i++) ExFreePoolWithTag(Sid[i], TAG_CMSD);
    ExFreePoolWithTag(Acl, TAG_CMSD);

    /* Return the security descriptor */
    return SecurityDescriptor;
}

static
NTSTATUS
CmpQueryStubSecurityDescriptor(IN SECURITY_INFORMATION SecurityInformation,
                               OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
                               IN OUT PULONG BufferLength)
{
    PISECURITY_DESCRIPTOR_RELATIVE RelSd;
    ULONG SidSize;
    ULONG AclSize;
    ULONG SdSize;
    NTSTATUS Status;
    SECURITY_DESCRIPTOR_CONTROL Control = 0;
    ULONG Owner = 0;
    ULONG Group = 0;
    ULONG Dacl = 0;

    DPRINT("CmpQueryStubSecurityDescriptor()\n");

    SidSize = RtlLengthSid(SeWorldSid);
    RelSd = SecurityDescriptor;
    SdSize = sizeof(*RelSd);

    if (SecurityInformation & OWNER_SECURITY_INFORMATION)
    {
        Owner = SdSize;
        SdSize += SidSize;
    }

    if (SecurityInformation & GROUP_SECURITY_INFORMATION)
    {
        Group = SdSize;
        SdSize += SidSize;
    }

    if (SecurityInformation & DACL_SECURITY_INFORMATION)
    {
        Control |= SE_DACL_PRESENT;
        Dacl = SdSize;
        AclSize = sizeof(ACL) + sizeof(ACE) + SidSize;
        SdSize += AclSize;
    }

    if (SecurityInformation & SACL_SECURITY_INFORMATION)
    {
        Control |= SE_SACL_PRESENT;
    }

    if (*BufferLength < SdSize)
    {
        *BufferLength = SdSize;
        return STATUS_BUFFER_TOO_SMALL;
    }

    *BufferLength = SdSize;

    Status = RtlCreateSecurityDescriptorRelative(RelSd,
                                                 SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status))
        return Status;

    RelSd->Control |= Control;
    RelSd->Owner = Owner;
    RelSd->Group = Group;
    RelSd->Dacl = Dacl;

    if (Owner)
        RtlCopyMemory((PUCHAR)RelSd + Owner,
                      SeWorldSid,
                      SidSize);

    if (Group)
        RtlCopyMemory((PUCHAR)RelSd + Group,
                      SeWorldSid,
                      SidSize);

    if (Dacl)
    {
        Status = RtlCreateAcl((PACL)((PUCHAR)RelSd + Dacl),
                              AclSize,
                              ACL_REVISION);
        if (NT_SUCCESS(Status))
        {
            Status = RtlAddAccessAllowedAce((PACL)((PUCHAR)RelSd + Dacl),
                                            ACL_REVISION,
                                            GENERIC_ALL,
                                            SeWorldSid);
        }
    }

    ASSERT(Status == STATUS_SUCCESS);
    return Status;
}

NTSTATUS
CmpQuerySecurityDescriptor(IN PCM_KEY_CONTROL_BLOCK Kcb,
                           IN SECURITY_INFORMATION SecurityInformation,
                           OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
                           IN OUT PULONG BufferLength)
{
    PCM_KEY_NODE KeyNode;
    PCM_KEY_SECURITY Security;
    PSECURITY_DESCRIPTOR ObjectSd;
    NTSTATUS Status;

    DPRINT("CmpQuerySecurityDescriptor()\n");

    if (SecurityInformation == 0)
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Map the key node so we can find its stored security cell */
    KeyNode = (PCM_KEY_NODE)HvGetCell(Kcb->KeyHive, Kcb->KeyCell);
    if (KeyNode == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * On Windows every key has a security cell. ReactOS still creates keys
     * (and the in-memory registry roots) without one because security
     * assignment is not yet implemented for those paths, so fall back to a
     * permissive descriptor when the key has no stored security. This keeps
     * boot-time access checks working until assignment is implemented.
     */
    if (KeyNode->Security == HCELL_NIL)
    {
        HvReleaseCell(Kcb->KeyHive, Kcb->KeyCell);
        return CmpQueryStubSecurityDescriptor(SecurityInformation,
                                              SecurityDescriptor,
                                              BufferLength);
    }

    /* Map the security cell and hand its descriptor to the Se helper */
    Security = (PCM_KEY_SECURITY)HvGetCell(Kcb->KeyHive, KeyNode->Security);
    if (Security == NULL)
    {
        HvReleaseCell(Kcb->KeyHive, Kcb->KeyCell);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ASSERT(Security->Signature == CM_KEY_SECURITY_SIGNATURE);

    ObjectSd = &Security->Descriptor;
    Status = SeQuerySecurityDescriptorInfo(&SecurityInformation,
                                           SecurityDescriptor,
                                           BufferLength,
                                           &ObjectSd);

    HvReleaseCell(Kcb->KeyHive, KeyNode->Security);
    HvReleaseCell(Kcb->KeyHive, Kcb->KeyCell);
    return Status;
}

NTSTATUS
CmpSetSecurityDescriptor(IN PCM_KEY_CONTROL_BLOCK Kcb,
                         IN PSECURITY_INFORMATION SecurityInformation,
                         IN PSECURITY_DESCRIPTOR SecurityDescriptor,
                         IN POOL_TYPE PoolType,
                         IN PGENERIC_MAPPING GenericMapping)
{
    DPRINT("CmpSetSecurityDescriptor()\n");
    return STATUS_SUCCESS;
}

/*
 * Allocate a fresh self-referential security cell holding the supplied
 * self-relative descriptor and point the key cell at it. HvAllocateCell may
 * move bins, so all cell pointers are fetched only after the allocation.
 *
 * ReactOS has no descriptor-sharing security cache, and the read path
 * (SeQuerySecurityDescriptorInfo) only follows KeyNode->Security->Descriptor,
 * never the Flink/Blink ring, so a standalone cell is sufficient. When the key
 * already had a security cell it is simply unreferenced (left to the hive); the
 * one-cell leak only happens for the handful of boot hive roots reassigned at
 * startup, which is harmless on the in-memory boot hives.
 */
static
NTSTATUS
CmpAssignSecurityToCell(IN PHHIVE Hive,
                        IN HCELL_INDEX KeyCellIndex,
                        IN PSECURITY_DESCRIPTOR SelfRelativeSd)
{
    HCELL_INDEX SecurityCellIndex;
    PCM_KEY_NODE KeyNode;
    PCM_KEY_SECURITY Security;
    ULONG DescriptorLength;

    DescriptorLength = RtlLengthSecurityDescriptor(SelfRelativeSd);

    SecurityCellIndex = HvAllocateCell(Hive,
                                       FIELD_OFFSET(CM_KEY_SECURITY, Descriptor) +
                                       DescriptorLength,
                                       Stable,
                                       HCELL_NIL);
    if (SecurityCellIndex == HCELL_NIL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Security = (PCM_KEY_SECURITY)HvGetCell(Hive, SecurityCellIndex);
    KeyNode = (PCM_KEY_NODE)HvGetCell(Hive, KeyCellIndex);
    if (Security == NULL || KeyNode == NULL)
    {
        HvFreeCell(Hive, SecurityCellIndex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Security->Signature = CM_KEY_SECURITY_SIGNATURE;
    Security->ReferenceCount = 1;
    Security->DescriptorLength = DescriptorLength;
    Security->Flink = Security->Blink = SecurityCellIndex;
    RtlCopyMemory(&Security->Descriptor, SelfRelativeSd, DescriptorLength);

    KeyNode->Security = SecurityCellIndex;
    HvMarkCellDirty(Hive, KeyCellIndex, FALSE);
    HvMarkCellDirty(Hive, SecurityCellIndex, FALSE);

    HvReleaseCell(Hive, KeyCellIndex);
    HvReleaseCell(Hive, SecurityCellIndex);
    return STATUS_SUCCESS;
}

NTSTATUS
CmpAssignSecurityDescriptor(IN PCM_KEY_CONTROL_BLOCK Kcb,
                            IN PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    DPRINT("CmpAssignSecurityDescriptor(%p %p)\n", Kcb, SecurityDescriptor);

    /* The Object Manager hands us a self-relative descriptor built by
     * SeAssignSecurity; refuse anything else rather than corrupt the hive. */
    if (SecurityDescriptor == NULL ||
        !(((PISECURITY_DESCRIPTOR)SecurityDescriptor)->Control & SE_SELF_RELATIVE))
    {
        return STATUS_INVALID_SECURITY_DESCR;
    }

    return CmpAssignSecurityToCell(Kcb->KeyHive, Kcb->KeyCell, SecurityDescriptor);
}

#if (NTDDI_VERSION >= NTDDI_WIN8)

typedef struct _CMP_KEY_ACE
{
    PSID *Sid;
    ACCESS_MASK AccessMask;
    UCHAR AceFlags;
} CMP_KEY_ACE;

/*
 * Build a self-relative key security descriptor from a table of allowed-access
 * ACEs, with Owner = Administrators and Group = LocalSystem (both explicit, not
 * defaulted), matching how Windows stamps the persistent registry hives. The
 * returned descriptor is allocated from paged pool (TAG_CMSD) and must be freed
 * by the caller.
 */
static
PSECURITY_DESCRIPTOR
CmpBuildKeySelfRelativeSd(IN const CMP_KEY_ACE *Aces,
                         IN ULONG AceCount)
{
    SECURITY_DESCRIPTOR AbsoluteSd;
    PACL Dacl;
    ULONG AclLength, i;
    ULONG Length = 0;
    PSECURITY_DESCRIPTOR SelfRelativeSd = NULL;
    NTSTATUS Status;

    /* Size and build the DACL */
    AclLength = sizeof(ACL);
    for (i = 0; i < AceCount; i++)
    {
        AclLength += FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) +
                     SeLengthSid(*Aces[i].Sid);
    }

    Dacl = ExAllocatePoolWithTag(PagedPool, AclLength, TAG_CMSD);
    if (Dacl == NULL) return NULL;

    Status = RtlCreateAcl(Dacl, AclLength, ACL_REVISION);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Dacl, TAG_CMSD);
        return NULL;
    }

    for (i = 0; i < AceCount; i++)
    {
        Status = RtlAddAccessAllowedAceEx(Dacl,
                                          ACL_REVISION,
                                          Aces[i].AceFlags,
                                          Aces[i].AccessMask,
                                          *Aces[i].Sid);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Dacl, TAG_CMSD);
            return NULL;
        }
    }

    /* Wrap it in an absolute descriptor, then marshal to self-relative */
    RtlCreateSecurityDescriptor(&AbsoluteSd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&AbsoluteSd, SeAliasAdminsSid, FALSE);
    RtlSetGroupSecurityDescriptor(&AbsoluteSd, SeLocalSystemSid, FALSE);
    RtlSetDaclSecurityDescriptor(&AbsoluteSd, TRUE, Dacl, FALSE);

    Status = RtlAbsoluteToSelfRelativeSD(&AbsoluteSd, NULL, &Length);
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        SelfRelativeSd = ExAllocatePoolWithTag(PagedPool, Length, TAG_CMSD);
        if (SelfRelativeSd != NULL)
        {
            Status = RtlAbsoluteToSelfRelativeSD(&AbsoluteSd, SelfRelativeSd, &Length);
            if (!NT_SUCCESS(Status))
            {
                ExFreePoolWithTag(SelfRelativeSd, TAG_CMSD);
                SelfRelativeSd = NULL;
            }
        }
    }

    ExFreePoolWithTag(Dacl, TAG_CMSD);
    return SelfRelativeSd;
}

/*
 * Stamp a boot hive root cell with the Windows 8 default key security. The
 * persistent machine/user hives are baked by mkhive with legacy (WS03-era)
 * descriptors; on an NT 6.2 target the kernel owns the canonical security, so
 * we reassign the correct shape to each hive root once at load time.
 */
VOID
NTAPI
CmpAssignBootHiveRootSecurity(IN PCMHIVE CmHive,
                              IN BOOLEAN SecurityHive)
{
    /* Standard machine/user hive root: SAM, SOFTWARE, SYSTEM, .DEFAULT */
    static const CMP_KEY_ACE StandardAces[] =
    {
        { &SeAliasUsersSid,     KEY_READ,       CONTAINER_INHERIT_ACE },
        { &SeAliasAdminsSid,    KEY_ALL_ACCESS, CONTAINER_INHERIT_ACE },
        { &SeLocalSystemSid,    KEY_ALL_ACCESS, CONTAINER_INHERIT_ACE },
        { &SeCreatorOwnerSid,   KEY_ALL_ACCESS, CONTAINER_INHERIT_ACE },
        { &SeAllAppPackagesSid, KEY_READ,       CONTAINER_INHERIT_ACE },
    };
    /* The SECURITY hive is locked down to SYSTEM, with admins able only to
     * read/take ownership of the descriptor itself. */
    static const CMP_KEY_ACE SecurityAces[] =
    {
        { &SeLocalSystemSid, KEY_ALL_ACCESS,           CONTAINER_INHERIT_ACE },
        { &SeAliasAdminsSid, WRITE_DAC | READ_CONTROL, CONTAINER_INHERIT_ACE },
    };
    PSECURITY_DESCRIPTOR SelfRelativeSd;

    PAGED_CODE();

    if (SecurityHive)
        SelfRelativeSd = CmpBuildKeySelfRelativeSd(SecurityAces, RTL_NUMBER_OF(SecurityAces));
    else
        SelfRelativeSd = CmpBuildKeySelfRelativeSd(StandardAces, RTL_NUMBER_OF(StandardAces));

    if (SelfRelativeSd != NULL)
    {
        CmpAssignSecurityToCell(&CmHive->Hive,
                                CmHive->Hive.BaseBlock->RootCell,
                                SelfRelativeSd);
        ExFreePoolWithTag(SelfRelativeSd, TAG_CMSD);
    }
}

#endif /* (NTDDI_VERSION >= NTDDI_WIN8) */

NTSTATUS
NTAPI
CmpSecurityMethod(IN PVOID ObjectBody,
                  IN SECURITY_OPERATION_CODE OperationCode,
                  IN PSECURITY_INFORMATION SecurityInformation,
                  IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
                  IN OUT PULONG BufferLength,
                  IN OUT PSECURITY_DESCRIPTOR *OldSecurityDescriptor,
                  IN POOL_TYPE PoolType,
                  IN PGENERIC_MAPPING GenericMapping)
{
    PCM_KEY_CONTROL_BLOCK Kcb;
    NTSTATUS Status = STATUS_SUCCESS;

    DBG_UNREFERENCED_PARAMETER(OldSecurityDescriptor);
    DBG_UNREFERENCED_PARAMETER(GenericMapping);

    Kcb = ((PCM_KEY_BODY)ObjectBody)->KeyControlBlock;

    /*
     * Acquire the hive lock. Assigning/setting a security descriptor can
     * allocate or free security cells (CmpAssignSecurityToCell -> HvAllocateCell),
     * structurally mutating the hive's shared storage; that must exclude other
     * CPUs walking the same hive under a shared registry lock. A pure query only
     * reads, so it takes the registry shared.
     */
    if (OperationCode == QuerySecurityDescriptor)
        CmpLockRegistry();
    else
        CmpLockRegistryExclusive();

    /* Acquire the KCB lock */
    if (OperationCode == QuerySecurityDescriptor)
    {
        /* Avoid recursive locking if somebody already holds it */
        if (!((PCM_KEY_BODY)ObjectBody)->KcbLocked)
        {
            CmpAcquireKcbLockShared(Kcb);
        }
    }
    else
    {
        ASSERT(!((PCM_KEY_BODY)ObjectBody)->KcbLocked);
        CmpAcquireKcbLockExclusive(Kcb);
    }

    /* Don't touch deleted keys */
    if (Kcb->Delete)
    {
        /* Release the KCB lock */
        CmpReleaseKcbLock(Kcb);

        /* Release the hive lock */
        CmpUnlockRegistry();
        return STATUS_KEY_DELETED;
    }

    switch (OperationCode)
    {
        case SetSecurityDescriptor:
            DPRINT("Set security descriptor\n");
            ASSERT((PoolType == PagedPool) || (PoolType == NonPagedPool));
            Status = CmpSetSecurityDescriptor(Kcb,
                                              SecurityInformation,
                                              SecurityDescriptor,
                                              PoolType,
                                              GenericMapping);
            break;

        case QuerySecurityDescriptor:
            DPRINT("Query security descriptor\n");
            Status = CmpQuerySecurityDescriptor(Kcb,
                                                *SecurityInformation,
                                                SecurityDescriptor,
                                                BufferLength);
            break;

        case DeleteSecurityDescriptor:
            DPRINT("Delete security descriptor\n");
            /* HACK */
            break;

        case AssignSecurityDescriptor:
            DPRINT("Assign security descriptor\n");
            Status = CmpAssignSecurityDescriptor(Kcb,
                                                 SecurityDescriptor);
            break;

        default:
            KeBugCheckEx(SECURITY_SYSTEM, 0, STATUS_INVALID_PARAMETER, 0, 0);
    }

    /*
     * Release the KCB lock, but only if we locked it ourselves and
     * nobody else was locking it by themselves.
     */
    if (!((PCM_KEY_BODY)ObjectBody)->KcbLocked)
    {
        CmpReleaseKcbLock(Kcb);
    }

    /* Release the hive lock */
    CmpUnlockRegistry();

    return Status;
}
