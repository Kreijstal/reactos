/*
 * devquery.h - Device object queries (devquerydef.h, devfiltertypes.h)
 *
 * This file is part of the ReactOS PSDK package.
 */

#pragma once

#include <devpropdef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (NTDDI_VERSION >= NTDDI_WIN8)

/* devfiltertypes.h */
typedef enum _DEVPROP_OPERATOR
{
    DEVPROP_OPERATOR_MODIFIER_NOT = 0x00010000,
    DEVPROP_OPERATOR_MODIFIER_IGNORE_CASE = 0x00020000,
    DEVPROP_OPERATOR_NONE = 0x00000000,
    DEVPROP_OPERATOR_EXISTS = 0x00000001,
    DEVPROP_OPERATOR_NOT_EXISTS = 0x00010001,
    DEVPROP_OPERATOR_EQUALS = 0x00000002,
    DEVPROP_OPERATOR_NOT_EQUALS = 0x00010002,
    DEVPROP_OPERATOR_GREATER_THAN = 0x00000003,
    DEVPROP_OPERATOR_LESS_THAN = 0x00000004,
    DEVPROP_OPERATOR_GREATER_THAN_EQUALS = 0x00000005,
    DEVPROP_OPERATOR_LESS_THAN_EQUALS = 0x00000006,
    DEVPROP_OPERATOR_EQUALS_IGNORE_CASE = 0x00020002,
    DEVPROP_OPERATOR_NOT_EQUALS_IGNORE_CASE = 0x00030002,
    DEVPROP_OPERATOR_BITWISE_AND = 0x00000007,
    DEVPROP_OPERATOR_BITWISE_OR = 0x00000008,
    DEVPROP_OPERATOR_BEGINS_WITH = 0x00000009,
    DEVPROP_OPERATOR_ENDS_WITH = 0x0000000a,
    DEVPROP_OPERATOR_CONTAINS = 0x0000000b,
    DEVPROP_OPERATOR_BEGINS_WITH_IGNORE_CASE = 0x00020009,
    DEVPROP_OPERATOR_ENDS_WITH_IGNORE_CASE = 0x0002000a,
    DEVPROP_OPERATOR_CONTAINS_IGNORE_CASE = 0x0002000b,
    DEVPROP_OPERATOR_LIST_CONTAINS = 0x00001000,
    DEVPROP_OPERATOR_LIST_ELEMENT_BEGINS_WITH = 0x00002000,
    DEVPROP_OPERATOR_LIST_ELEMENT_ENDS_WITH = 0x00003000,
    DEVPROP_OPERATOR_LIST_ELEMENT_CONTAINS = 0x00004000,
    DEVPROP_OPERATOR_LIST_CONTAINS_IGNORE_CASE = 0x00021000,
    DEVPROP_OPERATOR_LIST_ELEMENT_BEGINS_WITH_IGNORE_CASE = 0x00022000,
    DEVPROP_OPERATOR_LIST_ELEMENT_ENDS_WITH_IGNORE_CASE = 0x00023000,
    DEVPROP_OPERATOR_LIST_ELEMENT_CONTAINS_IGNORE_CASE = 0x00024000,
    DEVPROP_OPERATOR_AND_OPEN = 0x00100000,
    DEVPROP_OPERATOR_AND_CLOSE = 0x00200000,
    DEVPROP_OPERATOR_OR_OPEN = 0x00300000,
    DEVPROP_OPERATOR_OR_CLOSE = 0x00400000,
    DEVPROP_OPERATOR_NOT_OPEN = 0x00500000,
    DEVPROP_OPERATOR_NOT_CLOSE = 0x00600000,
    DEVPROP_OPERATOR_ARRAY_CONTAINS = 0x10000000,
    DEVPROP_OPERATOR_MASK_EVAL = 0x00000FFF,
    DEVPROP_OPERATOR_MASK_LIST = 0x0000F000,
    DEVPROP_OPERATOR_MASK_MODIFIER = 0x000F0000,
    DEVPROP_OPERATOR_MASK_NOT_LOGICAL = 0xF00FFFFF,
    DEVPROP_OPERATOR_MASK_LOGICAL = 0x0FF00000,
    DEVPROP_OPERATOR_MASK_ARRAY = 0xF0000000
} DEVPROP_OPERATOR, *PDEVPROP_OPERATOR;

