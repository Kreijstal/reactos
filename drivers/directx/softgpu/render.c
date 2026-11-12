/*
 * PROJECT:     ReactOS WDDM Null/Software GPU Miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     CPU-executed GPU over the QEMU/Bochs standard VGA: VRAM
 *              segment, standard allocations, DxgkDdiPresent(Blt), Patch,
 *              DMA buffer execution and DISPI scanout
 * COPYRIGHT:   Copyright 2026 ReactOS WDDM Team
 *
 * The device behaves like a simple 2D GPU so that dxgkrnl's kernel present
 * path (system/GDI contexts, DMA buffers, Patch, SubmitCommand, fences,
 * SetVidPnSourceAddress) runs end to end:
 *
 *  - Its one memory segment is the adapter's VRAM (PCI BAR0) above the boot
 *    framebuffer, which stays the firmware framebuffer until dxgkrnl moves
 *    the scanout to the shared primary.  GPU addresses are VRAM offsets.
 *  - DxgkDdiPresent(Blt) writes a SOFTGPU_CMD_BLT packet whose source and
 *    destination addresses DxgkDdiPatch fills from the allocation list.
 *  - DxgkDdiSubmitCommand executes the packets of the DMA buffer at once
 *    (the "GPU" is the CPU) and the completion DPC reports the fence.
 *  - DxgkDdiSetVidPnSourceAddress points the Bochs DISPI scanout at the
 *    primary: VIRT_WIDTH = pitch / 4, Y_OFFSET = address / pitch.
 */

#include "softgpu.h"

/* Bochs DISPI (QEMU -vga std) */
#define VBE_DISPI_IOPORT_INDEX      0x01CE
#define VBE_DISPI_IOPORT_DATA       0x01CF
#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40
#define VBE_DISPI_NOCLEARMEM        0x80
#define VBE_DISPI_ID0               0xB0C0

/* Smallest VRAM BAR treated as display memory. */
#define SOFTGPU_MIN_VRAM            (4 * 1024 * 1024)

static volatile LONG g_SoftGpuBltLog = 0;

static USHORT
SoftGpuDispiRead(
    _In_ USHORT Index)
{
    WRITE_PORT_USHORT((PUSHORT)VBE_DISPI_IOPORT_INDEX, Index);
    return READ_PORT_USHORT((PUSHORT)VBE_DISPI_IOPORT_DATA);
}

static VOID
SoftGpuDispiWrite(
    _In_ USHORT Index,
    _In_ USHORT Value)
{
    WRITE_PORT_USHORT((PUSHORT)VBE_DISPI_IOPORT_INDEX, Index);
    WRITE_PORT_USHORT((PUSHORT)VBE_DISPI_IOPORT_DATA, Value);
}

/* Primaries are scanned out with VIRT_WIDTH = pitch / 4: a power of two
 * pitch lets the allocation alignment keep every offset on a scanline. */
static ULONG
SoftGpuPrimaryPitch(
    _In_ ULONG Width)
{
    ULONG Pitch = 1024;

    while (Pitch < Width * 4)
        Pitch <<= 1;
    return Pitch;
}

/* ========================================================================= */

