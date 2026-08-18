/*
 * PROJECT:     ReactOS Kernel Debugger over Network
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Contract between kdnet.dll and an OS-side NDIS miniport that
 *              shares the debugger's network adapter
 *
 * KDNET owns the debug NIC exclusively: it programs the descriptor rings from
 * KdDebuggerInitialize0, long before the PnP or NDIS stacks exist, and it must
 * keep working with every other processor frozen inside a bugcheck.  That is
 * why pci.sys rewrites the device's identifiers to VEN_DEAD&DEV_BEEF - so no
 * ordinary miniport can ever bind to it and start driving the same rings.
 *
 * The result is that a machine with a single NIC can have a kernel debugger or
 * a network, never both.  This interface removes that limitation without
 * giving up KDNET's exclusive ownership: the OS-side miniport never touches
 * the hardware.  It hands frames to KDNET for transmission and asks KDNET to
 * drain the receive ring on its behalf, and KDNET keeps for itself only the
 * frames that belong to the debugger session.
 *
 * The interface is inert unless /KDNETSHARE appears in the boot options.  With
 * the option absent, KdNetShareRegister fails with STATUS_NOT_SUPPORTED, no
 * share state is ever allocated, and the debugger's code paths execute exactly
 * as they did before this interface existed.
 */

#ifndef _KDNETSHARE_H_
#define _KDNETSHARE_H_

#define KDNET_SHARE_INTERFACE_VERSION 1

/* An Ethernet frame is handed up exactly once, and only for the duration of
 * the callback: the buffer is KDNET's receive ring and is recycled as soon as
 * the callback returns.  The callee must copy anything it wants to keep.
 *
 * Called at HIGH_LEVEL with KDNET's share lock held.  It must not block, must
 * not touch pageable memory, and - most importantly - must not call anything
 * that can print, because DbgPrint reaches KdSendPacket and would re-enter the
 * very transport whose lock is held. */
typedef VOID
(NTAPI *PKDNET_SHARE_RECEIVE_CALLBACK)(
    _In_opt_ PVOID Context,
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length);

typedef struct _KDNET_SHARE_REGISTRATION
{
    ULONG Version;                          /* KDNET_SHARE_INTERFACE_VERSION */
    PKDNET_SHARE_RECEIVE_CALLBACK Receive;
    PVOID Context;
} KDNET_SHARE_REGISTRATION, *PKDNET_SHARE_REGISTRATION;

typedef struct _KDNET_SHARE_INFO
{
    ULONG Version;
    UCHAR MacAddress[6];    /* The adapter's real MAC; the OS shares this L2
                             * identity with the debugger, because it is one
                             * adapter. */
    ULONG DebuggerHostIp;   /* Host byte order.  Informational: the OS stack */
    ULONG DebuggerTargetIp; /* must not configure DebuggerTargetIp as its own */
    USHORT DebuggerPort;    /* address, or the two would collide. */
    USHORT Reserved;
    ULONG LinkSpeedMbps;
} KDNET_SHARE_INFO, *PKDNET_SHARE_INFO;

/* Exported by kdnet.dll.  A driver that imports these will fail to load when
 * the debug transport is not kdnet.dll, which is the intended behaviour: the
 * sharing miniport is meaningful only on a KDNET boot. */

/* Claims the share slot.  There is exactly one: the debug NIC is one adapter.
 * Returns STATUS_NOT_SUPPORTED when /KDNETSHARE was not specified,
 * STATUS_DEVICE_NOT_READY before the transport has finished initializing, and
 * STATUS_DEVICE_BUSY if another miniport already registered.
 * IRQL: PASSIVE_LEVEL. */
NTSTATUS
NTAPI
KdNetShareRegister(
    _In_ PKDNET_SHARE_REGISTRATION Registration);

/* Releases the share slot.  On return the receive callback is guaranteed not
 * to be running and will never be entered again, so the caller may free its
 * context.  IRQL: PASSIVE_LEVEL. */
VOID
NTAPI
KdNetShareDeregister(VOID);

/* Reports the shared adapter's parameters.  IRQL: <= DISPATCH_LEVEL. */
NTSTATUS
NTAPI
KdNetShareQuery(
    _Out_ PKDNET_SHARE_INFO Info);

/* Transmits one complete Ethernet frame, including its header, through the
 * debug NIC.  IRQL: <= DISPATCH_LEVEL. */
NTSTATUS
NTAPI
KdNetShareTransmit(
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length);

/* Drains up to MaxFrames frames from the receive ring, dispatching those that
 * belong to the OS through the registered callback and retaining those that
 * belong to the debugger.  Returns the number of frames handed to the
 * callback.  IRQL: <= DISPATCH_LEVEL. */
ULONG
NTAPI
KdNetSharePoll(
    _In_ ULONG MaxFrames);

#endif /* _KDNETSHARE_H_ */
