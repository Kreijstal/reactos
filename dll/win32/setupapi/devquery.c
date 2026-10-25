/*
 * PROJECT:     ReactOS Setup API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device object queries (devquery.h)
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A limited implementation of the Windows device query API: device
 * interface objects only, filtered by an AND of simple comparisons on the
 * interface class, the enabled state and the owning device instance.
 * Wintun, for one, waits for its network interface this way.
 *
 * Windows gets change notifications from the PnP manager. We have no
 * user-mode interface arrival notifications yet, so a query that asks for
 * updates polls the interface lists from a worker thread and reports the
 * difference.
 */

#include "setupapi_private.h"

#include <devquery.h>
#include <initguid.h>
#include <devpkey.h>

#define DEVQUERY_MAGIC 0x51766544 /* "DevQ" */
#define DEVQUERY_POLL_INTERVAL 200 /* ms */
#define DEVQUERY_KNOWN_FLAGS (DevQueryFlagUpdateResults | DevQueryFlagAllProperties | \
                              DevQueryFlagLocalize | DevQueryFlagAsyncClose)

/* The properties we know of a device interface object */
typedef enum _DEVQ_PROP
{
    DevqPropInstanceId,
    DevqPropEnabled,
    DevqPropClassGuid,
    DevqPropReferenceString,
    DevqPropMax
} DEVQ_PROP;

static const DEVPROPKEY *
DevqPropKey(DEVQ_PROP Prop)
{
    switch (Prop)
    {
        case DevqPropInstanceId: return &DEVPKEY_Device_InstanceId;
        case DevqPropEnabled: return &DEVPKEY_DeviceInterface_Enabled;
        case DevqPropClassGuid: return &DEVPKEY_DeviceInterface_ClassGuid;
        case DevqPropReferenceString: return &DEVPKEY_DeviceInterface_ReferenceString;
        default: return NULL;
    }
}

/* One device interface, with the values of the properties we know */
typedef struct _DEVQ_ITEM
{
    struct _DEVQ_ITEM *Next;
    PWSTR ObjectId;            /* the symbolic link name */
    PWSTR InstanceId;          /* owning device, NULL if unknown */
    PWSTR ReferenceString;     /* NULL if none */
    DEVPROP_BOOLEAN Enabled;
    GUID ClassGuid;
    BOOL Seen;                 /* scratch for the change computation */
} DEVQ_ITEM, *PDEVQ_ITEM;

typedef struct _DEV_QUERY
{
    DWORD Magic;
    LONG RefCount;
    ULONG Flags;
    ULONG cRequested;
    DEVPROPCOMPKEY *Requested;
    ULONG cFilter;
    DEVPROP_FILTER_EXPRESSION *Filter;
    PDEV_QUERY_RESULT_CALLBACK Callback;
    PVOID Context;
    HANDLE StopEvent;
    HANDLE Thread;
    DWORD ThreadId;
    PDEVQ_ITEM Results;        /* the objects reported so far */
} DEV_QUERY, *PDEV_QUERY;

static
VOID
DevqFreeItems(
    _In_opt_ PDEVQ_ITEM Item)
{
    PDEVQ_ITEM Next;

    for (; Item; Item = Next)
    {
        Next = Item->Next;
        HeapFree(GetProcessHeap(), 0, Item->ObjectId);
        HeapFree(GetProcessHeap(), 0, Item->InstanceId);
        HeapFree(GetProcessHeap(), 0, Item->ReferenceString);
        HeapFree(GetProcessHeap(), 0, Item);
    }
}

static
PWSTR
DevqStrDup(
    _In_ PCWSTR String,
    _In_ SIZE_T Length)
{
    PWSTR Copy = HeapAlloc(GetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));

    if (Copy)
    {
        memcpy(Copy, String, Length * sizeof(WCHAR));
        Copy[Length] = UNICODE_NULL;
    }
    return Copy;
}

/*
 * The registry keeps an interface under
 * DeviceClasses\{class}\##?#<device>#{class}\#<reference>, with the owning
 * device in the DeviceInstance value of the ##?# key. The symbolic link is
 * \\?\<device>#{class}[\<reference>].
 */
