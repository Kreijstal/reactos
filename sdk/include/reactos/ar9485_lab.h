/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: shared kernel/user ABI.
 *
 * The lab exists because ar9485.sys is a PnP-bound NDIS miniport and cannot
 * be swapped on a live system, so every register-level hypothesis otherwise
 * costs a deploy and a reboot.  The kernel side is therefore a FIXED
 * instrument -- an interpreter for a batched program of register, PCI-config
 * and DMA operations -- and all volatile bring-up logic (reset sequences,
 * initval tables, descriptor layouts, ring shapes) lives on the host and is
 * pushed as data.
 *
 * The surface is deliberately general: anything expressible as "touch the
 * BAR, touch PCI config, shape a DMA buffer, wait, look" must not require
 * new kernel code, because new kernel code costs a reboot.
 *
 * Every program is submitted in ONE IOCTL.  An ath9k reset is ~1000 register
 * writes; one IOCTL per write across the luagent transport would take
 * minutes.
 */

#ifndef _AR9485_LAB_H_
#define _AR9485_LAB_H_

/* Bumped whenever the layouts below change.  The user-mode tool refuses to
 * run against a mismatched driver rather than misparsing its results. */
#define AR9485LAB_ABI_VERSION           1

#define AR9485LAB_DEVICE_NAME           L"\\Device\\AR9485Lab"
#define AR9485LAB_SYMBOLIC_NAME         L"\\DosDevices\\AR9485Lab"
#define AR9485LAB_WIN32_NAME            L"\\\\.\\AR9485Lab"

/* Administrators and LocalSystem only.  The RUN opcode set is an arbitrary
 * MMIO write primitive on this device; it must not be reachable by a normal
 * user even on a development machine. */
#define AR9485LAB_SDDL                  L"D:P(A;;GA;;;SY)(A;;GA;;;BA)"

#define IOCTL_AR9485LAB_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AR9485LAB_CLAIM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AR9485LAB_RELEASE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AR9485LAB_RUN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ===========================================================================
 *  Limits
 *
 *  These are not tuning knobs.  Each one bounds how long a single program can
 *  hold the chip, and the laptop this runs on powers itself off during a
 *  prolonged stall.
 * ===========================================================================
 */
#define AR9485LAB_MAX_OPS               4096
#define AR9485LAB_MAX_PROGRAM_BYTES     (256 * 1024)
#define AR9485LAB_MAX_RESULT_BYTES      (256 * 1024)
#define AR9485LAB_MAX_DUMP_DWORDS       4096
/* Per-op ceilings, plus a whole-program budget so a thousand short delays
 * cannot add up to a hang. */
#define AR9485LAB_MAX_DELAY_US          100000      /* 100 ms */
#define AR9485LAB_MAX_POLL_US           1000000     /* 1 s */
#define AR9485LAB_MAX_TOTAL_WAIT_US     5000000     /* 5 s */
/* Runtime DMA, for descriptor rings the fixed RX pool cannot express. */
#define AR9485LAB_MAX_DMA_ALLOC         64
#define AR9485LAB_MAX_DMA_ALLOC_SIZE    (64 * 1024)
#define AR9485LAB_MAX_DMA_ALLOC_TOTAL   (2 * 1024 * 1024)

/* ===========================================================================
 *  Opcodes
 * ===========================================================================
 */
