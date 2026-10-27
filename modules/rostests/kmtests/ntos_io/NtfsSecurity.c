/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS security descriptors
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box security-descriptor coverage through ZwSetSecurityObject /
 * ZwQuerySecurityObject and the SecurityDescriptor of ZwCreateFile:
 * the default SD of a new file, explicit SDs at create time, set/query
 * round trips (same size, grown, persisted across reopen), partial
 * SECURITY_INFORMATION merges, a NULL DACL, and ACE inheritance from the
 * parent directory.
 *
 * Replaces the host tests test_security_roundtrip and the SD half of
 * test_ntfs_metadata_features (per-file $SECURITY_DESCRIPTOR and $Secure).
 *
 * Windows reference: NtQuerySecurityObject / NtSetSecurityObject contract
 * (STATUS_BUFFER_TOO_SMALL + LengthNeeded), SeAssignSecurity inheritance
 * rules (inherited ACEs carry INHERITED_ACE; a file drops OI/CI).
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define ALL_INFO (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION)

typedef struct _TEST_ACE
{
    PSID Sid;
    ACCESS_MASK Mask;
    UCHAR Flags;
} TEST_ACE, *PTEST_ACE;

/* Build an absolute SD in one pool block. Free with NtfsKmtFree. */
static
PSECURITY_DESCRIPTOR
BuildSd(
    _In_opt_ PSID Owner,
    _In_opt_ PSID Group,
    _In_ BOOLEAN DaclPresent,
    _In_reads_(AceCount) const TEST_ACE *Aces,
    _In_ ULONG AceCount,
    _In_ BOOLEAN Protected)
{
    PSECURITY_DESCRIPTOR Sd;
    PACL Acl = NULL;
    ULONG AclSize = sizeof(ACL);
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < AceCount; i++)
        AclSize += FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(Aces[i].Sid);

    Sd = NtfsKmtAlloc(sizeof(SECURITY_DESCRIPTOR) + AclSize);
    if (Sd == NULL)
        return NULL;
    Status = RtlCreateSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);

    if (DaclPresent && AceCount != 0)
    {
        Acl = (PACL)((PUCHAR)Sd + sizeof(SECURITY_DESCRIPTOR));
        Status = RtlCreateAcl(Acl, AclSize, ACL_REVISION);
        ok_eq_hex(Status, STATUS_SUCCESS);
        for (i = 0; i < AceCount; i++)
        {
            Status = RtlAddAccessAllowedAceEx(Acl, ACL_REVISION, Aces[i].Flags,
                                              Aces[i].Mask, Aces[i].Sid);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }
    if (DaclPresent)
    {
        Status = RtlSetDaclSecurityDescriptor(Sd, TRUE, Acl, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Protected)
        ((PISECURITY_DESCRIPTOR)Sd)->Control |= SE_DACL_PROTECTED;
    if (Owner != NULL)
        RtlSetOwnerSecurityDescriptor(Sd, Owner, FALSE);
    if (Group != NULL)
        RtlSetGroupSecurityDescriptor(Sd, Group, FALSE);
    return Sd;
}

/* Query an SD; returns a pool copy (self-relative) or NULL. */
static
PSECURITY_DESCRIPTOR
QuerySd(
    _In_ HANDLE Handle,
    _In_ SECURITY_INFORMATION Info)
{
    NTSTATUS Status;
    ULONG Needed = 0;
    PSECURITY_DESCRIPTOR Sd;

    Status = ZwQuerySecurityObject(Handle, Info, NULL, 0, &Needed);
    ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);
    ok(Needed >= sizeof(SECURITY_DESCRIPTOR_RELATIVE), "LengthNeeded %lu\n", Needed);
    if (Status != STATUS_BUFFER_TOO_SMALL || Needed == 0 || Needed > 64 * 1024)
        return NULL;

    Sd = NtfsKmtAlloc(Needed);
    if (Sd == NULL)
        return NULL;
    Status = ZwQuerySecurityObject(Handle, Info, Sd, Needed, &Needed);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtfsKmtFree(Sd);
        return NULL;
    }
    ok(RtlValidSecurityDescriptor(Sd), "returned SD is not valid\n");
    ok(((PISECURITY_DESCRIPTOR)Sd)->Control & SE_SELF_RELATIVE, "returned SD is not self-relative\n");
    return Sd;
}

static
PSID
SdOwner(
    _In_ PSECURITY_DESCRIPTOR Sd)
{
    PSID Sid = NULL;
    BOOLEAN Defaulted;

    RtlGetOwnerSecurityDescriptor(Sd, &Sid, &Defaulted);
    return Sid;
}