static
VOID
DevqReadInterfaceOwner(
    _In_ HKEY ClassesKey,
    _In_ PCWSTR ClassString,
    _Inout_ PDEVQ_ITEM Item)
{
    PCWSTR Path = Item->ObjectId, Reference;
    WCHAR KeyName[MAX_PATH];
    WCHAR Value[MAX_DEVICE_ID_LEN];
    DWORD Type, Size = sizeof(Value) - sizeof(WCHAR);
    SIZE_T PathLength;
    HKEY Key;

    if (wcslen(Path) < 4 || Path[0] != L'\\' || Path[1] != L'\\' || Path[3] != L'\\')
        return;
    Path += 4;

    Reference = wcschr(Path, L'\\');
    PathLength = Reference ? (SIZE_T)(Reference - Path) : wcslen(Path);
    if (Reference && Reference[1])
        Item->ReferenceString = DevqStrDup(Reference + 1, wcslen(Reference + 1));

    if (wcslen(ClassString) + 1 + 4 + PathLength >= ARRAYSIZE(KeyName))
        return;
    wcscpy(KeyName, ClassString);
    wcscat(KeyName, L"\\##?#");
    wcsncat(KeyName, Path, PathLength);

    if (RegOpenKeyExW(ClassesKey, KeyName, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(Key, L"DeviceInstance", NULL, &Type, (PBYTE)Value, &Size) == ERROR_SUCCESS &&
        Type == REG_SZ)
    {
        Value[Size / sizeof(WCHAR)] = UNICODE_NULL;
        Item->InstanceId = DevqStrDup(Value, wcslen(Value));
    }
    RegCloseKey(Key);
}

/* Get the interface list of one class, as a multi-string */
static
DWORD
DevqGetInterfaceList(
    _In_ LPGUID ClassGuid,
    _In_ ULONG Flags,
    _Out_ PWSTR *List)
{
    CONFIGRET Cr;
    ULONG Length;
    PWSTR Buffer;

    *List = NULL;
    for (;;)
    {
        Cr = CM_Get_Device_Interface_List_SizeW(&Length, ClassGuid, NULL, Flags);
        if (Cr != CR_SUCCESS)
            return CM_MapCrToWin32Err(Cr, ERROR_GEN_FAILURE);
        Buffer = HeapAlloc(GetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));
        if (!Buffer)
            return ERROR_NOT_ENOUGH_MEMORY;
        Cr = CM_Get_Device_Interface_ListW(ClassGuid, NULL, Buffer, Length + 1, Flags);
        if (Cr == CR_SUCCESS)
        {
            *List = Buffer;
            return ERROR_SUCCESS;
        }
        HeapFree(GetProcessHeap(), 0, Buffer);
        /* The list grew between the two calls */
        if (Cr != CR_BUFFER_SMALL)
            return CM_MapCrToWin32Err(Cr, ERROR_GEN_FAILURE);
    }
}

static
BOOL
DevqMultiSzContains(
    _In_opt_ PCWSTR List,
    _In_ PCWSTR String)
{
    for (; List && *List; List += wcslen(List) + 1)
    {
        if (!_wcsicmp(List, String))
            return TRUE;
    }
    return FALSE;
}

/* Collect the interfaces of one class */
static
DWORD
DevqEnumerateClass(
    _In_ HKEY ClassesKey,
    _In_ LPGUID ClassGuid,
    _Inout_ PDEVQ_ITEM *Items)
{
    WCHAR ClassString[MAX_GUID_STRING_LEN];
    PWSTR All, Present, Path;
    PDEVQ_ITEM Item;
    DWORD Error;

    _snwprintf(ClassString, ARRAYSIZE(ClassString), L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             ClassGuid->Data1, ClassGuid->Data2, ClassGuid->Data3,
             ClassGuid->Data4[0], ClassGuid->Data4[1], ClassGuid->Data4[2], ClassGuid->Data4[3],
             ClassGuid->Data4[4], ClassGuid->Data4[5], ClassGuid->Data4[6], ClassGuid->Data4[7]);

    Error = DevqGetInterfaceList(ClassGuid, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, &All);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = DevqGetInterfaceList(ClassGuid, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, &Present);
    if (Error != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, All);
        return Error;
    }

    for (Path = All; *Path; Path += wcslen(Path) + 1)
    {
        Item = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Item));
        if (!Item || !(Item->ObjectId = DevqStrDup(Path, wcslen(Path))))
        {
            HeapFree(GetProcessHeap(), 0, Item);
            Error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        Item->ClassGuid = *ClassGuid;
        Item->Enabled = DevqMultiSzContains(Present, Path) ? DEVPROP_TRUE : DEVPROP_FALSE;
        DevqReadInterfaceOwner(ClassesKey, ClassString, Item);
        Item->Next = *Items;
        *Items = Item;
    }

    HeapFree(GetProcessHeap(), 0, Present);
    HeapFree(GetProcessHeap(), 0, All);
    return Error;
}

/* The class a filter pins the query to, if any: no need to look at the others */
static
const GUID *
DevqFilterClass(
    _In_ ULONG cFilter,
    _In_reads_opt_(cFilter) const DEVPROP_FILTER_EXPRESSION *Filter)
{
    ULONG i;

    for (i = 0; i < cFilter; i++)
    {
        if (Filter[i].Operator == DEVPROP_OPERATOR_EQUALS &&
            IsEqualDevPropKey(Filter[i].Property.CompKey.Key, DEVPKEY_DeviceInterface_ClassGuid) &&
            Filter[i].Property.Type == DEVPROP_TYPE_GUID)
        {
            return Filter[i].Property.Buffer;
        }
    }
    return NULL;
}