typedef enum _AR9485LAB_OPCODE
{
    AR9485LAB_OP_END        = 0,
    /* MMIO over BAR0.  A=byte offset (dword-aligned, < BAR length). */
    AR9485LAB_OP_READ32     = 1,    /* A=offset                  -> Value */
    AR9485LAB_OP_WRITE32    = 2,    /* A=offset B=value                   */
    AR9485LAB_OP_RMW32      = 3,    /* A=offset B=and C=or       -> Value (new) */
    AR9485LAB_OP_POLL       = 4,    /* A=offset B=mask C=value D=timeout_us
                                     *                          -> Value (last read) */
    AR9485LAB_OP_DELAY_US   = 5,    /* A=microseconds                     */
    AR9485LAB_OP_DUMP       = 6,    /* A=offset B=dword count    -> data  */
    AR9485LAB_OP_MARK       = 7,    /* A=tag                     -> Value=tag */
    AR9485LAB_OP_BARRIER    = 8,    /* full memory barrier                */
    /* PCI config space of the AR9485 function itself. */
    AR9485LAB_OP_PCI_READ   = 9,    /* A=offset B=width(1|2|4)   -> Value */
    AR9485LAB_OP_PCI_WRITE  = 10,   /* A=offset B=value C=width           */
    /* DMA buffers: the fixed RX pool plus anything allocated at runtime. */
    AR9485LAB_OP_DMA_LIST   = 11,   /* -> data: AR9485LAB_DMA_ENTRY[]     */
    AR9485LAB_OP_DMA_ALLOC  = 12,   /* A=count B=size            -> Value=first index */
    AR9485LAB_OP_DMA_FREE   = 13,   /* A=index, or AR9485LAB_DMA_ALL      */
    AR9485LAB_OP_DMA_READ   = 14,   /* A=index B=offset C=length -> data  */
    AR9485LAB_OP_DMA_WRITE  = 15,   /* A=index B=offset C=length D=program data offset */
    AR9485LAB_OP_DMA_ZERO   = 16,   /* A=index B=offset C=length          */
    /* Escape hatch: call the miniport's own ath9k helpers, so a user-mode
     * sequence can be compared against the kernel path without a rebuild. */
    AR9485LAB_OP_CALL_HW    = 17,   /* A=AR9485LAB_HW_* B,C=args -> Value */
    /* Bus addresses are only known at run time, so a script cannot carry
     * them as literals.  These two are what make a descriptor ring
     * expressible from user mode at all: one programs a buffer's address
     * into a register, the other into a field of another buffer. */
    AR9485LAB_OP_WRITE_DMA_PA = 18, /* A=BAR offset B=dma index C=offset in buffer
                                     *                          -> Value=written PA */
    AR9485LAB_OP_DMA_POKE_PA  = 19, /* A=dst index B=offset in dst
                                     * C=src index D=offset in src
                                     *                          -> Value=written PA */
    AR9485LAB_OP_MAX
} AR9485LAB_OPCODE;

/* AR9485LAB_OP_DMA_FREE: release every runtime allocation.  The fixed RX
 * pool belongs to the miniport and is never freeable from a program. */
#define AR9485LAB_DMA_ALL               0xFFFFFFFF

/* AR9485LAB_OP_CALL_HW function ids. */
typedef enum _AR9485LAB_HW_CALL
{
    AR9485LAB_HW_START      = 0,    /* B=channel MHz -> ar9485_hw_start()  */
    AR9485LAB_HW_QUEUE_RX   = 1,    /* the miniport's own RX arming        */
    AR9485LAB_HW_HARVEST    = 2,    /* B=frequency -> Value = frames done  */
    AR9485LAB_HW_LOG        = 3,    /* B=DMA buffer index -> Value = bytes  */
                                    /* of the driver's DPRINT ring copied  */
    AR9485LAB_HW_STATE      = 4,    /* Value = MlmeState | AuthSeen<<8 |   */
                                    /* AssocSeen<<9 | Deauth<<10 |         */
                                    /* RxArmed<<11 | TxReady<<12 |         */
                                    /* TxPending<<16 | ScanInProgress<<24 */
    AR9485LAB_HW_MAX
} AR9485LAB_HW_CALL;

/* ===========================================================================
 *  Program layout:  header | AR9485LAB_OP[OpCount] | data blob
 * ===========================================================================
 */
typedef struct _AR9485LAB_OP
{
    ULONG Op;
    ULONG A;
    ULONG B;
    ULONG C;
    ULONG D;
} AR9485LAB_OP, *PAR9485LAB_OP;

typedef struct _AR9485LAB_PROGRAM
{
    ULONG AbiVersion;
    ULONG OpCount;
    ULONG DataOffset;       /* from the start of the program buffer */
    ULONG DataLength;
    ULONG Flags;
    ULONG Reserved;
    /* AR9485LAB_OP Ops[OpCount]; then the data blob. */
} AR9485LAB_PROGRAM, *PAR9485LAB_PROGRAM;

/* Keep executing after an op fails, instead of stopping at the first error.
 * Off by default: a reset sequence whose third write failed has produced
 * meaningless results from every write after it. */
