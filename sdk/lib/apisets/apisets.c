/*
 * PROJECT:     ReactOS apisets
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Resolving the apiset to a ReactOS system dll
 * COPYRIGHT:   Copyright 2024 Mark Jansen <mark.jansen@reactos.org>
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include "apisetsp.h"


const ULONGLONG API_ = (ULONGLONG)0x2D004900500041; /// L"API-"
const ULONGLONG EXT_ = (ULONGLONG)0x2D005400580045; /// L"EXT-";

WORD PrefixSize = sizeof(L"api-") - sizeof(WCHAR);
WORD ExtensionSize = sizeof(L".dll") - sizeof(WCHAR);


// The official prototype according to the Windows kit 8.1 is:
//NTSTATUS
//ApiSetResolveToHost (
//    _In_ PCAPI_SET_NAMESPACE_ARRAY Schema,
//    _In_ PCUNICODE_STRING FileNameIn,
//    _In_opt_ PCUNICODE_STRING ParentName,
//    _Out_ PBOOLEAN Resolved,
//    _Out_ PUNICODE_STRING HostBinary
//    );


/* Cuts the trailing "-<version>" off an apiset name */
static
VOID
ApiSetBaseLength(
    _Inout_ PUNICODE_STRING Name)
{
    USHORT Index = Name->Length / sizeof(WCHAR);

    while (Index > 0)
    {
        Index--;
        if (Name->Buffer[Index] == L'-')
        {
            Name->Length = Index * sizeof(WCHAR);
            return;
        }
    }
}


NTSTATUS
ApiSetResolveToHost(
    _In_ DWORD ApisetVersion,
    _In_ PCUNICODE_STRING ApiToResolve,
    _Out_ PBOOLEAN Resolved,
    _Out_ PUNICODE_STRING Output
)
{
    if (ApiToResolve->Length < PrefixSize)
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // Grab the first four chars from the string, converting the first 3 to uppercase
    PWSTR ApiSetNameBuffer = ApiToResolve->Buffer;
    ULONGLONG ApiSetNameBufferPrefix = ((ULONGLONG *)ApiSetNameBuffer)[0] & 0xFFFFFFDFFFDFFFDF;
    // Check if it matches either 'api-' or 'ext-'
    if (!(ApiSetNameBufferPrefix == API_ || ApiSetNameBufferPrefix == EXT_))
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // If there is an extension, cut it off (we store apisets without extension)
    UNICODE_STRING Tmp = *ApiToResolve;
    const WCHAR *Extension = Tmp.Buffer + (Tmp.Length - ExtensionSize) / sizeof(WCHAR);
    if (!_wcsnicmp(Extension, L".dll", ExtensionSize / sizeof(WCHAR)))
        Tmp.Length -= ExtensionSize;

    DWORD VersionMask = ApisetVersion | (ApisetVersion - 1);
    const ROS_APISET *Match = NULL;

    /* An exact name wins. Win10/11 inherit every downlevel apiset name as a
     * forwarder alias, so match an entry if the requested version or any
     * older bit is set on it. The auto-generated table only marks the
     * highest Windows release that exposes a given name in its
     * apisetschema, but the real schema contains all earlier names as
     * downlevel aliases pointing at the same host DLL. */
    for (LONG Index = 0; Index < g_ApisetsCount; Index++)
    {
        if (RtlEqualUnicodeString(&Tmp, &g_Apisets[Index].Name, TRUE) &&
            (g_Apisets[Index].dwOsVersions & VersionMask))
        {
            Match = &g_Apisets[Index];
            break;
        }
    }

    /* Like Windows, the version after the last hyphen does not take part
     * in the lookup otherwise: "api-ms-win-core-file-l1-1-0" resolves
     * through the schema entry for "api-ms-win-core-file-l1-1", whatever
     * minor version the system ships. Prefer the entry of the version we
     * run as, then any older one. */
    if (Match == NULL)
    {
        UNICODE_STRING Base = Tmp;
        ApiSetBaseLength(&Base);

        for (LONG Index = 0; Index < g_ApisetsCount; Index++)
        {
            UNICODE_STRING EntryBase = g_Apisets[Index].Name;
            ApiSetBaseLength(&EntryBase);

            if (!RtlEqualUnicodeString(&Base, &EntryBase, TRUE))
                continue;

            if (g_Apisets[Index].dwOsVersions & ApisetVersion)
            {
                Match = &g_Apisets[Index];
                break;
            }
            if (!Match && (g_Apisets[Index].dwOsVersions & VersionMask))
                Match = &g_Apisets[Index];
        }
    }

    if (Match)
    {
        // Return a static string (does not have to be freed)
        *Resolved = TRUE;
        *Output = Match->Target;
        return STATUS_SUCCESS;
    }

    *Resolved = FALSE;
    return STATUS_SUCCESS;
}