/* Collect the interfaces of every class, or just of the one the filter names */
static
DWORD
DevqEnumerate(
    _In_ PDEV_QUERY Query,
    _Out_ PDEVQ_ITEM *Items)
{
    WCHAR Name[MAX_GUID_STRING_LEN + 1];
    const GUID *OnlyClass;
    GUID ClassGuid;
    DWORD Index, Length, Error = ERROR_SUCCESS;
    HKEY ClassesKey;

    *Items = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses",
                      0, KEY_ENUMERATE_SUB_KEYS, &ClassesKey) != ERROR_SUCCESS)
    {
        /* No interface was ever registered */
        return ERROR_SUCCESS;
    }

    OnlyClass = DevqFilterClass(Query->cFilter, Query->Filter);
    if (OnlyClass)
    {
        ClassGuid = *OnlyClass;
        Error = DevqEnumerateClass(ClassesKey, &ClassGuid, Items);
    }
    else
    {
        for (Index = 0; ; Index++)
        {
            Length = ARRAYSIZE(Name);
            if (RegEnumKeyExW(ClassesKey, Index, Name, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (Length != MAX_GUID_STRING_LEN - 1 || Name[0] != L'{')
                continue;
            Name[MAX_GUID_STRING_LEN - 2] = UNICODE_NULL;
            if (UuidFromStringW((RPC_WSTR)&Name[1], &ClassGuid) != RPC_S_OK)
                continue;
            Error = DevqEnumerateClass(ClassesKey, &ClassGuid, Items);
            if (Error != ERROR_SUCCESS)
                break;
        }
    }

    RegCloseKey(ClassesKey);
    if (Error != ERROR_SUCCESS)
    {
        DevqFreeItems(*Items);
        *Items = NULL;
    }
    return Error;
}

/* The value of a property of an item, as a DEVPROPERTY without a key */
static
VOID
DevqGetItemProperty(
    _In_ const DEVQ_ITEM *Item,
    _In_ DEVQ_PROP Prop,
    _Out_ DEVPROPTYPE *Type,
    _Out_ const VOID **Buffer,
    _Out_ ULONG *BufferSize)
{
    PCWSTR String = NULL;

    *Type = DEVPROP_TYPE_EMPTY;
    *Buffer = NULL;
    *BufferSize = 0;
    switch (Prop)
    {
        case DevqPropInstanceId:
            String = Item->InstanceId;
            break;
        case DevqPropReferenceString:
            String = Item->ReferenceString;
            break;
        case DevqPropEnabled:
            *Type = DEVPROP_TYPE_BOOLEAN;
            *Buffer = &Item->Enabled;
            *BufferSize = sizeof(Item->Enabled);
            return;
        case DevqPropClassGuid:
            *Type = DEVPROP_TYPE_GUID;
            *Buffer = &Item->ClassGuid;
            *BufferSize = sizeof(Item->ClassGuid);
            return;
        default:
            return;
    }
    if (String)
    {
        *Type = DEVPROP_TYPE_STRING;
        *Buffer = String;
        *BufferSize = (ULONG)((wcslen(String) + 1) * sizeof(WCHAR));
    }
}

static
DEVQ_PROP
DevqLookupKey(
    _In_ const DEVPROPCOMPKEY *CompKey)
{
    DEVQ_PROP Prop;

    if (CompKey->Store != DEVPROP_STORE_SYSTEM || CompKey->LocaleName)
        return DevqPropMax;
    for (Prop = 0; Prop < DevqPropMax; Prop++)
    {
        if (IsEqualDevPropKey(CompKey->Key, *DevqPropKey(Prop)))
            return Prop;
    }
    return DevqPropMax;
}

/* A filter string, which need not be terminated within its buffer */
static
BOOL
DevqFilterString(
    _In_ const DEVPROPERTY *Property,
    _Out_ PCWSTR *String,
    _Out_ SIZE_T *Length)
{
    PCWSTR Buffer = Property->Buffer;
    SIZE_T Count = Property->BufferSize / sizeof(WCHAR);

    if (!Buffer)
        return FALSE;
    *Length = 0;
    while (*Length < Count && Buffer[*Length])
        (*Length)++;
    *String = Buffer;
    return TRUE;
}

static
BOOL
DevqCompareString(
    _In_ DEVPROP_OPERATOR Eval,
    _In_ BOOL IgnoreCase,
    _In_ PCWSTR Value,
    _In_ PCWSTR Operand,
    _In_ SIZE_T OperandLength)
{
    SIZE_T ValueLength = wcslen(Value), i;
#define Compare(a, b, n) (IgnoreCase ? _wcsnicmp((a), (b), (n)) : wcsncmp((a), (b), (n)))

    switch (Eval)
    {
        case DEVPROP_OPERATOR_EQUALS:
            return ValueLength == OperandLength && !Compare(Value, Operand, OperandLength);
        case DEVPROP_OPERATOR_BEGINS_WITH:
            return ValueLength >= OperandLength && !Compare(Value, Operand, OperandLength);
        case DEVPROP_OPERATOR_ENDS_WITH:
            return ValueLength >= OperandLength &&
                   !Compare(Value + ValueLength - OperandLength, Operand, OperandLength);
        case DEVPROP_OPERATOR_CONTAINS:
            for (i = 0; i + OperandLength <= ValueLength; i++)
            {
                if (!Compare(Value + i, Operand, OperandLength))
                    return TRUE;
            }
            return FALSE;
        default:
            return FALSE;
    }
#undef Compare
}

/* Check that we can evaluate a filter expression */
static
HRESULT
DevqValidateExpression(
    _In_ const DEVPROP_FILTER_EXPRESSION *Expression)
{
    DEVPROP_OPERATOR Op = Expression->Operator;
    DEVPROP_OPERATOR Eval = Op & DEVPROP_OPERATOR_MASK_EVAL;
    DEVPROPTYPE Type = Expression->Property.Type;
    DEVQ_PROP Prop;

    /* Only an implicit AND of plain comparisons */
    if (Op & (DEVPROP_OPERATOR_MASK_LOGICAL | DEVPROP_OPERATOR_MASK_LIST | DEVPROP_OPERATOR_MASK_ARRAY))
        return E_NOTIMPL;
    if (Op & ~(DEVPROP_OPERATOR_MASK_EVAL | DEVPROP_OPERATOR_MODIFIER_NOT | DEVPROP_OPERATOR_MODIFIER_IGNORE_CASE))
        return E_INVALIDARG;

    Prop = DevqLookupKey(&Expression->Property.CompKey);
    if (Prop == DevqPropMax)
        return E_NOTIMPL;

    switch (Eval)
    {
        case DEVPROP_OPERATOR_EXISTS:
            return S_OK;
        case DEVPROP_OPERATOR_EQUALS:
            break;
        case DEVPROP_OPERATOR_BEGINS_WITH:
        case DEVPROP_OPERATOR_ENDS_WITH:
        case DEVPROP_OPERATOR_CONTAINS:
            if (Type != DEVPROP_TYPE_STRING)
                return E_NOTIMPL;
            break;
        case DEVPROP_OPERATOR_NONE:
            return E_INVALIDARG;
        default:
            return E_NOTIMPL;
    }

    /* Comparing with nothing tests that the property is absent */
    if (Type == DEVPROP_TYPE_EMPTY)
        return S_OK;
    if (!Expression->Property.Buffer)
        return E_INVALIDARG;
    switch (Type)
    {
        case DEVPROP_TYPE_STRING:
            return S_OK;
        case DEVPROP_TYPE_BOOLEAN:
            return Expression->Property.BufferSize == sizeof(DEVPROP_BOOLEAN) ? S_OK : E_INVALIDARG;
        case DEVPROP_TYPE_GUID:
            return Expression->Property.BufferSize == sizeof(GUID) ? S_OK : E_INVALIDARG;
        default:
            return E_NOTIMPL;
    }
}

static
BOOL
DevqMatchExpression(
    _In_ const DEVQ_ITEM *Item,
    _In_ const DEVPROP_FILTER_EXPRESSION *Expression)
{
    DEVPROP_OPERATOR Op = Expression->Operator;
    DEVPROP_OPERATOR Eval = Op & DEVPROP_OPERATOR_MASK_EVAL;
    const DEVPROPERTY *Operand = &Expression->Property;
    DEVPROPTYPE Type;
    const VOID *Buffer;
    ULONG Size;
    PCWSTR String;
    SIZE_T Length;
    BOOL Match;

    DevqGetItemProperty(Item, DevqLookupKey(&Operand->CompKey), &Type, &Buffer, &Size);

    if (Eval == DEVPROP_OPERATOR_EXISTS)
        Match = (Type != DEVPROP_TYPE_EMPTY);
    else if (Operand->Type == DEVPROP_TYPE_EMPTY)
        Match = (Type == DEVPROP_TYPE_EMPTY);
    else if (Type != Operand->Type)
        Match = FALSE;
    else if (Type == DEVPROP_TYPE_STRING)
        Match = DevqFilterString(Operand, &String, &Length) &&
                DevqCompareString(Eval, !!(Op & DEVPROP_OPERATOR_MODIFIER_IGNORE_CASE),
                                  Buffer, String, Length);
    else if (Type == DEVPROP_TYPE_BOOLEAN)
        Match = !*(const DEVPROP_BOOLEAN *)Buffer == !*(const DEVPROP_BOOLEAN *)Operand->Buffer;
    else
        Match = (Size == Operand->BufferSize && !memcmp(Buffer, Operand->Buffer, Size));

    return (Op & DEVPROP_OPERATOR_MODIFIER_NOT) ? !Match : Match;
}

static
BOOL
DevqMatch(
    _In_ ULONG cFilter,
    _In_reads_opt_(cFilter) const DEVPROP_FILTER_EXPRESSION *Filter,
    _In_ const DEVQ_ITEM *Item)
{
    ULONG i;

    for (i = 0; i < cFilter; i++)
    {
        if (!DevqMatchExpression(Item, &Filter[i]))
            return FALSE;
    }
    return TRUE;
}

/* Copy the arguments of a query, rejecting what we cannot do */
static
HRESULT
DevqInitQuery(
    _Out_ PDEV_QUERY Query,
    _In_ DEV_OBJECT_TYPE ObjectType,
    _In_ ULONG QueryFlags,
    _In_ ULONG cRequestedProperties,
    _In_reads_opt_(cRequestedProperties) const DEVPROPCOMPKEY *pRequestedProperties,
    _In_ ULONG cFilterExpressionCount,
    _In_reads_opt_(cFilterExpressionCount) const DEVPROP_FILTER_EXPRESSION *pFilter)
{
    HRESULT hr;
    ULONG i;

    ZeroMemory(Query, sizeof(*Query));

    if ((cRequestedProperties && !pRequestedProperties) || (cFilterExpressionCount && !pFilter))
        return E_INVALIDARG;
    if (QueryFlags & ~DEVQUERY_KNOWN_FLAGS)
        return E_INVALIDARG;
    if ((QueryFlags & DevQueryFlagAllProperties) && cRequestedProperties)
        return E_INVALIDARG;
    if (ObjectType <= DevObjectTypeUnknown || ObjectType > DevObjectTypeDevicePanel)
        return E_INVALIDARG;
    if (ObjectType != DevObjectTypeDeviceInterface)
    {
        FIXME("Object type %d is not supported\n", ObjectType);
        return E_NOTIMPL;
    }

    for (i = 0; i < cFilterExpressionCount; i++)
    {
        hr = DevqValidateExpression(&pFilter[i]);
        if (FAILED(hr))
        {
            FIXME("Filter expression %lu (operator 0x%lx) is not supported\n", i, pFilter[i].Operator);
            return hr;
        }
    }
    for (i = 0; i < cRequestedProperties; i++)
    {
        if (pRequestedProperties[i].Store != DEVPROP_STORE_SYSTEM &&
            pRequestedProperties[i].Store != DEVPROP_STORE_USER)
            return E_INVALIDARG;
    }

    Query->Flags = QueryFlags;
    if (cRequestedProperties)
    {
        Query->Requested = HeapAlloc(GetProcessHeap(), 0, cRequestedProperties * sizeof(*Query->Requested));
        if (!Query->Requested)
            return E_OUTOFMEMORY;
        /* We report no localized values, so we need not keep the locale names */
        for (i = 0; i < cRequestedProperties; i++)
        {
            Query->Requested[i].Key = pRequestedProperties[i].Key;
            Query->Requested[i].Store = pRequestedProperties[i].Store;
            Query->Requested[i].LocaleName = NULL;
        }
        Query->cRequested = cRequestedProperties;
    }
    else if (QueryFlags & DevQueryFlagAllProperties)
    {
        Query->Requested = HeapAlloc(GetProcessHeap(), 0, DevqPropMax * sizeof(*Query->Requested));
        if (!Query->Requested)
            return E_OUTOFMEMORY;
        for (i = 0; i < DevqPropMax; i++)
        {
            Query->Requested[i].Key = *DevqPropKey(i);
            Query->Requested[i].Store = DEVPROP_STORE_SYSTEM;
            Query->Requested[i].LocaleName = NULL;
        }
        Query->cRequested = DevqPropMax;
    }

    if (cFilterExpressionCount)
    {
        SIZE_T Size = cFilterExpressionCount * sizeof(*Query->Filter);
        PBYTE Data;

        /* The filter operands go right behind the expressions */
        for (i = 0; i < cFilterExpressionCount; i++)
            Size += (pFilter[i].Property.Buffer ? pFilter[i].Property.BufferSize : 0) + sizeof(ULONG_PTR);
        Query->Filter = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Query->Filter)
        {
            HeapFree(GetProcessHeap(), 0, Query->Requested);
            Query->Requested = NULL;
            return E_OUTOFMEMORY;
        }
        Data = (PBYTE)&Query->Filter[cFilterExpressionCount];
        for (i = 0; i < cFilterExpressionCount; i++)
        {
            Query->Filter[i] = pFilter[i];
            Query->Filter[i].Property.CompKey.LocaleName = NULL;
            if (pFilter[i].Property.Buffer)
            {
                memcpy(Data, pFilter[i].Property.Buffer, pFilter[i].Property.BufferSize);
                Query->Filter[i].Property.Buffer = Data;
                Data += ALIGN_UP_BY(pFilter[i].Property.BufferSize, sizeof(ULONG_PTR));
            }
        }
        Query->cFilter = cFilterExpressionCount;
    }

    return S_OK;
}

