/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: script runner.
 *
 * Reads a text script, encodes it as ONE batched program, submits it to
 * \\.\AR9485Lab, and prints the results.  The point of the exercise is that
 * an AR9485 experiment costs a push-and-run rather than a deploy and a
 * reboot, so this tool holds no bring-up knowledge of its own: the register
 * sequences live in the scripts.
 *
 * The claim is taken for the lifetime of this process and dropped when the
 * handle closes -- including on a crash, via IRP_MJ_CLEANUP.  There is
 * deliberately no way to leave the chip claimed after the tool exits.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

#include <reactos/ar9485_lab.h>
#include "lab_script.h"

#define MAX_SCRIPT_OPS  AR9485LAB_MAX_OPS

static AR9485LAB_OP g_OpStorage[MAX_SCRIPT_OPS];
static LAB_SCRIPT g_Script;
static int g_Failed = 0;

/* Where results go.  luagent's guest side has been observed reporting process
 * exit before it has drained the child's stdout, which silently truncates a
 * result stream (7 of 14 records on one run, 14 on the next two).  With -o the
 * host fetches a FILE instead, which cannot lose its tail. */
static FILE *g_Out;

static void
Emit(const char *Format, ...)
{
    va_list Arguments;

    va_start(Arguments, Format);
    vfprintf(g_Out, Format, Arguments);
    va_end(Arguments);
}

static int
ParseStream(FILE *Stream)
{
    return LabScriptParse(Stream, &g_Script);
}

/* ===========================================================================
 *  Result printing
 * ===========================================================================
 */

static void
PrintHex(const unsigned char *Data, ULONG Length, ULONG BaseOffset)
{
    ULONG i;

    for (i = 0; i < Length; i += 16)
    {
        ULONG j;
        ULONG Run = (Length - i < 16) ? Length - i : 16;

        Emit("  %04x:", (unsigned)(BaseOffset + i));
        for (j = 0; j < Run; ++j)
            Emit(" %02x", Data[i + j]);
        Emit("\n");
    }
}

static void
PrintResults(const unsigned char *Out, ULONG Length)
{
    const AR9485LAB_RESULT_HEADER *Header;
    const AR9485LAB_RESULT *Records;
    ULONG i;

    if (Length < sizeof(AR9485LAB_RESULT_HEADER))
    {
        fprintf(stderr, "ar9485lab: short result (%lu bytes)\n", (unsigned long)Length);
        g_Failed = 1;
        return;
    }

    Header = (const AR9485LAB_RESULT_HEADER *)Out;
    Records = (const AR9485LAB_RESULT *)(Out + sizeof(AR9485LAB_RESULT_HEADER));

    Emit("RUN executed=%lu records=%lu waited=%luus\n",
           (unsigned long)Header->Executed,
           (unsigned long)Header->RecordCount,
           (unsigned long)Header->WaitedMicroseconds);

    for (i = 0; i < Header->RecordCount; ++i)
    {
        const AR9485LAB_RESULT *Record = &Records[i];

        Emit("[%lu] %s value=0x%08lx",
               (unsigned long)Record->Index,
               LabScriptOpName(Record->Op),
               (unsigned long)Record->Value);

        if (Record->Op == AR9485LAB_OP_POLL)
            Emit(" elapsed=%luus", (unsigned long)Record->Extra);
        if (Record->Status != AR9485LAB_ST_OK)
            Emit(" status=%s", LabScriptStatusName(Record->Status));
        Emit("\n");

        if (Record->DataLength != 0 &&
            Record->DataOffset + Record->DataLength <= Length)
        {
            /* DMA_LIST is the one record whose payload is worth decoding
             * here: a script author needs the bus addresses to build a
             * descriptor, and hex would just be transcription work. */
            if (Record->Op == AR9485LAB_OP_DMA_LIST)
            {
                ULONG Count = Record->DataLength / sizeof(AR9485LAB_DMA_ENTRY);
                ULONG e;
                const AR9485LAB_DMA_ENTRY *Entries =
                    (const AR9485LAB_DMA_ENTRY *)(Out + Record->DataOffset);

                for (e = 0; e < Count; ++e)
                {
                    Emit("  dma[%lu] pa=0x%08lx%08lx len=%lu %s\n",
                           (unsigned long)Entries[e].Index,
                           (unsigned long)Entries[e].PhysicalHigh,
                           (unsigned long)Entries[e].PhysicalLow,
                           (unsigned long)Entries[e].Length,
                           (Entries[e].Flags & AR9485LAB_DMA_FIXED) ? "fixed"
                                                                    : "runtime");
                }
            }
            else
            {
                PrintHex(Out + Record->DataOffset, Record->DataLength,
                         (Record->Op == AR9485LAB_OP_DUMP) ? Record->Value : 0);
            }
        }
    }

    if (Header->FailedIndex != 0xFFFFFFFF)
    {
        Emit("FAIL index=%lu status=%s\n",
               (unsigned long)Header->FailedIndex,
               LabScriptStatusName(Header->FailedStatus));
        g_Failed = 1;
    }
    else
    {
        Emit("DONE\n");
    }
}

