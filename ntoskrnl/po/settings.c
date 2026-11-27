/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power setting callbacks (PoRegisterPowerSettingCallback)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * The power manager keeps one value per power source (AC, DC, short-term
 * "hot" UPS) for every power setting that has one, and reports the value of
 * the current source to each registered callback: once after registration,
 * and again whenever that value changes because the setting was written or
 * because the system switched power sources.  A registration remembers the
 * value object it was last given; the worker calls every registration whose
 * remembered value is not the current one, so a change between two passes is
 * never lost and an unchanged value is never reported twice.
 *
 * Values come from two producers:
 *  - the kernel itself for system state (power source, console display
 *    state, monitor, away mode);
 *  - the power scheme store in the registry (Control\Power): the active
 *    scheme's override under User\PowerSchemes\<scheme>\<subgroup>\<setting>,
 *    else PowerSettings\<subgroup>\<setting>\DefaultPowerSchemeValues\<scheme>.
 *    On Windows the user-mode power service reads the store and pushes the
 *    values; ReactOS has no such service, so the worker reads the store when a
 *    setting is first registered and again whenever Control\Power changes.
 *
 * Callbacks run on the power setting worker thread at PASSIVE_LEVEL with no
 * lock held.  Unregistering waits for a running callback, except from inside
 * that callback, where the registration is freed when the callback returns.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES *********************************************************************/

#define TAG_PO_SETTING          'tSoP'
#define TAG_PO_SETTING_VALUE    'vSoP'
/* What Windows stores in its registrations; checked by unregister */
#define POP_SETTING_SIGNATURE   'teSP'

typedef struct _POP_SETTING_VALUE
{
    LONG RefCount;
    ULONG Length;
    UCHAR Data[ANYSIZE_ARRAY];
} POP_SETTING_VALUE, *PPOP_SETTING_VALUE;

typedef struct _POP_POWER_SETTING
{
    LIST_ENTRY Link;
    GUID Guid;
    LIST_ENTRY Registrations;
    PPOP_SETTING_VALUE Value[PoConditionMaximum];
    /* Produced by the kernel; the scheme store does not hold it */
    BOOLEAN Builtin;
    /* The scheme store was read since it last changed */
    BOOLEAN SchemeRead;
} POP_POWER_SETTING, *PPOP_POWER_SETTING;

typedef struct _POP_SETTING_REGISTRATION
{
    LIST_ENTRY Link;
    ULONG Signature;
    BOOLEAN Unregistering;
    BOOLEAN FreeOnReturn;
    PKTHREAD CallbackThread;
    PKEVENT ReturnEvent;
    PPOP_POWER_SETTING Setting;
    PPOP_SETTING_VALUE Delivered;
    PPOWER_SETTING_CALLBACK Callback;
    PVOID Context;
    PDEVICE_OBJECT DeviceObject;
    GUID Guid;
} POP_SETTING_REGISTRATION, *PPOP_SETTING_REGISTRATION;

/* GLOBALS *******************************************************************/

/* Session-specific settings only reach user-mode recipients */
static const GUID PopGuidSessionDisplayStatus =
    { 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };
static const GUID PopGuidSessionUserPresence =
    { 0x3c0f4548, 0xc03f, 0x4c4d, { 0xb9, 0xf2, 0x23, 0x7e, 0xde, 0x68, 0x63, 0x76 } };

static KGUARDED_MUTEX PopSettingLock;
static LIST_ENTRY PopPowerSettings;
static SYSTEM_POWER_CONDITION PopPowerSource = PoAc;
static KEVENT PopSettingWorkEvent;
static BOOLEAN PopPowerSourceRefresh;

static const UNICODE_STRING PopPowerKeyName =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Power");
static IO_STATUS_BLOCK PopSettingNotifyIosb;

/* PRIVATE FUNCTIONS *********************************************************/