static
VOID
DevqFreeQueryData(
    _Inout_ PDEV_QUERY Query)
{
    DevqFreeItems(Query->Results);
    HeapFree(GetProcessHeap(), 0, Query->Requested);
    HeapFree(GetProcessHeap(), 0, Query->Filter);
}

/*
 * Build the DEV_OBJECT of an item, the requested properties included.
 * Everything lives in one allocation, freed with HeapFree.
 */
static
DEVPROPERTY *
DevqBuildProperties(
    _In_ PDEV_QUERY Query,
    _In_ const DEVQ_ITEM *Item)
{
    DEVPROPERTY *Properties;
    DEVPROPTYPE Type;
    const VOID *Buffer;
    ULONG BufferSize, i;
    SIZE_T Size;
    PBYTE Data;

    if (!Query->cRequested)
        return NULL;

    Size = Query->cRequested * sizeof(DEVPROPERTY);
    for (i = 0; i < Query->cRequested; i++)
    {
        DevqGetItemProperty(Item, DevqLookupKey(&Query->Requested[i]), &Type, &Buffer, &BufferSize);
        Size += ALIGN_UP_BY(BufferSize, sizeof(ULONG_PTR));
    }

    Properties = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Properties)
        return NULL;

    Data = (PBYTE)&Properties[Query->cRequested];
    for (i = 0; i < Query->cRequested; i++)
    {
        DevqGetItemProperty(Item, DevqLookupKey(&Query->Requested[i]), &Type, &Buffer, &BufferSize);
        Properties[i].CompKey = Query->Requested[i];
        Properties[i].Type = Type;
        Properties[i].BufferSize = BufferSize;
        if (BufferSize)
        {
            memcpy(Data, Buffer, BufferSize);
            Properties[i].Buffer = Data;
            Data += ALIGN_UP_BY(BufferSize, sizeof(ULONG_PTR));
        }
    }
    return Properties;
}

