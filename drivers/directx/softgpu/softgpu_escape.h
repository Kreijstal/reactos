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

#define SOFTGPU_ESCAPE_DISPLAY_STATE        0x53444753UL    /* 'SGDS' */

typedef struct _SOFTGPU_ESCAPE_DISPLAY_STATE_DATA
{
    unsigned long       Code;               /* in: SOFTGPU_ESCAPE_DISPLAY_STATE  */
    unsigned long       LateConnectMs;      /* out: test mode, 0 = off           */
    unsigned long       MonitorConnected;   /* out                               */
    unsigned long       IndicateCount;      /* out: DxgkCbIndicateChildStatus calls */
    long                IndicateStatus;     /* out: its last status              */
    unsigned long       CommitCount;        /* out: DxgkDdiCommitVidPn calls     */
    unsigned long       CommitPathCount;    /* out: paths of the last commit     */
    unsigned long       CommitSourceWidth;  /* out: its pinned source mode       */
    unsigned long       CommitSourceHeight;
    unsigned long       ChildRelationsCount; /* out: DxgkDdiQueryChildRelations calls */
} SOFTGPU_ESCAPE_DISPLAY_STATE_DATA;

/*
 * SOFTGPU_ESCAPE_LID notifies \Callback\PowerState as the power manager
 * does when the lid opens or closes.  SOFTGPU_ESCAPE_LID_STATE returns, in
 * softgpu's lid test mode (service value LidTest), the order (sequence
 * numbers, 0 = never) in which the lid notifications
 * (DxgkDdiNotifyAcpiEvent) and the calls that hide or show the source
 * (DxgkDdiSetVidPnSourceVisibility) and power the panel
 * (DxgkDdiSetPowerState on the child) came.
 */
#define SOFTGPU_ESCAPE_LID                  0x4C4C4753UL    /* 'SGLL' */
#define SOFTGPU_ESCAPE_LID_STATE            0x534C4753UL    /* 'SGLS' */

typedef struct _SOFTGPU_ESCAPE_LID_DATA
{
    unsigned long       Code;           /* in: SOFTGPU_ESCAPE_LID               */
    unsigned long       Open;           /* in: 0 closed, 1 open                 */
} SOFTGPU_ESCAPE_LID_DATA;

typedef struct _SOFTGPU_ESCAPE_LID_STATE_DATA
{
    unsigned long       Code;           /* in: SOFTGPU_ESCAPE_LID_STATE         */
    unsigned long       LidTest;        /* out: test mode, 0 = off              */
    unsigned long       LidEventCount;  /* out: DxgkDdiNotifyAcpiEvent calls    */
    unsigned long       LidEventType;   /* out: the last one's EventType        */
    unsigned long       LidEvent;       /* out: its Event                       */
    unsigned long       LidArgument;    /* out: its Argument                    */
    long                LidSyncStatus;  /* out: DxgkCbSynchronizeExecution at
                                           DISPATCH_LEVEL on the last close     */
    long                LidCloseSeq;    /* out: last lid closed notification    */
    long                LidOpenSeq;     /* out: last lid open notification      */
    long                HideSeq;        /* out: last Visible = FALSE            */
    long                ShowSeq;        /* out: last Visible = TRUE             */
    long                PanelD3Seq;     /* out: last panel D3                   */
    long                PanelD0Seq;     /* out: last panel D0                   */
    unsigned long       PanelPowerState; /* out: DEVICE_POWER_STATE             */
    unsigned long       PanelBlanked;   /* out: blanked since the lid closed    */
} SOFTGPU_ESCAPE_LID_STATE_DATA;
