/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Notification Facility primitives (ReactOS stub)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * Microsoft did not publish MxWnf. WDF only uses it for Sleep Study, which
 * ReactOS does not support, so every operation fails with
 * STATUS_NOT_SUPPORTED and the framework keeps Sleep Study disabled.
 */

#ifndef _MXWNF_H_
#define _MXWNF_H_

#ifdef FX_INCLUDE_WNF_TYPES

#ifndef _WNF_STATE_NAME_DEFINED
#define _WNF_STATE_NAME_DEFINED
typedef struct _WNF_STATE_NAME {
    ULONG Data[2];
} WNF_STATE_NAME, *PWNF_STATE_NAME;
typedef const WNF_STATE_NAME *PCWNF_STATE_NAME;
#endif

/* Defined by Windows in wnfnamesp.h */
#ifndef WNF_PO_DRIPS_DEVICE_CONSTRAINTS_REGISTERED
#define WNF_PO_DRIPS_DEVICE_CONSTRAINTS_REGISTERED { { 0xA3BC48F5, 0x41960B3A } }
#endif

#endif /* FX_INCLUDE_WNF_TYPES */

typedef struct _MxWnfSubscriptionContext *PMxWnfSubscriptionContext;

typedef
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
MX_WNF_CALLBACK(
    _In_ PMxWnfSubscriptionContext SubscriptionContext,
    _In_ PVOID CallbackContext);

typedef MX_WNF_CALLBACK *PFN_MX_WNF_CALLBACK;

class MxWnf {
public:
    static
    NTSTATUS
    MxSubscribeWnfStateChange(
        _Out_ PMxWnfSubscriptionContext* SubscriptionContext,
        _In_ PCWNF_STATE_NAME StateName,
        _In_ PFN_MX_WNF_CALLBACK Callback,
        _In_opt_ PVOID CallbackContext,
        _In_ PVOID Tag)
    {
        UNREFERENCED_PARAMETER(StateName);
        UNREFERENCED_PARAMETER(Callback);
        UNREFERENCED_PARAMETER(CallbackContext);
        UNREFERENCED_PARAMETER(Tag);
        *SubscriptionContext = NULL;
        return STATUS_NOT_SUPPORTED;
    }

    static
    VOID
    MxUnsubscribeWnfStateChange(
        _Inout_ PMxWnfSubscriptionContext* SubscriptionContext)
    {
        *SubscriptionContext = NULL;
    }

    static
    NTSTATUS
    MxQueryWnfStateData(
        _In_ PMxWnfSubscriptionContext SubscriptionContext,
        _Out_writes_bytes_(*BufferSize) PVOID Buffer,
        _Inout_ PULONG BufferSize)
    {
        UNREFERENCED_PARAMETER(SubscriptionContext);
        UNREFERENCED_PARAMETER(Buffer);
        UNREFERENCED_PARAMETER(BufferSize);
        return STATUS_NOT_SUPPORTED;
    }
};

#endif /* _MXWNF_H_ */