static
BOOL
DevqIsStopping(
    _In_ PDEV_QUERY Query)
{
    return WaitForSingleObject(Query->StopEvent, 0) == WAIT_OBJECT_0;
}

static
VOID
DevqReportObject(
    _In_ PDEV_QUERY Query,
    _In_ DEV_QUERY_RESULT_ACTION Action,
    _In_ const DEVQ_ITEM *Item)
{
    DEV_QUERY_RESULT_ACTION_DATA ActionData;
    DEVPROPERTY *Properties = DevqBuildProperties(Query, Item);

    ZeroMemory(&ActionData, sizeof(ActionData));
    ActionData.Action = Action;
    ActionData.Data.DeviceObject.ObjectType = DevObjectTypeDeviceInterface;
    ActionData.Data.DeviceObject.pszObjectId = Item->ObjectId;
    /* Without the memory for the properties, report the object without them */
    ActionData.Data.DeviceObject.cPropertyCount = Properties ? Query->cRequested : 0;
    ActionData.Data.DeviceObject.pProperties = Properties;
    Query->Callback((HDEVQUERY)Query, Query->Context, &ActionData);
    HeapFree(GetProcessHeap(), 0, Properties);
}

static
VOID
DevqReportState(
    _In_ PDEV_QUERY Query,
    _In_ DEV_QUERY_STATE State)
{
    DEV_QUERY_RESULT_ACTION_DATA ActionData;

    ZeroMemory(&ActionData, sizeof(ActionData));
    ActionData.Action = DevQueryResultStateChange;
    ActionData.Data.State = State;
    Query->Callback((HDEVQUERY)Query, Query->Context, &ActionData);
}

