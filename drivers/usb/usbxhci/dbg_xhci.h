/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         debug header files
 * COPYRIGHT:       Rama Teja Gampa <ramateja.g@gmail.com>
 */

#pragma once

#if DBG

    #ifndef NDEBUG_XHCI_TRACE
        #define DPRINT1_XHCI(fmt, ...) do { \
            if (DbgPrint("(%s:%d) " fmt, __RELFILE__, __LINE__, ##__VA_ARGS__))  \
                DbgPrint("(%s:%d) DbgPrint() failed!\n", __RELFILE__, __LINE__); \
        } while (0)
    #else
        #if defined(_MSC_VER)
            #define DPRINT1_XHCI __noop
        #else
            #define DPRINT1_XHCI(...) do {if(0) {DbgPrint(__VA_ARGS__);}} while(0)
        #endif
    #endif

    #ifndef NDEBUG_XHCI_ROOT_HUB
        #define DPRINT_RH(fmt, ...) do { \
            if (DbgPrint("(%s:%d) " fmt, __RELFILE__, __LINE__, ##__VA_ARGS__))  \
                DbgPrint("(%s:%d) DbgPrint() failed!\n", __RELFILE__, __LINE__); \
        } while (0)
    #else
        #if defined(_MSC_VER)
            #define DPRINT_RH __noop
        #else
            #define DPRINT_RH(...) do {if(0) {DbgPrint(__VA_ARGS__);}} while(0)
        #endif
    #endif

#else /* not DBG */

    #if defined(_MSC_VER)
        #define DPRINT1_XHCI __noop
        #define DPRINT_RH __noop
    #else
        #define DPRINT1_XHCI(...) do {if(0) {DbgPrint(__VA_ARGS__);}} while(0)
        #define DPRINT_RH(...) do {if(0) {DbgPrint(__VA_ARGS__);}} while(0)
    #endif /* _MSC_VER */

#endif /* not DBG */

/*
 * Live instrumentation block for debugging transfers that never complete.
 *
 * A lost bulk IN transfer produces silence, so DbgPrint is the wrong tool: the
 * interesting question is what did NOT happen, and the 32 KiB buffered ring is
 * long overwritten by the time anybody notices.  Instead this block accumulates
 * counters and last-event snapshots that a kernel debugger can read at any time
 * with a single memory read, cheaply and repeatedly, while the machine is wedged.
 *
 * It also republishes the controller's mapped register windows so the debugger
 * can read USBSTS, IMAN and ERDP straight from the live hardware without having
 * to walk the driver's private structures first.
 *
 * Sampling the counters twice a few seconds apart answers the decisive question
 * directly: if IsrEntries is still advancing while TransferEvents is frozen, the
 * controller is interrupting but producing no transfer events; if IsrEntries is
 * also frozen the controller has stopped interrupting altogether.
 */
#define XHCI_DBG_STATS_SIGNATURE 0x47424458 /* 'XDBG' */
#define XHCI_DBG_STATS_VERSION   1

typedef struct _XHCI_DBG_STATS
{
    ULONG Signature;
    ULONG Version;

    /* Mapped register windows, so a debugger can read the live controller. */
    PULONG BaseIoAdress;
    PULONG OperationalRegs;
    PULONG RunTimeRegisterBase;
    PULONG DoorBellRegisterBase;
    PVOID HcResourcesVA;
    ULONGLONG HcResourcesPA;

    /* Counters.  Incremented with Interlocked ops; no lock is taken. */
    volatile LONG IsrEntries;
    volatile LONG IsrClaimedIman;
    volatile LONG IsrClaimedUsbSts;
    volatile LONG IsrDeclined;
    volatile LONG DpcEntries;
    volatile LONG EventsProcessed;
    volatile LONG TransferEvents;
    volatile LONG CommandEvents;
    volatile LONG PortEvents;
    volatile LONG BulkSubmits;
    volatile LONG Doorbells;
    volatile LONG TransferEventNoMatch;

    /* Last bulk transfer handed to the controller. */
    ULONGLONG LastBulkTrbPA;
    ULONGLONG LastBulkTime;
    ULONG LastBulkLength;
    ULONG LastBulkSlot;
    ULONG LastBulkDci;

    /* Last doorbell written. */
    ULONGLONG LastDoorbellTime;
    ULONG LastDoorbellSlot;
    ULONG LastDoorbellTarget;

    /* Last transfer event taken off the event ring. */
    ULONGLONG LastEventTrbPA;
    ULONGLONG LastEventTime;
    ULONG LastEventCode;
    ULONG LastEventSlot;
    ULONG LastEventEp;
    ULONG LastEventLength;

    /* Register snapshot taken on the last interrupt. */
    ULONGLONG LastIsrTime;
    ULONG LastIsrIman;
    ULONG LastIsrUsbSts;
} XHCI_DBG_STATS, *PXHCI_DBG_STATS;

extern XHCI_DBG_STATS XhciDbgStats;
