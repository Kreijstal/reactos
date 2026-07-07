/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/lock.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"

PVOID GetLockedData(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    ASSERT(Irp->MdlAddress);
    ASSERT(Irp->Tail.Overlay.DriverContext[0]);

    UNREFERENCED_PARAMETER(IrpSp);

    return Irp->Tail.Overlay.DriverContext[0];
}

/* Lock a method_neither request so it'll be available from DISPATCH_LEVEL.
 *
 * dev-nt6-1: ReactOS's MmProbeAndLockPages raises STATUS_ACCESS_VIOLATION
 * even on valid user-mode stack pointers. Instead of locking the user
 * pages, allocate a kernel pool copy and SEH-copy from user space. This
 * is what real Windows does for small METHOD_NEITHER buffers and
 * matches the contract: GetLockedData returns DriverContext[0] (the
 * kernel pool copy), and UnlockRequest writes back via DriverContext[1]
 * which now stores the original user pointer for the SEH writeback.
 * MdlAddress is built from the kernel pool buffer so MmGetMdlByteCount
 * still works for downstream consumers. */
PVOID LockRequest( PIRP Irp,
                   PIO_STACK_LOCATION IrpSp,
                   BOOLEAN Output,
                   KPROCESSOR_MODE *LockMode) {
    PVOID  UserBuffer;
    ULONG  BufferLength;
    PVOID  KernelBuffer;
    NTSTATUS CopyStatus = STATUS_SUCCESS;

    ASSERT(!Irp->MdlAddress);

    switch (IrpSp->MajorFunction)
    {
        case IRP_MJ_DEVICE_CONTROL:
        case IRP_MJ_INTERNAL_DEVICE_CONTROL:
            ASSERT(IrpSp->Parameters.DeviceIoControl.Type3InputBuffer);
            ASSERT(IrpSp->Parameters.DeviceIoControl.InputBufferLength);
            UserBuffer   = IrpSp->Parameters.DeviceIoControl.Type3InputBuffer;
            BufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            /* Allocate the kernel pool copy. */
            KernelBuffer = ExAllocatePoolWithTag(NonPagedPool, BufferLength,
                                                 TAG_AFD_DATA_BUFFER);
            if (KernelBuffer == NULL)
            {
                AFD_DbgPrint(MIN_TRACE,("Failed to allocate kernel buffer\n"));
                return NULL;
            }

            /* SEH-guarded copy from user space - bypasses ReactOS's
             * broken MmProbeAndLockPages. */
            _SEH2_TRY {
                RtlCopyMemory(KernelBuffer, UserBuffer, BufferLength);
            } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                CopyStatus = _SEH2_GetExceptionCode();
            } _SEH2_END;

            if (!NT_SUCCESS(CopyStatus))
            {
                AFD_DbgPrint(MIN_TRACE,("SEH-copy from user buffer failed 0x%08lx\n",
                                        (ULONG)CopyStatus));
                ExFreePoolWithTag(KernelBuffer, TAG_AFD_DATA_BUFFER);
                return NULL;
            }

            /* Build a fake MDL wrapping the kernel pool buffer so the
             * downstream code that calls MmGetMdlByteCount(Irp->MdlAddress)
             * still gets the right answer. The MDL describes nonpaged
             * pool, so MmBuildMdlForNonPagedPool fills out the PFN array
             * without needing to lock anything. */
            Irp->MdlAddress = IoAllocateMdl(KernelBuffer, BufferLength,
                                            FALSE, FALSE, NULL);
            if (Irp->MdlAddress == NULL)
            {
                AFD_DbgPrint(MIN_TRACE,("Failed to allocate MDL wrapper\n"));
                ExFreePoolWithTag(KernelBuffer, TAG_AFD_DATA_BUFFER);
                return NULL;
            }
            MmBuildMdlForNonPagedPool(Irp->MdlAddress);

            /* DriverContext[0] = the working kernel buffer.
             * DriverContext[1] = original user buffer if writeback needed,
             *                    NULL otherwise.
             * DriverContext[2] = originating PEPROCESS (for KeStackAttachProcess
             *                    in UnlockRequest, since IRP completion may
             *                    fire from a TDI worker thread that is not
             *                    in the requestor's address space). */
            Irp->Tail.Overlay.DriverContext[0] = KernelBuffer;
            Irp->Tail.Overlay.DriverContext[1] = Output ? UserBuffer : NULL;
            Irp->Tail.Overlay.DriverContext[2] = (Output && UserBuffer) ?
                                                 PsGetCurrentProcess() : NULL;

            if (LockMode != NULL)
                *LockMode = UserMode;
            break;

        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
        {
            PAFD_RECV_INFO AfdInfo;

            ASSERT(Irp->UserBuffer);

            UserBuffer   = Irp->UserBuffer;
            BufferLength = (IrpSp->MajorFunction == IRP_MJ_READ)
                ? IrpSp->Parameters.Read.Length
                : IrpSp->Parameters.Write.Length;

            /* AFD expects an AFD_RECV_INFO struct describing the read/write
             * buffer. LockBuffers owns the actual data staging buffers, so
             * this descriptor must keep the original user pointer. */
            AfdInfo = ExAllocatePoolWithTag(NonPagedPool,
                                            sizeof(AFD_RECV_INFO) + sizeof(AFD_WSABUF),
                                            TAG_AFD_DATA_BUFFER);
            if (!AfdInfo)
            {
                return NULL;
            }

            AfdInfo->BufferArray = (PAFD_WSABUF)(AfdInfo + 1);
            AfdInfo->BufferCount = 1;
            AfdInfo->AfdFlags    = 0;
            AfdInfo->TdiFlags    = 0;
            AfdInfo->BufferArray[0].buf = (PCHAR)UserBuffer;
            AfdInfo->BufferArray[0].len = BufferLength;

            Irp->MdlAddress = IoAllocateMdl(AfdInfo,
                                            sizeof(AFD_RECV_INFO) + sizeof(AFD_WSABUF),
                                            FALSE, FALSE, NULL);
            if (Irp->MdlAddress == NULL)
            {
                ExFreePoolWithTag(AfdInfo, TAG_AFD_DATA_BUFFER);
                return NULL;
            }
            MmBuildMdlForNonPagedPool(Irp->MdlAddress);

            Irp->Tail.Overlay.DriverContext[0] = AfdInfo;
            Irp->Tail.Overlay.DriverContext[1] = NULL;
            Irp->Tail.Overlay.DriverContext[2] = NULL;

            if (LockMode != NULL)
                *LockMode = KernelMode;
            break;
        }

        default:
            ASSERT(FALSE);
            return NULL;
    }

    return GetLockedData(Irp, IrpSp);
}

