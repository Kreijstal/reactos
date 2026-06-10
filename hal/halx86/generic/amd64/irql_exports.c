/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     amd64 exported IRQL helpers
 */

#define KeGetCurrentIrql HalpInlineKeGetCurrentIrql
#define KeLowerIrql HalpInlineKeLowerIrql
#include <hal.h>
#undef KeGetCurrentIrql
#undef KeLowerIrql

KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    return (KIRQL)__readcr8();
}

VOID
NTAPI
KeLowerIrql(KIRQL NewIrql)
{
    __writecr8(NewIrql);
}