/* Unlink and return the item with the given object id */
static
PDEVQ_ITEM
DevqTakeItem(
    _Inout_ PDEVQ_ITEM *List,
    _In_ PCWSTR ObjectId)
{
    PDEVQ_ITEM *Link, Item;

    for (Link = List; (Item = *Link); Link = &Item->Next)
    {
        if (!_wcsicmp(Item->ObjectId, ObjectId))
        {
            *Link = Item->Next;
            Item->Next = NULL;
            return Item;
        }
    }
    return NULL;
}

static
BOOL
DevqSameRequested(
    _In_ PDEV_QUERY Query,
    _In_ const DEVQ_ITEM *Old,
    _In_ const DEVQ_ITEM *New)
{
    DEVPROPTYPE OldType, NewType;
    const VOID *OldBuffer, *NewBuffer;
    ULONG OldSize, NewSize, i;
    DEVQ_PROP Prop;

    for (i = 0; i < Query->cRequested; i++)
    {
        Prop = DevqLookupKey(&Query->Requested[i]);
        DevqGetItemProperty(Old, Prop, &OldType, &OldBuffer, &OldSize);
        DevqGetItemProperty(New, Prop, &NewType, &NewBuffer, &NewSize);
        if (OldType != NewType || OldSize != NewSize || (OldSize && memcmp(OldBuffer, NewBuffer, OldSize)))
            return FALSE;
    }
    return TRUE;
}

/*
 * Bring the reported results up to date with the current state, reporting
 * each new, changed or vanished match. Returns FALSE if the enumeration
 * failed.
 */
