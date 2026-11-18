/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel Shim Engine device data (KseQueryDeviceFlags and friends)
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Drivers look up per-device errata by a device key of the form
 * "<PROVIDER>:<hardware id>" (USBXHCI:PCI\VEN_1022&DEV_7814) and a provider
 * name (USBXHCI). The answer comes from, in this order:
 *  1. the value <provider> under
 *     HKLM\System\CurrentControlSet\Control\Compatibility\Device\<device key>,
 *     with every backslash of the device key replaced by '!' (KseSetDeviceFlags
 *     writes there, so an administrator can override the database);
 *  2. the KDEVICE entries of the driver database (drvmain.sdb).
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define KSE_DEVICE_DATA_FROM_REGISTRY   0x10000000  /* Set in *Type on return */
#define KSE_DEVICE_DATA_SKIP_REGISTRY   0x20000000  /* Set in *Type by the caller */
#define KSE_DEVICE_DATA_TYPE_FLAGS      (KSE_DEVICE_DATA_FROM_REGISTRY | KSE_DEVICE_DATA_SKIP_REGISTRY)

static const WCHAR KsepCompatibilityKey[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Compatibility";
static const WCHAR KsepDeviceKeyName[] = L"Device";

/* Builds Compatibility\Device\<DeviceKey with '\' replaced by '!'> */
static
NTSTATUS
KsepBuildDeviceKeyPath(
    _In_ PCWSTR DeviceKey,
    _Out_ PUNICODE_STRING Path)
{
    SIZE_T KeyLength = wcslen(DeviceKey);
    SIZE_T Length;
    PWCHAR p;

    Length = (RTL_NUMBER_OF(KsepCompatibilityKey) - 1 + 1 +
              RTL_NUMBER_OF(KsepDeviceKeyName) - 1 + 1 + KeyLength) * sizeof(WCHAR);
    if (KeyLength == 0 || Length > UNICODE_STRING_MAX_BYTES)
        return STATUS_INVALID_PARAMETER;

    Path->Buffer = ExAllocatePoolWithTag(PagedPool, Length, TAG_KSE_REGISTRY);
    if (!Path->Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    Path->Length = 0;
    Path->MaximumLength = (USHORT)Length;

    RtlAppendUnicodeToString(Path, KsepCompatibilityKey);
    RtlAppendUnicodeToString(Path, L"\\");
    RtlAppendUnicodeToString(Path, KsepDeviceKeyName);
    RtlAppendUnicodeToString(Path, L"\\");
    p = &Path->Buffer[Path->Length / sizeof(WCHAR)];
    RtlAppendUnicodeToString(Path, DeviceKey);
    for (; KeyLength != 0; KeyLength--, p++)
    {
        if (*p == L'\\')
            *p = L'!';
    }
    return STATUS_SUCCESS;
}

static
NTSTATUS
KsepQueryRegistryDeviceData(
    _In_ PCWSTR DeviceKey,
    _In_ PCUNICODE_STRING Provider,
    _Inout_ PULONG Type,
    _Inout_ PULONG DataSize,
    _Out_writes_bytes_opt_(*DataSize) PVOID Data)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    PKEY_VALUE_PARTIAL_INFORMATION Info;
    UNICODE_STRING Path;
    HANDLE Key;
    ULONG Length;
    NTSTATUS Status;

    Status = KsepBuildDeviceKeyPath(DeviceKey, &Path);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &ObjectAttributes);
    ExFreePoolWithTag(Path.Buffer, TAG_KSE_REGISTRY);
    if (!NT_SUCCESS(Status))
        return STATUS_NOT_FOUND;

    Length = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + *DataSize;
    Info = ExAllocatePoolWithTag(PagedPool, Length, TAG_KSE_REGISTRY);
    if (!Info)
    {
        ZwClose(Key);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ZwQueryValueKey(Key, (PUNICODE_STRING)Provider, KeyValuePartialInformation, Info, Length, &Length);
    ZwClose(Key);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (NT_SUCCESS(Status))
    {
        if (Data)
            RtlCopyMemory(Data, Info->Data, Info->DataLength);
        *DataSize = Info->DataLength;
        *Type = Info->Type | KSE_DEVICE_DATA_FROM_REGISTRY;
    }
    else if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL)
    {
        *DataSize = Length - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
        Status = STATUS_BUFFER_TOO_SMALL;
    }

    ExFreePoolWithTag(Info, TAG_KSE_REGISTRY);
    return Status;
}

