
#include "k32_vista.h"
#include <ndk/exfuncs.h>
#include "../include/base_x.h"

/*
 * @implemented
 */
HANDLE
WINAPI
DECLSPEC_HOTPATCH
CreateEventExA(IN LPSECURITY_ATTRIBUTES lpEventAttributes OPTIONAL,
               IN LPCSTR lpName OPTIONAL,
               IN DWORD dwFlags,
               IN DWORD dwDesiredAccess)
{
    ANSI_STRING AnsiName;
    UNICODE_STRING UnicodeName;
    NTSTATUS Status;
    HANDLE Handle;

    if (!lpName)
        return CreateEventExW(lpEventAttributes, NULL, dwFlags, dwDesiredAccess);

    RtlInitAnsiString(&AnsiName, lpName);
    Status = RtlAnsiStringToUnicodeString(&UnicodeName, &AnsiName, TRUE);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return NULL;
    }

    Handle = CreateEventExW(lpEventAttributes, UnicodeName.Buffer, dwFlags, dwDesiredAccess);
    RtlFreeUnicodeString(&UnicodeName);
    return Handle;
}

/*
 * @implemented
 */
HANDLE
WINAPI
DECLSPEC_HOTPATCH
CreateEventExW(IN LPSECURITY_ATTRIBUTES lpEventAttributes OPTIONAL,
               IN LPCWSTR lpName OPTIONAL,
               IN DWORD dwFlags,
               IN DWORD dwDesiredAccess)
{
    CreateNtObjectFromWin32Api(Event, Event, dwDesiredAccess,
                               lpEventAttributes,
                               lpName,
                               (dwFlags & CREATE_EVENT_MANUAL_RESET) ? NotificationEvent : SynchronizationEvent,
                               (dwFlags & CREATE_EVENT_INITIAL_SET) != 0);
}