VOID UnlockRequest( PIRP Irp, PIO_STACK_LOCATION IrpSp )
{
    PVOID KernelBuffer;
    PVOID UserBuffer;
    ULONG ByteCount;
    BOOLEAN IsRecvInfo;

    ASSERT(Irp->MdlAddress);
    ASSERT(Irp->Tail.Overlay.DriverContext[0]);

    /* dev-nt6-1: this matches the kernel-pool LockRequest above. The
     * kernel buffer in DriverContext[0] is either:
     *  - For DEVICE_CONTROL: the raw kernel buffer
     *  - For READ/WRITE:     an AFD_RECV_INFO descriptor; LockBuffers owns
     *                        and frees the actual data staging buffers
     * DriverContext[1] holds the original user pointer if a writeback
     * is needed, NULL otherwise. We SEH-copy back to user space and
     * skip MmUnlockPages because no lock was taken in the first place. */
    UserBuffer = Irp->Tail.Overlay.DriverContext[1];
    ByteCount  = MmGetMdlByteCount(Irp->MdlAddress);

    IsRecvInfo = (IrpSp->MajorFunction == IRP_MJ_READ ||
                  IrpSp->MajorFunction == IRP_MJ_WRITE);

    if (IsRecvInfo)
    {
        KernelBuffer = NULL;
    }
    else
    {
        KernelBuffer = Irp->Tail.Overlay.DriverContext[0];
    }

    if (UserBuffer != NULL && KernelBuffer != NULL)
    {
        /* IRP completion may run from a TDI worker thread; attach back to
         * the requestor before touching the user-mode VA. */
        PEPROCESS OriginalProcess = (PEPROCESS)Irp->Tail.Overlay.DriverContext[2];
        KAPC_STATE ApcState;
        BOOLEAN Attached = FALSE;
        if (OriginalProcess != NULL && OriginalProcess != PsGetCurrentProcess())
        {
            KeStackAttachProcess((PKPROCESS)OriginalProcess, &ApcState);
            Attached = TRUE;
        }
        _SEH2_TRY {
            RtlCopyMemory(UserBuffer, KernelBuffer, ByteCount);
        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            /* User buffer went away - keep going. */
        } _SEH2_END;
        if (Attached)
            KeUnstackDetachProcess(&ApcState);
    }

    if (IsRecvInfo)
    {
        /* LockBuffers already freed the data buffers. */
        ExFreePoolWithTag(Irp->Tail.Overlay.DriverContext[0], TAG_AFD_DATA_BUFFER);
    }
    else
    {
        ExFreePoolWithTag(Irp->Tail.Overlay.DriverContext[0], TAG_AFD_DATA_BUFFER);
    }

    IoFreeMdl(Irp->MdlAddress);
    Irp->MdlAddress = NULL;
}