static
BOOLEAN
PopIsSessionSpecificSetting(
    _In_ LPCGUID Guid)
{
    return IsEqualGUID(Guid, &PopGuidSessionDisplayStatus) ||
           IsEqualGUID(Guid, &PopGuidSessionUserPresence);
}

static
PPOP_SETTING_VALUE
PopAllocateSettingValue(
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    PPOP_SETTING_VALUE Value;

    Value = ExAllocatePoolWithTag(PagedPool,
                                  FIELD_OFFSET(POP_SETTING_VALUE, Data) + Length,
                                  TAG_PO_SETTING_VALUE);
    if (!Value)
        return NULL;

    Value->RefCount = 1;
    Value->Length = Length;
    RtlCopyMemory(Value->Data, Data, Length);
    return Value;
}

static
VOID
PopReferenceSettingValue(
    _In_ PPOP_SETTING_VALUE Value)
{
    LONG Count = InterlockedIncrement(&Value->RefCount);
    ASSERT(Count > 1);
    UNREFERENCED_PARAMETER(Count);
}

static
VOID
PopDereferenceSettingValue(
    _In_opt_ PPOP_SETTING_VALUE Value)
{
    LONG Count;

    if (!Value)
        return;

    Count = InterlockedDecrement(&Value->RefCount);
    ASSERT(Count >= 0);
    if (Count == 0)
        ExFreePoolWithTag(Value, TAG_PO_SETTING_VALUE);
}

static
BOOLEAN
PopSettingValuesEqual(
    _In_opt_ PPOP_SETTING_VALUE Value,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    return Value &&
           Value->Length == Length &&
           RtlCompareMemory(Value->Data, Data, Length) == Length;
}

/* Puts Value (whose reference the caller passes) into the slot of Condition */
static
VOID
PopStoreSettingValue(
    _Inout_ PPOP_POWER_SETTING Setting,
    _In_ SYSTEM_POWER_CONDITION Condition,
    _In_ PPOP_SETTING_VALUE Value)
{
    PPOP_SETTING_VALUE Old;

    ASSERT(Condition < PoConditionMaximum);

    Old = Setting->Value[Condition];
    Setting->Value[Condition] = Value;
    PopDereferenceSettingValue(Old);
}

static
PPOP_POWER_SETTING
PopFindPowerSetting(
    _In_ LPCGUID Guid)
{
    PLIST_ENTRY Entry;
    PPOP_POWER_SETTING Setting;

    for (Entry = PopPowerSettings.Flink; Entry != &PopPowerSettings; Entry = Entry->Flink)
    {
        Setting = CONTAINING_RECORD(Entry, POP_POWER_SETTING, Link);
        if (IsEqualGUID(&Setting->Guid, Guid))
            return Setting;
    }

    return NULL;
}

/* Settings live until shutdown: registrations and the worker keep pointers */
static
PPOP_POWER_SETTING
PopCreatePowerSetting(
    _In_ LPCGUID Guid)
{
    PPOP_POWER_SETTING Setting;

    Setting = ExAllocatePoolWithTag(PagedPool, sizeof(*Setting), TAG_PO_SETTING);
    if (!Setting)
        return NULL;

    RtlZeroMemory(Setting, sizeof(*Setting));
    Setting->Guid = *Guid;
    InitializeListHead(&Setting->Registrations);
    InsertTailList(&PopPowerSettings, &Setting->Link);
    return Setting;
}

static
VOID
PopFreeSettingRegistration(
    _In_ PPOP_SETTING_REGISTRATION Registration)
{
    ASSERT(Registration->CallbackThread == NULL);

    RemoveEntryList(&Registration->Link);
    Registration->Signature = 0;
    PopDereferenceSettingValue(Registration->Delivered);
    ExFreePoolWithTag(Registration, TAG_PO_SETTING);
}