NTSTATUS
NTAPI
KseQueryDeviceData(
    _In_ PCWSTR DeviceKey,
    _In_ PCWSTR ProviderName,
    _Inout_ PULONG Type,
    _Inout_ PULONG DataSize,
    _Out_writes_bytes_opt_(*DataSize) PVOID Data)
{
    UNICODE_STRING Provider, Device;
    ULONG64 Flags;
    BOOLEAN SkipRegistry;
    NTSTATUS Status;

    PAGED_CODE();

    if (!KsepDeviceShimsActive())
        return STATUS_NOT_FOUND;

    if (!DeviceKey || !ProviderName || !Type || !DataSize)
        return STATUS_INVALID_PARAMETER;

    SkipRegistry = (*Type & KSE_DEVICE_DATA_SKIP_REGISTRY) != 0;
    *Type = REG_NONE;
    RtlInitUnicodeString(&Provider, ProviderName);
    RtlInitUnicodeString(&Device, DeviceKey);

    if (!SkipRegistry)
    {
        Status = KsepQueryRegistryDeviceData(DeviceKey, &Provider, Type, DataSize, Data);
        if (Status != STATUS_NOT_FOUND)
            return Status;
    }

    /* The database only holds flags */
    Status = KseDbQueryDeviceFlags(&Device, &Provider, &Flags);
    if (!NT_SUCCESS(Status))
        return Status;

    *Type = REG_QWORD;
    if (*DataSize < sizeof(Flags))
    {
        *DataSize = sizeof(Flags);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Data)
        RtlCopyMemory(Data, &Flags, sizeof(Flags));
    *DataSize = sizeof(Flags);

    DPRINT("KSE: Query device [%S, %S]: flags 0x%I64x\n", DeviceKey, ProviderName, Flags);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KseQueryDeviceFlags(
    _In_ PCWSTR DeviceKey,
    _In_ PCWSTR ProviderName,
    _Out_ PULONG64 Flags)
{
    ULONG Type = REG_QWORD;
    ULONG Size = sizeof(*Flags);
    ULONG64 Value = 0;
    NTSTATUS Status;

    PAGED_CODE();

    if (!KsepDeviceShimsActive())
        return STATUS_NOT_FOUND;

    if (!DeviceKey || !ProviderName || !Flags)
        return STATUS_INVALID_PARAMETER;

    *Flags = 0;
    Status = KseQueryDeviceData(DeviceKey, ProviderName, &Type, &Size, &Value);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Only an 8 byte REG_QWORD is a set of flags */
    if ((Type & ~KSE_DEVICE_DATA_TYPE_FLAGS) != REG_QWORD || Size != sizeof(Value))
        return STATUS_NOT_FOUND;

    *Flags = Value;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KseSetDeviceFlags(
    _In_ PCWSTR DeviceKey,
    _In_ PCWSTR ProviderName,
    _In_ ULONG64 Flags)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Path, Provider;
    HANDLE Key;
    USHORT Length;
    NTSTATUS Status;

    PAGED_CODE();

    if (!KsepDeviceShimsActive())
        return STATUS_NOT_FOUND;

    if (!DeviceKey || !ProviderName)
        return STATUS_INVALID_PARAMETER;

    Status = KsepBuildDeviceKeyPath(DeviceKey, &Path);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Create Compatibility, Compatibility\Device and the device key in turn */
    Length = Path.Length;
    Path.Length = (USHORT)((RTL_NUMBER_OF(KsepCompatibilityKey) - 1) * sizeof(WCHAR));
    for (;;)
    {
        InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = ZwCreateKey(&Key, KEY_SET_VALUE, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
        if (!NT_SUCCESS(Status) || Path.Length == Length)
            break;

        ZwClose(Key);
        if (Path.Length == (RTL_NUMBER_OF(KsepCompatibilityKey) - 1) * sizeof(WCHAR))
            Path.Length += (USHORT)(RTL_NUMBER_OF(KsepDeviceKeyName) * sizeof(WCHAR));
        else
            Path.Length = Length;
    }
    ExFreePoolWithTag(Path.Buffer, TAG_KSE_REGISTRY);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&Provider, ProviderName);
    Status = ZwSetValueKey(Key, &Provider, 0, REG_QWORD, &Flags, sizeof(Flags));
    ZwClose(Key);
    return Status;
}
