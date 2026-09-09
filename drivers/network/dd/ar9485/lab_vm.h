/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: the portable opcode interpreter.
 *
 * The interpreter is deliberately free of NDIS and of the AR9485 itself.  It
 * reaches hardware only through LAB_TARGET, which lab.c fills in with the
 * miniport's BAR and DMA pool.  That split exists for one reason: the
 * interpreter has to be RIGHT THE FIRST TIME -- the whole point of the lab is
 * a two-reboot budget, and a bug in the interpreter is exactly the kind of
 * thing that would spend a third one.  Keeping it portable lets it be run and
 * asserted on the build host in a second, instead of by booting a machine.
 *
 * See tool/host/labtest.c for the harness that does that.
 */

#ifndef _AR9485_LAB_VM_H_
#define _AR9485_LAB_VM_H_

#ifdef AR9485_LAB_HOST

/* Host harness build: no NT headers, just enough to parse the shared ABI. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef unsigned char UCHAR;
typedef int BOOLEAN;
typedef void *PVOID;
typedef unsigned char *PUCHAR;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#ifndef MAXULONG
#define MAXULONG 0xFFFFFFFFu
#endif

#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED     0
#define FILE_ANY_ACCESS     0
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

#define RtlZeroMemory(Destination, Length)      memset((Destination), 0, (Length))
#define RtlCopyMemory(Destination, Source, Len) memcpy((Destination), (Source), (Len))
#define RtlMoveMemory(Destination, Source, Len) memmove((Destination), (Source), (Len))

#else /* kernel build */

#include <ndis.h>

#endif /* AR9485_LAB_HOST */

#include <reactos/ar9485_lab.h>

/*
 * Everything the interpreter is allowed to touch.  A target that cannot do
 * something leaves the entry NULL and the opcode reports UNSUPPORTED rather
 * than the interpreter guessing.
 */
typedef struct _LAB_TARGET
{
    void *Context;

    /* MMIO window.  Offsets are validated against WindowLength and rejected,
     * never clamped -- a clamped offset returns a plausible value from the
     * wrong register, which is worse than an error. */
    ULONG WindowLength;
    ULONG (*Read32)(void *Context, ULONG Offset);
    void (*Write32)(void *Context, ULONG Offset, ULONG Value);

    void (*Stall)(void *Context, ULONG Microseconds);
    void (*Barrier)(void *Context);

    /* DMA buffers, fixed pool and runtime allocations in one index space. */
    ULONG (*DmaCount)(void *Context);
    BOOLEAN (*DmaBuffer)(void *Context, ULONG Index, PUCHAR *VirtualAddress,
                         ULONG *Length, ULONG *PhysicalLow, ULONG *Flags);
    ULONG (*DmaAlloc)(void *Context, ULONG Count, ULONG Size);  /* MAXULONG = failed */
    void (*DmaFreeAll)(void *Context);

    /* Optional.  Both return an AR9485LAB_ST_* code. */
    ULONG (*PciAccess)(void *Context, BOOLEAN Write, ULONG Offset, ULONG Width,
                       ULONG *Value);
    ULONG (*CallHw)(void *Context, ULONG Function, ULONG Argument, ULONG *Value);
} LAB_TARGET, *PLAB_TARGET;

/*
 * Execute one program.  Returns an AR9485LAB_VM_* code describing whether the
 * program was well-formed; whether the OPS succeeded is reported per-record in
 * the result buffer, and summarised by the header's FailedIndex.
 *
 * In and Out must not overlap.
 */
#define AR9485LAB_VM_OK             0
#define AR9485LAB_VM_MALFORMED      1
#define AR9485LAB_VM_ABI_MISMATCH   2
#define AR9485LAB_VM_OUT_TOO_SMALL  3

ULONG
LabVmRun(
    const LAB_TARGET *Target,
    const UCHAR *In,
    ULONG InLength,
    UCHAR *Out,
    ULONG OutLength,
    ULONG *Written);

#endif /* _AR9485_LAB_VM_H_ */
