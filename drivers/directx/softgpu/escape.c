/*
 * PROJECT:     ReactOS WDDM Null/Software GPU Miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     DxgkDdiEscape: driver-private escapes used by the d3dkmt
 *              apitests to exercise dxgkrnl services from a miniport
 * COPYRIGHT:   Copyright 2026 ReactOS WDDM Team
 */

#include "softgpu.h"
#include "softgpu_escape.h"

typedef NTSTATUS
(APIENTRY *PFN_SOFTGPU_QUERY_SERVICES)(
    _In_    HANDLE  DeviceHandle,
    _In_    ULONG   ServicesType,
    _Inout_ PVOID   Interface);

#define MS_100NS(ms) ((LONGLONG)(ms) * 10 * 1000)

/* Short intervals with the performance counter: the interrupt time only
 * moves once per clock tick. */
static LONGLONG
SoftGpuNow100ns(VOID)
{
    LARGE_INTEGER Frequency, Counter = KeQueryPerformanceCounter(&Frequency);

    return (LONGLONG)((Counter.QuadPart / Frequency.QuadPart) * 10000000 +
                      ((Counter.QuadPart % Frequency.QuadPart) * 10000000) / Frequency.QuadPart);
}

static VOID
SoftGpuTimedOpScenario(
    _In_    PSOFTGPU_DEVICE              Device,
    _Inout_ SOFTGPU_ESCAPE_TIMEDOP_DATA *Data)
{
    PFN_SOFTGPU_QUERY_SERVICES QueryServices =
        (PFN_SOFTGPU_QUERY_SERVICES)Device->DxgkInterface.DxgkCbQueryServices;
    DXGK_TIMED_OPERATION_INTERFACE Timed;
    DXGK_TIMED_OPERATION Op;
    LARGE_INTEGER Timeout, Interval;
    LONGLONG Start, FineStart;
    KEVENT Event;
    ULONG Limit;

    PAGED_CODE();

    if (QueryServices == NULL)
    {
        Data->QueryStatus = STATUS_NOT_SUPPORTED;
        return;
    }

    RtlZeroMemory(&Timed, sizeof(Timed));
    Timed.Size = sizeof(Timed);
    Timed.Version = DXGK_TIMED_OPERATION_INTERFACE_VERSION_1;
    Data->QueryStatus = QueryServices(Device->DxgkInterface.DeviceHandle,
                                      DxgkServicesTimedOperation, &Timed);
    if (!NT_SUCCESS(Data->QueryStatus))
        return;

    /* The driver presets Size, as the WDK asks (atikmdag does). */
    RtlZeroMemory(&Op, sizeof(Op));
    Op.Size = sizeof(Op);

    switch (Data->Scenario)
    {
        case SOFTGPU_TIMEDOP_ZERO_TIMEOUT:      Timeout.QuadPart = 0; break;
        case SOFTGPU_TIMEDOP_POLL:
        case SOFTGPU_TIMEDOP_POSITIVE_INTERVAL: Timeout.QuadPart = MS_100NS(2000); break;
        case SOFTGPU_TIMEDOP_NEGATIVE_TIMEOUT:  Timeout.QuadPart = -MS_100NS(2000); break;
        case SOFTGPU_TIMEDOP_CLAMP:             Timeout.QuadPart = MS_100NS(10000); break;
        case SOFTGPU_TIMEDOP_DEADLINE:          Timeout.QuadPart = MS_100NS(100); break;
        case SOFTGPU_TIMEDOP_BAD_SIZE:          Timeout.QuadPart = MS_100NS(100); Op.Size = 0; break;
        default:
            Data->StartStatus = STATUS_INVALID_PARAMETER;
            return;
    }

    Start = (LONGLONG)KeQueryInterruptTime();
    FineStart = SoftGpuNow100ns();
    Data->StartStatus = Timed.TimedOperationStart(&Op, &Timeout, FALSE);
    if (!NT_SUCCESS(Data->StartStatus))
        goto Done;

    switch (Data->Scenario)
    {
        case SOFTGPU_TIMEDOP_ZERO_TIMEOUT:
        case SOFTGPU_TIMEDOP_POLL:
        case SOFTGPU_TIMEDOP_NEGATIVE_TIMEOUT:
            Data->OpStatus = Timed.TimedOperationDelay(&Op, KernelMode, FALSE, NULL);
            Data->OpCount = 1;
            break;

        case SOFTGPU_TIMEDOP_POSITIVE_INTERVAL:
            /* The sign of the interval is ignored. */
            Interval.QuadPart = MS_100NS(20);
            Data->OpStatus = Timed.TimedOperationDelay(&Op, KernelMode, FALSE, &Interval);
            Data->OpCount = 1;
            break;

        case SOFTGPU_TIMEDOP_CLAMP:
            KeInitializeEvent(&Event, NotificationEvent, FALSE);
            Data->OpStatus = Timed.TimedOperationWaitForSingleObject(&Op, &Event, Executive,
                                                                     KernelMode, FALSE, NULL);
            Data->OpCount = 1;
            break;

        case SOFTGPU_TIMEDOP_DEADLINE:
            /* atikmdag's poll loop: Delay(NULL) until the deadline passes. */
            for (Limit = 0; Limit < 100000000; Limit++)
            {
                Data->OpStatus = Timed.TimedOperationDelay(&Op, KernelMode, FALSE, NULL);
                Data->OpCount++;
                if (Data->OpStatus != STATUS_SUCCESS)
                    break;
                if ((LONGLONG)KeQueryInterruptTime() - Start > MS_100NS(10000))
                    break;
            }
            break;
    }

Done:
    Data->Elapsed100ns = (LONGLONG)KeQueryInterruptTime() - Start;
    Data->FineElapsed100ns = SoftGpuNow100ns() - FineStart;
    Data->TimeoutTriggered = Op.TimeoutTriggered;
    DPRINT1("SOFTGPU: timed-op scenario %lu: query 0x%08lx start 0x%08lx op 0x%08lx "
            "x%lu elapsed %I64d00ns triggered %u\n",
            Data->Scenario, Data->QueryStatus, Data->StartStatus, Data->OpStatus,
            Data->OpCount, Data->Elapsed100ns, Data->TimeoutTriggered);
}