/* Note: We add an extra buffer if LockAddress is true.  This allows us to
 * treat the address buffer as an ordinary client buffer.  It's only used
 * for datagrams. */

PAFD_WSABUF LockBuffers( PAFD_WSABUF Buf, UINT Count,
                         PVOID AddressBuf, PINT AddressLen,
                         BOOLEAN Write, BOOLEAN LockAddress,
                         KPROCESSOR_MODE LockMode,
                         BOOLEAN Wow64Is32Bit) 
{
    UINT i;
    /* Copy the buffer array so we don't lose it */
    UINT Lock = LockAddress ? 2 : 0;
    UINT Size = (sizeof(AFD_WSABUF) + sizeof(AFD_MAPBUF)) * (Count + Lock);
    PAFD_WSABUF NewBuf = ExAllocatePoolWithTag(PagedPool, Size, TAG_AFD_WSA_BUFFER);
    BOOLEAN LockFailed = FALSE;
    PAFD_MAPBUF MapBuf;

    AFD_DbgPrint(MID_TRACE,("Called(%p)\n", NewBuf));

    if( NewBuf ) {
        RtlZeroMemory(NewBuf, Size);

        MapBuf = (PAFD_MAPBUF)(NewBuf + Count + Lock);

        _SEH2_TRY {
#ifdef _WIN64
            if (Wow64Is32Bit)
            {
                for (i = 0; i < Count; i++)
                {
                    NewBuf[i].buf = (PVOID)(ULONG_PTR)((PAFD_WSABUF32)Buf)[i].buf;
                    NewBuf[i].len = ((PAFD_WSABUF32)Buf)[i].len;
                }
            }
            else
#endif
            {
                RtlCopyMemory(NewBuf, Buf, sizeof(AFD_WSABUF) * Count);
            }
            
            if( LockAddress ) {
                if (AddressBuf && AddressLen) {
                    NewBuf[Count].buf = AddressBuf;
                    NewBuf[Count].len = *AddressLen;
                    NewBuf[Count + 1].buf = (PVOID)AddressLen;
                    NewBuf[Count + 1].len = sizeof(*AddressLen);
                }
                Count += 2;
            }
        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            AFD_DbgPrint(MIN_TRACE,("Access violation copying buffer info "
                                    "from userland (%p %p)\n",
                                    Buf, AddressLen));
            ExFreePoolWithTag(NewBuf, TAG_AFD_WSA_BUFFER);
            _SEH2_YIELD(return NULL);
        } _SEH2_END;

        for( i = 0; i < Count; i++ ) {
            PVOID UserBuf;
            ULONG UserLen;
            PVOID KernelBuf;
            NTSTATUS CopyStatus;

            AFD_DbgPrint(MID_TRACE,("Locking buffer %u (%p:%u)\n",
                                    i, NewBuf[i].buf, NewBuf[i].len));

            UserBuf = NewBuf[i].buf;
            UserLen = NewBuf[i].len;

            if( !UserBuf || !UserLen ) {
                MapBuf[i].BufferAddress = NULL;
                MapBuf[i].Mdl = NULL;
                MapBuf[i].OriginalUserBuffer = NULL;
                continue;
            }

            /* dev-nt6-1: same kernel-pool trick as LockRequest. ReactOS's
             * MmProbeAndLockPages raises STATUS_ACCESS_VIOLATION on valid
             * user buffers, so we allocate a kernel-pool buffer the same
             * size and SEH-copy the user data in. The MDL we hand back
             * wraps the kernel buffer (always valid). UnlockBuffers does
             * the SEH-copy back to user space and frees the kernel buffer. */
            KernelBuf = ExAllocatePoolWithTag(NonPagedPool, UserLen,
                                              TAG_AFD_DATA_BUFFER);
            if (KernelBuf == NULL) {
                AFD_DbgPrint(MIN_TRACE,("Failed to allocate kernel buffer\n"));
                ExFreePoolWithTag(NewBuf, TAG_AFD_WSA_BUFFER);
                return NULL;
            }

            /* `Write` semantics from the caller:
             *   Write=FALSE → kernel READS from this buffer (SEND path)
             *                 → must copy user→kernel NOW so the
             *                   kernel buffer holds the user data.
             *   Write=TRUE  → kernel WRITES to this buffer (RECV path)
             *                 → no copy now; UnlockBuffers will copy
             *                   kernel→user after the operation. */
            CopyStatus = STATUS_SUCCESS;
            if (!Write)
            {
                _SEH2_TRY {
                    RtlCopyMemory(KernelBuf, UserBuf, UserLen);
                } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                    CopyStatus = _SEH2_GetExceptionCode();
                } _SEH2_END;
            }

            if (!NT_SUCCESS(CopyStatus)) {
                AFD_DbgPrint(MIN_TRACE,("SEH-copy from user failed 0x%08lx\n",
                                        (ULONG)CopyStatus));
                ExFreePoolWithTag(KernelBuf, TAG_AFD_DATA_BUFFER);
                ExFreePoolWithTag(NewBuf, TAG_AFD_WSA_BUFFER);
                return NULL;
            }

            MapBuf[i].Mdl = IoAllocateMdl(KernelBuf, UserLen,
                                          FALSE, FALSE, NULL);
            if (MapBuf[i].Mdl == NULL) {
                ExFreePoolWithTag(KernelBuf, TAG_AFD_DATA_BUFFER);
                ExFreePoolWithTag(NewBuf, TAG_AFD_WSA_BUFFER);
                return NULL;
            }
            /* Lock the MDL via KernelMode probe - kernel pool is always
             * resident so this should always succeed. We deliberately
             * do NOT use MmBuildMdlForNonPagedPool because that sets
             * MDL_SOURCE_IS_NONPAGED_POOL which trips an assertion in
             * MmMapLockedPages when downstream callers (read.c:123,
             * write.c:207) try to map it. With a "real" lock the MDL
             * is mappable like any user-locked MDL. */
            _SEH2_TRY {
                MmProbeAndLockPages(MapBuf[i].Mdl, KernelMode,
                                    Write ? IoWriteAccess : IoReadAccess);
            } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                LockFailed = TRUE;
            } _SEH2_END;

            if (LockFailed) {
                IoFreeMdl(MapBuf[i].Mdl);
                MapBuf[i].Mdl = NULL;
                ExFreePoolWithTag(KernelBuf, TAG_AFD_DATA_BUFFER);
                ExFreePoolWithTag(NewBuf, TAG_AFD_WSA_BUFFER);
                return NULL;
            }

            /* Stash the user pointer in a dedicated field so UnlockBuffers
             * can copy back at the end of a RECV operation. We must NOT
             * reuse BufferAddress here because SatisfyPacketRecvRequest,
             * TryToSatisfyRecvRequestFromBuffer, and AfdConnectedSocketWriteData
             * all transiently overwrite BufferAddress with the result of
             * MmMapLockedPages - which destroys the user pointer before
             * UnlockBuffers can read it. SEND buffers don't need a
             * writeback (kernel only reads from them). */
            MapBuf[i].BufferAddress = NULL;
            MapBuf[i].OriginalUserBuffer = Write ? UserBuf : NULL;
            /* Capture the originating process. UnlockBuffers can run from
             * a TDI completion (ReceiveComplete → ReceiveActivity) in the
             * context of an unrelated thread, so we must KeStackAttachProcess
             * back to this process before touching OriginalUserBuffer. */
            MapBuf[i].OriginalProcess = (Write && UserBuf) ? PsGetCurrentProcess() : NULL;

            /* Re-point the WSABUF entry at the kernel copy so that any
             * downstream code (e.g. DGDeliverData copying into Current->Buffer
             * via the MDL system VA) writes to the kernel buffer. */
            NewBuf[i].buf = KernelBuf;
        }
    }

    AFD_DbgPrint(MID_TRACE,("Leaving %p\n", NewBuf));

    return NewBuf;
}