typedef struct _DEVPROP_FILTER_EXPRESSION
{
    DEVPROP_OPERATOR Operator;
    DEVPROPERTY Property;
} DEVPROP_FILTER_EXPRESSION, *PDEVPROP_FILTER_EXPRESSION;

/* devquerydef.h */
typedef enum _DEV_OBJECT_TYPE
{
    DevObjectTypeUnknown,
    DevObjectTypeDeviceInterface,
    DevObjectTypeDeviceContainer,
    DevObjectTypeDevice,
    DevObjectTypeDeviceInterfaceClass,
    DevObjectTypeAEP,
    DevObjectTypeAEPContainer,
    DevObjectTypeDeviceInstallerClass,
    DevObjectTypeDeviceInterfaceDisplay,
    DevObjectTypeDeviceContainerDisplay,
    DevObjectTypeAEPService,
    DevObjectTypeDevicePanel
} DEV_OBJECT_TYPE, *PDEV_OBJECT_TYPE;

typedef enum _DEV_QUERY_FLAGS
{
    DevQueryFlagNone = 0x00000000,
    DevQueryFlagUpdateResults = 0x00000001,
    DevQueryFlagAllProperties = 0x00000002,
    DevQueryFlagLocalize = 0x00000004,
    DevQueryFlagAsyncClose = 0x00000008
} DEV_QUERY_FLAGS, *PDEV_QUERY_FLAGS;

typedef enum _DEV_QUERY_STATE
{
    DevQueryStateInitialized,
    DevQueryStateEnumCompleted,
    DevQueryStateAborted,
    DevQueryStateClosed
} DEV_QUERY_STATE, *PDEV_QUERY_STATE;

typedef enum _DEV_QUERY_RESULT_ACTION
{
    DevQueryResultStateChange,
    DevQueryResultAdd,
    DevQueryResultUpdate,
    DevQueryResultRemove
} DEV_QUERY_RESULT_ACTION, *PDEV_QUERY_RESULT_ACTION;

typedef struct _DEV_OBJECT
{
    DEV_OBJECT_TYPE ObjectType;
    PCWSTR pszObjectId;
    ULONG cPropertyCount;
    const DEVPROPERTY *pProperties;
} DEV_OBJECT, *PDEV_OBJECT;

typedef struct _DEV_QUERY_RESULT_ACTION_DATA
{
    DEV_QUERY_RESULT_ACTION Action;
    union
    {
        DEV_QUERY_STATE State;
        DEV_OBJECT DeviceObject;
    } Data;
} DEV_QUERY_RESULT_ACTION_DATA, *PDEV_QUERY_RESULT_ACTION_DATA;

DECLARE_HANDLE(HDEVQUERY);
typedef HDEVQUERY *PHDEVQUERY;

typedef VOID (WINAPI *PDEV_QUERY_RESULT_CALLBACK)(
    _In_ HDEVQUERY hDevQuery,
    _In_opt_ PVOID pContext,
    _In_ const DEV_QUERY_RESULT_ACTION_DATA *pActionData);

/* devquery.h */
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
    _Out_ PHDEVQUERY phDevQuery);

VOID
WINAPI
DevCloseObjectQuery(
    _In_ HDEVQUERY hDevQuery);

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
    _Outptr_result_buffer_maybenull_(*pcObjectCount) const DEV_OBJECT **ppObjects);

VOID
WINAPI
DevFreeObjects(
    _In_ ULONG cObjectCount,
    _In_reads_(cObjectCount) const DEV_OBJECT *pObjects);

#endif /* (NTDDI_VERSION >= NTDDI_WIN8) */

#ifdef __cplusplus
}
#endif