static
BOOL
DevqUpdateResults(
    _In_ PDEV_QUERY Query)
{
    PDEVQ_ITEM Current, Item, Next, Old, Kept = NULL;

    if (DevqEnumerate(Query, &Current) != ERROR_SUCCESS)
        return FALSE;

    /* Reverse the list: report in enumeration order */
    for (Item = Current, Current = NULL; Item; Item = Next)
    {
        Next = Item->Next;
        Item->Next = Current;
        Current = Item;
    }

    for (Item = Current; Item && !DevqIsStopping(Query); Item = Next)
    {
        Next = Item->Next;
        Item->Next = NULL;
        Old = DevqTakeItem(&Query->Results, Item->ObjectId);
        if (!DevqMatch(Query->cFilter, Query->Filter, Item))
        {
            DevqFreeItems(Item);
            if (Old)
            {
                DevqReportObject(Query, DevQueryResultRemove, Old);
                DevqFreeItems(Old);
            }
            continue;
        }

        if (!Old)
            DevqReportObject(Query, DevQueryResultAdd, Item);
        else if (!DevqSameRequested(Query, Old, Item))
            DevqReportObject(Query, DevQueryResultUpdate, Item);
        DevqFreeItems(Old);
        Item->Next = Kept;
        Kept = Item;
    }
    /* Stopped half way */
    DevqFreeItems(Item);

    /* What is left of the old results has gone away */
    for (Item = Query->Results; Item && !DevqIsStopping(Query); Item = Item->Next)
        DevqReportObject(Query, DevQueryResultRemove, Item);
    DevqFreeItems(Query->Results);
    Query->Results = Kept;
    return TRUE;
}

static
VOID
DevqRelease(
    _In_ PDEV_QUERY Query)
{
    if (InterlockedDecrement(&Query->RefCount) != 0)
        return;
    if (Query->Thread)
        CloseHandle(Query->Thread);
    if (Query->StopEvent)
        CloseHandle(Query->StopEvent);
    DevqFreeQueryData(Query);
    Query->Magic = 0;
    HeapFree(GetProcessHeap(), 0, Query);
}

static
DWORD
WINAPI
DevqWorker(
    _In_ PVOID Parameter)
{
    PDEV_QUERY Query = Parameter;

    if (!DevqUpdateResults(Query))
    {
        if (!DevqIsStopping(Query))
            DevqReportState(Query, DevQueryStateAborted);
    }
    else if (!DevqIsStopping(Query))
    {
        DevqReportState(Query, DevQueryStateEnumCompleted);

        /* We have no arrival notifications: watch for changes */
        if (Query->Flags & DevQueryFlagUpdateResults)
        {
            while (WaitForSingleObject(Query->StopEvent, DEVQUERY_POLL_INTERVAL) == WAIT_TIMEOUT)
            {
                if (!DevqUpdateResults(Query))
                {
                    if (!DevqIsStopping(Query))
                        DevqReportState(Query, DevQueryStateAborted);
                    break;
                }
            }
        }
    }

    /* An asynchronous close is over once the callback hears of it */
    if (Query->Flags & DevQueryFlagAsyncClose)
    {
        WaitForSingleObject(Query->StopEvent, INFINITE);
        DevqReportState(Query, DevQueryStateClosed);
    }

    DevqRelease(Query);
    return 0;
}

/***********************************************************************
 *		DevCreateObjectQuery (SETUPAPI.@)
 */
HRESULT
WINAPI
DevCreateObjectQuery(
    _In_ DEV_OBJECT_TYPE ObjectType,
    _In_ ULONG QueryFlags,
    _In_ ULONG cRequestedProperties,
    _In_reads_opt_(cRequestedProperties) const DEVPROPCOMPKEY *pRequestedProperties,
    _In_ ULONG cFilterExpressionCount,
    _In_reads_opt_(cFilterExpressionCount) const DEVPROP_FILTER_EXPRESSION *pFilter,
    _In_ PDEV_QUERY_RESULT_CALLBACK pCallback,
    _In_opt_ PVOID pContext,
    _Out_ PHDEVQUERY phDevQuery)
{
    PDEV_QUERY Query;
    HRESULT hr;

    TRACE("DevCreateObjectQuery(%d 0x%lx %lu %p %lu %p %p %p %p)\n",
          ObjectType, QueryFlags, cRequestedProperties, pRequestedProperties,
          cFilterExpressionCount, pFilter, pCallback, pContext, phDevQuery);

    if (!phDevQuery)
        return E_INVALIDARG;
    *phDevQuery = NULL;
    if (!pCallback)
        return E_INVALIDARG;

    Query = HeapAlloc(GetProcessHeap(), 0, sizeof(*Query));
    if (!Query)
        return E_OUTOFMEMORY;
    hr = DevqInitQuery(Query, ObjectType, QueryFlags, cRequestedProperties, pRequestedProperties,
                       cFilterExpressionCount, pFilter);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, Query);
        return hr;
    }
    Query->Magic = DEVQUERY_MAGIC;
    Query->Callback = pCallback;
    Query->Context = pContext;
    /* One reference for the handle, one for the worker */
    Query->RefCount = 2;

    Query->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Query->StopEvent)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        DevqFreeQueryData(Query);
        HeapFree(GetProcessHeap(), 0, Query);
        return hr;
    }

    /* The callbacks may come before we return: the handle must be valid by then */
    Query->Thread = CreateThread(NULL, 0, DevqWorker, Query, CREATE_SUSPENDED, &Query->ThreadId);
    if (!Query->Thread)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(Query->StopEvent);
        DevqFreeQueryData(Query);
        HeapFree(GetProcessHeap(), 0, Query);
        return hr;
    }
    *phDevQuery = (HDEVQUERY)Query;
    ResumeThread(Query->Thread);
    return S_OK;
}