VOID UnlockBuffers( PAFD_WSABUF Buf, UINT Count, BOOL Address ) {
    UINT Lock = Address ? 2 : 0;
    PAFD_MAPBUF Map = (PAFD_MAPBUF)(Buf + Count + Lock);
    UINT i;

    if( !Buf ) return;

    /* dev-nt6-1: each MapBuf entry now wraps a kernel-pool buffer that
     * was set up by LockBuffers. For read (RX) buffers, Map[i].OriginalUserBuffer
     * holds the original user-mode pointer - copy back via SEH so the
     * user-mode recv() returns the data. For write (TX) buffers,
     * OriginalUserBuffer is NULL and there's nothing to copy back.
     * BufferAddress is NOT used here because downstream callers
     * (SatisfyPacketRecvRequest, TryToSatisfyRecvRequestFromBuffer,
     * AfdConnectedSocketWriteData) overwrite it with transient
     * MmMapLockedPages results. */
    for( i = 0; i < Count + Lock; i++ ) {
        if( Map[i].Mdl ) {
            PVOID KernelBuf = Buf[i].buf;                /* kernel copy */
            PVOID UserBuf   = Map[i].OriginalUserBuffer; /* original user ptr */
            ULONG Len       = Buf[i].len;
            PEPROCESS OriginalProcess = Map[i].OriginalProcess;

            if (UserBuf != NULL && KernelBuf != NULL && Len > 0)
            {
                /* IRP completion may fire from a TDI worker thread in
                 * arbitrary process context. Attach back to the requestor
                 * so the user-mode VA resolves correctly; without this the
                 * writeback hits whatever process is currently on the CPU
                 * and recv() returns N bytes of zeros to the caller. */
                KAPC_STATE ApcState;
                BOOLEAN Attached = FALSE;
                if (OriginalProcess != NULL && OriginalProcess != PsGetCurrentProcess())
                {
                    KeStackAttachProcess((PKPROCESS)OriginalProcess, &ApcState);
                    Attached = TRUE;
                }
                _SEH2_TRY {
                    RtlCopyMemory(UserBuf, KernelBuf, Len);
                } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                    /* User buffer went away - keep going. */
                } _SEH2_END;
                if (Attached)
                    KeUnstackDetachProcess(&ApcState);
            }

            MmUnlockPages( Map[i].Mdl );
            IoFreeMdl( Map[i].Mdl );
            Map[i].Mdl = NULL;
            Map[i].OriginalUserBuffer = NULL;
            Map[i].OriginalProcess = NULL;

            if (KernelBuf != NULL)
                ExFreePoolWithTag(KernelBuf, TAG_AFD_DATA_BUFFER);
        }
    }

    ExFreePoolWithTag(Buf, TAG_AFD_WSA_BUFFER);
    Buf = NULL;
}