static void
PrintInfo(const AR9485LAB_INFO *Info)
{
    Emit("INFO abi=%lu bar=0x%08lx%08lx len=%lu srev=0x%08lx "
           "mac=%lu.%lu dma=%lux%lu runtime=%lu chan=%lu%s%s%s\n",
           (unsigned long)Info->AbiVersion,
           (unsigned long)Info->BarPhysicalHigh,
           (unsigned long)Info->BarPhysicalLow,
           (unsigned long)Info->BarLength,
           (unsigned long)Info->SregRaw,
           (unsigned long)Info->MacVersion,
           (unsigned long)Info->MacRevision,
           (unsigned long)Info->DmaFixedCount,
           (unsigned long)Info->DmaFixedSize,
           (unsigned long)Info->DmaRuntimeCount,
           (unsigned long)Info->CurrentChannelMHz,
           (Info->Flags & AR9485LAB_INFO_SCRATCH_TARGET) ? " SCRATCH" : "",
           (Info->Flags & AR9485LAB_INFO_PHY_UP) ? " PHYUP" : "",
           (Info->Flags & AR9485LAB_INFO_CLAIMED) ? " CLAIMED" : "");

    if (!(Info->Flags & AR9485LAB_INFO_SCRATCH_TARGET))
    {
        Emit("INFO permanent-mac=%02x:%02x:%02x:%02x:%02x:%02x devid=0x%04x\n",
               Info->PermanentMacAddress[0], Info->PermanentMacAddress[1],
               Info->PermanentMacAddress[2], Info->PermanentMacAddress[3],
               Info->PermanentMacAddress[4], Info->PermanentMacAddress[5],
               Info->DeviceId);
    }
}

/* ===========================================================================
 *  -s / -l: read-only views of the running driver, no claim
 *
 *  A claim stands the chip worker down and the release resets the chip, so a
 *  script run ends the very association a post-mortem wants to look at.
 *  These two only copy driver memory and can be polled while the link is up.
 * ===========================================================================
 */

