/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: .lab script parser and program encoder.
 *
 * Shared by ar9485lab.exe (which submits the program to the driver) and by the
 * host harness (which runs it against a simulated target).  Both MUST encode
 * identically -- a harness that agreed with the tool only by coincidence would
 * prove nothing about what runs on the machine.
 */

#ifndef _AR9485_LAB_SCRIPT_H_
#define _AR9485_LAB_SCRIPT_H_

#ifdef AR9485_LAB_HOST
#include "lab_vm.h"
#else
#include <windows.h>
#include <winioctl.h>
#include <reactos/ar9485_lab.h>
#endif

#include <stdio.h>

#define LAB_SCRIPT_MAX_DATA (64 * 1024)

typedef struct _LAB_SCRIPT
{
    AR9485LAB_OP *Ops;
    ULONG OpCapacity;
    ULONG OpCount;
    unsigned char Data[LAB_SCRIPT_MAX_DATA];
    ULONG DataUsed;
    ULONG Flags;
} LAB_SCRIPT;

/* Parse one stream into Script.  Returns 1 on success, 0 on a syntax error
 * (already reported to stderr). */
int LabScriptParse(FILE *Stream, LAB_SCRIPT *Script);

/* Encoded size of the program Script describes. */
ULONG LabScriptEncodedSize(const LAB_SCRIPT *Script);

/* Serialise into Buffer; returns the number of bytes written, or 0 if the
 * buffer is too small. */
ULONG LabScriptEncode(const LAB_SCRIPT *Script, unsigned char *Buffer,
                      ULONG BufferLength);

const char *LabScriptOpName(ULONG Op);
const char *LabScriptStatusName(ULONG Status);

#endif /* _AR9485_LAB_SCRIPT_H_ */