/* Produce a kernel-land handle array with handles replaced by object
 * pointers.  This will allow the system to do proper alerting */
PAFD_HANDLE LockHandles(PAFD_HANDLE HandleArray, 
                        UINT HandleCount, 
                        BOOLEAN Wow64Is32Bit) 
{
    UINT i;
    NTSTATUS Status = STATUS_SUCCESS;
#ifdef _WIN64
    PAFD_HANDLE32 HandleArray32 = (PAFD_HANDLE32)HandleArray;
#endif
    SOCKET handle;

    PAFD_HANDLE FileObjects = ExAllocatePoolWithTag(NonPagedPool,
                                                    HandleCount * sizeof(AFD_HANDLE),
                                                    TAG_AFD_POLL_HANDLE);

    for( i = 0; FileObjects && i < HandleCount; i++ ) 
    {
#ifdef _WIN64
        if (Wow64Is32Bit)
        {
            handle = HandleArray32[i].Handle;
            FileObjects[i].Events = HandleArray32[i].Events;
        }
        else
#endif
        {
            handle = HandleArray[i].Handle;
            FileObjects[i].Events = HandleArray[i].Events;
        }
    
        FileObjects[i].Status = 0;
        FileObjects[i].Handle = 0;
        
        if(!handle) continue;
        
        if(NT_SUCCESS(Status)) 
        {
            Status = ObReferenceObjectByHandle((PVOID)handle,
                                               FILE_ALL_ACCESS,
                                               NULL,
                                               KernelMode,
                                               (PVOID*)&FileObjects[i].Handle,
                                               NULL);
        }

        if(!NT_SUCCESS(Status))
        {
            AFD_DbgPrint(MIN_TRACE,("Failed to reference handles (0x%x)\n", Status));
            FileObjects[i].Handle = 0;
        }
    }

    if(!NT_SUCCESS(Status)) 
    {
        UnlockHandles( FileObjects, HandleCount );
        return NULL;
    }

    return FileObjects;
}