static HANDLE
OpenLab(void)
{
    HANDLE Device = CreateFileW(AR9485LAB_WIN32_NAME,
                                GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        fprintf(stderr, "ar9485lab: cannot open %ls (error %lu)\n",
                AR9485LAB_WIN32_NAME, GetLastError());
    return Device;
}

/* Milliseconds from an interrupt-time stamp to Now; -1 for "never". */
static long long
AgoMs(ULONGLONG Now, ULONGLONG Then)
{
    if (Then == 0)
        return -1;
    return (long long)((Now - Then) / 10000ULL);
}

static int
PrintLinkStatus(void)
{
    static const char *const LostName[] = { "none", "probe", "beacon", "tx-stall" };
    AR9485LAB_LINK_STATUS S;
    DWORD Returned = 0;
    HANDLE Device = OpenLab();

    if (Device == INVALID_HANDLE_VALUE)
        return 1;
    memset(&S, 0, sizeof(S));
    if (!DeviceIoControl(Device, IOCTL_AR9485LAB_LINK_STATUS, NULL, 0,
                         &S, sizeof(S), &Returned, NULL) ||
        Returned < sizeof(S) || S.Version != AR9485LAB_LINK_STATUS_VERSION)
    {
        fprintf(stderr, "ar9485lab: LINK_STATUS failed (error %lu, %lu bytes, "
                "version %lu)\n", GetLastError(), Returned, S.Version);
        CloseHandle(Device);
        return 1;
    }
    CloseHandle(Device);

    Emit("LINK now=%I64u mlme=%lu ch=%lu bssid=%02x:%02x:%02x:%02x:%02x:%02x "
         "aid=%u up_s=%I64d\n",
         S.Now, S.MlmeState, S.ChannelMHz,
         S.Bssid[0], S.Bssid[1], S.Bssid[2], S.Bssid[3], S.Bssid[4], S.Bssid[5],
         S.AssociationId,
         (S.MlmeState == 4) ? AgoMs(S.Now, S.LinkStart) / 1000 : -1LL);
    Emit("LINKMON links=%lu lost=%lu last_lost=%s lost_ago_ms=%I64d peer_disc=%lu "
         "deauth_reason=%lu\n",
         S.Links, S.LinksLost,
         (S.LastLostReason < 4) ? LostName[S.LastLostReason] : "?",
         AgoMs(S.Now, S.LastLostTime), S.PeerDisconnects, S.DeauthReason);
    Emit("LINKMON probes=%lu acked=%lu failed=%lu timedout=%lu idle=%lu "
         "beacon_loss=%lu beacon_ago_ms=%I64d ack_ago_ms=%I64d probe_ago_ms=%I64d\n",
         S.ProbesSent, S.ProbesAcked, S.ProbesFailed, S.ProbesTimedOut,
         S.IdleProbes, S.BeaconLossEvents, AgoMs(S.Now, S.LastBeacon),
         AgoMs(S.Now, S.LastAck), AgoMs(S.Now, S.LastProbe));
    Emit("TX ok=%I64u fail=%I64u pending=%lu\n",
         S.TxFrameCount, S.TxFailureCount, S.TxPending);
    Emit("RX frames=%I64u uc=%I64u mc=%I64u bc=%I64u err=%I64u crypto=%I64u "
         "michael_drop=%lu grp_mic_ok=%lu\n",
         S.RxFrameCount, S.RxUnicastCount, S.RxMulticastCount,
         S.RxBroadcastCount, S.RxErrorCount, S.RxCryptoErrorCount,
         S.RxDropMichael, S.RxGroupMicOkCount);
    Emit("GTK installs=%lu slots=0x%lx last_id=%lu rx_unknown_id=%lu\n",
         S.GroupKeyInstalls, S.GroupKeyMask, S.LastGroupKeyId,
         S.RxGroupUnknownKeyId);
    return 0;
}

static int
PrintLog(void)
{
    static char Ring[16384 + 1];
    DWORD Returned = 0;
    HANDLE Device = OpenLab();

    if (Device == INVALID_HANDLE_VALUE)
        return 1;
    if (!DeviceIoControl(Device, IOCTL_AR9485LAB_READ_LOG, NULL, 0,
                         Ring, sizeof(Ring) - 1, &Returned, NULL))
    {
        fprintf(stderr, "ar9485lab: READ_LOG failed (error %lu)\n",
                GetLastError());
        CloseHandle(Device);
        return 1;
    }
    CloseHandle(Device);
    fwrite(Ring, 1, Returned, g_Out);
    return 0;
}

/* ===========================================================================
 *  main
 * ===========================================================================
 */

static void
Usage(void)
{
    printf("usage: ar9485lab [-o file] [-i | -s | -l | script ...]\n"
           "  -i          print device info and exit\n"
           "  -s          print link/keep-alive/key counters (no claim)\n"
           "  -l          dump the driver log ring (no claim)\n"
           "  -o file     write results to a file instead of stdout\n"
           "  script      script file, or - for stdin\n"
           "\n"
           "script statements:\n"
           "  r <off>                       read a register\n"
           "  w <off> <value>               write a register\n"
           "  rmw <off> <and> <or>          read-modify-write\n"
           "  poll <off> <mask> <val> <us>  wait for (reg & mask) == val\n"
           "  delay <us>                    wait\n"
           "  dump <off> <dwords>           dump a register range\n"
           "  mark <tag>                    label a point in the results\n"
           "  continue                      keep going past a failing op\n"
           "  barrier                       memory barrier\n"
           "  pciread <off> <width>         read PCI config\n"
           "  pciwrite <off> <val> <width>  write PCI config\n"
           "  dmalist                       list DMA buffers and bus addresses\n"
           "  dmaalloc <count> <size>       allocate runtime DMA buffers\n"
           "  dmafree                       free every runtime buffer\n"
           "  dmaread <idx> <off> <len>     read a DMA buffer\n"
           "  dmawrite <idx> <off> <hex>    write bytes into a DMA buffer\n"
           "  dmazero <idx> <off> <len>     zero part of a DMA buffer\n"
           "  wdmapa <off> <idx> <boff>     write buffer's bus address to a register\n"
           "  dmapokepa <dst> <doff> <src> <soff>\n"
           "                                write src's bus address into dst\n"
           "  callhw start|queuerx|harvest [arg]\n"
           "\n"
           "  -p <pa> <bytes>               dump physical memory (no driver involved)\n");
}

/* ---------------------------------------------------------------------------
 * -p <hex-pa> <bytes>: dump physical memory.
 *
 * The lab's DMA table carries only the RX buffer pool, so the driver's own
 * transmit-status ring is invisible to `dmaread`.  Rather than spend a reboot
 * teaching the driver to register it, map \Device\PhysicalMemory from here:
 * this tool is pushed fresh over luagent on every run, so adding to it costs
 * nothing.  Prototypes are declared locally and the entry points resolved at
 * run time, so the import list and CMakeLists stay untouched.
 * ------------------------------------------------------------------------ */
typedef LONG NTSTATUS_L;
typedef struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } UNICODE_STRING_L;
typedef struct {
    ULONG Length; HANDLE RootDirectory; UNICODE_STRING_L *ObjectName;
    ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES_L;

typedef NTSTATUS_L (__stdcall *PFN_OPEN_SECTION)(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES_L *);
typedef NTSTATUS_L (__stdcall *PFN_MAP_VIEW)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T,
                                             LARGE_INTEGER *, SIZE_T *, ULONG, ULONG, ULONG);
