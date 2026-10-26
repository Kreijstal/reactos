/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boundary descriptors and private namespaces
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/*
 * The kernel finds a private namespace by its boundary descriptor and hands
 * out a directory handle. The alias prefix belongs to the process: a name
 * such as "Alias\Object" passed to CreateMutex, OpenEvent and friends is
 * looked up relative to the namespace created or opened with that alias.
 */
typedef struct _BASEP_PRIVATE_NAMESPACE
{
    LIST_ENTRY ListEntry;
    HANDLE Handle;
    UNICODE_STRING Alias;
} BASEP_PRIVATE_NAMESPACE, *PBASEP_PRIVATE_NAMESPACE;

static LIST_ENTRY BasepPrivateNamespaceList = { &BasepPrivateNamespaceList, &BasepPrivateNamespaceList };
RTL_CRITICAL_SECTION BasepPrivateNamespaceLock;

/* PRIVATE FUNCTIONS **********************************************************/

/*
 * If Name starts with the alias of one of our private namespaces and a
 * backslash, strip that prefix and return the namespace directory to
 * resolve the rest of the name in. The latest namespace with an alias wins.
 */
HANDLE
WINAPI
BasepGetPrivateNamespaceDirectory(
    _Inout_ PUNICODE_STRING Name)
{
    UNICODE_STRING Prefix;
    PLIST_ENTRY ListEntry;
    HANDLE Directory = NULL;
    USHORT i;

    if (IsListEmpty(&BasepPrivateNamespaceList))
        return NULL;

    for (i = 0; i < Name->Length / sizeof(WCHAR); i++)
    {
        if (Name->Buffer[i] == OBJ_NAME_PATH_SEPARATOR)
            break;
    }
    if (i == 0 || i == Name->Length / sizeof(WCHAR))
        return NULL;

    Prefix.Buffer = Name->Buffer;
    Prefix.Length = Prefix.MaximumLength = i * sizeof(WCHAR);

    RtlEnterCriticalSection(&BasepPrivateNamespaceLock);
    for (ListEntry = BasepPrivateNamespaceList.Blink;
         ListEntry != &BasepPrivateNamespaceList;
         ListEntry = ListEntry->Blink)
    {
        PBASEP_PRIVATE_NAMESPACE Namespace =
            CONTAINING_RECORD(ListEntry, BASEP_PRIVATE_NAMESPACE, ListEntry);
        if (RtlEqualUnicodeString(&Namespace->Alias, &Prefix, TRUE))
        {
            Directory = Namespace->Handle;
            break;
        }
    }
    RtlLeaveCriticalSection(&BasepPrivateNamespaceLock);

    if (Directory)
    {
        Name->Buffer += i + 1;
        Name->Length -= (i + 1) * sizeof(WCHAR);
        Name->MaximumLength = Name->Length;
    }
    return Directory;
}

/* Root directory for a Win32 object name: a private namespace or BaseNamedObjects */
HANDLE
WINAPI
BasepGetObjectRootDirectory(
    _Inout_ PUNICODE_STRING Name)
{
    HANDLE Directory = BasepGetPrivateNamespaceDirectory(Name);
    return Directory ? Directory : BasepGetNamedObjectDirectory();
}

#if (DLL_EXPORT_VERSION >= _WIN32_WINNT_VISTA)