static
PSID
SdGroup(
    _In_ PSECURITY_DESCRIPTOR Sd)
{
    PSID Sid = NULL;
    BOOLEAN Defaulted;

    RtlGetGroupSecurityDescriptor(Sd, &Sid, &Defaulted);
    return Sid;
}

static
PACL
SdDacl(
    _In_ PSECURITY_DESCRIPTOR Sd,
    _Out_ PBOOLEAN Present)
{
    PACL Dacl = NULL;
    BOOLEAN Defaulted;

    *Present = FALSE;
    RtlGetDaclSecurityDescriptor(Sd, Present, &Dacl, &Defaulted);
    return Dacl;
}

static
VOID
CheckDacl(
    _In_ PSECURITY_DESCRIPTOR Sd,
    _In_reads_(Count) const TEST_ACE *Aces,
    _In_ ULONG Count,
    _In_ PCSTR What)
{
    BOOLEAN Present;
    PACL Dacl = SdDacl(Sd, &Present);
    ULONG i;

    ok(Present, "%s: DACL not present\n", What);
    ok(Dacl != NULL, "%s: NULL DACL\n", What);
    if (Dacl == NULL)
        return;
    ok(Dacl->AceCount == Count, "%s: %u ACEs, expected %lu\n", What, Dacl->AceCount, Count);
    for (i = 0; i < Count && i < Dacl->AceCount; i++)
    {
        PACCESS_ALLOWED_ACE Ace;
        if (!NT_SUCCESS(RtlGetAce(Dacl, i, (PVOID *)&Ace)))
            continue;
        ok(Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE, "%s: ACE %lu type %u\n", What, i, Ace->Header.AceType);
        ok(RtlEqualSid((PSID)&Ace->SidStart, Aces[i].Sid), "%s: ACE %lu SID mismatch\n", What, i);
        ok(Ace->Mask == Aces[i].Mask, "%s: ACE %lu mask 0x%lx, expected 0x%lx\n", What, i, Ace->Mask, Aces[i].Mask);
        ok(Ace->Header.AceFlags == Aces[i].Flags, "%s: ACE %lu flags 0x%x, expected 0x%x\n",
           What, i, Ace->Header.AceFlags, Aces[i].Flags);
    }
}

static
NTSTATUS
CreateWithSd(
    _Out_ PHANDLE Handle,
    _In_ PCWSTR Path,
    _In_opt_ PSECURITY_DESCRIPTOR Sd,
    _In_ BOOLEAN Directory)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&ObjectAttributes, &Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, Sd);
    return ZwCreateFile(Handle,
                        READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE |
                            (Directory ? FILE_LIST_DIRECTORY : GENERIC_READ | GENERIC_WRITE),
                        &ObjectAttributes, &IoStatus, NULL, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_CREATE,
                        (Directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
                            FILE_SYNCHRONOUS_IO_NONALERT,
                        NULL, 0);
}