typedef NTSTATUS_L (__stdcall *PFN_UNMAP_VIEW)(HANDLE, PVOID);

static int
DumpPhysical(unsigned __int64 Pa, ULONG Bytes)
{
    HMODULE Ntdll = GetModuleHandleA("ntdll.dll");
    PFN_OPEN_SECTION NtOpenSectionF;
    PFN_MAP_VIEW NtMapViewF;
    PFN_UNMAP_VIEW NtUnmapViewF;
    UNICODE_STRING_L Name;
    OBJECT_ATTRIBUTES_L Attr;
    static WCHAR Path[] = L"\\Device\\PhysicalMemory";
    HANDLE Section = NULL;
    LARGE_INTEGER Offset;
    SIZE_T ViewSize;
    PVOID Base = NULL;
    unsigned __int64 PageBase = Pa & ~(unsigned __int64)0xFFF;
    ULONG Skew = (ULONG)(Pa - PageBase);
    NTSTATUS_L Status;
    ULONG i;

    if (Ntdll == NULL)
    {
        fprintf(stderr, "ar9485lab: no ntdll\n");
        return 1;
    }
    NtOpenSectionF = (PFN_OPEN_SECTION)GetProcAddress(Ntdll, "NtOpenSection");
    NtMapViewF = (PFN_MAP_VIEW)GetProcAddress(Ntdll, "NtMapViewOfSection");
    NtUnmapViewF = (PFN_UNMAP_VIEW)GetProcAddress(Ntdll, "NtUnmapViewOfSection");
    if (NtOpenSectionF == NULL || NtMapViewF == NULL || NtUnmapViewF == NULL)
    {
        fprintf(stderr, "ar9485lab: ntdll is missing the section entry points\n");
        return 1;
    }

    Name.Buffer = Path;
    Name.Length = (USHORT)(wcslen(Path) * sizeof(WCHAR));
    Name.MaximumLength = (USHORT)(Name.Length + sizeof(WCHAR));

    memset(&Attr, 0, sizeof(Attr));
    Attr.Length = sizeof(Attr);
    Attr.ObjectName = &Name;
    Attr.Attributes = 0x00000040; /* OBJ_CASE_INSENSITIVE */

    Status = NtOpenSectionF(&Section, SECTION_MAP_READ, &Attr);
    if (Status < 0)
    {
        fprintf(stderr, "ar9485lab: NtOpenSection(PhysicalMemory) = 0x%08lx\n",
                (unsigned long)Status);
        return 1;
    }

    ViewSize = (SIZE_T)Skew + Bytes;
    Offset.QuadPart = (LONGLONG)PageBase;
    Status = NtMapViewF(Section, GetCurrentProcess(), &Base, 0, ViewSize,
                        &Offset, &ViewSize, 1 /* ViewShare */, 0, PAGE_READONLY);
    if (Status < 0)
    {
        fprintf(stderr, "ar9485lab: NtMapViewOfSection(pa=0x%08lx%08lx) = 0x%08lx\n",
                (unsigned long)(PageBase >> 32), (unsigned long)PageBase,
                (unsigned long)Status);
        CloseHandle(Section);
        return 1;
    }

    Emit("PMEM pa=0x%08lx%08lx bytes=%lu\n",
         (unsigned long)(Pa >> 32), (unsigned long)Pa, (unsigned long)Bytes);
    for (i = 0; i < Bytes; i += 16)
    {
        const unsigned char *Row = (const unsigned char *)Base + Skew + i;
        ULONG j;

        Emit("  +%04lx ", (unsigned long)i);
        for (j = 0; j < 16 && i + j < Bytes; ++j)
            Emit("%02x", Row[j]);
        Emit("\n");
    }

    NtUnmapViewF(GetCurrentProcess(), Base);
    CloseHandle(Section);
    return 0;
}

