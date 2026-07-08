/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-environment shim for the AtomBIOS interpreter imported
 *              from the Linux radeon driver (drivers/gpu/drm/radeon/, v6.6).
 *              Maps the small set of Linux kernel services the interpreter
 *              uses onto videoprt/ntoskrnl equivalents so that atom.c can be
 *              carried near-verbatim.
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#ifndef _RADEON_ATOM_OS_H_
#define _RADEON_ATOM_OS_H_

#include <ntddk.h>
#include <dderror.h>
#define __BROKEN__
#include <miniport.h>
#undef __BROKEN__
#include <video.h>
#include <devioctl.h>

/* Fixed-width aliases used by the imported Linux sources.  The ReactOS DDK
 * headers already provide UCHAR/USHORT/ULONG with the layout AtomBIOS
 * expects (ULONG is 32-bit on both i386 and amd64). */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#ifndef __cplusplus
typedef UCHAR atom_bool_t;
#define bool atom_bool_t
#define true TRUE
#define false FALSE
#endif

/* Linux errno values kept by the imported code (only used as failure
 * indications; callers just test for non-zero). */
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

/* Little-endian only targets (i386/amd64); byte-swaps are identity. */
#define cpu_to_le32(x) (x)
#define cpu_to_le16(x) (x)
#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)

/* x86 tolerates unaligned loads; keep the accessor for documentation. */
static __inline uint32_t
get_unaligned_le32(const void *p)
{
    return *(const uint32_t UNALIGNED *)p;
}

/* Pool helpers.  The interpreter only allocates from PASSIVE_LEVEL paths
 * (adapter probe and mode-set requests), NonPagedPool keeps it callable
 * from any videoprt context. */
#define ATOM_POOL_TAG 'BotA'

static __inline PVOID
atom_zalloc(SIZE_T Size)
{
    PVOID Ptr = ExAllocatePoolWithTag(NonPagedPool, Size, ATOM_POOL_TAG);
    if (Ptr != NULL)
        RtlZeroMemory(Ptr, Size);
    return Ptr;
}

static __inline VOID
atom_free(PVOID Ptr)
{
    if (Ptr != NULL)
        ExFreePoolWithTag(Ptr, ATOM_POOL_TAG);
}

#define kzalloc(size, flags)     atom_zalloc(size)
#define kcalloc(n, size, flags)  atom_zalloc((SIZE_T)(n) * (size))
#define kmalloc(size, flags)     ExAllocatePoolWithTag(NonPagedPool, (size), ATOM_POOL_TAG)
#define kfree(p)                 atom_free(p)
#define GFP_KERNEL 0

/* Debug output.  KERN_* markers collapse to empty string literals so the
 * printk(KERN_DEBUG "...") concatenation in the imported code keeps
 * compiling. */
#define KERN_DEBUG ""
#define KERN_INFO ""
#define printk(...)     VideoPortDebugPrint(Info, __VA_ARGS__)
#define pr_info(...)    VideoPortDebugPrint(Info, "radeonfb: " __VA_ARGS__)
#define DRM_ERROR(...)  VideoPortDebugPrint(Error, "radeonfb: " __VA_ARGS__)
#define DRM_INFO(...)   VideoPortDebugPrint(Info, "radeonfb: " __VA_ARGS__)
#define DRM_DEBUG(...)  VideoPortDebugPrint(Trace, "radeonfb: " __VA_ARGS__)
#define DRM_DEBUG_KMS(...) VideoPortDebugPrint(Trace, "radeonfb: " __VA_ARGS__)

/* Delays: VideoPortStallExecution busy-waits in microseconds. */
#define udelay(us) VideoPortStallExecution(us)

static __inline VOID
atom_mdelay(ULONG Milliseconds)
{
    ULONG i;
    for (i = 0; i < Milliseconds; i++)
        VideoPortStallExecution(1000);
}

#define mdelay(ms) atom_mdelay(ms)
#define msleep(ms) atom_mdelay(ms)

/* 100ns-resolution wall clock used by the interpreter's runaway-loop guard
 * (replaces jiffies). */
static __inline ULONGLONG
atom_time_100ns(VOID)
{
    LARGE_INTEGER Time;
    VideoPortQuerySystemTime(&Time);
    return (ULONGLONG)Time.QuadPart;
}

#endif /* _RADEON_ATOM_OS_H_ */
