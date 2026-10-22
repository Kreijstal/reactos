/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Etw functions of Vista+
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 Kreijstal <rainb@tfwno.gf>
 */

#include <ntdef.h>
#include <ntifs.h>

/*
 * ReactOS has no ETW sessions. Providers can register and write events the
 * way they do on Windows when no trace session has enabled them: registration
 * hands out a real handle, nothing is ever enabled, so the enable callback is
 * never called, and writes succeed without logging anything.
 */

#define TAG_ETW 'gRwE'
#define ETW_REG_SIGNATURE 'gRwE'

typedef struct _ETW_REGISTRATION
{
    ULONG Signature;
    GUID ProviderId;
    PETWENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
} ETW_REGISTRATION, *PETW_REGISTRATION;

static
PETW_REGISTRATION
EtwpReferenceRegistration(
    _In_ REGHANDLE RegHandle)
{
    PETW_REGISTRATION Registration = (PETW_REGISTRATION)(ULONG_PTR)RegHandle;

    if (!Registration || (Registration->Signature != ETW_REG_SIGNATURE))
        return NULL;

    return Registration;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETW_REGISTRATION Registration;

    if (!ProviderId || !RegHandle)
        return STATUS_INVALID_PARAMETER;

    *RegHandle = 0;

    Registration = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Registration), TAG_ETW);
    if (!Registration)
        return STATUS_NO_MEMORY;

    Registration->Signature = ETW_REG_SIGNATURE;
    Registration->ProviderId = *ProviderId;
    Registration->EnableCallback = EnableCallback;
    Registration->CallbackContext = CallbackContext;

    *RegHandle = (REGHANDLE)(ULONG_PTR)Registration;
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETW_REGISTRATION Registration = EtwpReferenceRegistration(RegHandle);

    if (!Registration)
        return STATUS_INVALID_HANDLE;

    Registration->Signature = 0;
    ExFreePoolWithTag(Registration, TAG_ETW);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTKRNLVISTAAPI
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    /* No session can enable a provider */
    return FALSE;
}

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTKRNLVISTAAPI
NTAPI
EtwProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    return FALSE;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, NULL,
                            UserDataCount, UserData);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    if (!EtwpReferenceRegistration(RegHandle))
        return STATUS_INVALID_HANDLE;

    if (!EventDescriptor || (UserDataCount > MAX_EVENT_DATA_DESCRIPTORS) ||
        (UserDataCount && !UserData))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Nobody is listening */
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, RelatedActivityId,
                            UserDataCount, UserData);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_opt_ LPCGUID ActivityId,
    _In_ PCWSTR String)
{
    if (!EtwpReferenceRegistration(RegHandle))
        return STATUS_INVALID_HANDLE;

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ EVENT_INFO_CLASS InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    if (!EtwpReferenceRegistration(RegHandle))
        return STATUS_INVALID_HANDLE;

    switch (InformationClass)
    {
        case EventProviderBinaryTrackInfo:
        case EventProviderSetTraits:
        case EventProviderUseDescriptorType:
            /* Accepted; there is no session to forward them to */
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}
