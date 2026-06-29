/*
 * Locale support
 *
 * Copyright 1995 Martin von Loewis
 * Copyright 1998 David Lee Lambert
 * Copyright 2000 Julio César Gázquez
 * Copyright 2002 Alexandre Julliard for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define WIN32_NO_STATUS
#include <wine/unicode.h>

#undef WIN32_NO_STATUS
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <ndk/rtlfuncs.h>

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
RtlNormalizeString(
    _In_ ULONG NormForm,
    _In_ PCWSTR SourceString,
    _In_ LONG SourceStringLength,
    _Out_writes_to_(*DestinationStringLength, *DestinationStringLength) PWSTR DestinationString,
    _Inout_ PLONG DestinationStringLength);

NTSTATUS
NTAPI
RtlIsNormalizedString(
    _In_ ULONG NormForm,
    _In_ PCWSTR SourceString,
    _In_ LONG SourceStringLength,
    _Out_ PBOOLEAN Normalized);

/* Taken from Wine kernel32/locale.c */

/******************************************************************************
 *           NormalizeString (KERNEL32.@)
 */
INT WINAPI NormalizeString(NORM_FORM NormForm, LPCWSTR lpSrcString, INT cwSrcLength,
                           LPWSTR lpDstString, INT cwDstLength)
{
    LONG DstLength = cwDstLength;
    NTSTATUS Status;

    Status = RtlNormalizeString(NormForm, lpSrcString, cwSrcLength, lpDstString, &DstLength);
    if (NT_SUCCESS(Status))
    {
        SetLastError(ERROR_SUCCESS);
        return DstLength;
    }

    switch (Status)
    {
        case STATUS_OBJECT_NAME_NOT_FOUND:
        case STATUS_INVALID_PARAMETER:
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;

        case STATUS_BUFFER_TOO_SMALL:
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return -DstLength;

        case STATUS_NO_UNICODE_TRANSLATION:
            SetLastError(ERROR_NO_UNICODE_TRANSLATION);
            return -DstLength;

        default:
            SetLastError(RtlNtStatusToDosError(Status));
            return 0;
    }
}

/******************************************************************************
 *           IsNormalizedString (KERNEL32.@)
 */
BOOL WINAPI IsNormalizedString(NORM_FORM NormForm, LPCWSTR lpString, INT cwLength)
{
    BOOLEAN Normalized;
    NTSTATUS Status;

    Status = RtlIsNormalizedString(NormForm, lpString, cwLength, &Normalized);
    if (NT_SUCCESS(Status))
    {
        SetLastError(ERROR_SUCCESS);
        return Normalized;
    }

    SetLastError((Status == STATUS_OBJECT_NAME_NOT_FOUND) ? ERROR_INVALID_PARAMETER :
                 RtlNtStatusToDosError(Status));
    return FALSE;
}