VOID UnlockHandles( PAFD_HANDLE HandleArray, UINT HandleCount ) {
    UINT i;

    for( i = 0; i < HandleCount; i++ ) {
        if( HandleArray[i].Handle )
            ObDereferenceObject( (PVOID)HandleArray[i].Handle );
    }

    ExFreePoolWithTag(HandleArray, TAG_AFD_POLL_HANDLE);
    HandleArray = NULL;
}

BOOLEAN SocketAcquireStateLock( PAFD_FCB FCB ) {
    if( !FCB ) return FALSE;

    return !KeWaitForMutexObject(&FCB->Mutex,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);
}

VOID SocketStateUnlock( PAFD_FCB FCB ) {
    KeReleaseMutex(&FCB->Mutex, FALSE);
}

NTSTATUS NTAPI UnlockAndMaybeComplete
( PAFD_FCB FCB, NTSTATUS Status, PIRP Irp,
  UINT Information ) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    if (Irp->MdlAddress && Irp->Tail.Overlay.DriverContext[0])
        UnlockRequest(Irp, IoGetCurrentIrpStackLocation(Irp));
    (void)IoSetCancelRoutine(Irp, NULL);
    SocketStateUnlock( FCB );
    IoCompleteRequest( Irp, IO_NETWORK_INCREMENT );
    return Status;
}


NTSTATUS LostSocket( PIRP Irp ) {
    NTSTATUS Status = STATUS_FILE_CLOSED;
    AFD_DbgPrint(MIN_TRACE,("Called.\n"));
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = Status;
    if (Irp->MdlAddress && Irp->Tail.Overlay.DriverContext[0])
        UnlockRequest(Irp, IoGetCurrentIrpStackLocation(Irp));
    IoCompleteRequest( Irp, IO_NO_INCREMENT );
    return Status;
}

NTSTATUS QueueUserModeIrp(PAFD_FCB FCB, PIRP Irp, UINT Function)
{
    NTSTATUS Status;

    /* Add the IRP to the queue in all cases (so AfdCancelHandler will work properly) */
    InsertTailList( &FCB->PendingIrpList[Function],
                   &Irp->Tail.Overlay.ListEntry );

    /* Acquire the cancel spin lock and check the cancel bit */
    IoAcquireCancelSpinLock(&Irp->CancelIrql);
    if (!Irp->Cancel)
    {
        /* We are not cancelled; we're good to go so
         * set the cancel routine, release the cancel spin lock,
         * mark the IRP as pending, and
         * return STATUS_PENDING to the caller
         */
        (void)IoSetCancelRoutine(Irp, AfdCancelHandler);
        IoReleaseCancelSpinLock(Irp->CancelIrql);
        IoMarkIrpPending(Irp);
        Status = STATUS_PENDING;
    }
    else
    {
        /* We were already cancelled before we were able to register our cancel routine
         * so we are to call the cancel routine ourselves right here to cancel the IRP
         * (which handles all the stuff we do above) and return STATUS_CANCELLED to the caller
         */
        AfdCancelHandler(IoGetCurrentIrpStackLocation(Irp)->DeviceObject,
                         Irp);
        Status = STATUS_CANCELLED;
    }

    return Status;
}

NTSTATUS LeaveIrpUntilLater( PAFD_FCB FCB, PIRP Irp, UINT Function ) {
    NTSTATUS Status;

    Status = QueueUserModeIrp(FCB, Irp, Function);

    SocketStateUnlock( FCB );

    return Status;
} 