static
HANDLE
BasepAddPrivateNamespace(
    _In_ HANDLE Handle,
    _In_ LPCWSTR AliasPrefix)
{
    PBASEP_PRIVATE_NAMESPACE Namespace;
    SIZE_T Length = wcslen(AliasPrefix) * sizeof(WCHAR);

    /* An alias is a single, non-empty name component */
    if (Length == 0 || Length > UNICODE_STRING_MAX_BYTES || wcschr(AliasPrefix, OBJ_NAME_PATH_SEPARATOR))
    {
        NtClose(Handle);
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    Namespace = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Namespace) + Length);
    if (!Namespace)
    {
        NtClose(Handle);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    Namespace->Handle = Handle;
    Namespace->Alias.Buffer = (PWSTR)(Namespace + 1);
    Namespace->Alias.Length = Namespace->Alias.MaximumLength = (USHORT)Length;
    RtlCopyMemory(Namespace->Alias.Buffer, AliasPrefix, Length);

    RtlEnterCriticalSection(&BasepPrivateNamespaceLock);
    InsertTailList(&BasepPrivateNamespaceList, &Namespace->ListEntry);
    RtlLeaveCriticalSection(&BasepPrivateNamespaceLock);

    SetLastError(ERROR_SUCCESS);
    return Handle;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
HANDLE
WINAPI
CreateBoundaryDescriptorW(
    _In_ LPCWSTR Name,
    _In_ ULONG Flags)
{
    UNICODE_STRING NameString;
    POBJECT_BOUNDARY_DESCRIPTOR Boundary;

    if (!Name || !*Name || !NT_SUCCESS(RtlInitUnicodeStringEx(&NameString, Name)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    Boundary = RtlCreateBoundaryDescriptor(&NameString, Flags);
    if (!Boundary)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return Boundary;
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreateBoundaryDescriptorA(
    _In_ LPCSTR Name,
    _In_ ULONG Flags)
{
    PUNICODE_STRING NameString = Basep8BitStringToStaticUnicodeString(Name);
    if (!NameString)
        return NULL;
    return CreateBoundaryDescriptorW(NameString->Buffer, Flags);
}

/*
 * @implemented
 */
BOOL
WINAPI
AddSIDToBoundaryDescriptor(
    _Inout_ HANDLE *BoundaryDescriptor,
    _In_ PSID RequiredSid)
{
    NTSTATUS Status;

    Status = RtlAddSIDToBoundaryDescriptor((POBJECT_BOUNDARY_DESCRIPTOR *)BoundaryDescriptor, RequiredSid);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
AddIntegrityLabelToBoundaryDescriptor(
    _Inout_ HANDLE *BoundaryDescriptor,
    _In_ PSID IntegrityLabel)
{
    NTSTATUS Status;

    Status = RtlAddIntegrityLabelToBoundaryDescriptor((POBJECT_BOUNDARY_DESCRIPTOR *)BoundaryDescriptor,
                                                      IntegrityLabel);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 */
VOID
WINAPI
DeleteBoundaryDescriptor(
    _In_ HANDLE BoundaryDescriptor)
{
    RtlDeleteBoundaryDescriptor(BoundaryDescriptor);
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreatePrivateNamespaceW(
    _In_opt_ LPSECURITY_ATTRIBUTES lpPrivateNamespaceAttributes,
    _In_ LPVOID lpBoundaryDescriptor,
    _In_ LPCWSTR lpAliasPrefix)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;
    NTSTATUS Status;

    if (!lpBoundaryDescriptor || !lpAliasPrefix)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               (lpPrivateNamespaceAttributes &&
                                lpPrivateNamespaceAttributes->bInheritHandle) ? OBJ_INHERIT : 0,
                               NULL,
                               lpPrivateNamespaceAttributes ?
                                   lpPrivateNamespaceAttributes->lpSecurityDescriptor : NULL);

    Status = NtCreatePrivateNamespace(&Handle, DIRECTORY_ALL_ACCESS, &ObjectAttributes, lpBoundaryDescriptor);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return NULL;
    }

    return BasepAddPrivateNamespace(Handle, lpAliasPrefix);
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreatePrivateNamespaceA(
    _In_opt_ LPSECURITY_ATTRIBUTES lpPrivateNamespaceAttributes,
    _In_ LPVOID lpBoundaryDescriptor,
    _In_ LPCSTR lpAliasPrefix)
{
    PUNICODE_STRING AliasPrefix = Basep8BitStringToStaticUnicodeString(lpAliasPrefix);
    if (!AliasPrefix)
        return NULL;
    return CreatePrivateNamespaceW(lpPrivateNamespaceAttributes, lpBoundaryDescriptor, AliasPrefix->Buffer);
}

/*
 * @implemented
 */
HANDLE
WINAPI
OpenPrivateNamespaceW(
    _In_ LPVOID lpBoundaryDescriptor,
    _In_ LPCWSTR lpAliasPrefix)
{
    HANDLE Handle;
    NTSTATUS Status;

    if (!lpBoundaryDescriptor || !lpAliasPrefix)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    Status = NtOpenPrivateNamespace(&Handle, DIRECTORY_ALL_ACCESS, NULL, lpBoundaryDescriptor);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return NULL;
    }

    return BasepAddPrivateNamespace(Handle, lpAliasPrefix);
}

/*
 * @implemented
 */
HANDLE
WINAPI
OpenPrivateNamespaceA(
    _In_ LPVOID lpBoundaryDescriptor,
    _In_ LPCSTR lpAliasPrefix)
{
    PUNICODE_STRING AliasPrefix = Basep8BitStringToStaticUnicodeString(lpAliasPrefix);
    if (!AliasPrefix)
        return NULL;
    return OpenPrivateNamespaceW(lpBoundaryDescriptor, AliasPrefix->Buffer);
}

/*
 * @implemented
 */
BOOLEAN
WINAPI
ClosePrivateNamespace(
    _In_ HANDLE Handle,
    _In_ ULONG Flags)
{
    PLIST_ENTRY ListEntry;
    PBASEP_PRIVATE_NAMESPACE Namespace = NULL;
    NTSTATUS Status;

    if (Flags & ~PRIVATE_NAMESPACE_FLAG_DESTROY)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlEnterCriticalSection(&BasepPrivateNamespaceLock);
    for (ListEntry = BasepPrivateNamespaceList.Flink;
         ListEntry != &BasepPrivateNamespaceList;
         ListEntry = ListEntry->Flink)
    {
        PBASEP_PRIVATE_NAMESPACE Candidate =
            CONTAINING_RECORD(ListEntry, BASEP_PRIVATE_NAMESPACE, ListEntry);
        if (Candidate->Handle == Handle)
        {
            RemoveEntryList(&Candidate->ListEntry);
            Namespace = Candidate;
            break;
        }
    }
    RtlLeaveCriticalSection(&BasepPrivateNamespaceLock);

    if (Flags & PRIVATE_NAMESPACE_FLAG_DESTROY)
    {
        Status = NtDeletePrivateNamespace(Handle);
        if (!NT_SUCCESS(Status))
        {
            /* Not destroyed: the namespace stays usable */
            if (Namespace)
            {
                RtlEnterCriticalSection(&BasepPrivateNamespaceLock);
                InsertTailList(&BasepPrivateNamespaceList, &Namespace->ListEntry);
                RtlLeaveCriticalSection(&BasepPrivateNamespaceLock);
            }
            BaseSetLastNTError(Status);
            return FALSE;
        }
    }

    if (Namespace)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Namespace);

    Status = NtClose(Handle);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}

#endif /* DLL_EXPORT_VERSION >= _WIN32_WINNT_VISTA */

/* EOF */
