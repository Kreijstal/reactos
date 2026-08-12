/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ps/amd64/psctx.c
 * PURPOSE:         Process Manager: Set/Get Context for i386
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

static
VOID
PspGetThreadFloatingPointContext(
    _Inout_ PCONTEXT Context)
{
    DECLSPEC_ALIGN(16) XMM_SAVE_AREA32 FloatState;

    if ((Context->ContextFlags & CONTEXT_FLOATING_POINT) !=
        CONTEXT_FLOATING_POINT)
    {
        return;
    }

    _fxsave64(&FloatState);

    RtlCopyMemory(&Context->FltSave,
                  &FloatState,
                  FIELD_OFFSET(XMM_SAVE_AREA32, MxCsr));
    Context->FltSave.MxCsr = Context->MxCsr;
    Context->FltSave.MxCsr_Mask = FloatState.MxCsr_Mask;
    RtlCopyMemory(Context->FltSave.FloatRegisters,
                  FloatState.FloatRegisters,
                  sizeof(Context->FltSave.FloatRegisters));
}

static
VOID
PspSetThreadFloatingPointContext(
    _In_ PCONTEXT Context)
{
    DECLSPEC_ALIGN(16) XMM_SAVE_AREA32 FloatState;

    if ((Context->ContextFlags & CONTEXT_FLOATING_POINT) !=
        CONTEXT_FLOATING_POINT)
    {
        return;
    }

    _fxsave64(&FloatState);
    RtlCopyMemory(&FloatState,
                  &Context->FltSave,
                  FIELD_OFFSET(XMM_SAVE_AREA32, MxCsr));
    RtlCopyMemory(FloatState.FloatRegisters,
                  Context->FltSave.FloatRegisters,
                  sizeof(FloatState.FloatRegisters));
    _fxrstor64(&FloatState);
}


_IRQL_requires_(APC_LEVEL)
VOID
NTAPI
PspGetOrSetContextKernelRoutine(
    _In_ PKAPC Apc,
    _Inout_ PKNORMAL_ROUTINE* NormalRoutine,
    _Inout_ PVOID* NormalContext,
    _Inout_ PVOID* SystemArgument1,
    _Inout_ PVOID* SystemArgument2)
{
    PGET_SET_CTX_CONTEXT GetSetContext;
    PKTHREAD Thread;
    PKTRAP_FRAME TrapFrame = NULL;

    PAGED_CODE();

    /* Get the Context Structure */
    GetSetContext = CONTAINING_RECORD(Apc, GET_SET_CTX_CONTEXT, Apc);
    Thread = Apc->SystemArgument2;
    NT_ASSERT(KeGetCurrentThread() == Thread);

    /* If this is a kernel-mode request, grab the saved trap frame */
    if (GetSetContext->Mode == KernelMode)
    {
        TrapFrame = Thread->TrapFrame;
    }

    /* If we don't have one, grab it from the stack */
    if (TrapFrame == NULL)
    {
        /* Get the thread's base trap frame */
        TrapFrame = KeGetTrapFrame(KeGetCurrentThread());
    }

    /* Check if it's a set or get */
    if (Apc->SystemArgument1 != 0)
    {
        /* Set the nonvolatiles on the stack, target frame is the trap frame */
        KiSetTrapContext(TrapFrame, &GetSetContext->Context, GetSetContext->Mode);
        PspSetThreadFloatingPointContext(&GetSetContext->Context);
    }
    else
    {
        /* Get the nonvolatiles from the stack */
        KiGetTrapContext(TrapFrame, &GetSetContext->Context);
        PspGetThreadFloatingPointContext(&GetSetContext->Context);

        /* Report the original user stack top for a thread that has not
           entered its start routine yet. */
        if ((GetSetContext->Mode == UserMode) &&
            ((GetSetContext->Context.ContextFlags & CONTEXT_CONTROL) ==
             CONTEXT_CONTROL) &&
            (GetSetContext->Context.Rip ==
             (ULONG64)CONTAINING_RECORD(Thread, ETHREAD, Tcb)->StartAddress))
        {
            GetSetContext->Context.Rsp =
                ALIGN_UP_BY(GetSetContext->Context.Rsp, PAGE_SIZE);
        }
    }

    /* Notify the Native API that we are done */
    KeSetEvent(&GetSetContext->Event, IO_NO_INCREMENT, FALSE);
}

/* EOF */