static
VOID
TestDefaultAndRoundTrip(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR Sd, Got;
    BOOLEAN Present;
    TEST_ACE One[1];
    TEST_ACE Three[3];
    PACL Dacl;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"sec.txt");
    Status = CreateWithSd(&Handle, Path, NULL, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Default SD of a new file: owner, group and a DACL. */
    Got = QuerySd(Handle, ALL_INFO);
    if (skip(Got != NULL, "Cannot query the security descriptor\n"))
    {
        ZwClose(Handle);
        return;
    }
    ok(SdOwner(Got) != NULL, "new file has no owner\n");
    ok(SdGroup(Got) != NULL, "new file has no group\n");
    Dacl = SdDacl(Got, &Present);
    ok(Present && Dacl != NULL, "new file has no DACL\n");
    NtfsKmtFree(Got);

    /* Set owner + one-ACE DACL, read it back. */
    One[0].Sid = SeExports->SeWorldSid;
    One[0].Mask = FILE_ALL_ACCESS;
    One[0].Flags = 0;
    Sd = BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, One, 1, TRUE);
    Status = ZwSetSecurityObject(Handle, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, Sd);
    NtfsKmtFree(Sd);
    if (skip(!NtfsKmtIsUnimplemented(Status), "Setting security not supported (0x%lx)\n", Status))
    {
        ZwClose(Handle);
        return;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    Got = QuerySd(Handle, ALL_INFO);
    if (Got != NULL)
    {
        ok(SdOwner(Got) && RtlEqualSid(SdOwner(Got), SeExports->SeAliasAdminsSid), "owner not Administrators\n");
        CheckDacl(Got, One, 1, "one-ACE DACL");
        NtfsKmtFree(Got);
    }

    /* Grow it: three ACEs (the stored descriptor must be resized). */
    Three[0].Sid = SeExports->SeWorldSid;       Three[0].Mask = FILE_GENERIC_READ; Three[0].Flags = 0;
    Three[1].Sid = SeExports->SeAliasAdminsSid; Three[1].Mask = FILE_ALL_ACCESS;   Three[1].Flags = 0;
    Three[2].Sid = SeExports->SeLocalSystemSid; Three[2].Mask = FILE_ALL_ACCESS;   Three[2].Flags = 0;
    Sd = BuildSd(NULL, NULL, TRUE, Three, 3, TRUE);
    Status = ZwSetSecurityObject(Handle, DACL_SECURITY_INFORMATION, Sd);
    NtfsKmtFree(Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Partial update: only the group changes, owner and DACL stay. */
    Sd = BuildSd(NULL, SeExports->SeLocalSystemSid, FALSE, NULL, 0, FALSE);
    Status = ZwSetSecurityObject(Handle, GROUP_SECURITY_INFORMATION, Sd);
    NtfsKmtFree(Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);

    /* Reopen: everything was persisted. */
    Status = NtfsKmtOpen(&Handle, Path, READ_CONTROL | WRITE_DAC, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Got = QuerySd(Handle, ALL_INFO);
    if (Got != NULL)
    {
        ok(SdOwner(Got) && RtlEqualSid(SdOwner(Got), SeExports->SeAliasAdminsSid), "owner lost after reopen\n");
        ok(SdGroup(Got) && RtlEqualSid(SdGroup(Got), SeExports->SeLocalSystemSid), "group not SYSTEM after reopen\n");
        CheckDacl(Got, Three, 3, "three-ACE DACL after reopen");
        ok(((PISECURITY_DESCRIPTOR)Got)->Control & SE_DACL_PROTECTED, "SE_DACL_PROTECTED lost\n");
        NtfsKmtFree(Got);
    }

    /* Only the requested parts come back. */
    Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
    if (Got != NULL)
    {
        ok(SdOwner(Got) == NULL, "owner returned without OWNER_SECURITY_INFORMATION\n");
        ok(SdGroup(Got) == NULL, "group returned without GROUP_SECURITY_INFORMATION\n");
        CheckDacl(Got, Three, 3, "DACL-only query");
        NtfsKmtFree(Got);
    }

    /* Shrink back to one ACE. */
    Sd = BuildSd(NULL, NULL, TRUE, One, 1, TRUE);
    Status = ZwSetSecurityObject(Handle, DACL_SECURITY_INFORMATION, Sd);
    NtfsKmtFree(Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
    if (Got != NULL)
    {
        CheckDacl(Got, One, 1, "shrunk DACL");
        NtfsKmtFree(Got);
    }

    /* A NULL DACL (present, no ACL) is stored as such. */
    Sd = BuildSd(NULL, NULL, TRUE, NULL, 0, FALSE);
    Status = ZwSetSecurityObject(Handle, DACL_SECURITY_INFORMATION, Sd);
    NtfsKmtFree(Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
    if (Got != NULL)
    {
        Dacl = SdDacl(Got, &Present);
        ok(Present, "NULL DACL: DACL not present\n");
        ok(Dacl == NULL, "NULL DACL came back as an ACL with %u ACEs\n", Dacl ? Dacl->AceCount : 0);
        NtfsKmtFree(Got);
    }
    ZwClose(Handle);

    /* Two files with the same descriptor each read it back unchanged. */
    {
        WCHAR Path2[MAX_PATH];
        HANDLE Handle2;

        NtfsKmtJoin(Path2, RTL_NUMBER_OF(Path2), Dir, L"sec2.txt");
        Sd = BuildSd(SeExports->SeAliasAdminsSid, SeExports->SeLocalSystemSid, TRUE, Three, 3, TRUE);
        Status = CreateWithSd(&Handle2, Path2, Sd, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Got = QuerySd(Handle2, ALL_INFO);
            if (Got != NULL)
            {
                ok(SdOwner(Got) && RtlEqualSid(SdOwner(Got), SeExports->SeAliasAdminsSid),
                   "explicit create owner mismatch\n");
                CheckDacl(Got, Three, 3, "explicit SD at create");
                NtfsKmtFree(Got);
            }
            ZwClose(Handle2);
        }
        NtfsKmtFree(Sd);
    }
}

static
VOID
TestInheritance(
    _In_ PCWSTR Dir)
{
    WCHAR Parent[MAX_PATH];
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR Sd, Got;
    TEST_ACE Aces[2];
    TEST_ACE Expect[2];

    /* Parent directory with two inheritable ACEs and a protected DACL. */
    Aces[0].Sid = SeExports->SeAliasAdminsSid;
    Aces[0].Mask = FILE_ALL_ACCESS;
    Aces[0].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    Aces[1].Sid = SeExports->SeAliasUsersSid;
    Aces[1].Mask = FILE_GENERIC_READ;
    Aces[1].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    Sd = BuildSd(SeExports->SeAliasAdminsSid, SeExports->SeLocalSystemSid, TRUE, Aces, 2, TRUE);
    NtfsKmtJoin(Parent, RTL_NUMBER_OF(Parent), Dir, L"inherit");
    Status = CreateWithSd(&Handle, Parent, Sd, TRUE);
    NtfsKmtFree(Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
    if (Got != NULL)
    {
        CheckDacl(Got, Aces, 2, "inheritable parent DACL");
        NtfsKmtFree(Got);
    }
    ZwClose(Handle);

    /* A file inherits both ACEs, marked INHERITED_ACE, without OI/CI. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"child.txt");
    Status = CreateWithSd(&Handle, Path, NULL, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Expect[0] = Aces[0];
        Expect[0].Flags = INHERITED_ACE;
        Expect[1] = Aces[1];
        Expect[1].Flags = INHERITED_ACE;
        Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
        if (Got != NULL)
        {
            CheckDacl(Got, Expect, 2, "file inherited DACL");
            NtfsKmtFree(Got);
        }
        ZwClose(Handle);
    }

    /* A subdirectory inherits them and keeps OI/CI for its own children. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"childdir");
    Status = CreateWithSd(&Handle, Path, NULL, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Expect[0] = Aces[0];
        Expect[0].Flags = INHERITED_ACE | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        Expect[1] = Aces[1];
        Expect[1].Flags = INHERITED_ACE | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
        if (Got != NULL)
        {
            CheckDacl(Got, Expect, 2, "directory inherited DACL");
            NtfsKmtFree(Got);
        }
        ZwClose(Handle);
    }

    /* An explicit protected DACL at create time is used as given. */
    {
        TEST_ACE Own[1];
        Own[0].Sid = SeExports->SeWorldSid;
        Own[0].Mask = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
        Own[0].Flags = 0;
        Sd = BuildSd(NULL, NULL, TRUE, Own, 1, TRUE);
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"explicit.txt");
        Status = CreateWithSd(&Handle, Path, Sd, FALSE);
        NtfsKmtFree(Sd);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Got = QuerySd(Handle, DACL_SECURITY_INFORMATION);
            if (Got != NULL)
            {
                CheckDacl(Got, Own, 1, "protected explicit DACL under inheritable parent");
                NtfsKmtFree(Got);
            }
            ZwClose(Handle);
        }
    }
}

static NTSTATUS
ForceCreate(PHANDLE Handle, PCWSTR Path, ACCESS_MASK Desired, ULONG Disposition, ULONG Options, BOOLEAN Synchronous)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    *Handle = NULL;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name,
                                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_FORCE_ACCESS_CHECK, NULL, NULL);
    if (Synchronous)
    {
        Desired |= SYNCHRONIZE;
        Options |= FILE_SYNCHRONOUS_IO_NONALERT;
    }
    return ZwCreateFile(Handle, Desired, &Attributes, &IoStatus, NULL, FILE_ATTRIBUTE_NORMAL,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         Disposition, Options, NULL, 0);
}

static NTSTATUS
ForceOpen(PHANDLE Handle, PCWSTR Path, ACCESS_MASK Desired, ULONG Disposition, BOOLEAN Synchronous)
{
    return ForceCreate(Handle, Path, Desired, Disposition, FILE_NON_DIRECTORY_FILE, Synchronous);
}

static NTSTATUS
ForceOpenById(PHANDLE Handle, HANDLE Volume, ULONGLONG Id, ACCESS_MASK Desired)
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Name;
    IO_STATUS_BLOCK IoStatus;

    *Handle = NULL;
    Name.Buffer = (PWCHAR)&Id;
    Name.Length = Name.MaximumLength = sizeof(Id);
    InitializeObjectAttributes(&Attributes, &Name,
                                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_FORCE_ACCESS_CHECK, Volume, NULL);
    return ZwCreateFile(Handle, Desired | SYNCHRONIZE, &Attributes, &IoStatus, NULL, FILE_ATTRIBUTE_NORMAL,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                         FILE_OPEN_BY_FILE_ID | FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
}

static VOID
CheckGranted(HANDLE Handle, ACCESS_MASK Required, ACCESS_MASK Forbidden)
{
    OBJECT_BASIC_INFORMATION Basic;
    NTSTATUS Status;

    Status = ZwQueryObject(Handle, ObjectBasicInformation, &Basic, sizeof(Basic), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok((Basic.GrantedAccess & Required) == Required, "missing rights: granted %lx, required %lx\n",
           Basic.GrantedAccess, Required);
        ok(!(Basic.GrantedAccess & Forbidden), "excess rights: granted %lx, forbidden %lx\n",
           Basic.GrantedAccess, Forbidden);
    }
}

static VOID
ReplaceAccessDacl(HANDLE Handle, PSECURITY_DESCRIPTOR Sd)
{
    NTSTATUS Status;

    if (Sd == NULL) return;
    Status = ZwSetSecurityObject(Handle, DACL_SECURITY_INFORMATION, Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtFree(Sd);
}

static VOID
TestStoredAccess(PCWSTR Dir, WCHAR Letter)
{
    WCHAR Path[MAX_PATH], Alias[MAX_PATH], Stream[MAX_PATH], Missing[MAX_PATH];
    HANDLE Control = NULL, File = NULL, Volume = NULL, Blocker = NULL;
    PSECURITY_DESCRIPTOR Sd;
    TEST_ACE Ace;
    FILE_STANDARD_INFORMATION Standard;
    FILE_BASIC_INFORMATION Basic;
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING IdName;
    IO_STATUS_BLOCK IoStatus;
    ULONGLONG Id;
    NTSTATUS Status;
    PACL Empty;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"access.bin");
    NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Dir, L"access-link.bin");
    RtlStringCchPrintfW(Stream, RTL_NUMBER_OF(Stream), L"%ls:private", Path);
    RtlStringCchPrintfW(Missing, RTL_NUMBER_OF(Missing), L"%ls:new", Path);
    Status = NtfsKmtCreateFileWithData(Path, "read", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Stream, "ads!", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Control, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Id = NtfsKmtQueryFileId(Control);
    Status = NtfsKmtLink(Control, Alias, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Ace.Sid = SeExports->SeWorldSid;
    Ace.Mask = FILE_GENERIC_READ;
    Ace.Flags = 0;
    ReplaceAccessDacl(Control, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));

    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { CheckGranted(File, FILE_READ_DATA, FILE_WRITE_DATA); ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Path, FILE_WRITE_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Path, MAXIMUM_ALLOWED, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { CheckGranted(File, FILE_READ_DATA, FILE_WRITE_DATA | FILE_APPEND_DATA); ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Alias, FILE_WRITE_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Stream, FILE_READ_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Stream, FILE_WRITE_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Missing, FILE_READ_DATA, FILE_CREATE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = NtfsKmtOpen(&File, Missing, FILE_READ_ATTRIBUTES, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        IdName.Buffer = (PWCHAR)&Id;
        IdName.Length = IdName.MaximumLength = sizeof(Id);
        InitializeObjectAttributes(&Attributes, &IdName,
                                    OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_FORCE_ACCESS_CHECK, Volume, NULL);
        Status = ZwCreateFile(&File, FILE_WRITE_DATA | SYNCHRONIZE, &Attributes, &IoStatus,
                               NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_OPEN_BY_FILE_ID |
                               FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        ok_eq_hex(Status, STATUS_ACCESS_DENIED);
        if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
        ZwClose(Volume); Volume = NULL;
    }
    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OVERWRITE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = NtfsKmtQueryStandard(Control, &Standard);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ok_eq_longlong(Standard.EndOfFile.QuadPart, 4);

    /* MAXIMUM_ALLOWED must participate in sharing with its granted read
     * rights, rather than being treated as a metadata-only open. */
    ZwClose(Control); Control = NULL;
    Status = NtfsKmtCreate(&Blocker, NULL, Path, FILE_WRITE_DATA, FILE_ATTRIBUTE_NORMAL,
                            FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
                            NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ForceOpen(&File, Path, MAXIMUM_ALLOWED, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    if (Blocker != NULL) { ZwClose(Blocker); Blocker = NULL; }
    Status = NtfsKmtOpen(&Control, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    /* An empty DACL denies data, but the parent can grant attribute reads. */
    Sd = BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, NULL, 0, TRUE);
    if (Sd != NULL)
    {
        Empty = (PACL)((PUCHAR)Sd + sizeof(SECURITY_DESCRIPTOR));
        Status = RtlCreateAcl(Empty, sizeof(ACL), ACL_REVISION);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = RtlSetDaclSecurityDescriptor(Sd, TRUE, Empty, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ReplaceAccessDacl(Control, Sd);
    }
    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Path, FILE_READ_ATTRIBUTES, FILE_OPEN, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { CheckGranted(File, FILE_READ_ATTRIBUTES, FILE_READ_DATA | FILE_WRITE_DATA); ZwClose(File); File = NULL; }
    Status = ForceOpen(&File, Path, 0, FILE_OPEN, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { CheckGranted(File, 0, FILE_READ_DATA | FILE_WRITE_DATA); ZwClose(File); File = NULL; }
    /* Kernel-mode opens without FORCE_ACCESS_CHECK retain their bypass. */
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }

    Ace.Mask = FILE_ALL_ACCESS;
    ReplaceAccessDacl(Control, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
    Status = ForceOpen(&File, Alias, FILE_WRITE_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    /* Overwrite implies write access. A sharing denial must leave data
     * intact even when the explicit desired-access mask only asks to read. */
    ZwClose(Control); Control = NULL;
    Status = NtfsKmtCreate(&Blocker, NULL, Path, FILE_READ_DATA, FILE_ATTRIBUTE_NORMAL,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
                            NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OVERWRITE, TRUE);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    if (Blocker != NULL)
    {
        Status = NtfsKmtQueryStandard(Blocker, &Standard);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status)) ok_eq_longlong(Standard.EndOfFile.QuadPart, 4);
        ZwClose(Blocker); Blocker = NULL;
    }
    Status = NtfsKmtOpen(&Control, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    RtlZeroMemory(&Basic, sizeof(Basic));
    Basic.FileAttributes = FILE_ATTRIBUTE_READONLY;
    Status = NtfsKmtSetInfo(Control, &Basic, sizeof(Basic), FileBasicInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ForceOpen(&File, Path, MAXIMUM_ALLOWED, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckGranted(File, FILE_READ_DATA, FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD);
        ZwClose(File); File = NULL;
    }
    Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    Status = NtfsKmtSetInfo(Control, &Basic, sizeof(Basic), FileBasicInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OVERWRITE, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* Overwrite checks write permission and sharing, but the returned
         * handle retains the caller's explicitly requested access. */
        CheckGranted(File, FILE_READ_DATA, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA);
        Status = NtfsKmtQueryStandard(File, &Standard);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status)) ok_eq_longlong(Standard.EndOfFile.QuadPart, 0);
        ZwClose(File);
    }
    ZwClose(Control);
}

/* Use a private impersonation token so disabling traverse bypass cannot
 * change privileges of the test process or any other guest thread. */
static NTSTATUS
BeginNoTraverseToken(PHANDLE Previous, PHANDLE Restricted)
{
    HANDLE Source = NULL;
    OBJECT_ATTRIBUTES Attributes;
    SECURITY_QUALITY_OF_SERVICE Qos;
    TOKEN_PRIVILEGES Privileges;
    NTSTATUS Status;

    *Previous = NULL;
    *Restricted = NULL;
    Status = ZwOpenThreadToken(NtCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE,
                                TRUE, Previous);
    if (Status == STATUS_NO_TOKEN)
    {
        Status = ZwOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &Source);
        if (!NT_SUCCESS(Status)) return Status;
    }
    else if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(&Qos, sizeof(Qos));
    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_STATIC_TRACKING;
    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Attributes.SecurityQualityOfService = &Qos;
    Status = ZwDuplicateToken(Source != NULL ? Source : *Previous,
                               TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE,
                               &Attributes, FALSE, TokenImpersonation, Restricted);
    if (Source != NULL) ZwClose(Source);
    if (NT_SUCCESS(Status))
    {
        Privileges.PrivilegeCount = 1;
        Privileges.Privileges[0].Luid = RtlConvertLongToLuid(SE_CHANGE_NOTIFY_PRIVILEGE);
        Privileges.Privileges[0].Attributes = 0;
        Status = ZwAdjustPrivilegesToken(*Restricted, FALSE, &Privileges, 0, NULL, NULL);
    }
    if (Status == STATUS_SUCCESS)
        Status = ZwSetInformationThread(NtCurrentThread(), ThreadImpersonationToken, Restricted, sizeof(*Restricted));
    if (Status != STATUS_SUCCESS)
    {
        if (*Restricted != NULL) ZwClose(*Restricted);
        if (*Previous != NULL) ZwClose(*Previous);
        *Restricted = *Previous = NULL;
    }
    return Status;
}

static VOID
CheckAccessMissing(PCWSTR Path)
{
    HANDLE File;
    NTSTATUS Status = NtfsKmtOpen(&File, Path, FILE_READ_ATTRIBUTES, FILE_NON_DIRECTORY_FILE);

    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status)) ZwClose(File);
}

static VOID
TestParentAccess(PCWSTR Dir, WCHAR Letter)
{
    WCHAR Parent[MAX_PATH], Path[MAX_PATH], Stream[MAX_PATH];
    HANDLE Directory = NULL, File = NULL, Child = NULL, Volume = NULL;
    HANDLE Previous = NULL, Restricted = NULL;
    TEST_ACE Ace;
    NTSTATUS Status;
    ULONGLONG ChildId;

    NtfsKmtJoin(Parent, RTL_NUMBER_OF(Parent), Dir, L"parent-access");
    Status = NtfsKmtCreateDirectory(Parent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Directory, Parent, FILE_ALL_ACCESS, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Ace.Sid = SeExports->SeWorldSid;
    Ace.Flags = 0;
    Ace.Mask = FILE_GENERIC_READ | FILE_TRAVERSE;
    ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"denied.bin");
    Status = ForceCreate(&File, Path, FILE_ALL_ACCESS, FILE_CREATE, FILE_NON_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    CheckAccessMissing(Path);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"denied-dir");
    Status = ForceCreate(&File, Path, FILE_ALL_ACCESS, FILE_CREATE, FILE_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    CheckAccessMissing(Path);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"absent");
    RtlStringCchPrintfW(Stream, RTL_NUMBER_OF(Stream), L"%ls:ads", Path);
    Status = ForceCreate(&File, Stream, FILE_ALL_ACCESS, FILE_CREATE, FILE_NON_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    CheckAccessMissing(Path);

    Ace.Mask |= FILE_ADD_FILE;
    ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"allowed.bin");
    Status = ForceCreate(&File, Path, MAXIMUM_ALLOWED, FILE_CREATE, FILE_NON_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { CheckGranted(File, FILE_ALL_ACCESS, 0); ZwClose(File); File = NULL; }
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"still-denied-dir");
    Status = ForceCreate(&File, Path, FILE_ALL_ACCESS, FILE_CREATE, FILE_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    CheckAccessMissing(Path);

    Ace.Mask = FILE_GENERIC_READ | FILE_TRAVERSE | FILE_ADD_SUBDIRECTORY;
    ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"allowed-dir");
    Status = ForceCreate(&File, Path, FILE_ALL_ACCESS, FILE_CREATE, FILE_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"still-denied.bin");
    Status = ForceCreate(&File, Path, FILE_ALL_ACCESS, FILE_CREATE, FILE_NON_DIRECTORY_FILE, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    CheckAccessMissing(Path);

    /* Keep the child cached while changing only its ancestor's descriptor. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Parent, L"walk.bin");
    Status = NtfsKmtCreateFileWithData(Path, "walk", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Child, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto CleanupParent;
    ChildId = NtfsKmtQueryFileId(Child);
    Ace.Mask = FILE_GENERIC_READ;
    ReplaceAccessDacl(Child, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
    ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS); /* Default traverse bypass is enabled. */
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto CleanupParent;
    Status = BeginNoTraverseToken(&Previous, &Restricted);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS) goto CleanupParent;
    ok(!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_CHANGE_NOTIFY_PRIVILEGE), UserMode),
       "traverse bypass is still enabled on the private token\n");
    Status = ForceOpen(&File, Path, FILE_READ_DATA, FILE_OPEN, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status)) { ZwClose(File); File = NULL; }
    /* A direct file-ID open has no path ancestors to traverse. */
    Status = ForceOpenById(&File, Volume, ChildId, FILE_READ_DATA);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) { CheckGranted(File, FILE_READ_DATA, FILE_WRITE_DATA); ZwClose(File); File = NULL; }
CleanupParent:
    if (Restricted != NULL)
    {
        Status = ZwSetInformationThread(NtCurrentThread(), ThreadImpersonationToken, &Previous, sizeof(Previous));
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Restricted);
        if (Previous != NULL) ZwClose(Previous);
    }
    if (Volume != NULL) ZwClose(Volume);
    if (File != NULL) ZwClose(File);
    if (Child != NULL) ZwClose(Child);
    if (Directory != NULL) ZwClose(Directory);
}

