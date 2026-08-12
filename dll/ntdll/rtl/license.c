/*
 * PROJECT: ReactOS Native Library
 * PURPOSE: License policy value queries
 */

#include <ntdll.h>

NTSTATUS
NTAPI
NtQueryLicenseValue(
    _In_ PUNICODE_STRING ValueName,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(DataSize) PVOID Data,
    _In_ ULONG DataSize,
    _Out_ PULONG ResultDataSize)
{
    static const UNICODE_STRING LanguageName =
        RTL_CONSTANT_STRING(L"Kernel-MUI-Language-Allowed");
    static const UNICODE_STRING NumberName =
        RTL_CONSTANT_STRING(L"Kernel-MUI-Number-Allowed");
    static const WCHAR LanguageValue[] = L"EMPTY";
    static const ULONG NumberValue = 1;
    const VOID *Value;
    ULONG ValueType, ValueSize;

    if (!ValueName || !ResultDataSize)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        if (!ValueName->Buffer || !ValueName->Length ||
            (ValueName->Length & (sizeof(WCHAR) - 1)) ||
            ValueName->MaximumLength < ValueName->Length)
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }

        if (RtlEqualUnicodeString(ValueName, &LanguageName, TRUE))
        {
            Value = LanguageValue;
            ValueSize = sizeof(LanguageValue);
            ValueType = REG_SZ;
        }
        else if (RtlEqualUnicodeString(ValueName, &NumberName, TRUE))
        {
            Value = &NumberValue;
            ValueSize = sizeof(NumberValue);
            ValueType = REG_DWORD;
        }
        else
        {
            _SEH2_YIELD(return STATUS_OBJECT_NAME_NOT_FOUND);
        }

        if (Type)
            *Type = ValueType;
        *ResultDataSize = ValueSize;

        if (DataSize < ValueSize)
            _SEH2_YIELD(return STATUS_BUFFER_TOO_SMALL);
        if (!Data)
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);

        RtlCopyMemory(Data, Value, ValueSize);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}
