/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Manifest-based ETW event provider registration
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * Providers register here and get a real registration handle back. ReactOS
 * has no ETW session support, so no session ever enables a provider: the
 * enable callback is never invoked, EtwEventEnabled reports FALSE and writing
 * an event to a valid handle succeeds without anything consuming it - which
 * is what Windows does for a provider nobody is tracing.
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#include <ndk/sefuncs.h>
#include <evntprov.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

typedef struct _ETWP_REGISTRATION
{
    LIST_ENTRY ListEntry;
    GUID ProviderId;
    PENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
    USHORT Sequence;
} ETWP_REGISTRATION, *PETWP_REGISTRATION;

static RTL_SRWLOCK EtwpRegistrationLock = RTL_SRWLOCK_INIT;
static LIST_ENTRY EtwpRegistrationList = { &EtwpRegistrationList, &EtwpRegistrationList };
static USHORT EtwpRegistrationSequence;

/* The upper 16 bits of a handle carry a nonzero sequence number */
#define ETWP_HANDLE_SEQUENCE(Handle)    ((USHORT)((Handle) >> 48))
#define ETWP_HANDLE_POINTER(Handle)     ((PETWP_REGISTRATION)(ULONG_PTR)((Handle) & 0xFFFFFFFFFFFFULL))

/* PRIVATE FUNCTIONS ********************************************************/

/* Must be called with the registration lock held */
static
PETWP_REGISTRATION
EtwpLookupRegistration(
    _In_ REGHANDLE RegHandle)
{
    PETWP_REGISTRATION Registration, Candidate;
    PLIST_ENTRY Entry;

    if ((ETWP_HANDLE_SEQUENCE(RegHandle) == 0) || (RegHandle & 1))
        return NULL;

    Candidate = ETWP_HANDLE_POINTER(RegHandle);
    for (Entry = EtwpRegistrationList.Flink;
         Entry != &EtwpRegistrationList;
         Entry = Entry->Flink)
    {
        Registration = CONTAINING_RECORD(Entry, ETWP_REGISTRATION, ListEntry);
        if ((Registration == Candidate) &&
            (Registration->Sequence == ETWP_HANDLE_SEQUENCE(RegHandle)))
        {
            return Registration;
        }
    }

    return NULL;
}

static
BOOLEAN
EtwpIsValidRegistration(
    _In_ REGHANDLE RegHandle)
{
    BOOLEAN Valid;

    RtlAcquireSRWLockShared(&EtwpRegistrationLock);
    Valid = (EtwpLookupRegistration(RegHandle) != NULL);
    RtlReleaseSRWLockShared(&EtwpRegistrationLock);

    return Valid;
}

/* A new, locally unique activity identifier */
static
NTSTATUS
EtwpCreateActivityId(
    _Out_ LPGUID ActivityId)
{
    ULARGE_INTEGER Time;
    ULONG Range, Sequence;
    UCHAR Seed[6];
    NTSTATUS Status;

    /* UUIDs from the system UUID allocator are unique on this machine */
    Status = NtAllocateUuids(&Time, &Range, &Sequence, Seed);
    if (!NT_SUCCESS(Status))
        return Status;

    ActivityId->Data1 = Time.LowPart;
    ActivityId->Data2 = (USHORT)Time.HighPart;
    ActivityId->Data3 = (USHORT)((Time.HighPart >> 16) & 0x0FFF) | (1 << 12);
    ActivityId->Data4[0] = (UCHAR)((Sequence >> 8) & 0x3F) | 0x80;
    ActivityId->Data4[1] = (UCHAR)Sequence;
    RtlCopyMemory(&ActivityId->Data4[2], Seed, sizeof(Seed));
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS *********************************************************/

ULONG
NTAPI
EtwEventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_REGISTRATION Registration;

    if ((ProviderId == NULL) || (RegHandle == NULL))
        return ERROR_INVALID_PARAMETER;

    Registration = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Registration));
    if (Registration == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    /* Heap blocks are aligned, which keeps bit 0 of the handle clear */
    ASSERT(((ULONG_PTR)Registration & 1) == 0);

    Registration->ProviderId = *ProviderId;
    Registration->EnableCallback = EnableCallback;
    Registration->CallbackContext = CallbackContext;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    if (++EtwpRegistrationSequence == 0)
        EtwpRegistrationSequence = 1;
    Registration->Sequence = EtwpRegistrationSequence;
    InsertTailList(&EtwpRegistrationList, &Registration->ListEntry);
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    *RegHandle = ((REGHANDLE)Registration->Sequence << 48) |
                 (REGHANDLE)(ULONG_PTR)Registration;
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETWP_REGISTRATION Registration;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    Registration = EtwpLookupRegistration(RegHandle);
    if (Registration)
        RemoveEntryList(&Registration->ListEntry);
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    if (Registration == NULL)
        return ERROR_INVALID_HANDLE;

    RtlFreeHeap(RtlGetProcessHeap(), 0, Registration);
    return ERROR_SUCCESS;
}

BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);

    /* No trace session can enable a provider */
    return FALSE;
}

BOOLEAN
NTAPI
EtwEventProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);

    /* No trace session can enable a provider */
    return FALSE;
}

ULONG
NTAPI
EtwEventWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_range_(0, MAX_EVENT_DATA_DESCRIPTORS) ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);
    UNREFERENCED_PARAMETER(UserDataCount);
    UNREFERENCED_PARAMETER(UserData);

    if (EventDescriptor == NULL)
        return ERROR_INVALID_PARAMETER;

    if (!EtwpIsValidRegistration(RegHandle))
        return ERROR_INVALID_HANDLE;

    /* The provider is not enabled by any session: nothing to log */
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_range_(0, MAX_EVENT_DATA_DESCRIPTORS) ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwEventWriteTransfer(RegHandle,
                                 EventDescriptor,
                                 NULL,
                                 NULL,
                                 UserDataCount,
                                 UserData);
}

ULONG
NTAPI
EtwEventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    UNREFERENCED_PARAMETER(String);

    if (!EtwpIsValidRegistration(RegHandle))
        return ERROR_INVALID_HANDLE;

    /* The provider is not enabled by any session: nothing to log */
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ EVENT_INFO_CLASS InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(EventInformation);
    UNREFERENCED_PARAMETER(InformationLength);

    if (!EtwpIsValidRegistration(RegHandle))
        return ERROR_INVALID_HANDLE;

    switch (InformationClass)
    {
        case EventProviderBinaryTrackInfo:
        case EventProviderSetTraits:
        case EventProviderUseDescriptorType:
            /* These only shape what a session receives; there are none */
            return ERROR_SUCCESS;

        default:
            return ERROR_NOT_SUPPORTED;
    }
}

ULONG
NTAPI
EtwEventActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    PTEB Teb = NtCurrentTeb();
    GUID Previous;

    if (ActivityId == NULL)
        return ERROR_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            *ActivityId = Teb->ActivityId;
            break;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            Teb->ActivityId = *ActivityId;
            break;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
            return NT_SUCCESS(EtwpCreateActivityId(ActivityId)) ?
                   ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            Previous = Teb->ActivityId;
            Teb->ActivityId = *ActivityId;
            *ActivityId = Previous;
            break;

        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            Previous = Teb->ActivityId;
            if (!NT_SUCCESS(EtwpCreateActivityId(&Teb->ActivityId)))
                return ERROR_NOT_ENOUGH_MEMORY;
            *ActivityId = Previous;
            break;

        default:
            return ERROR_INVALID_PARAMETER;
    }

    return ERROR_SUCCESS;
}

/* EOF */