/* A setting the kernel produces, with one ULONG per power source */
CODE_SEG("INIT")
static
VOID
PopCreateBuiltinSetting(
    _In_ LPCGUID Guid,
    _In_ ULONG AcValue,
    _In_ ULONG DcValue,
    _In_ ULONG HotValue)
{
    const ULONG Values[PoConditionMaximum] = { AcValue, DcValue, HotValue };
    PPOP_POWER_SETTING Setting;
    PPOP_SETTING_VALUE Value;
    ULONG Condition, Previous;

    Setting = PopCreatePowerSetting(Guid);
    if (!Setting)
        KeBugCheckEx(INTERNAL_POWER_ERROR, 0x500, 0, 0, 0);

    Setting->Builtin = TRUE;
    Setting->SchemeRead = TRUE;

    for (Condition = PoAc; Condition < PoConditionMaximum; Condition++)
    {
        /* Sources with the same value share it, so switching does not report it again */
        for (Previous = PoAc; Previous < Condition; Previous++)
        {
            if (Values[Previous] == Values[Condition])
                break;
        }

        if (Previous < Condition)
        {
            Value = Setting->Value[Previous];
            PopReferenceSettingValue(Value);
        }
        else
        {
            Value = PopAllocateSettingValue(&Values[Condition], sizeof(ULONG));
            if (!Value)
                KeBugCheckEx(INTERNAL_POWER_ERROR, 0x501, 0, 0, 0);
        }

        PopStoreSettingValue(Setting, (SYSTEM_POWER_CONDITION)Condition, Value);
    }
}

static
NTSTATUS
PopOpenSettingKey(
    _Out_ PHANDLE KeyHandle,
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name)
{
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               Root,
                               NULL);
    return ZwOpenKey(KeyHandle, KEY_READ, &ObjectAttributes);
}

static
BOOLEAN
PopReadSettingIndex(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Out_ PULONG Index)
{
    UNICODE_STRING ValueName;
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    ULONG Length;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation,
                             Info, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status))
        return FALSE;

    /* The store is data anybody can write; take only well-formed indexes */
    if (Info->Type != REG_DWORD || Info->DataLength != sizeof(ULONG))
    {
        DPRINT1("Power setting index %S has type %lu, length %lu\n",
                Name, Info->Type, Info->DataLength);
        return FALSE;
    }

    *Index = *(PULONG)Info->Data;
    return TRUE;
}

