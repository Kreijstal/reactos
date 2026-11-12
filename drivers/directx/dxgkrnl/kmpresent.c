/*
 * PROJECT:     ReactOS WDDM DirectX Graphics Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel system/GDI contexts, paging submission and the GPU
 *              shadow-to-primary present of a full WDDM miniport
 * COPYRIGHT:   Copyright 2026 ReactOS WDDM Team
 *
 * What Windows does and this file mirrors:
 *
 *  - VidMm creates a system device and a paging context
 *    (DXGK_CREATEDEVICEFLAGS.SystemDevice, DXGK_CREATECONTEXTFLAGS
 *    .SystemContext) on node 0 when the adapter starts.  Every
 *    DxgkDdiBuildPagingBuffer call carries that context, and the commands
 *    the miniport emits go to the GPU through DxgkDdiSubmitCommand with
 *    Flags.Paging, from a paging buffer placed in
 *    DXGK_QUERYSEGMENTOUT.PagingBufferSegmentId.
 *
 *  - GDI (CDD) owns a device and context flagged GdiDevice/GdiContext.
 *    It draws into a linear DXGK_STDALLOCATION_SHADOWSURFACE and copies
 *    it to the shared primary with DxgkDdiPresent(Blt) on that context:
 *    DMA buffer from DXGK_CONTEXTINFO.DmaBufferSegmentSet, allocation list
 *    with the residency of both surfaces, DxgkDdiPatch, then
 *    DxgkDdiSubmitCommand with a fresh fence.  The buffer is reused once
 *    DXGK_INTERRUPT_DMA_COMPLETED reports that fence.
 *
 * Here win32k's framebuf.dll still draws into Adapter->ShadowFb, so each
 * frame first copies the dirty rectangle of it into the GPU shadow.  The
 * scanout moves from the firmware framebuffer to the primary only after
 * the first Blt retired; until then, and whenever a stage fails, the CPU
 * blit to the firmware framebuffer in display.c stays in charge.
 */

#include "dxgkrnl_private.h"
#include "vidmm.h"

/* Write one cache line back to memory (CLFLUSH). */
FORCEINLINE VOID
DxgkpKmClflush(_In_ const void *Line)
{
#if defined(__GNUC__) || defined(__clang__)
    /* Not __builtin_ia32_clflush: that needs -msse2, which x86 builds lack */
    __asm__ __volatile__("clflush %0" : : "m" (*(volatile const char *)Line) : "memory");
#else
    _mm_clflush(Line);
#endif
}

#ifndef DXGK_PRESENT_MAX_INDEX
#define DXGK_PRESENT_MAX_INDEX DXGK_PRESENT_DESTINATION_INDEX
#endif

#define TAG_DXGK_KMPRESENT          'KxgD'

/* Bounded wait for a fence before a stage is declared failed. */
#define DXGK_KM_FENCE_TIMEOUT_MS    500

/* Per-frame and per-paging-operation log lines printed before going quiet. */
#define DXGK_KM_LOG_LIMIT           3

/* Consecutive deferred frames before a Blt in flight is reported stalled. */
#define DXGK_KM_STALL_FRAMES        120

/* CPU-only scratch handed to BuildPagingBuffer before the paging buffer exists. */
#define DXGK_KM_BOOTSTRAP_DMA_SIZE  PAGE_SIZE

typedef struct _DXGK_KM_CONTEXT
{
    /* Runtime-side objects; their addresses are the hDevice/hContext tokens. */
    DXGKRNL_DEVICE              Device;
    DXGKRNL_CONTEXT             Context;
    BOOLEAN                     DeviceCreated;
    BOOLEAN                     ContextCreated;
    DXGK_CONTEXTINFO            Info;

    /* GPU-addressable DMA buffer and its CPU-only companions. */
    DXGKVMM_KERNEL_BUFFER       Dma;
    PVOID                       DmaPrivate;
    UINT                        DmaPrivateSize;
    D3DDDI_PATCHLOCATIONLIST   *PatchList;
    UINT                        PatchListSize;
} DXGK_KM_CONTEXT, *PDXGK_KM_CONTEXT;

typedef struct _DXGK_KM_PRESENT
{
    DXGK_KM_CONTEXT             System;
    DXGK_KM_CONTEXT             Gdi;

    /* Serialises Patch + SubmitCommand so fences reach the node in order. */
    FAST_MUTEX                  SubmitMutex;

    /* Paging: one paging buffer, one operation at a time. */
    FAST_MUTEX                  PagingMutex;
    DXGKVMM_KERNEL_BUFFER       PagingBuffer;
    PVOID                       PagingPrivate;
    UINT                        PagingPrivateSize;
    PVOID                       BootstrapScratch;
    BOOLEAN                     PagingBufferReady;
    BOOLEAN                     PagingBroken;
    LONG                        PagingLogCount;

    /*
     * A fence that never retired leaves its buffers owned by the GPU: they
     * are never freed or reused after that.
     */
    BOOLEAN                     GpuOwnsBuffers;

    /* Fence completion seen only through DxgkDdiQueryCurrentFence. */
    BOOLEAN                     QueryFenceRetire;

    /* GPU present state (touched only by the serialised present worker). */
    BOOLEAN                     PresentDisabled;
    BOOLEAN                     SurfacesTried;
    BOOLEAN                     ScanoutActive;
    HANDLE                      ShadowAllocationHandle;
    PDXGKVMM_ALLOCATION         Shadow;
    PUCHAR                      ShadowVa;
    ULONG                       ShadowPitch;
    ULONG                       ShadowWidth;
    ULONG                       ShadowHeight;
    PVOID                       ShadowResourcePrivate;
    UINT                        ShadowResourcePrivateSize;
    HANDLE                      ShadowOpen;
    HANDLE                      PrimaryAllocationHandle;
    PDXGKVMM_ALLOCATION         Primary;
    HANDLE                      PrimaryOpen;
    ULONG                       InflightFence;
    LONG                        FrameCount;
    LONG                        DeferredCount;
    ULONG                       ConsecutiveDeferrals;
} DXGK_KM_PRESENT, *PDXGK_KM_PRESENT;

/* HELPERS ********************************************************************/

/*
 * Present, Patch, BuildPagingBuffer and QueryCurrentFence are
 * _IRQL_requires_(PASSIVE_LEVEL): hold our locks inside a critical region
 * instead of at the APC_LEVEL ExAcquireFastMutex raises to.
 */
static VOID
DxgkpKmLock(
    _Inout_ PFAST_MUTEX Mutex)
{
    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(Mutex);
}

static VOID
DxgkpKmUnlock(
    _Inout_ PFAST_MUTEX Mutex)
{
    ExReleaseFastMutexUnsafe(Mutex);
    KeLeaveCriticalRegion();
}

static BOOLEAN
DxgkpKmIsFullWddm(
    _In_ PDXGKRNL_ADAPTER Adapter)
{
    PDXGKRNL_MINIPORT_CONTEXT Miniport = Adapter->MiniportContext;

    return (Miniport != NULL &&
            !Miniport->IsDisplayOnlyDriver &&
            !Miniport->UseDodLayout &&
            Miniport->InitData.s.DxgkDdiCommitVidPn != NULL);
}

static NTSTATUS
DxgkpKmQueryCurrentFence(
    _In_  PDXGKRNL_ADAPTER Adapter,
    _Out_ PULONG           CurrentFence)
{
    DXGKARG_QUERYCURRENTFENCE Query;
    NTSTATUS Status;

    *CurrentFence = 0;
    if (Adapter->MiniportContext->InitData.s.DxgkDdiQueryCurrentFence == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Query, sizeof(Query));
    Query.NodeOrdinal = 0;
    Query.EngineOrdinal = 0;
    Status = Adapter->MiniportContext->InitData.s.DxgkDdiQueryCurrentFence(
        Adapter->MiniportDeviceContext,
        &Query);
    if (NT_SUCCESS(Status))
        *CurrentFence = Query.CurrentFence;
    return Status;
}