NTSTATUS
APIENTRY
SoftGpuDdiEscape(
    _In_ CONST HANDLE           hAdapter,
    _In_ CONST DXGKARG_ESCAPE  *Escape)
{
    PSOFTGPU_DEVICE Device = (PSOFTGPU_DEVICE)hAdapter;
    SOFTGPU_ESCAPE_TIMEDOP_DATA *Data;

    PAGED_CODE();

    if (Device == NULL || Device->Magic != SOFTGPU_DEVICE_MAGIC || Escape == NULL ||
        Escape->pPrivateDriverData == NULL || Escape->PrivateDriverDataSize < sizeof(ULONG))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Data = Escape->pPrivateDriverData;
    if (Data->Code == SOFTGPU_ESCAPE_TIMEDOP)
    {
        if (Escape->PrivateDriverDataSize < sizeof(*Data))
            return STATUS_BUFFER_TOO_SMALL;
        SoftGpuTimedOpScenario(Device, Data);
        return STATUS_SUCCESS;
    }
    if (Data->Code == SOFTGPU_ESCAPE_PRESENT_STATS)
    {
        SOFTGPU_ESCAPE_PRESENT_STATS_DATA *Stats = Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(*Stats))
            return STATUS_BUFFER_TOO_SMALL;
        Stats->BltCount = (unsigned long)Device->BltCount;
        Stats->FullBltCount = (unsigned long)Device->FullBltCount;
        Stats->LastLeft = Device->LastBltBounds.left;
        Stats->LastTop = Device->LastBltBounds.top;
        Stats->LastRight = Device->LastBltBounds.right;
        Stats->LastBottom = Device->LastBltBounds.bottom;
        Stats->DstWidth = Device->LastBltDstWidth;
        Stats->DstHeight = Device->LastBltDstHeight;
        return STATUS_SUCCESS;
    }
    if (Data->Code == SOFTGPU_ESCAPE_SEGMENT)
    {
        SOFTGPU_ESCAPE_SEGMENT_DATA *Seg = Escape->pPrivateDriverData;
        DXGK_SEGMENTDESCRIPTOR3 Desc;

        if (Escape->PrivateDriverDataSize < sizeof(*Seg))
            return STATUS_BUFFER_TOO_SMALL;
        SoftGpuDescribeSegment(Device, &Desc);
        Seg->Flags = Desc.Flags.Value;
        Seg->BaseAddress = (unsigned long long)Desc.BaseAddress.QuadPart;
        Seg->CpuTranslatedAddress = (unsigned long long)Desc.CpuTranslatedAddress.QuadPart;
        Seg->Size = Desc.Size;
        Seg->HasVram = Device->HasVram;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}