int
main(int argc, char **argv)
{
    HANDLE Device;
    AR9485LAB_INFO Info;
    DWORD Returned = 0;
    unsigned char *In = NULL;
    unsigned char *Out = NULL;
    ULONG InLength, OutLength;
    int InfoOnly = 0;
    int QueryMode = 0;
    int Parsed = 0;
    int i;
    int Result = 1;
    const char *OutPath = NULL;

    g_Out = stdout;
    g_Script.Ops = g_OpStorage;
    g_Script.OpCapacity = MAX_SCRIPT_OPS;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            Usage();
            return 0;
        }
        if (strcmp(argv[i], "-p") == 0 && i + 2 < argc)
        {
            unsigned __int64 Pa = _strtoui64(argv[i + 1], NULL, 0);
            ULONG Bytes = (ULONG)strtoul(argv[i + 2], NULL, 0);

            i += 2;
            return DumpPhysical(Pa, Bytes);
        }
        if (strcmp(argv[i], "-i") == 0)
        {
            InfoOnly = 1;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-l") == 0)
        {
            QueryMode = argv[i][1];
            continue;
        }
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            OutPath = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-") == 0)
        {
            if (!ParseStream(stdin))
                return 1;
        }
        else
        {
            FILE *Script = fopen(argv[i], "r");

            if (Script == NULL)
            {
                fprintf(stderr, "ar9485lab: cannot open %s\n", argv[i]);
                return 1;
            }
            if (!ParseStream(Script))
            {
                fclose(Script);
                return 1;
            }
            fclose(Script);
        }
        Parsed = 1;
    }

    if (QueryMode != 0)
    {
        int QueryResult;

        if (OutPath != NULL && (g_Out = fopen(OutPath, "w")) == NULL)
        {
            fprintf(stderr, "ar9485lab: cannot write %s\n", OutPath);
            return 1;
        }
        QueryResult = (QueryMode == 's') ? PrintLinkStatus() : PrintLog();
        if (g_Out != stdout)
            fclose(g_Out);
        return QueryResult;
    }

    if (!InfoOnly && !Parsed)
    {
        /* No script and no -i: read one from stdin, which is how the host
         * driver script feeds experiments over luagent. */
        if (!ParseStream(stdin))
            return 1;
    }

    if (OutPath != NULL)
    {
        g_Out = fopen(OutPath, "w");
        if (g_Out == NULL)
        {
            fprintf(stderr, "ar9485lab: cannot write %s\n", OutPath);
            return 1;
        }
    }

    Device = CreateFileW(AR9485LAB_WIN32_NAME,
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         0,
                         NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr,
                "ar9485lab: cannot open %ls (error %lu). Is ar9485.sys built "
                "with AR9485_LAB and loaded?\n",
                AR9485LAB_WIN32_NAME, GetLastError());
        return 1;
    }

    memset(&Info, 0, sizeof(Info));
    if (!DeviceIoControl(Device, IOCTL_AR9485LAB_INFO, NULL, 0,
                         &Info, sizeof(Info), &Returned, NULL) ||
        Returned < sizeof(Info))
    {
        fprintf(stderr, "ar9485lab: INFO failed (error %lu)\n", GetLastError());
        goto Cleanup;
    }

    if (Info.AbiVersion != AR9485LAB_ABI_VERSION)
    {
        /* Refuse rather than misparse: a driver from a different build would
         * hand back records this tool would happily print as nonsense. */
        fprintf(stderr, "ar9485lab: ABI mismatch, driver %lu, tool %u\n",
                (unsigned long)Info.AbiVersion, AR9485LAB_ABI_VERSION);
        goto Cleanup;
    }

    PrintInfo(&Info);

    if (InfoOnly || g_Script.OpCount == 0)
    {
        Result = 0;
        goto Cleanup;
    }

    if (!DeviceIoControl(Device, IOCTL_AR9485LAB_CLAIM, NULL, 0,
                         &Info, sizeof(Info), &Returned, NULL))
    {
        fprintf(stderr, "ar9485lab: CLAIM failed (error %lu)\n", GetLastError());
        goto Cleanup;
    }

    InLength = LabScriptEncodedSize(&g_Script);
    OutLength = sizeof(AR9485LAB_RESULT_HEADER) +
                g_Script.OpCount * sizeof(AR9485LAB_RESULT) + AR9485LAB_MAX_DUMP_DWORDS * 4;
    if (OutLength > AR9485LAB_MAX_RESULT_BYTES)
        OutLength = AR9485LAB_MAX_RESULT_BYTES;

    In = (unsigned char *)calloc(1, InLength);
    Out = (unsigned char *)calloc(1, OutLength);
    if (In == NULL || Out == NULL)
    {
        fprintf(stderr, "ar9485lab: out of memory\n");
        goto Cleanup;
    }

    /* The same encoder the host harness uses, so what is exercised there is
     * byte-for-byte what the driver receives here. */
    if (LabScriptEncode(&g_Script, In, InLength) != InLength)
    {
        fprintf(stderr, "ar9485lab: failed to encode the program\n");
        goto Cleanup;
    }

    Returned = 0;
    if (!DeviceIoControl(Device, IOCTL_AR9485LAB_RUN, In, InLength,
                         Out, OutLength, &Returned, NULL))
    {
        fprintf(stderr, "ar9485lab: RUN failed (error %lu)\n", GetLastError());
        goto Cleanup;
    }

    PrintResults(Out, Returned);
    Result = g_Failed ? 2 : 0;

Cleanup:
    /* Explicit release, though closing the handle would do it: an operator
     * reading the transcript should see the chip handed back. */
    if (Device != INVALID_HANDLE_VALUE)
    {
        DeviceIoControl(Device, IOCTL_AR9485LAB_RELEASE, NULL, 0, NULL, 0,
                        &Returned, NULL);
        CloseHandle(Device);
    }
    free(In);
    free(Out);
    if (g_Out != stdout)
    {
        /* Flush and close before exiting: the whole point of -o is that the
         * host reads a complete file. */
        fclose(g_Out);
    }
    return Result;
}