static BOOLEAN
DxgkpKmFenceReached(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km,
    _In_ ULONG            Fence)
{
    ULONG Current;

    if ((LONG)(Adapter->LastCompletedSubmissionFenceId - Fence) >= 0)
        return TRUE;

    if (Km->QueryFenceRetire &&
        NT_SUCCESS(DxgkpKmQueryCurrentFence(Adapter, &Current)) &&
        (LONG)(Current - Fence) >= 0)
    {
        return TRUE;
    }

    return FALSE;
}

/*
 * Waits up to DXGK_KM_FENCE_TIMEOUT_MS for DMA_COMPLETED(Fence).  When the
 * interrupt never reports it, DxgkDdiQueryCurrentFence tells a GPU that did
 * the work (broken interrupt delivery: logged, then polled from then on)
 * from a GPU that did not (timeout: the caller falls back).
 */
static NTSTATUS
DxgkpKmWaitFence(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km,
    _In_ ULONG            Fence,
    _In_ PCSTR            What)
{
    LARGE_INTEGER Interval;
    ULONGLONG Start;
    ULONG Current = 0;
    NTSTATUS QueryStatus;

    PAGED_CODE();

    Interval.QuadPart = -10000LL; /* 1 ms */
    Start = KeQueryInterruptTime();

    for (;;)
    {
        if (DxgkpKmFenceReached(Adapter, Km, Fence))
            return STATUS_SUCCESS;

        if (KeQueryInterruptTime() - Start >=
            (ULONGLONG)DXGK_KM_FENCE_TIMEOUT_MS * 10000ULL)
        {
            break;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    QueryStatus = DxgkpKmQueryCurrentFence(Adapter, &Current);
    if (NT_SUCCESS(QueryStatus) && (LONG)(Current - Fence) >= 0)
    {
        if (!Km->QueryFenceRetire)
        {
            DXGKRNL_WARN("KmPresent: %s fence %lu done on the GPU (current %lu) "
                         "but no DMA_COMPLETED arrived (last %lu, ISR calls %ld); "
                         "polling DxgkDdiQueryCurrentFence from now on\n",
                         What, Fence, Current,
                         Adapter->LastCompletedSubmissionFenceId,
                         Adapter->InterruptCount);
            Km->QueryFenceRetire = TRUE;
        }
        return STATUS_SUCCESS;
    }

    DXGKRNL_WARN("KmPresent: %s fence %lu TIMEOUT after %u ms: last DMA_COMPLETED "
                 "%lu, QueryCurrentFence 0x%08lX current %lu, ISR calls %ld\n",
                 What, Fence, DXGK_KM_FENCE_TIMEOUT_MS,
                 Adapter->LastCompletedSubmissionFenceId,
                 QueryStatus, Current, Adapter->InterruptCount);
    Km->GpuOwnsBuffers = TRUE;
    return STATUS_IO_TIMEOUT;
}

/*
 * Patch + SubmitCommand of one DMA buffer on Ctx.  Windows patches every
 * buffer (paging buffers with Flags.Paging) and submits at DISPATCH_LEVEL
 * (DXGKDDI_SUBMITCOMMAND is _IRQL_requires_(DISPATCH_LEVEL)).
 */
static NTSTATUS
DxgkpKmSubmit(
    _In_  PDXGKRNL_ADAPTER             Adapter,
    _In_  PDXGK_KM_PRESENT             Km,
    _In_  PDXGK_KM_CONTEXT             Ctx,
    _In_  PDXGKVMM_KERNEL_BUFFER       Dma,
    _In_  UINT                         DmaUsed,
    _In_opt_ PVOID                     Private,
    _In_  UINT                         PrivateSize,
    _In_  UINT                         PrivateUsed,
    _In_opt_ CONST DXGK_ALLOCATIONLIST *AllocationList,
    _In_  UINT                         AllocationCount,
    _In_opt_ CONST D3DDDI_PATCHLOCATIONLIST *PatchList,
    _In_  UINT                         PatchListSize,
    _In_  UINT                         PatchCount,
    _In_  BOOLEAN                      Paging,
    _Out_ PULONG                       OutFence)
{
    DXGKARG_PATCH PatchArgs;
    DXGKARG_SUBMITCOMMAND SubmitArgs;
    ULONG Fence;
    KIRQL OldIrql;
    NTSTATUS Status;

    PAGED_CODE();

    *OutFence = 0;

    DxgkpKmLock(&Km->SubmitMutex);

    Fence = DxgkAllocateSubmissionFenceId(Adapter);

    RtlZeroMemory(&PatchArgs, sizeof(PatchArgs));
    PatchArgs.hContext = Ctx->Context.hMiniportContext;
    PatchArgs.DmaBufferSegmentId = Dma->SegmentId;
    PatchArgs.DmaBufferPhysicalAddress = Dma->GpuAddress;
    PatchArgs.pDmaBuffer = Dma->CpuVa;
    PatchArgs.DmaBufferSize = (UINT)Dma->Size;
    PatchArgs.DmaBufferSubmissionStartOffset = 0;
    PatchArgs.DmaBufferSubmissionEndOffset = DmaUsed;
    PatchArgs.pDmaBufferPrivateData = Private;
    PatchArgs.DmaBufferPrivateDataSize = PrivateSize;
    PatchArgs.DmaBufferPrivateDataSubmissionStartOffset = 0;
    PatchArgs.DmaBufferPrivateDataSubmissionEndOffset = PrivateUsed;
    PatchArgs.pAllocationList = AllocationList;
    PatchArgs.AllocationListSize = AllocationCount;
    PatchArgs.pPatchLocationList = PatchList;
    PatchArgs.PatchLocationListSize = PatchListSize;
    PatchArgs.PatchLocationListSubmissionStart = 0;
    PatchArgs.PatchLocationListSubmissionLength = PatchCount;
    PatchArgs.SubmissionFenceId = Fence;
    PatchArgs.Flags.Value = 0;
    PatchArgs.Flags.Paging = Paging ? 1 : 0;
    PatchArgs.Flags.Present = Paging ? 0 : 1;
    PatchArgs.EngineOrdinal = 0;

    Status = Adapter->MiniportContext->InitData.s.DxgkDdiPatch(
        Adapter->MiniportDeviceContext,
        &PatchArgs);
    if (!NT_SUCCESS(Status))
    {
        DxgkpKmUnlock(&Km->SubmitMutex);
        DXGKRNL_WARN("KmPresent: DxgkDdiPatch(%s) failed 0x%08lX\n",
                     Paging ? "paging" : "present", Status);
        return Status;
    }

    RtlZeroMemory(&SubmitArgs, sizeof(SubmitArgs));
    SubmitArgs.hContext = Ctx->Context.hMiniportContext;
    SubmitArgs.DmaBufferSegmentId = Dma->SegmentId;
    SubmitArgs.DmaBufferPhysicalAddress = Dma->GpuAddress;
    SubmitArgs.DmaBufferSize = (UINT)Dma->Size;
    SubmitArgs.DmaBufferSubmissionStartOffset = 0;
    SubmitArgs.DmaBufferSubmissionEndOffset = DmaUsed;
    SubmitArgs.pDmaBufferPrivateData = Private;
    SubmitArgs.DmaBufferPrivateDataSize = PrivateSize;
    SubmitArgs.DmaBufferPrivateDataSubmissionStartOffset = 0;
    SubmitArgs.DmaBufferPrivateDataSubmissionEndOffset = PrivateUsed;
    SubmitArgs.SubmissionFenceId = Fence;
    SubmitArgs.VidPnSourceId = 0;
    SubmitArgs.FlipInterval = D3DDDI_FLIPINTERVAL_IMMEDIATE;
    SubmitArgs.Flags.Value = 0;
    SubmitArgs.Flags.Paging = Paging ? 1 : 0;
    SubmitArgs.Flags.Present = Paging ? 0 : 1;
    SubmitArgs.EngineOrdinal = 0;
    SubmitArgs.NodeOrdinal = 0;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Status = Adapter->MiniportContext->InitData.s.DxgkDdiSubmitCommand(
        Adapter->MiniportDeviceContext,
        &SubmitArgs);
    KeLowerIrql(OldIrql);

    DxgkpKmUnlock(&Km->SubmitMutex);

    if (!NT_SUCCESS(Status))
    {
        DXGKRNL_WARN("KmPresent: DxgkDdiSubmitCommand(%s) failed 0x%08lX\n",
                     Paging ? "paging" : "present", Status);
        return Status;
    }

    *OutFence = Fence;
    return STATUS_SUCCESS;
}

static NTSTATUS
DxgkpKmCreateContext(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_CONTEXT Ctx,
    _In_ BOOLEAN          System)
{
    DXGKARG_CREATEDEVICE DeviceArgs;
    DXGKARG_CREATECONTEXT ContextArgs;
    NTSTATUS Status;

    PAGED_CODE();

    Ctx->Device.Adapter = Adapter;
    InitializeListHead(&Ctx->Device.DeviceListEntry);
    InitializeListHead(&Ctx->Device.ContextListHead);
    InitializeListHead(&Ctx->Device.SyncObjListHead);
    ExInitializeFastMutex(&Ctx->Device.DeviceMutex);

    RtlZeroMemory(&DeviceArgs, sizeof(DeviceArgs));
    DeviceArgs.hDevice = (HANDLE)&Ctx->Device;
    DeviceArgs.Flags.SystemDevice = System ? 1 : 0;
    DeviceArgs.Flags.GdiDevice = System ? 0 : 1;

    Status = Adapter->MiniportContext->InitData.s.DxgkDdiCreateDevice(
        Adapter->MiniportDeviceContext,
        &DeviceArgs);
    if (!NT_SUCCESS(Status))
        return Status;

    Ctx->Device.hMiniportDevice = DeviceArgs.hDevice;
    Ctx->DeviceCreated = TRUE;

    Ctx->Context.Device = &Ctx->Device;
    Ctx->Context.NodeOrdinal = 0;
    Ctx->Context.EngineAffinity = 0;
    InitializeListHead(&Ctx->Context.ContextListEntry);

    RtlZeroMemory(&ContextArgs, sizeof(ContextArgs));
    ContextArgs.hContext = (HANDLE)&Ctx->Context;
    ContextArgs.NodeOrdinal = 0;
    ContextArgs.EngineAffinity = 0;
    ContextArgs.Flags.Value = 0;
    ContextArgs.Flags.SystemContext = System ? 1 : 0;
    ContextArgs.Flags.GdiContext = System ? 0 : 1;

    Status = Adapter->MiniportContext->InitData.s.DxgkDdiCreateContext(
        Ctx->Device.hMiniportDevice,
        &ContextArgs);
    if (!NT_SUCCESS(Status))
        return Status;

    Ctx->Context.hMiniportContext = ContextArgs.hContext;
    Ctx->ContextCreated = TRUE;
    Ctx->Info = ContextArgs.ContextInfo;
    return STATUS_SUCCESS;
}

static VOID
DxgkpKmDestroyContext(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km,
    _In_ PDXGK_KM_CONTEXT Ctx)
{
    PAGED_CODE();

    /* Buffers the GPU may still read stay allocated (see GpuOwnsBuffers). */
    if (!Km->GpuOwnsBuffers)
    {
        if (Ctx->Dma.Size != 0)
            DxgkVidMmFreeKernelBuffer(Adapter, &Ctx->Dma);
        if (Ctx->DmaPrivate != NULL)
            ExFreePoolWithTag(Ctx->DmaPrivate, TAG_DXGK_KMPRESENT);
        if (Ctx->PatchList != NULL)
            ExFreePoolWithTag(Ctx->PatchList, TAG_DXGK_KMPRESENT);
        Ctx->DmaPrivate = NULL;
        Ctx->PatchList = NULL;
    }

    if (Ctx->ContextCreated &&
        Adapter->MiniportContext->InitData.s.DxgkDdiDestroyContext != NULL)
    {
        Adapter->MiniportContext->InitData.s.DxgkDdiDestroyContext(
            Ctx->Context.hMiniportContext);
    }
    Ctx->ContextCreated = FALSE;

    if (Ctx->DeviceCreated &&
        Adapter->MiniportContext->InitData.s.DxgkDdiDestroyDevice != NULL)
    {
        Adapter->MiniportContext->InitData.s.DxgkDdiDestroyDevice(
            Ctx->Device.hMiniportDevice);
    }
    Ctx->DeviceCreated = FALSE;
}

/* DMA buffer, private data and patch list of a context, per its ContextInfo. */
static NTSTATUS
DxgkpKmAllocateContextBuffers(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_CONTEXT Ctx)
{
    NTSTATUS Status;

    PAGED_CODE();

    if (Ctx->Info.DmaBufferSize == 0)
        return STATUS_INVALID_PARAMETER;

    Status = DxgkVidMmAllocateKernelBuffer(Adapter,
                                           Ctx->Info.DmaBufferSegmentSet,
                                           Ctx->Info.DmaBufferSize,
                                           &Ctx->Dma);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Ctx->Info.DmaBufferPrivateDataSize != 0)
    {
        Ctx->DmaPrivateSize = Ctx->Info.DmaBufferPrivateDataSize;
        Ctx->DmaPrivate = ExAllocatePoolZero(NonPagedPool,
                                             Ctx->DmaPrivateSize,
                                             TAG_DXGK_KMPRESENT);
        if (Ctx->DmaPrivate == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Ctx->Info.PatchLocationListSize != 0)
    {
        Ctx->PatchListSize = Ctx->Info.PatchLocationListSize;
        Ctx->PatchList = ExAllocatePoolZero(NonPagedPool,
                                            Ctx->PatchListSize *
                                                sizeof(D3DDDI_PATCHLOCATIONLIST),
                                            TAG_DXGK_KMPRESENT);
        if (Ctx->PatchList == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

/* PAGING *********************************************************************/

/*
 * DxgkKmExecutePagingOperation
 *
 * Builds one paging operation on the system context and, when the miniport
 * emitted GPU commands, patches and submits them with Flags.Paging and
 * waits for their fence.  Returns STATUS_DEVICE_NOT_READY when the adapter
 * has no system context.
 */
NTSTATUS
DxgkKmExecutePagingOperation(
    _In_    PDXGKRNL_ADAPTER           Adapter,
    _Inout_ PDXGKARG_BUILDPAGINGBUFFER BuildArgs)
{
    PDXGK_KM_PRESENT Km = (PDXGK_KM_PRESENT)Adapter->KmPresent;
    PVOID Base;
    UINT Size;
    UINT Used, PrivateUsed;
    ULONG Fence = 0;
    LONG LogSeq;
    NTSTATUS Status;

    PAGED_CODE();

    if (Km == NULL || !Km->System.ContextCreated)
        return STATUS_DEVICE_NOT_READY;

    DxgkpKmLock(&Km->PagingMutex);

    if (Km->PagingBroken)
    {
        DxgkpKmUnlock(&Km->PagingMutex);
        return STATUS_IO_TIMEOUT;
    }

    if (Km->PagingBufferReady)
    {
        Base = Km->PagingBuffer.CpuVa;
        Size = (UINT)Km->PagingBuffer.Size;
    }
    else
    {
        Base = Km->BootstrapScratch;
        Size = DXGK_KM_BOOTSTRAP_DMA_SIZE;
    }

    BuildArgs->pDmaBuffer = Base;
    BuildArgs->DmaSize = Size;
    BuildArgs->pDmaBufferPrivateData = Km->PagingPrivate;
    BuildArgs->DmaBufferPrivateDataSize = Km->PagingPrivateSize;
    BuildArgs->MultipassOffset = 0;
    BuildArgs->hSystemContext = Km->System.Context.hMiniportContext;

    Status = Adapter->MiniportContext->InitData.s.DxgkDdiBuildPagingBuffer(
        Adapter->MiniportDeviceContext,
        BuildArgs);

    Used = (UINT)((PUCHAR)BuildArgs->pDmaBuffer - (PUCHAR)Base);
    PrivateUsed = (Km->PagingPrivate != NULL)
        ? (UINT)((PUCHAR)BuildArgs->pDmaBufferPrivateData - (PUCHAR)Km->PagingPrivate)
        : 0;

    LogSeq = InterlockedIncrement(&Km->PagingLogCount);

    if (NT_SUCCESS(Status) && Used != 0)
    {
        if (!Km->PagingBufferReady)
        {
            DXGKRNL_WARN("KmPresent: paging op %d emitted %u bytes before the "
                         "paging buffer is GPU-addressable; not submitted\n",
                         (int)BuildArgs->Operation, Used);
            Status = STATUS_NOT_SUPPORTED;
        }
        else
        {
            Status = DxgkpKmSubmit(Adapter, Km, &Km->System, &Km->PagingBuffer,
                                   Used, Km->PagingPrivate, Km->PagingPrivateSize,
                                   PrivateUsed, NULL, 0, NULL, 0, 0, TRUE, &Fence);
            if (NT_SUCCESS(Status))
            {
                Status = DxgkpKmWaitFence(Adapter, Km, Fence, "paging");
                if (!NT_SUCCESS(Status))
                    Km->PagingBroken = TRUE;
            }
        }
    }

    if (LogSeq <= DXGK_KM_LOG_LIMIT || !NT_SUCCESS(Status))
    {
        DXGKRNL_WARN("KmPresent: paging op %d seg=%u -> 0x%08lX, %u bytes, fence %lu\n",
                     (int)BuildArgs->Operation,
                     BuildArgs->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ?
                         BuildArgs->MapApertureSegment.SegmentId : 0,
                     Status, Used, Fence);
    }

    DxgkpKmUnlock(&Km->PagingMutex);
    return Status;
}

/* INITIALISATION *************************************************************/

VOID
DxgkKmPresentInitialize(
    _In_ PDXGKRNL_ADAPTER Adapter)
{
    PDXGKRNL_MINIPORT_CONTEXT Miniport = Adapter->MiniportContext;
    PDXGKRNL_SEGMENT Segment0;
    PDXGK_KM_PRESENT Km;
    UINT PagingSegmentId, PagingSize, PagingSet;
    NTSTATUS Status;

    PAGED_CODE();

    if (!DxgkpKmIsFullWddm(Adapter) || Adapter->KmPresent != NULL)
        return;

    if (Miniport->InitData.s.DxgkDdiCreateDevice == NULL ||
        Miniport->InitData.s.DxgkDdiCreateContext == NULL ||
        Miniport->InitData.s.DxgkDdiBuildPagingBuffer == NULL ||
        Miniport->InitData.s.DxgkDdiPatch == NULL ||
        Miniport->InitData.s.DxgkDdiSubmitCommand == NULL ||
        Adapter->Segments == NULL)
    {
        DXGKRNL_WARN("KmPresent: no system context (missing DDI or segments)\n");
        return;
    }

    Km = ExAllocatePoolZero(NonPagedPool, sizeof(*Km), TAG_DXGK_KMPRESENT);
    if (Km == NULL)
        return;

    ExInitializeFastMutex(&Km->SubmitMutex);
    ExInitializeFastMutex(&Km->PagingMutex);

    Km->BootstrapScratch = ExAllocatePoolZero(NonPagedPool,
                                              DXGK_KM_BOOTSTRAP_DMA_SIZE,
                                              TAG_DXGK_KMPRESENT);
    Segment0 = &((PDXGKRNL_SEGMENT)Adapter->Segments)[0];
    Km->PagingPrivateSize = Segment0->PagingBufferPrivateDataSize;
    if (Km->PagingPrivateSize != 0)
    {
        Km->PagingPrivate = ExAllocatePoolZero(NonPagedPool,
                                               Km->PagingPrivateSize,
                                               TAG_DXGK_KMPRESENT);
    }

    if (Km->BootstrapScratch == NULL ||
        (Km->PagingPrivateSize != 0 && Km->PagingPrivate == NULL))
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        Status = DxgkpKmCreateContext(Adapter, &Km->System, TRUE);
    }

    DXGKRNL_WARN("KmPresent: system device/context -> 0x%08lX (dma=0x%x segset=0x%x "
                 "priv=0x%x alloc=%u patch=%u)\n",
                 Status,
                 Km->System.Info.DmaBufferSize,
                 Km->System.Info.DmaBufferSegmentSet,
                 Km->System.Info.DmaBufferPrivateDataSize,
                 Km->System.Info.AllocationListSize,
                 Km->System.Info.PatchLocationListSize);
    if (!NT_SUCCESS(Status))
    {
        DxgkpKmDestroyContext(Adapter, Km, &Km->System);
        if (Km->PagingPrivate != NULL)
            ExFreePoolWithTag(Km->PagingPrivate, TAG_DXGK_KMPRESENT);
        if (Km->BootstrapScratch != NULL)
            ExFreePoolWithTag(Km->BootstrapScratch, TAG_DXGK_KMPRESENT);
        ExFreePoolWithTag(Km, TAG_DXGK_KMPRESENT);
        return;
    }

    /* From here on paging operations carry hSystemContext. */
    Adapter->KmPresent = Km;

    PagingSegmentId = Segment0->PagingBufferSegmentId;
    PagingSize = Segment0->PagingBufferSize;
    PagingSet = (PagingSegmentId >= 1 && PagingSegmentId <= 32)
        ? (1U << (PagingSegmentId - 1)) : 0;

    Status = DxgkVidMmAllocateKernelBuffer(Adapter, PagingSet, PagingSize,
                                           &Km->PagingBuffer);
    DXGKRNL_WARN("KmPresent: paging buffer seg=%u gpu=0x%I64x size=0x%Ix -> 0x%08lX\n",
                 Km->PagingBuffer.SegmentId,
                 Km->PagingBuffer.GpuAddress.QuadPart,
                 Km->PagingBuffer.Size, Status);
    if (NT_SUCCESS(Status))
        Km->PagingBufferReady = TRUE;

    /* GDI device and context: the present path of the GPU Blt. */
    Km->PresentDisabled = TRUE;
    if (Miniport->InitData.s.DxgkDdiPresent == NULL ||
        Miniport->InitData.s.DxgkDdiOpenAllocation == NULL)
    {
        DXGKRNL_WARN("KmPresent: no DxgkDdiPresent/OpenAllocation; CPU present only\n");
        return;
    }

    Status = DxgkpKmCreateContext(Adapter, &Km->Gdi, FALSE);
    DXGKRNL_WARN("KmPresent: GDI device/context -> 0x%08lX (dma=0x%x segset=0x%x "
                 "priv=0x%x alloc=%u patch=%u)\n",
                 Status,
                 Km->Gdi.Info.DmaBufferSize,
                 Km->Gdi.Info.DmaBufferSegmentSet,
                 Km->Gdi.Info.DmaBufferPrivateDataSize,
                 Km->Gdi.Info.AllocationListSize,
                 Km->Gdi.Info.PatchLocationListSize);
    if (!NT_SUCCESS(Status))
        return;

    if (Km->Gdi.Info.AllocationListSize < DXGK_PRESENT_MAX_INDEX + 1)
    {
        DXGKRNL_WARN("KmPresent: GDI allocation list of %u entries is too small\n",
                     Km->Gdi.Info.AllocationListSize);
        return;
    }

    Status = DxgkpKmAllocateContextBuffers(Adapter, &Km->Gdi);
    DXGKRNL_WARN("KmPresent: GDI DMA buffer seg=%u gpu=0x%I64x size=0x%Ix -> 0x%08lX\n",
                 Km->Gdi.Dma.SegmentId, Km->Gdi.Dma.GpuAddress.QuadPart,
                 Km->Gdi.Dma.Size, Status);
    if (!NT_SUCCESS(Status))
        return;

    Km->PresentDisabled = FALSE;
}

/* PRESENT ********************************************************************/

static NTSTATUS
DxgkpKmOpenAllocation(
    _In_  PDXGKRNL_ADAPTER    Adapter,
    _In_  PDXGK_KM_PRESENT    Km,
    _In_  PDXGKVMM_ALLOCATION Allocation,
    _In_opt_ PVOID            ResourcePrivate,
    _In_  UINT                ResourcePrivateSize,
    _Out_ PHANDLE             OpenHandle)
{
    DXGK_OPENALLOCATIONINFO OpenInfo;
    DXGKARG_OPENALLOCATION OpenArgs;
    NTSTATUS Status;

    PAGED_CODE();

    *OpenHandle = NULL;

    /*
     * As after DxgkDdiCreateAllocation in Windows: the allocation is opened
     * on the device that uses it with Create set, and with the private
     * data it was created with.
     */
    RtlZeroMemory(&OpenInfo, sizeof(OpenInfo));
    OpenInfo.hAllocation = Allocation->Handle;
    OpenInfo.pPrivateDriverData = Allocation->PrivateDriverData;
    OpenInfo.PrivateDriverDataSize = Allocation->PrivateDriverDataSize;

    RtlZeroMemory(&OpenArgs, sizeof(OpenArgs));
    OpenArgs.NumAllocations = 1;
    OpenArgs.pOpenAllocation = &OpenInfo;
    OpenArgs.pPrivateDriverData = ResourcePrivate;
    OpenArgs.PrivateDriverSize = ResourcePrivateSize;
    OpenArgs.Flags.Value = 0;
    OpenArgs.Flags.Create = 1;

    Status = Adapter->MiniportContext->InitData.s.DxgkDdiOpenAllocation(
        Km->Gdi.Device.hMiniportDevice,
        &OpenArgs);
    if (NT_SUCCESS(Status) && OpenInfo.hDeviceSpecificAllocation == NULL)
        Status = STATUS_INVALID_HANDLE;
    if (NT_SUCCESS(Status))
        *OpenHandle = OpenInfo.hDeviceSpecificAllocation;
    return Status;
}

static VOID
DxgkpKmCloseAllocation(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km,
    _Inout_ PHANDLE       OpenHandle)
{
    DXGKARG_CLOSEALLOCATION CloseArgs;

    if (*OpenHandle == NULL ||
        Adapter->MiniportContext->InitData.s.DxgkDdiCloseAllocation == NULL)
    {
        return;
    }

    RtlZeroMemory(&CloseArgs, sizeof(CloseArgs));
    CloseArgs.NumAllocations = 1;
    CloseArgs.pOpenHandleList = OpenHandle;
    Adapter->MiniportContext->InitData.s.DxgkDdiCloseAllocation(
        Km->Gdi.Device.hMiniportDevice,
        &CloseArgs);
    *OpenHandle = NULL;
}

/*
 * The GPU shadow: a DXGK_STDALLOCATION_SHADOWSURFACE at the committed mode,
 * linear by contract (the driver returns its pitch), CPU-mapped so the
 * framebuf.dll shadow can be copied into it, resident where the GPU reads.
 */
static NTSTATUS
DxgkpKmCreateShadow(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km)
{
    DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA QueryArgs;
    D3DKMDT_SHADOWSURFACEDATA SurfaceData;
    DXGK_ALLOCATIONINFO AllocInfo;
    DXGK_CREATEALLOCATIONFLAGS CreateFlags;
    PVOID AllocationPrivate = NULL;
    HANDLE ResourceHandle = NULL;
    PVOID Va = NULL;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(&SurfaceData, sizeof(SurfaceData));
    SurfaceData.Width = Adapter->CommittedWidth;
    SurfaceData.Height = Adapter->CommittedHeight;
    SurfaceData.Format = D3DDDIFMT_X8R8G8B8;

    RtlZeroMemory(&QueryArgs, sizeof(QueryArgs));
    QueryArgs.StandardAllocationType = DXGK_STDALLOCATION_SHADOWSURFACE;
    QueryArgs.pCreateShadowSurfaceData = &SurfaceData;
    Status = Adapter->MiniportContext->InitData.s.DxgkDdiGetStandardAllocationDriverData(
        Adapter->MiniportDeviceContext, &QueryArgs);
    if (!NT_SUCCESS(Status))
        return Status;

    if (QueryArgs.AllocationPrivateDriverDataSize != 0)
    {
        AllocationPrivate = ExAllocatePoolZero(PagedPool,
                                               QueryArgs.AllocationPrivateDriverDataSize,
                                               TAG_DXGK_KMPRESENT);
        if (AllocationPrivate == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (QueryArgs.ResourcePrivateDriverDataSize != 0)
    {
        Km->ShadowResourcePrivate = ExAllocatePoolZero(NonPagedPool,
                                                       QueryArgs.ResourcePrivateDriverDataSize,
                                                       TAG_DXGK_KMPRESENT);
        if (Km->ShadowResourcePrivate == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        Km->ShadowResourcePrivateSize = QueryArgs.ResourcePrivateDriverDataSize;
    }

    QueryArgs.pAllocationPrivateDriverData = AllocationPrivate;
    QueryArgs.pResourcePrivateDriverData = Km->ShadowResourcePrivate;
    Status = Adapter->MiniportContext->InitData.s.DxgkDdiGetStandardAllocationDriverData(
        Adapter->MiniportDeviceContext, &QueryArgs);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (SurfaceData.Pitch < SurfaceData.Width * 4)
    {
        DXGKRNL_WARN("KmPresent: shadow pitch %u < %u\n",
                     SurfaceData.Pitch, SurfaceData.Width * 4);
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    RtlZeroMemory(&AllocInfo, sizeof(AllocInfo));
    AllocInfo.pPrivateDriverData = AllocationPrivate;
    AllocInfo.PrivateDriverDataSize = QueryArgs.AllocationPrivateDriverDataSize;
    AllocInfo.Size = (SIZE_T)SurfaceData.Pitch * SurfaceData.Height;

    RtlZeroMemory(&CreateFlags, sizeof(CreateFlags));
    CreateFlags.Resource = 1;

    Status = DxgkVidMmCreateAllocation(Adapter, NULL, &AllocInfo,
                                       Km->ShadowResourcePrivate,
                                       Km->ShadowResourcePrivateSize,
                                       NULL, CreateFlags,
                                       &Km->ShadowAllocationHandle,
                                       &ResourceHandle);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Km->Shadow = DxgkVidMmHandleToAllocation(Km->ShadowAllocationHandle);
    if (Km->Shadow == NULL || !Km->Shadow->Resident)
    {
        DXGKRNL_WARN("KmPresent: shadow is not resident in any segment\n");
        Status = STATUS_GRAPHICS_NO_VIDEO_MEMORY;
        goto Cleanup;
    }

    Status = DxgkVidMmEnsureAllocationApertureMapped(Km->Shadow);
    if (NT_SUCCESS(Status))
        Status = DxgkVidMmMapAllocationCpu(Km->Shadow, &Va);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Km->ShadowVa = Va;
    Km->ShadowPitch = SurfaceData.Pitch;
    Km->ShadowWidth = SurfaceData.Width;
    Km->ShadowHeight = SurfaceData.Height;

Cleanup:
    if (AllocationPrivate != NULL)
        ExFreePoolWithTag(AllocationPrivate, TAG_DXGK_KMPRESENT);
    UNREFERENCED_PARAMETER(ResourceHandle);
    return Status;
}

/* One-time setup on the first frame, once the mode is committed. */
static NTSTATUS
DxgkpKmPrepareSurfaces(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km)
{
    PDXGKVMM_RESOURCE PrimaryResource;
    NTSTATUS Status;

    PAGED_CODE();

    Status = DxgkEnsureBootPrimary(Adapter, &Km->Primary);
    if (NT_SUCCESS(Status) && !Km->Primary->Resident)
        Status = STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    if (NT_SUCCESS(Status))
        Status = DxgkVidMmEnsureAllocationApertureMapped(Km->Primary);
    DXGKRNL_WARN("KmPresent: primary %ux%u seg=%u gpu=0x%I64x size=0x%Ix -> 0x%08lX\n",
                 Adapter->SharedPrimaryWidth, Adapter->SharedPrimaryHeight,
                 Km->Primary ? Km->Primary->SegmentId : 0,
                 Km->Primary ? Km->Primary->PhysicalAddress.QuadPart : 0,
                 Km->Primary ? Km->Primary->Size : 0, Status);
    if (!NT_SUCCESS(Status))
        return Status;
    Km->PrimaryAllocationHandle = Adapter->SharedPrimaryAllocationHandle;

    Status = DxgkpKmCreateShadow(Adapter, Km);
    DXGKRNL_WARN("KmPresent: shadow %ux%u pitch=%u seg=%u gpu=0x%I64x cpu=%p -> 0x%08lX\n",
                 Km->ShadowWidth, Km->ShadowHeight, Km->ShadowPitch,
                 Km->Shadow ? Km->Shadow->SegmentId : 0,
                 Km->Shadow ? Km->Shadow->PhysicalAddress.QuadPart : 0,
                 Km->ShadowVa, Status);
    if (!NT_SUCCESS(Status))
        return Status;

    PrimaryResource = Km->Primary->Resource;
    Status = DxgkpKmOpenAllocation(Adapter, Km, Km->Primary,
                                   PrimaryResource ? PrimaryResource->ResourcePrivateDriverData : NULL,
                                   PrimaryResource ? PrimaryResource->ResourcePrivateDriverDataSize : 0,
                                   &Km->PrimaryOpen);
    if (NT_SUCCESS(Status))
    {
        Status = DxgkpKmOpenAllocation(Adapter, Km, Km->Shadow,
                                       Km->ShadowResourcePrivate,
                                       Km->ShadowResourcePrivateSize,
                                       &Km->ShadowOpen);
    }
    DXGKRNL_WARN("KmPresent: OpenAllocation on GDI device: primary=%p shadow=%p -> 0x%08lX\n",
                 Km->PrimaryOpen, Km->ShadowOpen, Status);
    return Status;
}

static VOID
DxgkpKmFillAllocationEntry(
    _Out_ PDXGK_ALLOCATIONLIST Entry,
    _In_  HANDLE               OpenHandle,
    _In_  PDXGKVMM_ALLOCATION  Allocation,
    _In_  BOOLEAN              Write)
{
    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->hDeviceSpecificAllocation = OpenHandle;
    Entry->WriteOperation = Write ? 1 : 0;
    Entry->SegmentId = Allocation->SegmentId;
    Entry->PhysicalAddress = Allocation->PhysicalAddress;
}

/*
 * Builds, patches and submits one shadow -> primary Blt of Rect, the way
 * Windows' GDI present does: source and destination rectangles are the
 * dirty bounding rectangle (same size, no stretch) and the sub-rectangles
 * list what to copy inside it.
 */
static NTSTATUS
DxgkpKmSubmitBlt(
    _In_  PDXGKRNL_ADAPTER Adapter,
    _In_  PDXGK_KM_PRESENT Km,
    _In_  const RECT      *Rect,
    _In_  BOOLEAN          Log,
    _Out_ PULONG           OutFence)
{
    DXGK_ALLOCATIONLIST AllocationList[DXGK_PRESENT_MAX_INDEX + 1];
    DXGKARG_PRESENT PresentArgs;
    RECT SubRect;
    UINT DmaUsed, PrivateUsed, PatchCount;
    NTSTATUS Status;

    PAGED_CODE();

    *OutFence = 0;

    RtlZeroMemory(AllocationList, sizeof(AllocationList));
    DxgkpKmFillAllocationEntry(&AllocationList[DXGK_PRESENT_SOURCE_INDEX],
                               Km->ShadowOpen, Km->Shadow, FALSE);
    DxgkpKmFillAllocationEntry(&AllocationList[DXGK_PRESENT_DESTINATION_INDEX],
                               Km->PrimaryOpen, Km->Primary, TRUE);

    ASSERT(Rect->left >= 0 && Rect->top >= 0 &&
           Rect->left < Rect->right && Rect->top < Rect->bottom &&
           Rect->right <= (LONG)Km->ShadowWidth &&
           Rect->bottom <= (LONG)Km->ShadowHeight);
    SubRect = *Rect;

    if (Km->Gdi.DmaPrivate != NULL)
        RtlZeroMemory(Km->Gdi.DmaPrivate, Km->Gdi.DmaPrivateSize);

    RtlZeroMemory(&PresentArgs, sizeof(PresentArgs));
    PresentArgs.pDmaBuffer = Km->Gdi.Dma.CpuVa;
    PresentArgs.DmaSize = (UINT)Km->Gdi.Dma.Size;
    PresentArgs.pDmaBufferPrivateData = Km->Gdi.DmaPrivate;
    PresentArgs.DmaBufferPrivateDataSize = Km->Gdi.DmaPrivateSize;
    PresentArgs.pAllocationList = AllocationList;
    PresentArgs.pPatchLocationListOut = Km->Gdi.PatchList;
    PresentArgs.PatchLocationListOutSize = Km->Gdi.PatchListSize;
    PresentArgs.MultipassOffset = 0;
    PresentArgs.Color = 0;
    /*
     * As CDD presents GDI: Src/DstRect are the whole surfaces, the dirty area
     * is only the sub-rectangle. AMD derives the surface layout from SrcRect;
     * a dirty-sized SrcRect gave sub-rect Blts the wrong stride (8-row
     * staircase artifacts on the tiled primary).
     */
    PresentArgs.SrcRect.left = 0;
    PresentArgs.SrcRect.top = 0;
    PresentArgs.SrcRect.right = (LONG)Km->ShadowWidth;
    PresentArgs.SrcRect.bottom = (LONG)Km->ShadowHeight;
    PresentArgs.DstRect = PresentArgs.SrcRect;
    PresentArgs.SubRectCnt = 1;
    PresentArgs.pDstSubRects = &SubRect;
    PresentArgs.FlipInterval = D3DDDI_FLIPINTERVAL_IMMEDIATE;
    PresentArgs.Flags.Value = 0;
    PresentArgs.Flags.Blt = 1;
    PresentArgs.DmaBufferSegmentId = Km->Gdi.Dma.SegmentId;
    PresentArgs.DmaBufferPhysicalAddress = Km->Gdi.Dma.GpuAddress;

    /* DxgkDdiPresent takes the context handle, not the adapter. */
    Status = Adapter->MiniportContext->InitData.s.DxgkDdiPresent(
        Km->Gdi.Context.hMiniportContext,
        &PresentArgs);

    DmaUsed = (UINT)((PUCHAR)PresentArgs.pDmaBuffer - (PUCHAR)Km->Gdi.Dma.CpuVa);
    PrivateUsed = (Km->Gdi.DmaPrivate != NULL)
        ? (UINT)((PUCHAR)PresentArgs.pDmaBufferPrivateData - (PUCHAR)Km->Gdi.DmaPrivate)
        : 0;
    PatchCount = (Km->Gdi.PatchList != NULL)
        ? (UINT)(PresentArgs.pPatchLocationListOut - Km->Gdi.PatchList)
        : 0;

    if (Log || !NT_SUCCESS(Status))
    {
        DXGKRNL_WARN("KmPresent: DxgkDdiPresent(Blt (%ld,%ld)-(%ld,%ld)) -> 0x%08lX "
                     "dma=%u priv=%u patches=%u\n",
                     SubRect.left, SubRect.top, SubRect.right, SubRect.bottom,
                     Status, DmaUsed, PrivateUsed, PatchCount);
    }

    if (Status == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
    {
        /* One full-screen Blt must fit one DMA buffer; no multipass here. */
        DXGKRNL_WARN("KmPresent: Blt needs more than a %Iu-byte DMA buffer "
                     "(MultipassOffset %u)\n",
                     Km->Gdi.Dma.Size, PresentArgs.MultipassOffset);
        return Status;
    }
    if (!NT_SUCCESS(Status))
        return Status;
    if (DmaUsed == 0 || DmaUsed > Km->Gdi.Dma.Size ||
        PatchCount > Km->Gdi.PatchListSize ||
        PrivateUsed > Km->Gdi.DmaPrivateSize)
    {
        DXGKRNL_WARN("KmPresent: DxgkDdiPresent produced dma=%u/%Iu patches=%u/%u "
                     "priv=%u/%u\n",
                     DmaUsed, Km->Gdi.Dma.Size, PatchCount, Km->Gdi.PatchListSize,
                     PrivateUsed, Km->Gdi.DmaPrivateSize);
        return STATUS_INVALID_PARAMETER;
    }

    /* Residency can only have been refreshed by VidMm; patch with it. */
    DxgkpKmFillAllocationEntry(&AllocationList[DXGK_PRESENT_SOURCE_INDEX],
                               Km->ShadowOpen, Km->Shadow, FALSE);
    DxgkpKmFillAllocationEntry(&AllocationList[DXGK_PRESENT_DESTINATION_INDEX],
                               Km->PrimaryOpen, Km->Primary, TRUE);

    Status = DxgkpKmSubmit(Adapter, Km, &Km->Gdi, &Km->Gdi.Dma, DmaUsed,
                           Km->Gdi.DmaPrivate, Km->Gdi.DmaPrivateSize, PrivateUsed,
                           AllocationList, RTL_NUMBER_OF(AllocationList),
                           Km->Gdi.PatchList, Km->Gdi.PatchListSize, PatchCount,
                           FALSE, OutFence);
    if (Log && NT_SUCCESS(Status))
        DXGKRNL_WARN("KmPresent: Blt submitted, fence %lu\n", *OutFence);
    return Status;
}

static VOID
DxgkpKmCopyShadow(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ PDXGK_KM_PRESENT Km,
    _In_ const RECT      *Rect)
{
    PUCHAR Src = (PUCHAR)Adapter->ShadowFb;
    ULONG SrcPitch = Adapter->ShadowFbPitch;
    LONG SrcRows, Right, Bottom;
    SIZE_T RowBytes;
    LONG y;

    if (Src == NULL || SrcPitch == 0)
        return;

    /* Never read past the framebuf.dll shadow, whatever mode it was made for. */
    SrcRows = (LONG)(Adapter->ShadowFbSize / SrcPitch);
    Right = min(Rect->right, (LONG)(SrcPitch / 4));
    Bottom = min(Rect->bottom, SrcRows);
    if (Rect->left >= Right || Rect->top >= Bottom)
        return;

    RowBytes = (SIZE_T)(Right - Rect->left) * 4;
    for (y = Rect->top; y < Bottom; y++)
    {
        PUCHAR Dst = Km->ShadowVa + (SIZE_T)y * Km->ShadowPitch + (SIZE_T)Rect->left * 4;
        ULONG_PTR Line;

        RtlCopyMemory(Dst, Src + (SIZE_T)y * SrcPitch + (SIZE_T)Rect->left * 4, RowBytes);

        /*
         * The aperture backing is cached system memory; the GPU reads it over
         * the GART, which the miniport may map non-snooped. Write the lines
         * back to RAM so the Blt never sees stale memory under dirty cache
         * lines (seen on the X550DP as 64-byte streaks of old pixels).
         */
        for (Line = (ULONG_PTR)Dst & ~(ULONG_PTR)63;
             Line < (ULONG_PTR)Dst + RowBytes;
             Line += 64)
        {
            DxgkpKmClflush((const void *)Line);
        }
    }

    /* Order the write-backs before the Blt submission that follows. */
    KeMemoryBarrier();
}

/* The GPU path failed before it owned the screen: back to the CPU blit. */
static NTSTATUS
DxgkpKmDisablePresent(
    _In_ PDXGK_KM_PRESENT Km,
    _In_ PCSTR            Stage,
    _In_ NTSTATUS         Status)
{
    Km->PresentDisabled = TRUE;
    DXGKRNL_WARN("KmPresent: GPU present DISABLED at %s (0x%08lX) after %ld frames; "
                 "staying on the firmware framebuffer\n",
                 Stage, Status, Km->FrameCount);
    return STATUS_UNSUCCESSFUL;
}

/*
 * DxgkKmPresentShadow
 *
 * Presents DirtyRect of Adapter->ShadowFb through the GPU: only that
 * rectangle is copied into the GPU shadow and Blt to the primary (the very
 * first frame, which fills the primary before it is scanned out, is the
 * whole surface).  Returns
 *   STATUS_SUCCESS        handled (Blt submitted);
 *   STATUS_PENDING        not presented yet: a Blt is still in flight (the
 *                         shadow and DMA buffer are reused) or the submit
 *                         failed while the primary is on screen.  The
 *                         caller keeps DirtyRect dirty and presents it on
 *                         its next tick;
 *   STATUS_NOT_SUPPORTED  no GPU path: CPU-blit DirtyRect to the firmware
 *                         framebuffer as before;
 *   STATUS_UNSUCCESSFUL   the GPU path just failed: CPU-blit the whole
 *                         surface (a Blt may have reached the firmware
 *                         framebuffer's memory).
 * Runs in the serialised present worker at PASSIVE_LEVEL.
 */
NTSTATUS
DxgkKmPresentShadow(
    _In_ PDXGKRNL_ADAPTER Adapter,
    _In_ const RECT      *DirtyRect)
{
    PDXGK_KM_PRESENT Km = (PDXGK_KM_PRESENT)Adapter->KmPresent;
    RECT Rect;
    ULONG Fence;
    LONG Frame, Deferred;
    BOOLEAN Log;
    NTSTATUS Status;

    PAGED_CODE();

    if (Km == NULL)
        return STATUS_NOT_SUPPORTED;

    /*
     * Once the primary is scanned out the firmware framebuffer is no longer
     * on screen and may share memory with GPU allocations: never CPU-blit.
     */
    if (Km->PresentDisabled)
        return Km->ScanoutActive ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;

    if (!Km->SurfacesTried)
    {
        Km->SurfacesTried = TRUE;
        Status = DxgkpKmPrepareSurfaces(Adapter, Km);
        if (!NT_SUCCESS(Status))
        {
            DxgkpKmDisablePresent(Km, "surface setup", Status);
            return STATUS_NOT_SUPPORTED;
        }
    }

    if (Adapter->SharedPrimaryAllocationHandle != Km->PrimaryAllocationHandle ||
        Adapter->CommittedWidth != Km->ShadowWidth ||
        Adapter->CommittedHeight != Km->ShadowHeight)
    {
        /* A mode change replaced the primary under us. */
        if (Km->ScanoutActive)
        {
            DXGKRNL_WARN("KmPresent: primary/mode changed under the GPU path; "
                         "presents stop\n");
            Km->PresentDisabled = TRUE;
            return STATUS_SUCCESS;
        }
        return DxgkpKmDisablePresent(Km, "mode change", STATUS_INVALID_DEVICE_STATE);
    }

    Rect.left = max(DirtyRect->left, 0);
    Rect.top = max(DirtyRect->top, 0);
    Rect.right = min(DirtyRect->right, (LONG)Km->ShadowWidth);
    Rect.bottom = min(DirtyRect->bottom, (LONG)Km->ShadowHeight);
    if (!Km->ScanoutActive)
    {
        /* The first frame fills the whole primary. */
        Rect.left = 0;
        Rect.top = 0;
        Rect.right = (LONG)Km->ShadowWidth;
        Rect.bottom = (LONG)Km->ShadowHeight;
    }

    if (Rect.left >= Rect.right || Rect.top >= Rect.bottom)
        return STATUS_SUCCESS;

    /* At most one Blt in flight: the shadow and DMA buffer are reused. */
    if (Km->InflightFence != 0)
    {
        if (!DxgkpKmFenceReached(Adapter, Km, Km->InflightFence))
        {
            Deferred = InterlockedIncrement(&Km->DeferredCount);
            if (Deferred <= DXGK_KM_LOG_LIMIT)
            {
                DXGKRNL_WARN("KmPresent: frame deferred, fence %lu still in flight "
                             "(last DMA_COMPLETED %lu)\n",
                             Km->InflightFence, Adapter->LastCompletedSubmissionFenceId);
            }
            if (++Km->ConsecutiveDeferrals == DXGK_KM_STALL_FRAMES)
            {
                DXGKRNL_WARN("KmPresent: GPU STALLED: fence %lu not retired after %u "
                             "frames (last DMA_COMPLETED %lu, ISR calls %ld); "
                             "%ld deferrals in total\n",
                             Km->InflightFence, DXGK_KM_STALL_FRAMES,
                             Adapter->LastCompletedSubmissionFenceId,
                             Adapter->InterruptCount, Deferred);
            }
            return STATUS_PENDING;
        }
        Km->InflightFence = 0;
        Km->ConsecutiveDeferrals = 0;
    }
    Frame = InterlockedIncrement(&Km->FrameCount);
    Log = (Frame <= DXGK_KM_LOG_LIMIT);

    DxgkpKmCopyShadow(Adapter, Km, &Rect);

    Status = DxgkpKmSubmitBlt(Adapter, Km, &Rect, Log, &Fence);
    if (!NT_SUCCESS(Status))
    {
        if (Km->ScanoutActive)
        {
            /* The primary is on screen; the next tick retries. */
            return STATUS_PENDING;
        }
        return DxgkpKmDisablePresent(Km, "Blt submit", Status);
    }

    Km->InflightFence = Fence;
    if (Km->ScanoutActive)
        return STATUS_SUCCESS;

    /* First frame: the primary must hold the desktop before it is shown. */
    Status = DxgkpKmWaitFence(Adapter, Km, Fence, "first Blt");
    if (!NT_SUCCESS(Status))
        return DxgkpKmDisablePresent(Km, "first Blt retire", Status);
    Km->InflightFence = 0;
    DXGKRNL_WARN("KmPresent: first Blt retired (fence %lu)\n", Fence);

    Status = DxgkScanoutBootPrimary(Adapter);
    if (!NT_SUCCESS(Status))
        return DxgkpKmDisablePresent(Km, "scanout switch", Status);

    Km->ScanoutActive = TRUE;
    DXGKRNL_WARN("KmPresent: scanout on the primary; GPU Blt present active\n");
    return STATUS_SUCCESS;
}

/* TEARDOWN *******************************************************************/

VOID
DxgkKmPresentTeardown(
    _In_ PDXGKRNL_ADAPTER Adapter)
{
    PDXGK_KM_PRESENT Km = (PDXGK_KM_PRESENT)Adapter->KmPresent;

    PAGED_CODE();

    if (Km == NULL)
        return;

    Km->PresentDisabled = TRUE;

    if (Km->InflightFence != 0 &&
        !NT_SUCCESS(DxgkpKmWaitFence(Adapter, Km, Km->InflightFence, "teardown")))
    {
        DXGKRNL_WARN("KmPresent: teardown with a Blt in flight; leaking its buffers\n");
    }
    Km->InflightFence = 0;

    DxgkpKmCloseAllocation(Adapter, Km, &Km->ShadowOpen);
    DxgkpKmCloseAllocation(Adapter, Km, &Km->PrimaryOpen);

    if (Km->ShadowAllocationHandle != NULL && !Km->GpuOwnsBuffers)
    {
        DxgkVidMmDestroyAllocation(Adapter, Km->ShadowAllocationHandle);
        if (Km->ShadowResourcePrivate != NULL)
            ExFreePoolWithTag(Km->ShadowResourcePrivate, TAG_DXGK_KMPRESENT);
    }
    Km->ShadowAllocationHandle = NULL;
    Km->Shadow = NULL;
    Km->ShadowVa = NULL;
    Km->ShadowResourcePrivate = NULL;
    Km->Primary = NULL;

    DxgkpKmDestroyContext(Adapter, Km, &Km->Gdi);
}

VOID
DxgkKmPagingTeardown(
    _In_ PDXGKRNL_ADAPTER Adapter)
{
    PDXGK_KM_PRESENT Km = (PDXGK_KM_PRESENT)Adapter->KmPresent;
    BOOLEAN GpuOwnsBuffers;

    PAGED_CODE();

    if (Km == NULL)
        return;

    /* Unmapping the paging buffer from an aperture is itself a paging op. */
    if (Km->PagingBufferReady && !Km->GpuOwnsBuffers)
    {
        Km->PagingBufferReady = FALSE;
        DxgkVidMmFreeKernelBuffer(Adapter, &Km->PagingBuffer);
    }

    DxgkpKmDestroyContext(Adapter, Km, &Km->System);
    Adapter->KmPresent = NULL;

    GpuOwnsBuffers = Km->GpuOwnsBuffers;
    if (!GpuOwnsBuffers)
    {
        if (Km->PagingPrivate != NULL)
            ExFreePoolWithTag(Km->PagingPrivate, TAG_DXGK_KMPRESENT);
        if (Km->BootstrapScratch != NULL)
            ExFreePoolWithTag(Km->BootstrapScratch, TAG_DXGK_KMPRESENT);
        ExFreePoolWithTag(Km, TAG_DXGK_KMPRESENT);
    }
}

/* EOF */