/***********************************************************************
 *		DevCloseObjectQuery (SETUPAPI.@)
 */
VOID
WINAPI
DevCloseObjectQuery(
    _In_ HDEVQUERY hDevQuery)
{
    PDEV_QUERY Query = (PDEV_QUERY)hDevQuery;

    TRACE("DevCloseObjectQuery(%p)\n", hDevQuery);

    if (!Query || Query->Magic != DEVQUERY_MAGIC)
        return;
    Query->Magic = 0;

    SetEvent(Query->StopEvent);

    /*
     * Unless asked for an asynchronous close, no callback runs once we
     * return. A callback closing its own query cannot wait for itself:
     * no other callback follows it, as the worker checks for the stop.
     */
    if (!(Query->Flags & DevQueryFlagAsyncClose) && GetCurrentThreadId() != Query->ThreadId)
        WaitForSingleObject(Query->Thread, INFINITE);

    DevqRelease(Query);
}

/***********************************************************************
 *		DevGetObjects (SETUPAPI.@)
 */
HRESULT
WINAPI
DevGetObjects(
    _In_ DEV_OBJECT_TYPE ObjectType,
    _In_ ULONG QueryFlags,
    _In_ ULONG cRequestedProperties,
    _In_reads_opt_(cRequestedProperties) const DEVPROPCOMPKEY *pRequestedProperties,
    _In_ ULONG cFilterExpressionCount,
    _In_reads_opt_(cFilterExpressionCount) const DEVPROP_FILTER_EXPRESSION *pFilter,
    _Out_ PULONG pcObjectCount,
    _Outptr_result_buffer_maybenull_(*pcObjectCount) const DEV_OBJECT **ppObjects)
{
    DEV_QUERY Query;
    PDEVQ_ITEM Items, Item;
    DEV_OBJECT *Objects;
    ULONG Count = 0, i;
    DWORD Error;
    HRESULT hr;

    TRACE("DevGetObjects(%d 0x%lx %lu %p %lu %p %p %p)\n",
          ObjectType, QueryFlags, cRequestedProperties, pRequestedProperties,
          cFilterExpressionCount, pFilter, pcObjectCount, ppObjects);

    if (!pcObjectCount || !ppObjects)
        return E_INVALIDARG;
    *pcObjectCount = 0;
    *ppObjects = NULL;
    /* There is nothing to update or close */
    if (QueryFlags & (DevQueryFlagUpdateResults | DevQueryFlagAsyncClose))
        return E_INVALIDARG;

    hr = DevqInitQuery(&Query, ObjectType, QueryFlags, cRequestedProperties, pRequestedProperties,
                       cFilterExpressionCount, pFilter);
    if (FAILED(hr))
        return hr;

    Error = DevqEnumerate(&Query, &Items);
    if (Error != ERROR_SUCCESS)
    {
        DevqFreeQueryData(&Query);
        return HRESULT_FROM_WIN32(Error);
    }

    for (Item = Items; Item; Item = Item->Next)
    {
        Item->Seen = DevqMatch(Query.cFilter, Query.Filter, Item);
        if (Item->Seen)
            Count++;
    }

    Objects = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, max(Count, 1) * sizeof(*Objects));
    if (!Objects)
    {
        DevqFreeItems(Items);
        DevqFreeQueryData(&Query);
        return E_OUTOFMEMORY;
    }

    /* The items come in reverse enumeration order */
    i = Count;
    for (Item = Items; Item; Item = Item->Next)
    {
        if (!Item->Seen)
            continue;
        i--;
        Objects[i].ObjectType = DevObjectTypeDeviceInterface;
        Objects[i].pszObjectId = DevqStrDup(Item->ObjectId, wcslen(Item->ObjectId));
        Objects[i].pProperties = DevqBuildProperties(&Query, Item);
        Objects[i].cPropertyCount = Objects[i].pProperties ? Query.cRequested : 0;
        if (!Objects[i].pszObjectId || (Query.cRequested && !Objects[i].pProperties))
            hr = E_OUTOFMEMORY;
    }

    DevqFreeItems(Items);
    DevqFreeQueryData(&Query);
    if (FAILED(hr))
    {
        DevFreeObjects(Count, Objects);
        return hr;
    }

    *pcObjectCount = Count;
    *ppObjects = Objects;
    return S_OK;
}

/***********************************************************************
 *		DevFreeObjects (SETUPAPI.@)
 */
VOID
WINAPI
DevFreeObjects(
    _In_ ULONG cObjectCount,
    _In_reads_(cObjectCount) const DEV_OBJECT *pObjects)
{
    ULONG i;

    TRACE("DevFreeObjects(%lu %p)\n", cObjectCount, pObjects);

    if (!pObjects)
        return;
    for (i = 0; i < cObjectCount; i++)
    {
        HeapFree(GetProcessHeap(), 0, (PVOID)pObjects[i].pszObjectId);
        HeapFree(GetProcessHeap(), 0, (PVOID)pObjects[i].pProperties);
    }
    HeapFree(GetProcessHeap(), 0, (PVOID)pObjects);
}