/* Reads the AC and DC index of the setting in the active power scheme */
static
VOID
PopReadSchemeSetting(
    _In_ LPCGUID Guid,
    _Out_ PULONG AcIndex,
    _Out_ PBOOLEAN AcFound,
    _Out_ PULONG DcIndex,
    _Out_ PBOOLEAN DcFound)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE PowerKey = NULL, SchemesKey = NULL, SettingsKey = NULL, Key;
    WCHAR SettingName[40], Scheme[40], Path[200];
    UNICODE_STRING ValueName;
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(Scheme)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UCHAR NameBuffer[FIELD_OFFSET(KEY_BASIC_INFORMATION, Name) + sizeof(Path)];
    PKEY_BASIC_INFORMATION KeyInfo = (PKEY_BASIC_INFORMATION)NameBuffer;
    WCHAR SubGroup[40];
    ULONG Length, Index;
    NTSTATUS Status;

    PAGED_CODE();

    *AcFound = FALSE;
    *DcFound = FALSE;

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&PopPowerKeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenKey(&PowerKey, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return;

    RtlStringCbPrintfW(SettingName, sizeof(SettingName),
                       L"%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x",
                       Guid->Data1, Guid->Data2, Guid->Data3,
                       Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
                       Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);

    /* The active scheme */
    Status = PopOpenSettingKey(&SchemesKey, PowerKey, L"User\\PowerSchemes");
    if (!NT_SUCCESS(Status))
        goto Exit;

    RtlInitUnicodeString(&ValueName, L"ActivePowerScheme");
    Status = ZwQueryValueKey(SchemesKey, &ValueName, KeyValuePartialInformation,
                             Info, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status) || Info->Type != REG_SZ ||
        Info->DataLength < sizeof(WCHAR) || Info->DataLength > sizeof(Scheme))
    {
        DPRINT1("No usable ActivePowerScheme (0x%lx)\n", Status);
        goto Exit;
    }
    RtlZeroMemory(Scheme, sizeof(Scheme));
    RtlCopyMemory(Scheme, Info->Data, Info->DataLength);
    Scheme[RTL_NUMBER_OF(Scheme) - 1] = UNICODE_NULL;

    /* The subgroup that defines the setting */
    Status = PopOpenSettingKey(&SettingsKey, PowerKey, L"PowerSettings");
    if (!NT_SUCCESS(Status))
        goto Exit;

    for (Index = 0; ; Index++)
    {
        Status = ZwEnumerateKey(SettingsKey, Index, KeyBasicInformation,
                                KeyInfo, sizeof(NameBuffer) - sizeof(WCHAR), &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            goto Exit;
        if (!NT_SUCCESS(Status))
            continue;
        if (KeyInfo->NameLength >= sizeof(SubGroup))
            continue;

        RtlCopyMemory(SubGroup, KeyInfo->Name, KeyInfo->NameLength);
        SubGroup[KeyInfo->NameLength / sizeof(WCHAR)] = UNICODE_NULL;

        RtlStringCbPrintfW(Path, sizeof(Path), L"%s\\%s", SubGroup, SettingName);
        Status = PopOpenSettingKey(&Key, SettingsKey, Path);
        if (NT_SUCCESS(Status))
        {
            ZwClose(Key);
            break;
        }
    }

    /* The active scheme's own value comes first */
    RtlStringCbPrintfW(Path, sizeof(Path), L"%s\\%s\\%s", Scheme, SubGroup, SettingName);
    Status = PopOpenSettingKey(&Key, SchemesKey, Path);
    if (NT_SUCCESS(Status))
    {
        *AcFound = PopReadSettingIndex(Key, L"ACSettingIndex", AcIndex);
        *DcFound = PopReadSettingIndex(Key, L"DCSettingIndex", DcIndex);
        ZwClose(Key);
    }

    /* then the setting's default for that scheme */
    if (!*AcFound || !*DcFound)
    {
        RtlStringCbPrintfW(Path, sizeof(Path), L"%s\\%s\\DefaultPowerSchemeValues\\%s",
                           SubGroup, SettingName, Scheme);
        Status = PopOpenSettingKey(&Key, SettingsKey, Path);
        if (NT_SUCCESS(Status))
        {
            if (!*AcFound)
                *AcFound = PopReadSettingIndex(Key, L"ACSettingIndex", AcIndex);
            if (!*DcFound)
                *DcFound = PopReadSettingIndex(Key, L"DCSettingIndex", DcIndex);
            ZwClose(Key);
        }
    }

Exit:
    if (SettingsKey)
        ZwClose(SettingsKey);
    if (SchemesKey)
        ZwClose(SchemesKey);
    ZwClose(PowerKey);
}

/* Updates the scheme values of one setting; called with the lock held */
static
VOID
PopUpdateSchemeSetting(
    _Inout_ PPOP_POWER_SETTING Setting,
    _In_ BOOLEAN AcFound,
    _In_ ULONG AcIndex,
    _In_ BOOLEAN DcFound,
    _In_ ULONG DcIndex)
{
    PPOP_SETTING_VALUE Value;

    if (AcFound && !PopSettingValuesEqual(Setting->Value[PoAc], &AcIndex, sizeof(ULONG)))
    {
        Value = PopAllocateSettingValue(&AcIndex, sizeof(ULONG));
        if (Value)
            PopStoreSettingValue(Setting, PoAc, Value);
    }

    if (DcFound && !PopSettingValuesEqual(Setting->Value[PoDc], &DcIndex, sizeof(ULONG)))
    {
        /* Share the AC value when both are the same */
        Value = Setting->Value[PoAc];
        if (PopSettingValuesEqual(Value, &DcIndex, sizeof(ULONG)))
            PopReferenceSettingValue(Value);
        else
            Value = PopAllocateSettingValue(&DcIndex, sizeof(ULONG));
        if (Value)
            PopStoreSettingValue(Setting, PoDc, Value);
    }

    /* A short-term UPS is a DC source */
    if (Setting->Value[PoDc] && Setting->Value[PoHot] != Setting->Value[PoDc])
    {
        PopReferenceSettingValue(Setting->Value[PoDc]);
        PopStoreSettingValue(Setting, PoHot, Setting->Value[PoDc]);
    }
}