/* Build the public set-information IRP with a user requestor so the driver
 * checks the caller, while the kernel test retains kernel handles/buffers. */
static NTSTATUS NTAPI
NamespaceCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
ReplaceNamespace(HANDLE Handle, FILE_INFORMATION_CLASS Class, PCWSTR Leaf)
{
    UCHAR Buffer[FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + 64 * sizeof(WCHAR)];
    PFILE_RENAME_INFORMATION Info = (PVOID)Buffer;
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    KEVENT Event;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(Handle, DELETE, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Device = IoGetRelatedDeviceObject(File);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (Irp == NULL) { ObDereferenceObject(File); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Info->ReplaceIfExists = TRUE;
    Info->FileNameLength = (ULONG)wcslen(Leaf) * sizeof(WCHAR);
    ASSERT(Info->FileNameLength <= 64 * sizeof(WCHAR));
    RtlCopyMemory(Info->FileName, Leaf, Info->FileNameLength);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->RequestorMode = UserMode;
    Irp->AssociatedIrp.SystemBuffer = Info;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_SET_INFORMATION;
    Stack->FileObject = File;
    Stack->Parameters.SetFile.FileInformationClass = Class;
    Stack->Parameters.SetFile.Length = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + Info->FileNameLength;
    Stack->Parameters.SetFile.ReplaceIfExists = TRUE;
    IoSetCompletionRoutine(Irp, NamespaceCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
    IoFreeIrp(Irp);
    ObDereferenceObject(File);
    return Status;
}

static VOID
TestReplacementAccess(PCWSTR Dir)
{
    WCHAR Parent[MAX_PATH], Source[MAX_PATH], Target[MAX_PATH];
    HANDLE Directory = NULL, File = NULL, Victim = NULL;
    TEST_ACE Ace;
    ULONGLONG SourceId, TargetId;
    NTSTATUS Status;
    ULONG i;

    NtfsKmtJoin(Parent, RTL_NUMBER_OF(Parent), Dir, L"replace-access");
    Status = NtfsKmtCreateDirectory(Parent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Directory, Parent, FILE_ALL_ACCESS, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Parent, L"source.bin");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Parent, L"victim.bin");
    Ace.Sid = SeExports->SeWorldSid;
    Ace.Flags = 0;
    for (i = 0; i < 3; i++)
    {
        Ace.Mask = FILE_GENERIC_READ | FILE_TRAVERSE | FILE_ADD_FILE;
        ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
        Status = NtfsKmtCreateFileWithData(Source, "source", 6);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtCreateFileWithData(Target, "target", 6);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        SourceId = NtfsKmtQueryFileId(File);
        Status = NtfsKmtOpen(&Victim, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        TargetId = NtfsKmtQueryFileId(Victim);
        Ace.Mask = FILE_GENERIC_READ;
        ReplaceAccessDacl(Victim, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
        ZwClose(Victim); Victim = NULL;
        Status = ReplaceNamespace(File, i == 2 ? FileLinkInformation : FileRenameInformation, L"victim.bin");
        ok_eq_hex(Status, STATUS_ACCESS_DENIED);
        Status = NtfsKmtOpen(&Victim, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        ok(NtfsKmtQueryFileId(Victim) == TargetId, "denied replacement changed the target identity\n");
        ok(NtfsKmtQueryFileId(File) == SourceId, "denied replacement changed the source identity\n");
        if (i == 0)
        {
            Ace.Mask = FILE_GENERIC_READ | DELETE;
            ReplaceAccessDacl(Victim, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
        }
        else
        {
            Ace.Mask = FILE_GENERIC_READ | FILE_TRAVERSE | FILE_ADD_FILE | FILE_DELETE_CHILD;
            ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
        }
        ZwClose(Victim); Victim = NULL;
        Status = ReplaceNamespace(File, i == 2 ? FileLinkInformation : FileRenameInformation, L"victim.bin");
        ok(Status == STATUS_SUCCESS, "Replacement mode %lu returned %lx\n", i, Status);
        ZwClose(File); File = NULL;
        Status = NtfsKmtOpen(&Victim, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        ok(NtfsKmtQueryFileId(Victim) == SourceId, "authorized replacement did not install source\n");
        ZwClose(Victim); Victim = NULL;
        if (i != 2) CheckAccessMissing(Source);
        Status = NtfsKmtDeleteTree(Target);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    /* A relative rename creates a new link without a target-directory open
     * from the I/O manager. File and directory add rights remain distinct. */
    for (i = 0; i < 2; i++)
    {
        NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Parent, i == 0 ? L"source.bin" : L"source-dir");
        NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Parent, L"new-name");
        if (i != 0)
        {
            Status = NtfsKmtCreateDirectory(Source);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS,
                              i == 0 ? FILE_NON_DIRECTORY_FILE : FILE_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;
        Ace.Mask = FILE_GENERIC_READ | FILE_TRAVERSE |
                   (i == 0 ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE);
        ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
        Status = ReplaceNamespace(File, FileRenameInformation, i == 0 ? L"source.bin" : L"source-dir");
        ok_eq_hex(Status, STATUS_SUCCESS); /* Exact no-op needs no add right. */
        Status = ReplaceNamespace(File, FileRenameInformation, L"new-name");
        ok_eq_hex(Status, STATUS_ACCESS_DENIED);
        CheckAccessMissing(Target);
        Ace.Mask |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        ReplaceAccessDacl(Directory, BuildSd(SeExports->SeAliasAdminsSid, NULL, TRUE, &Ace, 1, TRUE));
        Status = ReplaceNamespace(File, FileRenameInformation, L"new-name");
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File); File = NULL;
        CheckAccessMissing(Source);
        Status = NtfsKmtDeleteTree(Target);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Victim != NULL) ZwClose(Victim);
    if (File != NULL) ZwClose(File);
    ZwClose(Directory);
}

START_TEST(NtfsSecurity)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH], Letter;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_security", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestDefaultAndRoundTrip(Dir);
    TestInheritance(Dir);
    TestStoredAccess(Dir, Letter);
    TestParentAccess(Dir, Letter);
    TestReplacementAccess(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