#define AR9485LAB_PROGRAM_CONTINUE_ON_ERROR 0x00000001

/* ===========================================================================
 *  Result layout:  header | AR9485LAB_RESULT[RecordCount] | data blob
 * ===========================================================================
 */
typedef struct _AR9485LAB_RESULT
{
    ULONG Index;            /* index of the op that produced this record */
    ULONG Op;
    ULONG Status;           /* AR9485LAB_ST_* */
    ULONG Value;
    ULONG DataOffset;       /* from the start of the result buffer */
    ULONG DataLength;
    /* Opcode-specific second value.  POLL reports how many microseconds it
     * waited -- "already set" and "settled after 800 us" are different
     * findings and must not look alike. */
    ULONG Extra;
} AR9485LAB_RESULT, *PAR9485LAB_RESULT;

typedef struct _AR9485LAB_RESULT_HEADER
{
    ULONG AbiVersion;
    ULONG RecordCount;
    ULONG DataOffset;
    ULONG DataLength;
    ULONG Executed;         /* ops actually executed */
    ULONG FailedIndex;      /* first failing op, or 0xFFFFFFFF */
    ULONG FailedStatus;
    ULONG WaitedMicroseconds;
    /* AR9485LAB_RESULT Records[RecordCount]; then the data blob. */
} AR9485LAB_RESULT_HEADER, *PAR9485LAB_RESULT_HEADER;

/* Per-op status.  Distinct from NTSTATUS so a script author can tell a
 * rejected program from a hardware timeout at a glance. */
#define AR9485LAB_ST_OK                 0
#define AR9485LAB_ST_BAD_OPCODE         1
#define AR9485LAB_ST_BAD_OFFSET         2   /* outside BAR, or unaligned */
#define AR9485LAB_ST_BAD_LENGTH         3
#define AR9485LAB_ST_BAD_INDEX          4   /* no such DMA buffer */
#define AR9485LAB_ST_TIMEOUT            5   /* POLL gave up */
#define AR9485LAB_ST_NO_RESOURCES       6
#define AR9485LAB_ST_LIMIT              7   /* exceeded a cap above */
#define AR9485LAB_ST_NOT_CLAIMED        8
#define AR9485LAB_ST_RESULT_FULL        9   /* output buffer too small */
#define AR9485LAB_ST_UNSUPPORTED        10

/* ===========================================================================
 *  DMA_LIST record
 * ===========================================================================
 */
typedef struct _AR9485LAB_DMA_ENTRY
{
    ULONG Index;
    ULONG Length;
    ULONG PhysicalLow;
    ULONG PhysicalHigh;
    ULONG Flags;            /* AR9485LAB_DMA_* */
    ULONG Reserved;
} AR9485LAB_DMA_ENTRY, *PAR9485LAB_DMA_ENTRY;

#define AR9485LAB_DMA_FIXED             0x00000001  /* miniport RX pool */
#define AR9485LAB_DMA_RUNTIME           0x00000002  /* DMA_ALLOC'd      */

/* ===========================================================================
 *  IOCTL_AR9485LAB_INFO / CLAIM output
 * ===========================================================================
 */
typedef struct _AR9485LAB_INFO
{
    ULONG AbiVersion;
    ULONG Flags;            /* AR9485LAB_INFO_* */
    ULONG BarPhysicalLow;
    ULONG BarPhysicalHigh;
    ULONG BarLength;
    ULONG DmaFixedCount;
    ULONG DmaFixedSize;
    ULONG DmaRuntimeCount;
    ULONG SregRaw;          /* AR_SREV as read at init; the self-test oracle */
    ULONG MacVersion;
    ULONG MacRevision;
    ULONG CurrentChannelMHz;
    UCHAR PermanentMacAddress[6];
    USHORT DeviceId;
} AR9485LAB_INFO, *PAR9485LAB_INFO;

#define AR9485LAB_INFO_CLAIMED          0x00000001
#define AR9485LAB_INFO_CLAIMED_BY_ME    0x00000002
#define AR9485LAB_INFO_PHY_UP           0x00000004
#define AR9485LAB_INFO_SCRATCH_TARGET   0x00000008  /* Stage 0 self-test mode */

#endif /* _AR9485_LAB_H_ */