/* Reads the store for every setting that has not seen its current contents */
static
VOID
PopReadSchemeSettings(VOID)
{
    PLIST_ENTRY Entry;
    PPOP_POWER_SETTING Setting;
    ULONG AcIndex = 0, DcIndex = 0;
    BOOLEAN AcFound, DcFound;

    PAGED_CODE();

    /* Settings are never removed, so an entry stays valid without the lock */
    KeAcquireGuardedMutex(&PopSettingLock);
    Entry = PopPowerSettings.Flink;
    while (Entry != &PopPowerSettings)
    {
        Setting = CONTAINING_RECORD(Entry, POP_POWER_SETTING, Link);
        if (!Setting->Builtin && !Setting->SchemeRead)
        {
            Setting->SchemeRead = TRUE;
            KeReleaseGuardedMutex(&PopSettingLock);

            PopReadSchemeSetting(&Setting->Guid, &AcIndex, &AcFound, &DcIndex, &DcFound);

            KeAcquireGuardedMutex(&PopSettingLock);
            PopUpdateSchemeSetting(Setting, AcFound, AcIndex, DcFound, DcIndex);
        }
        Entry = Entry->Flink;
    }
    KeReleaseGuardedMutex(&PopSettingLock);
}

/* Calls each registration whose value changed, one at a time */
static
VOID
PopDispatchPowerSettings(VOID)
{
    PLIST_ENTRY SettingEntry, Entry;
    PPOP_POWER_SETTING Setting;
    PPOP_SETTING_REGISTRATION Registration;
    PPOP_SETTING_VALUE Value, Old;

    PAGED_CODE();

    for (;;)
    {
        Registration = NULL;
        Value = NULL;

        KeAcquireGuardedMutex(&PopSettingLock);
        for (SettingEntry = PopPowerSettings.Flink;
             SettingEntry != &PopPowerSettings && !Registration;
             SettingEntry = SettingEntry->Flink)
        {
            Setting = CONTAINING_RECORD(SettingEntry, POP_POWER_SETTING, Link);
            Value = Setting->Value[PopPowerSource];
            if (!Value)
                continue;

            for (Entry = Setting->Registrations.Flink;
                 Entry != &Setting->Registrations;
                 Entry = Entry->Flink)
            {
                Registration = CONTAINING_RECORD(Entry, POP_SETTING_REGISTRATION, Link);
                if (Registration->Delivered != Value && !Registration->Unregistering)
                    break;
                Registration = NULL;
            }
        }

        if (!Registration)
        {
            KeReleaseGuardedMutex(&PopSettingLock);
            return;
        }

        ASSERT(Registration->CallbackThread == NULL);
        Registration->CallbackThread = KeGetCurrentThread();
        PopReferenceSettingValue(Value);
        Old = Registration->Delivered;
        Registration->Delivered = Value;
        KeReleaseGuardedMutex(&PopSettingLock);

        PopDereferenceSettingValue(Old);

        DPRINT("Power setting %p: callback %p, %lu bytes\n",
               Registration, Registration->Callback, Value->Length);
        Registration->Callback(&Registration->Guid,
                               Value->Data,
                               Value->Length,
                               Registration->Context);
        ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

        KeAcquireGuardedMutex(&PopSettingLock);
        Registration->CallbackThread = NULL;
        if (Registration->FreeOnReturn)
            PopFreeSettingRegistration(Registration);
        else if (Registration->ReturnEvent)
            KeSetEvent(Registration->ReturnEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseGuardedMutex(&PopSettingLock);
    }
}

/* Watches Control\Power so a changed scheme value or active scheme is reported */
static
PKEVENT
PopWatchSchemeStore(
    _Inout_ PHANDLE PowerKey,
    _Inout_ PHANDLE EventHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    PKEVENT Event;
    NTSTATUS Status;

    PAGED_CODE();

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateEvent(EventHandle, EVENT_ALL_ACCESS, &ObjectAttributes,
                           SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No power scheme store event (0x%lx)\n", Status);
        return NULL;
    }

    Status = ObReferenceObjectByHandle(*EventHandle, EVENT_ALL_ACCESS, ExEventObjectType,
                                       KernelMode, (PVOID*)&Event, NULL);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(*EventHandle);
        *EventHandle = NULL;
        return NULL;
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&PopPowerKeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwCreateKey(PowerKey, KEY_READ, &ObjectAttributes, 0, NULL,
                         REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Cannot open the power scheme store (0x%lx)\n", Status);
        ObDereferenceObject(Event);
        ZwClose(*EventHandle);
        *EventHandle = NULL;
        return NULL;
    }

    return Event;
}

static
NTSTATUS
PopArmSchemeStoreWatch(
    _In_ HANDLE PowerKey,
    _In_ HANDLE EventHandle)
{
    NTSTATUS Status;

    Status = ZwNotifyChangeKey(PowerKey,
                               EventHandle,
                               NULL,
                               NULL,
                               &PopSettingNotifyIosb,
                               REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                               TRUE,
                               NULL,
                               0,
                               TRUE);
    if (Status != STATUS_PENDING && !NT_SUCCESS(Status))
        DPRINT1("Cannot watch the power scheme store (0x%lx)\n", Status);
    return Status;
}

static KSTART_ROUTINE PopPowerSettingWorker;

static
VOID
NTAPI
PopPowerSettingWorker(
    _In_ PVOID StartContext)
{
    HANDLE PowerKey = NULL, EventHandle = NULL;
    PKEVENT StoreEvent;
    PVOID Objects[3];
    ULONG ObjectCount, StoreIndex;
    PLIST_ENTRY Entry;
    BOOLEAN Refresh;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(StartContext);
    PAGED_CODE();

    StoreEvent = PopWatchSchemeStore(&PowerKey, &EventHandle);
    if (StoreEvent && !NT_SUCCESS(PopArmSchemeStoreWatch(PowerKey, EventHandle)))
    {
        ObDereferenceObject(StoreEvent);
        StoreEvent = NULL;
    }

    Objects[0] = &PopSettingWorkEvent;
    Objects[1] = &PopBatteryWaitEvent;
    ObjectCount = 2;
    StoreIndex = MAXULONG;
    if (StoreEvent)
    {
        StoreIndex = ObjectCount;
        Objects[ObjectCount++] = StoreEvent;
    }

    /* The power source before the first values go out */
    PopRefreshPowerSource();

    for (;;)
    {
        KeAcquireGuardedMutex(&PopSettingLock);
        Refresh = PopPowerSourceRefresh;
        PopPowerSourceRefresh = FALSE;
        KeReleaseGuardedMutex(&PopSettingLock);
        if (Refresh)
            PopRefreshPowerSource();

        PopReadSchemeSettings();
        PopDispatchPowerSettings();

        Status = KeWaitForMultipleObjects(ObjectCount,
                                          Objects,
                                          WaitAny,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          NULL,
                                          NULL);
        if (Status == STATUS_WAIT_1)
        {
            /* The composite battery request completed */
            PopBatteryWaitCompleted();
        }
        else if (Status == STATUS_WAIT_0 + StoreIndex)
        {
            /* Re-arm first so a write during the re-read is not missed */
            if (!NT_SUCCESS(PopArmSchemeStoreWatch(PowerKey, EventHandle)))
                DPRINT1("Power scheme changes are no longer reported\n");

            KeAcquireGuardedMutex(&PopSettingLock);
            for (Entry = PopPowerSettings.Flink; Entry != &PopPowerSettings; Entry = Entry->Flink)
                CONTAINING_RECORD(Entry, POP_POWER_SETTING, Link)->SchemeRead = FALSE;
            KeReleaseGuardedMutex(&PopSettingLock);
        }
    }
}

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
PopInitializePowerSettings(VOID)
{
    KeInitializeGuardedMutex(&PopSettingLock);
    InitializeListHead(&PopPowerSettings);
    KeInitializeEvent(&PopSettingWorkEvent, SynchronizationEvent, FALSE);

    /* SYSTEM_POWER_CONDITION of the current source */
    PopCreateBuiltinSetting(&GUID_ACDC_POWER_SOURCE, PoAc, PoDc, PoHot);
    /* No display power management: the console is on, not in away mode */
    PopCreateBuiltinSetting(&GUID_CONSOLE_DISPLAY_STATE, PowerMonitorOn, PowerMonitorOn, PowerMonitorOn);
    PopCreateBuiltinSetting(&GUID_MONITOR_POWER_ON, 1, 1, 1);
    PopCreateBuiltinSetting(&GUID_SYSTEM_AWAYMODE, 0, 0, 0);
}

/* Starts reporting values; registrations made earlier are reported then */
CODE_SEG("INIT")
VOID
NTAPI
PopStartPowerSettingWorker(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Thread;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, &ObjectAttributes,
                                  NULL, NULL, PopPowerSettingWorker, NULL);
    if (!NT_SUCCESS(Status))
        KeBugCheckEx(INTERNAL_POWER_ERROR, 0x502, Status, 0, 0);
    ZwClose(Thread);
}

