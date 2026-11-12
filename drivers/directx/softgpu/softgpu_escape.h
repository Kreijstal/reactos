/*
 * PROJECT:     ReactOS WDDM Null/Software GPU Miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     softgpu driver-private escapes (D3DKMTEscape DRIVERPRIVATE),
 *              shared with modules/rostests/apitests/d3dkmt
 * COPYRIGHT:   Copyright 2026 ReactOS WDDM Team
 */

#pragma once

/*
 * SOFTGPU_ESCAPE_TIMEDOP runs one scenario against dxgkrnl's
 * DxgkServicesTimedOperation interface from inside the miniport, the way a
 * real driver (atikmdag) uses it, and returns what the interface did.
 */
#define SOFTGPU_ESCAPE_TIMEDOP              0x4F544753UL    /* 'SGTO' */

#define SOFTGPU_TIMEDOP_ZERO_TIMEOUT        1   /* Start(0); Delay(NULL)          */
#define SOFTGPU_TIMEDOP_POLL                2   /* Start(2 s); Delay(NULL)        */
#define SOFTGPU_TIMEDOP_POSITIVE_INTERVAL   3   /* Start(2 s); Delay(+20 ms)      */
#define SOFTGPU_TIMEDOP_CLAMP               4   /* Start(10 s); Wait(NULL)        */
#define SOFTGPU_TIMEDOP_DEADLINE            5   /* Start(100 ms); Delay(NULL)...  */
#define SOFTGPU_TIMEDOP_BAD_SIZE            6   /* Start with Op.Size = 0         */
#define SOFTGPU_TIMEDOP_NEGATIVE_TIMEOUT    7   /* Start(-2 s); Delay(NULL)       */

typedef struct _SOFTGPU_ESCAPE_TIMEDOP_DATA
{
    unsigned long       Code;           /* in: SOFTGPU_ESCAPE_TIMEDOP           */
    unsigned long       Scenario;       /* in: SOFTGPU_TIMEDOP_*                */
    long                QueryStatus;    /* out: DxgkCbQueryServices             */
    long                StartStatus;    /* out: TimedOperationStart             */
    long                OpStatus;       /* out: last Delay/Wait status          */
    unsigned long       OpCount;        /* out: Delay/Wait calls made           */
    long long           Elapsed100ns;   /* out: Start -> last call return,
                                           interrupt time (clock-tick steps)    */
    unsigned char       TimeoutTriggered; /* out: Op->TimeoutTriggered          */
    unsigned char       Reserved[7];
    long long           FineElapsed100ns; /* out: the same, performance counter */
} SOFTGPU_ESCAPE_TIMEDOP_DATA;

/*
 * SOFTGPU_ESCAPE_SEGMENT returns the segment softgpu reports through
 * DXGKQAITYPE_QUERYSEGMENT/QUERYSEGMENT3, so a test can check what dxgkrnl
 * made of it (D3DKMTQueryAdapterInfo KMTQAITYPE_GETSEGMENTSIZE).
 */
#define SOFTGPU_ESCAPE_SEGMENT              0x47534753UL    /* 'SGSG' */

typedef struct _SOFTGPU_ESCAPE_SEGMENT_DATA
{
    unsigned long       Code;           /* in: SOFTGPU_ESCAPE_SEGMENT           */
    unsigned long       Flags;          /* out: DXGK_SEGMENTFLAGS.Value         */
    unsigned long long  BaseAddress;    /* out                                  */
    unsigned long long  CpuTranslatedAddress; /* out                            */
    unsigned long long  Size;           /* out                                  */
    unsigned long       HasVram;        /* out                                  */
    unsigned long       Reserved;
} SOFTGPU_ESCAPE_SEGMENT_DATA;

/*
 * SOFTGPU_ESCAPE_PRESENT_STATS returns what DxgkDdiPresent(Blt) has been
 * asked to copy: how many Blts, how many covered the whole destination,
 * and the last one's rectangles' bounding box.
 */
#define SOFTGPU_ESCAPE_PRESENT_STATS        0x53504753UL    /* 'SGPS' */

typedef struct _SOFTGPU_ESCAPE_PRESENT_STATS_DATA
{
    unsigned long       Code;           /* in: SOFTGPU_ESCAPE_PRESENT_STATS     */
    unsigned long       BltCount;       /* out                                  */
    unsigned long       FullBltCount;   /* out: Blts of the whole destination   */
    long                LastLeft, LastTop, LastRight, LastBottom; /* out        */
    unsigned long       DstWidth, DstHeight; /* out: last destination size      */
} SOFTGPU_ESCAPE_PRESENT_STATS_DATA;