NTSTATUS
SoftGpuVramInitialize(
    _Inout_ PSOFTGPU_DEVICE Device,
    _In_    PDXGK_INTERFACE DxgkInterface,
    _In_    ULONG           PostWidth,
    _In_    ULONG           PostHeight,
    _In_    ULONG           PostPitch)
{
    DXGK_DEVICE_INFO DeviceInfo;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Best = NULL;
    PCM_FULL_RESOURCE_DESCRIPTOR Full;
    ULONG i, j;
    NTSTATUS Status;

    PAGED_CODE();

    if (DxgkInterface->DxgkCbGetDeviceInformation == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&DeviceInfo, sizeof(DeviceInfo));
    Status = DxgkInterface->DxgkCbGetDeviceInformation(DxgkInterface->DeviceHandle,
                                                       &DeviceInfo);
    if (!NT_SUCCESS(Status) || DeviceInfo.TranslatedResourceList == NULL)
        return STATUS_NOT_SUPPORTED;

    Full = DeviceInfo.TranslatedResourceList->List;
    for (i = 0; i < DeviceInfo.TranslatedResourceList->Count; i++)
    {
        for (j = 0; j < Full->PartialResourceList.Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc =
                &Full->PartialResourceList.PartialDescriptors[j];

            if (Desc->Type == CmResourceTypeMemory &&
                Desc->u.Memory.Length >= SOFTGPU_MIN_VRAM &&
                (Best == NULL || Desc->u.Memory.Length > Best->u.Memory.Length))
            {
                Best = Desc;
            }
        }
        Full = (PCM_FULL_RESOURCE_DESCRIPTOR)
            &Full->PartialResourceList.PartialDescriptors[Full->PartialResourceList.Count];
    }

    if (Best == NULL || SoftGpuDispiRead(VBE_DISPI_INDEX_ID) < VBE_DISPI_ID0)
        return STATUS_NOT_SUPPORTED;

    Device->VramPhys = Best->u.Memory.Start;
    Device->VramSize = Best->u.Memory.Length;
    Device->VramVa = MmMapIoSpace(Device->VramPhys, Device->VramSize, MmWriteCombined);
    if (Device->VramVa == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Keep the boot framebuffer (still scanned out) outside the segment. */
    Device->SegmentBase = ALIGN_UP_BY((SIZE_T)PostPitch * PostHeight, 1024 * 1024);
    if (Device->SegmentBase + SOFTGPU_MIN_VRAM > Device->VramSize)
    {
        MmUnmapIoSpace(Device->VramVa, Device->VramSize);
        Device->VramVa = NULL;
        return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    }

    Device->HasVram = TRUE;
    DPRINT1("SOFTGPU: VRAM phys=0x%I64x size=0x%Ix va=%p; segment at +0x%Ix "
            "(boot fb %lux%lu pitch %lu), DISPI id 0x%x\n",
            Device->VramPhys.QuadPart, Device->VramSize, Device->VramVa,
            Device->SegmentBase, PostWidth, PostHeight, PostPitch,
            SoftGpuDispiRead(VBE_DISPI_INDEX_ID));
    return STATUS_SUCCESS;
}

VOID
SoftGpuVramTeardown(
    _Inout_ PSOFTGPU_DEVICE Device)
{
    if (Device->VramVa != NULL)
        MmUnmapIoSpace(Device->VramVa, Device->VramSize);
    Device->VramVa = NULL;
    Device->HasVram = FALSE;
}

/* GPU (VRAM offset) range -> CPU pointer, or NULL when outside VRAM. */
static PUCHAR
SoftGpuGpuToCpu(
    _In_ PSOFTGPU_DEVICE Device,
    _In_ ULONGLONG       GpuAddress,
    _In_ ULONGLONG       Length)
{
    if (GpuAddress > Device->VramSize || Length > Device->VramSize - GpuAddress)
        return NULL;
    return Device->VramVa + GpuAddress;
}

NTSTATUS
SoftGpuDisplayScanout(
    _In_ PSOFTGPU_DEVICE Device,
    _In_ ULONGLONG       GpuAddress,
    _In_ ULONG           Pitch)
{
    USHORT Enable;

    if (!Device->HasVram || Pitch == 0 || (Pitch & 3) != 0 || (GpuAddress % Pitch) != 0 ||
        GpuAddress / Pitch > 0xFFFF || Pitch / 4 > 0xFFFF)
    {
        return STATUS_GRAPHICS_INVALID_ALLOCATION_USAGE;
    }

    Enable = SoftGpuDispiRead(VBE_DISPI_INDEX_ENABLE);
    if (SoftGpuDispiRead(VBE_DISPI_INDEX_XRES) != Device->Width ||
        SoftGpuDispiRead(VBE_DISPI_INDEX_YRES) != Device->Height ||
        SoftGpuDispiRead(VBE_DISPI_INDEX_BPP) != 32 ||
        !(Enable & VBE_DISPI_ENABLED))
    {
        SoftGpuDispiWrite(VBE_DISPI_INDEX_ENABLE, 0);
        SoftGpuDispiWrite(VBE_DISPI_INDEX_XRES, (USHORT)Device->Width);
        SoftGpuDispiWrite(VBE_DISPI_INDEX_YRES, (USHORT)Device->Height);
        SoftGpuDispiWrite(VBE_DISPI_INDEX_BPP, 32);
        SoftGpuDispiWrite(VBE_DISPI_INDEX_ENABLE,
                          VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);
    }

    SoftGpuDispiWrite(VBE_DISPI_INDEX_VIRT_WIDTH, (USHORT)(Pitch / 4));
    SoftGpuDispiWrite(VBE_DISPI_INDEX_X_OFFSET, 0);
    SoftGpuDispiWrite(VBE_DISPI_INDEX_Y_OFFSET, (USHORT)(GpuAddress / Pitch));

    DPRINT1("SOFTGPU: scanout %lux%lu at gpu 0x%I64x pitch %lu (DISPI y_offset %u)\n",
            Device->Width, Device->Height, GpuAddress, Pitch,
            SoftGpuDispiRead(VBE_DISPI_INDEX_Y_OFFSET));
    return STATUS_SUCCESS;
}

/* ========================================================================= */

NTSTATUS
APIENTRY
SoftGpuDdiGetStandardAllocationDriverData(
    _In_    CONST HANDLE                             hAdapter,
    _Inout_ PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA Args)
{
    PSOFTGPU_DEVICE Device = (PSOFTGPU_DEVICE)hAdapter;
    PSOFTGPU_ALLOC_PRIVATE Private;
    SOFTGPU_ALLOC_PRIVATE Data;

    PAGED_CODE();

    if (Device == NULL || Device->Magic != SOFTGPU_DEVICE_MAGIC || Args == NULL)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Data, sizeof(Data));
    Data.Magic = SOFTGPU_ALLOC_PRIVATE_MAGIC;
    Data.StandardType = Args->StandardAllocationType;

    switch (Args->StandardAllocationType)
    {
        case DXGK_STDALLOCATION_SHAREDPRIMARYSURFACE:
            Data.Width = Args->pCreateSharedPrimarySurfaceData->Width;
            Data.Height = Args->pCreateSharedPrimarySurfaceData->Height;
            Data.Format = Args->pCreateSharedPrimarySurfaceData->Format;
            Data.Pitch = SoftGpuPrimaryPitch(Data.Width);
            break;

        case DXGK_STDALLOCATION_SHADOWSURFACE:
            Data.Width = Args->pCreateShadowSurfaceData->Width;
            Data.Height = Args->pCreateShadowSurfaceData->Height;
            Data.Format = Args->pCreateShadowSurfaceData->Format;
            Data.Pitch = ALIGN_UP_BY(Data.Width * 4, 64);
            Args->pCreateShadowSurfaceData->Pitch = Data.Pitch;
            break;

        case DXGK_STDALLOCATION_STAGINGSURFACE:
            Data.Width = Args->pCreateStagingSurfaceData->Width;
            Data.Height = Args->pCreateStagingSurfaceData->Height;
            Data.Format = D3DDDIFMT_X8R8G8B8;
            Data.Pitch = ALIGN_UP_BY(Data.Width * 4, 64);
            Args->pCreateStagingSurfaceData->Pitch = Data.Pitch;
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (Data.Width == 0 || Data.Height == 0)
        return STATUS_INVALID_PARAMETER;

    /* First call sizes the private data, the second fills it. */
    Args->ResourcePrivateDriverDataSize = 0;
    if (Args->pAllocationPrivateDriverData == NULL)
    {
        Args->AllocationPrivateDriverDataSize = sizeof(SOFTGPU_ALLOC_PRIVATE);
        return STATUS_SUCCESS;
    }
    if (Args->AllocationPrivateDriverDataSize < sizeof(SOFTGPU_ALLOC_PRIVATE))
        return STATUS_BUFFER_TOO_SMALL;

    Private = Args->pAllocationPrivateDriverData;
    *Private = Data;
    Args->AllocationPrivateDriverDataSize = sizeof(SOFTGPU_ALLOC_PRIVATE);
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
SoftGpuDdiOpenAllocation(
    _In_ CONST HANDLE                   hDevice,
    _In_ CONST DXGKARG_OPENALLOCATION  *Args)
{
    PSOFTGPU_DEVICE Device = SoftGpuDeviceFromHandle(hDevice);
    DXGKARGCB_GETHANDLEDATA HandleData;
    UINT i;

    if (Device == NULL || Args == NULL)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < Args->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO *Info = &Args->pOpenAllocation[i];
        PSOFTGPU_ALLOC Alloc;

        RtlZeroMemory(&HandleData, sizeof(HandleData));
        HandleData.hObject = Info->hAllocation;
        HandleData.Type = DXGK_HANDLE_ALLOCATION;
        Alloc = Device->DxgkInterface.DxgkCbGetHandleData(&HandleData);
        if (Alloc == NULL || Alloc->Magic != SOFTGPU_ALLOC_MAGIC)
            return STATUS_INVALID_HANDLE;

        /* The device-specific handle is the allocation itself. */
        Info->hDeviceSpecificAllocation = (HANDLE)Alloc;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
SoftGpuDdiCloseAllocation(
    _In_ CONST HANDLE                    hDevice,
    _In_ CONST DXGKARG_CLOSEALLOCATION  *Args)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(Args);
    return STATUS_SUCCESS;
}

/* ========================================================================= */

NTSTATUS
APIENTRY
SoftGpuDdiPresent(
    _In_    CONST HANDLE      hContext,
    _Inout_ PDXGKARG_PRESENT  Present)
{
    PSOFTGPU_CONTEXT Ctx = (PSOFTGPU_CONTEXT)hContext;
    PSOFTGPU_CMD_BLT_PACKET Packet;
    PSOFTGPU_ALLOC Src, Dst;
    UINT Rects, Room, i, PacketSize;
    RECT *Out;

    if (Ctx == NULL || Ctx->Magic != SOFTGPU_CONTEXT_MAGIC || Present == NULL)
        return STATUS_INVALID_PARAMETER;

    if (!Present->Flags.Blt)
        return STATUS_NOT_SUPPORTED;

    /* softgpu has no stretch engine. */
    if (Present->SrcRect.right - Present->SrcRect.left !=
            Present->DstRect.right - Present->DstRect.left ||
        Present->SrcRect.bottom - Present->SrcRect.top !=
            Present->DstRect.bottom - Present->DstRect.top)
    {
        return STATUS_NOT_SUPPORTED;
    }

    Src = (PSOFTGPU_ALLOC)Present->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
    Dst = (PSOFTGPU_ALLOC)Present->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
    if (Src == NULL || Dst == NULL ||
        Src->Magic != SOFTGPU_ALLOC_MAGIC || Dst->Magic != SOFTGPU_ALLOC_MAGIC ||
        Src->Pitch == 0 || Dst->Pitch == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Rects = Present->SubRectCnt - Present->MultipassOffset;
    if (Present->SubRectCnt == 0 || Present->MultipassOffset >= Present->SubRectCnt)
        return STATUS_INVALID_PARAMETER;

    if (Present->DmaSize < FIELD_OFFSET(SOFTGPU_CMD_BLT_PACKET, Rects[1]) ||
        Present->PatchLocationListOutSize < 2)
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    Room = (Present->DmaSize - FIELD_OFFSET(SOFTGPU_CMD_BLT_PACKET, Rects)) / sizeof(RECT);
    if (Rects > Room)
        Rects = Room;

    Packet = Present->pDmaBuffer;
    PacketSize = FIELD_OFFSET(SOFTGPU_CMD_BLT_PACKET, Rects[Rects]);
    RtlZeroMemory(Packet, PacketSize);
    Packet->Opcode = SOFTGPU_CMD_BLT;
    Packet->Size = PacketSize;
    Packet->SrcPitch = Src->Pitch;
    Packet->DstPitch = Dst->Pitch;
    Packet->SrcSize = (ULONG)Src->Size;
    Packet->DstSize = (ULONG)Dst->Size;
    Packet->SrcDx = Present->SrcRect.left - Present->DstRect.left;
    Packet->SrcDy = Present->SrcRect.top - Present->DstRect.top;

    Out = Packet->Rects;
    for (i = 0; i < Rects; i++)
    {
        const RECT *In = &Present->pDstSubRects[Present->MultipassOffset + i];

        /* Sub-rectangles are clipped to DstRect. */
        Out->left = max(In->left, Present->DstRect.left);
        Out->top = max(In->top, Present->DstRect.top);
        Out->right = min(In->right, Present->DstRect.right);
        Out->bottom = min(In->bottom, Present->DstRect.bottom);
        if (Out->left < Out->right && Out->top < Out->bottom)
            Out++;
    }
    Packet->NumRects = (ULONG)(Out - Packet->Rects);

    RtlZeroMemory(Present->pPatchLocationListOut, 2 * sizeof(D3DDDI_PATCHLOCATIONLIST));
    Present->pPatchLocationListOut[0].AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
    Present->pPatchLocationListOut[0].PatchOffset =
        (UINT)((PUCHAR)&Packet->SrcAddress - (PUCHAR)Present->pDmaBuffer);
    Present->pPatchLocationListOut[1].AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
    Present->pPatchLocationListOut[1].PatchOffset =
        (UINT)((PUCHAR)&Packet->DstAddress - (PUCHAR)Present->pDmaBuffer);
    Present->pPatchLocationListOut += 2;
    Present->pDmaBuffer = (PUCHAR)Present->pDmaBuffer + PacketSize;

    if (Ctx->Adapter != NULL && Packet->NumRects != 0)
    {
        PSOFTGPU_DEVICE Device = Ctx->Adapter;
        RECT Bounds = Packet->Rects[0];

        for (i = 1; i < Packet->NumRects; i++)
        {
            Bounds.left = min(Bounds.left, Packet->Rects[i].left);
            Bounds.top = min(Bounds.top, Packet->Rects[i].top);
            Bounds.right = max(Bounds.right, Packet->Rects[i].right);
            Bounds.bottom = max(Bounds.bottom, Packet->Rects[i].bottom);
        }
        Device->LastBltBounds = Bounds;
        Device->LastBltDstWidth = Dst->Width;
        Device->LastBltDstHeight = Dst->Height;
        if (Bounds.left == 0 && Bounds.top == 0 &&
            (ULONG)Bounds.right >= Dst->Width && (ULONG)Bounds.bottom >= Dst->Height)
        {
            InterlockedIncrement(&Device->FullBltCount);
        }
        InterlockedIncrement(&Device->BltCount);
    }

    if (InterlockedIncrement(&g_SoftGpuBltLog) <= 4)
    {
        DPRINT1("SOFTGPU: Present Blt %u rect(s), first (%ld,%ld)-(%ld,%ld), "
                "src pitch %lu dst pitch %lu\n",
                Packet->NumRects,
                Packet->NumRects ? Packet->Rects[0].left : 0,
                Packet->NumRects ? Packet->Rects[0].top : 0,
                Packet->NumRects ? Packet->Rects[0].right : 0,
                Packet->NumRects ? Packet->Rects[0].bottom : 0,
                Src->Pitch, Dst->Pitch);
    }

    if (Present->MultipassOffset + Rects < Present->SubRectCnt)
    {
        Present->MultipassOffset += Rects;
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */

static NTSTATUS
SoftGpuExecuteBlt(
    _In_ PSOFTGPU_DEVICE               Device,
    _In_ CONST SOFTGPU_CMD_BLT_PACKET *Packet)
{
    PUCHAR Src, Dst;
    ULONG i;

    Src = SoftGpuGpuToCpu(Device, Packet->SrcAddress, Packet->SrcSize);
    Dst = SoftGpuGpuToCpu(Device, Packet->DstAddress, Packet->DstSize);
    if (Src == NULL || Dst == NULL)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < Packet->NumRects; i++)
    {
        const RECT *R = &Packet->Rects[i];
        LONG SrcX = R->left + Packet->SrcDx, SrcY = R->top + Packet->SrcDy;
        SIZE_T Bytes = (SIZE_T)(R->right - R->left) * 4;
        LONG y;

        if (R->left < 0 || R->top < 0 || SrcX < 0 || SrcY < 0 ||
            (ULONGLONG)(R->bottom - 1) * Packet->DstPitch + (ULONGLONG)R->right * 4 > Packet->DstSize ||
            (ULONGLONG)(SrcY + R->bottom - R->top - 1) * Packet->SrcPitch +
                (ULONGLONG)(SrcX * 4) + Bytes > Packet->SrcSize)
        {
            return STATUS_INVALID_PARAMETER;
        }

        for (y = R->top; y < R->bottom; y++)
        {
            RtlCopyMemory(Dst + (SIZE_T)y * Packet->DstPitch + (SIZE_T)R->left * 4,
                          Src + (SIZE_T)(SrcY + y - R->top) * Packet->SrcPitch + (SIZE_T)SrcX * 4,
                          Bytes);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
SoftGpuExecuteDmaBuffer(
    _In_ PSOFTGPU_DEVICE              Device,
    _In_ CONST DXGKARG_SUBMITCOMMAND *Submit)
{
    PUCHAR Buffer, Cursor, End;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Device->HasVram ||
        Submit->DmaBufferSubmissionEndOffset <= Submit->DmaBufferSubmissionStartOffset)
    {
        return STATUS_SUCCESS;
    }

    /* DMA buffers live in the VRAM segment (DmaBufferSegmentSet). */
    Buffer = SoftGpuGpuToCpu(Device, (ULONGLONG)Submit->DmaBufferPhysicalAddress.QuadPart,
                             Submit->DmaBufferSubmissionEndOffset);
    if (Buffer == NULL || Submit->DmaBufferSegmentId != SOFTGPU_SEGMENT_ID)
    {
        DPRINT1("SOFTGPU: DMA buffer seg %u gpu 0x%I64x is not in VRAM\n",
                Submit->DmaBufferSegmentId, Submit->DmaBufferPhysicalAddress.QuadPart);
        return STATUS_INVALID_PARAMETER;
    }

    Cursor = Buffer + Submit->DmaBufferSubmissionStartOffset;
    End = Buffer + Submit->DmaBufferSubmissionEndOffset;
    while (Cursor + 2 * sizeof(ULONG) <= End && NT_SUCCESS(Status))
    {
        const SOFTGPU_CMD_BLT_PACKET *Packet = (const SOFTGPU_CMD_BLT_PACKET *)Cursor;

        if (Packet->Size < FIELD_OFFSET(SOFTGPU_CMD_BLT_PACKET, Rects) ||
            Packet->Size > (SIZE_T)(End - Cursor))
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        switch (Packet->Opcode)
        {
            case SOFTGPU_CMD_BLT:
                if (Packet->Size < FIELD_OFFSET(SOFTGPU_CMD_BLT_PACKET, Rects[Packet->NumRects]))
                    Status = STATUS_INVALID_PARAMETER;
                else
                    Status = SoftGpuExecuteBlt(Device, Packet);
                break;

            default:
                Status = STATUS_ILLEGAL_INSTRUCTION;
                break;
        }
        Cursor += Packet->Size;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SOFTGPU: DMA buffer fence %u faulted at +0x%Ix: 0x%08lx\n",
                Submit->SubmissionFenceId, (SIZE_T)(Cursor - Buffer), Status);
    }
    return Status;
}