/* The battery state says which source the system runs from */
VOID
NTAPI
PopSetPowerSource(
    _In_ SYSTEM_POWER_CONDITION Source)
{
    ASSERT(Source < PoConditionMaximum);

    KeAcquireGuardedMutex(&PopSettingLock);
    if (PopPowerSource != Source)
    {
        DPRINT1("Power source %d -> %d\n", PopPowerSource, Source);
        PopPowerSource = Source;
        KeSetEvent(&PopSettingWorkEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseGuardedMutex(&PopSettingLock);
}

/*
 * The lid device reported the lid state (GUID_LIDSWITCH_STATE_CHANGE: 0 closed,
 * 1 open).  The setting has no value, and its registrations get no call, until
 * the first report.
 */
VOID
NTAPI
PopSetLidSwitchState(
    _In_ BOOLEAN Open)
{
    ULONG State = Open ? 1 : 0;
    PPOP_POWER_SETTING Setting;
    PPOP_SETTING_VALUE Value;
    ULONG Condition;

    PAGED_CODE();

    Value = PopAllocateSettingValue(&State, sizeof(State));
    if (!Value)
    {
        DPRINT1("No memory for the lid state\n");
        return;
    }

    KeAcquireGuardedMutex(&PopSettingLock);
    Setting = PopFindPowerSetting(&GUID_LIDSWITCH_STATE_CHANGE);
    if (!Setting)
        Setting = PopCreatePowerSetting(&GUID_LIDSWITCH_STATE_CHANGE);
    if (Setting)
    {
        /* Produced by the kernel, never read from the scheme store */
        Setting->Builtin = TRUE;
        Setting->SchemeRead = TRUE;

        /* The same value on every power source */
        for (Condition = PoAc; Condition < PoConditionMaximum; Condition++)
        {
            PopReferenceSettingValue(Value);
            PopStoreSettingValue(Setting, (SYSTEM_POWER_CONDITION)Condition, Value);
        }
        KeSetEvent(&PopSettingWorkEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseGuardedMutex(&PopSettingLock);

    PopDereferenceSettingValue(Value);
}

/* A battery arrived: query the source again (on the worker, never in the PnP notification) */
VOID
NTAPI
PopRequestPowerSourceRefresh(VOID)
{
    KeAcquireGuardedMutex(&PopSettingLock);
    PopPowerSourceRefresh = TRUE;
    KeSetEvent(&PopSettingWorkEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseGuardedMutex(&PopSettingLock);
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
NTAPI
PoRegisterPowerSettingCallback(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ LPCGUID SettingGuid,
    _In_ PPOWER_SETTING_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_opt_ PVOID *Handle)
{
    PPOP_SETTING_REGISTRATION Registration;
    PPOP_POWER_SETTING Setting;

    PAGED_CODE();
    ASSERT(SettingGuid != NULL);
    ASSERT(Callback != NULL);

    if (PopIsSessionSpecificSetting(SettingGuid))
        return STATUS_INVALID_PARAMETER;

    Registration = ExAllocatePoolWithTag(PagedPool, sizeof(*Registration), TAG_PO_SETTING);
    if (!Registration)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Registration, sizeof(*Registration));
    Registration->Signature = POP_SETTING_SIGNATURE;
    Registration->Callback = Callback;
    Registration->Context = Context;
    Registration->DeviceObject = DeviceObject;
    Registration->Guid = *SettingGuid;

    KeAcquireGuardedMutex(&PopSettingLock);

    /* A setting nobody defines is kept too; it is reported once it gets a value */
    Setting = PopFindPowerSetting(SettingGuid);
    if (!Setting)
        Setting = PopCreatePowerSetting(SettingGuid);
    if (!Setting)
    {
        KeReleaseGuardedMutex(&PopSettingLock);
        ExFreePoolWithTag(Registration, TAG_PO_SETTING);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Registration->Setting = Setting;
    InsertTailList(&Setting->Registrations, &Registration->Link);
    if (Handle)
        *Handle = Registration;

    /* The worker makes the initial call (and reads the store first if needed) */
    KeSetEvent(&PopSettingWorkEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseGuardedMutex(&PopSettingLock);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
NTAPI
PoUnregisterPowerSettingCallback(
    _Inout_ PVOID Handle)
{
    PPOP_SETTING_REGISTRATION Registration = Handle;
    KEVENT ReturnEvent;

    PAGED_CODE();

    if (!Registration)
        return STATUS_INVALID_PARAMETER;

    KeAcquireGuardedMutex(&PopSettingLock);

    if (Registration->Signature != POP_SETTING_SIGNATURE || Registration->Unregistering)
    {
        KeReleaseGuardedMutex(&PopSettingLock);
        return STATUS_INVALID_PARAMETER;
    }
    Registration->Unregistering = TRUE;

    if (Registration->CallbackThread == KeGetCurrentThread())
    {
        /* From the callback itself: it goes once the callback returns */
        Registration->FreeOnReturn = TRUE;
        KeReleaseGuardedMutex(&PopSettingLock);
        return STATUS_SUCCESS;
    }

    if (Registration->CallbackThread)
    {
        KeInitializeEvent(&ReturnEvent, NotificationEvent, FALSE);
        Registration->ReturnEvent = &ReturnEvent;
        KeReleaseGuardedMutex(&PopSettingLock);

        KeWaitForSingleObject(&ReturnEvent, Executive, KernelMode, FALSE, NULL);

        KeAcquireGuardedMutex(&PopSettingLock);
        ASSERT(Registration->CallbackThread == NULL);
    }

    PopFreeSettingRegistration(Registration);
    KeReleaseGuardedMutex(&PopSettingLock);
    return STATUS_SUCCESS;
}
