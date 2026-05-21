/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Linux-kernel-primitive shim for the verbatim ath9k port.
 *              Maps Linux types/macros/IO/sleep helpers onto Windows /
 *              NDIS equivalents so the Linux ath9k_hw layer compiles with
 *              only #include-line edits (per the AR9485 port plan,
 *              "thin compat shim + verbatim Linux source" strategy).
 */

#ifndef _AR9485_LINUX_COMPAT_H_
#define _AR9485_LINUX_COMPAT_H_

#include <ntddk.h>

/* ---------- Linux fixed-width integer types ------------------------- */

typedef UINT8   u8;
typedef UINT16  u16;
typedef UINT32  u32;
typedef UINT64  u64;
typedef INT8    s8;
typedef INT16   s16;
typedef INT32   s32;
typedef INT64   s64;
typedef UINT16  __le16;
typedef UINT32  __le32;
typedef UINT64  __le64;
typedef UINT16  __be16;
typedef UINT32  __be32;

#ifndef __cplusplus
typedef int     bool;
#define true    1
#define false   0
#endif

/* ---------- Bit / mask / size macros -------------------------------- */

#define BIT(n)           (1U << (n))
#define BIT_ULL(n)       (1ULL << (n))

/* GENMASK(h, l) builds a contiguous bitmask covering bits l..h inclusive.
 * Linux's full implementation tolerates h<l, type-promotion etc.; for
 * verbatim ath9k we only need the common case which the simpler form
 * below handles correctly for h<32. */
#define GENMASK(h, l)    (((~0U) << (l)) & (~0U >> (31 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))

#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))

#ifndef min
#define min(a, b)        ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b)        ((a) > (b) ? (a) : (b))
#endif
#define min_t(t, a, b)   (((t)(a) < (t)(b)) ? (t)(a) : (t)(b))
#define max_t(t, a, b)   (((t)(a) > (t)(b)) ? (t)(a) : (t)(b))
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)

#define upper_32_bits(v) ((u32)(((u64)(v) >> 32) & 0xffffffffu))
#define lower_32_bits(v) ((u32)((u64)(v) & 0xffffffffu))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - FIELD_OFFSET(type, member)))

/* ---------- Linux error codes --------------------------------------
 *
 * Intentionally NOT redefined here.  ROS sdk/include/crt/errno.h (pulled
 * in transitively via ndis.h → ntddk.h → wdm.h → stddef.h → errno.h)
 * already provides EPERM/ENOMEM/EINVAL/ETIMEDOUT/... as positive ints.
 * ath9k uses them with a leading minus (`return -ENOMEM`) which works
 * unchanged.  The actual values differ from Linux (e.g. ETIMEDOUT==138
 * on ROS vs 110 on Linux) but ath9k only ever compares against `< 0`
 * or `IS_ERR`, not against literal numeric values.
 */

/* ---------- Memory ------------------------------------------------- */

#define AR9485_COMPAT_TAG 'C584'

static __inline void *
kzalloc(SIZE_T size, ULONG flags)
{
    PVOID p = ExAllocatePoolWithTag(NonPagedPool, size, AR9485_COMPAT_TAG);
    UNREFERENCED_PARAMETER(flags);
    if (p) RtlZeroMemory(p, size);
    return p;
}

static __inline void *
kmalloc(SIZE_T size, ULONG flags)
{
    UNREFERENCED_PARAMETER(flags);
    return ExAllocatePoolWithTag(NonPagedPool, size, AR9485_COMPAT_TAG);
}

static __inline void
kfree(void *p)
{
    if (p) ExFreePoolWithTag(p, AR9485_COMPAT_TAG);
}

#define GFP_KERNEL  0
#define GFP_ATOMIC  0

/* ---------- Register IO (mapped BAR) ------------------------------- */

static __inline u32
ioread32(volatile void *addr)
{
    return READ_REGISTER_ULONG((PULONG)addr);
}

static __inline void
iowrite32(u32 value, volatile void *addr)
{
    WRITE_REGISTER_ULONG((PULONG)addr, value);
}

/* ---------- Delays / sleeps ---------------------------------------- */

static __inline void
udelay(ULONG us)
{
    KeStallExecutionProcessor(us);
}

static __inline void
mdelay(ULONG ms)
{
    /* Stall millisecond-scale at DISPATCH+; ath9k calls this only from
     * init paths where IRQL is PASSIVE_LEVEL.  10us at a time keeps the
     * CPU honest about spin-time. */
    ULONG i;
    for (i = 0; i < ms * 100; i++) KeStallExecutionProcessor(10);
}

static __inline void
msleep(ULONG ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -((LONGLONG)ms * 10000);  /* relative 100ns ticks */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

#define usleep_range(min_us, max_us)  udelay(min_us)

/* ---------- Spin / mutex primitives -------------------------------- */

/* ath9k_hw uses spinlocks only as opaque tokens around HW register
 * banging.  The miniport already serializes register access via NDIS
 * spinlocks at the upper layer, so the inner spinlocks below are no-ops
 * until contention shows up in profiling. */
typedef struct { int _unused; } spinlock_t;
#define DEFINE_SPINLOCK(x)        spinlock_t x = { 0 }
#define spin_lock_init(x)         ((void)(x))
#define spin_lock(x)              ((void)(x))
#define spin_unlock(x)            ((void)(x))
#define spin_lock_bh(x)           ((void)(x))
#define spin_unlock_bh(x)         ((void)(x))
#define spin_lock_irqsave(x, f)   do { (f) = 0; (void)(x); } while (0)
#define spin_unlock_irqrestore(x, f) do { (void)(f); (void)(x); } while (0)

/* ---------- Misc --------------------------------------------------- */

#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(t, n)

#define __init
#define __exit
#define __read_mostly
#define __force
#define __must_check
#define __maybe_unused
#define __always_inline  FORCEINLINE
#define inline           __inline
#define noinline
#define likely(x)        (x)
#define unlikely(x)      (x)

#endif /* _AR9485_LINUX_COMPAT_H_ */
