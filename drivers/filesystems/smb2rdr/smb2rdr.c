/*
 * smb2rdr — SMB2/3 mini-redirector, skeleton.
 *
 * Shape follows drivers/filesystems/nfs/nfs41_driver.c: DriverEntry calls
 * RxDriverEntry + RxRegisterMinirdr, installs a MINIRDR_DISPATCH ops table,
 * and routes every IRP major function through a single FsdDispatch shim
 * that delegates to RxFsdDispatch.
 *
 * The MRx op table wires the minimum set of callbacks that rdbss touches
 * when MUP probes a UNC path (Start/Stop, CreateSrvCall, CreateVNetRoot,
 * ExtractNetRootName, Finalize*).  DriverEntry self-triggers the rdbss
 * start handshake (RxStartMinirdr) so FsRtlRegisterUncProvider registers
 * \Device\smb2rdr with MUP at load time, without waiting for a usermode
 * daemon IOCTL.  The libsmb2 share-connect happens lazily in
 * MRxCreateVNetRoot (where we know both server and share) by driving an
 * SMB2D_OP_CONNECT_SHARE upcall to the smb2d daemon; the opaque per-share
 * handle the daemon returns is stashed in SMB2RDR_V_NET_ROOT_EXTENSION
 * for subsequent op calldowns.
 */

#define MINIRDR__NAME "Value is ignored, only fact of definition"
#include <rx.h>
#include <windef.h>
#include <winerror.h>
#include <ntstrsafe.h>

#ifdef __REACTOS__
#include <pseh/pseh2.h>
#endif

#define NDEBUG
#include <debug.h>

#include "smb2rdr.h"
#include "upcall.h"

#define SMB2RDR_STAT_TYPE_DIRECTORY 1

typedef enum _SMB2RDR_STORAGE_TYPE_CODES {
    NTC_SMB2RDR_DEVICE_EXTENSION = (NODE_TYPE_CODE)0xFC10,
} SMB2RDR_STORAGE_TYPE_CODES;

#define RxDefineNode(node, type)               \
    (node)->NodeTypeCode = NTC_##type;         \
    (node)->NodeByteSize = sizeof(type)

typedef struct _SMB2RDR_DEVICE_EXTENSION {
    NODE_TYPE_CODE  NodeTypeCode;
    NODE_BYTE_SIZE  NodeByteSize;
    PRDBSS_DEVICE_OBJECT DeviceObject;
    ULONG           ActiveNodes;
    UCHAR           VolAttrs[256];
    ULONG           VolAttrsLen;
} SMB2RDR_DEVICE_EXTENSION, *PSMB2RDR_DEVICE_EXTENSION;

/* V_NET_ROOT extension: one per (logon, server, share) tuple.  rdbss
 * allocates it inline in the V_NET_ROOT object when
 * RDBSS_MANAGE_V_NET_ROOT_EXTENSION is set and MRxVNetRootSize is non-zero;
 * the pointer is stashed in V_NET_ROOT->Context.  Today the only field is
 * the opaque daemon-side connect-share handle returned by libsmb2; later
 * slices will grow it with per-share cached state. */
typedef struct _SMB2RDR_V_NET_ROOT_EXTENSION {
    ULONG64 DaemonHandle;       /* 0 if no connection was brought up */
} SMB2RDR_V_NET_ROOT_EXTENSION, *PSMB2RDR_V_NET_ROOT_EXTENSION;

#define Smb2RdrGetVNetRootExtension(V) \
    (((V) == NULL) ? NULL : (PSMB2RDR_V_NET_ROOT_EXTENSION)((V)->Context))

/* FCB extension: tracks the opaque daemon file handle associated with the
 * underlying libsmb2 smb2fh/smb2dir.  RDBSS may collapse compatible opens
 * onto the same SRV_OPEN, so this remains per-FCB/SRV_OPEN state while each
 * caller still gets its own FOBX from RxCreateNetFobx. */
typedef struct _SMB2RDR_FCB_EXTENSION {
    ULONG64        DaemonFileHandle;   /* 0 if no daemon-side open */
    BOOLEAN        IsDirectory;
    BOOLEAN        DeleteOnClose;      /* FileDispositionInformation flag,
                                         * consumed in MRxCloseSrvOpen */
    /* Share-relative path captured at MRxCreate time, needed so that the
     * rename/unlink upcalls can rebuild an SMB2 path without the name-table
     * entry (which rdbss drops before MRxSetFileInfo fires on dispose). */
    UNICODE_STRING Path;
} SMB2RDR_FCB_EXTENSION, *PSMB2RDR_FCB_EXTENSION;

#define Smb2RdrGetFcbExtension(F) \
    (((F) == NULL) ? NULL : (PSMB2RDR_FCB_EXTENSION)((F)->Context))

static struct _MINIRDR_DISPATCH smb2rdr_ops;
static PRDBSS_DEVICE_OBJECT     smb2rdr_dev;

/* ---------- unimplemented leaves ---------- */

NTSTATUS NTAPI
smb2rdr_Unimplemented(PRX_CONTEXT RxContext)
{
    UNREFERENCED_PARAMETER(RxContext);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS NTAPI
smb2rdr_AreFilesAliased(PFCB a, PFCB b)
{
    UNREFERENCED_PARAMETER(a);
    UNREFERENCED_PARAMETER(b);
    return STATUS_NOT_IMPLEMENTED;
}

/* rdbss calls MRxDeallocateForFcb right before it frees the FCB storage.
 * Our FCB extension is allocated inline by rdbss (MRxFcbSize > 0 plus the
 * RDBSS_MANAGE_FCB_EXTENSION flag), so rdbss reclaims it along with the
 * FCB itself.  The one dynamic thing we own is the Path buffer stashed at
 * MRxCreate time; release it here before rdbss zeroes the extension.
 * MRxCloseSrvOpen has already told the daemon to drop its libsmb2 handle,
 * so the handle slot sits at DaemonFileHandle == 0 by the time we get here. */
NTSTATUS NTAPI
smb2rdr_DeallocateForFcb(IN OUT PMRX_FCB pFcb)
{
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(pFcb);
    if (fcbExt != NULL && fcbExt->Path.Buffer != NULL) {
        ExFreePoolWithTag(fcbExt->Path.Buffer, 'pFcS');
        fcbExt->Path.Buffer = NULL;
        fcbExt->Path.Length = 0;
        fcbExt->Path.MaximumLength = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
smb2rdr_DeallocateForFobx(IN OUT PMRX_FOBX pFobx)
{
    UNREFERENCED_PARAMETER(pFobx);
    return STATUS_SUCCESS;
}

/* ---------- connection-bringup stubs ----------
 *
 * These stubs exist only so that rdbss has a non-NULL entry in each MRx slot
 * it touches while canonicalising a UNC path.  No libsmb2 work happens here;
 * the usermode daemon's upcall will land in MRxCreateSrvCall as the hook
 * point.  Every callback returns STATUS_NOT_IMPLEMENTED (or STATUS_SUCCESS
 * where rdbss treats failure as fatal to the driver) after logging. */

/* ---------- devfcb (IOCTL) dispatch ----------
 *
 * The usermode smb2d daemon opens \\.\smb2rdr and drives the upcall bridge
 * through two IOCTLs: IOCTL_SMB2RDR_READ pops the next pending upcall off
 * the queue and writes its header+payload into the daemon's output buffer;
 * IOCTL_SMB2RDR_WRITE delivers the matching downcall reply and wakes the
 * blocked kernel producer.  Everything else flows through the minirdr
 * stubs (or returns STATUS_INVALID_DEVICE_REQUEST so rdbss's default
 * handling wins). */
NTSTATUS NTAPI
smb2rdr_DevFcbXXXControlFile(IN OUT PRX_CONTEXT RxContext)
{
    NTSTATUS status;
    UCHAR op = RxContext->MajorFunction;
    PLOWIO_CONTEXT LowIo = &RxContext->LowIoContext;
    ULONG ioctl;
    ULONG info = 0;

    if (op != IRP_MJ_DEVICE_CONTROL && op != IRP_MJ_INTERNAL_DEVICE_CONTROL)
        return STATUS_INVALID_DEVICE_REQUEST;

    ioctl = LowIo->ParamsFor.IoCtl.IoControlCode;

    switch (ioctl) {
    case IOCTL_SMB2RDR_READ:
        status = SmbRdrUpcallIoctl(RxContext, &info);
        RxContext->InformationToReturn = info;
        break;
    case IOCTL_SMB2RDR_WRITE:
        status = SmbRdrDowncallIoctl(RxContext);
        RxContext->InformationToReturn = 0;
        break;
    default:
        DPRINT("SMB2RDR: DevFcbXXXControlFile unhandled ioctl=0x%08lx\n",
                 ioctl);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return status;
}

NTSTATUS NTAPI
smb2rdr_Start(IN OUT PRX_CONTEXT RxContext,
              IN OUT PRDBSS_DEVICE_OBJECT dev)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(dev);
    DPRINT1("SMB2RDR: Start called\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
smb2rdr_Stop(IN OUT PRX_CONTEXT RxContext,
             IN OUT PRDBSS_DEVICE_OBJECT dev)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(dev);
    DPRINT1("SMB2RDR: Stop called\n");
    return STATUS_SUCCESS;
}

/* CreateSrvCall is the first MRx hook after MUP picks us as the UNC
 * provider.  rdbss handles SrvCall objects as per-server singletons and
 * the actual share connect happens later in MRxCreateVNetRoot (where we
 * know both the server *and* the share), so this entry does nothing but
 * accept the calldown and signal success. */
NTSTATUS NTAPI
smb2rdr_CreateSrvCall(IN OUT PMRX_SRV_CALL SrvCall,
                      IN OUT PMRX_SRVCALL_CALLBACK_CONTEXT CallbackContext)
{
    PMRX_SRVCALLDOWN_STRUCTURE Calldown;

    ASSERT(SrvCall != NULL);
    ASSERT(NodeType(SrvCall) == RDBSS_NTC_SRVCALL);

    DPRINT1("SMB2RDR: MRxCreateSrvCall name=%wZ\n", SrvCall->pSrvCallName);

    Calldown = (PMRX_SRVCALLDOWN_STRUCTURE)CallbackContext->SrvCalldownStructure;
    CallbackContext->Status = STATUS_SUCCESS;
    CallbackContext->RecommunicateContext = NULL;
    Calldown->CallBack(CallbackContext);

    return STATUS_PENDING;
}

/* SrvCallWinnerNotify — rdbss invokes this on the mini-rdr that won the
 * CreateSrvCall race.  Required to exist and return STATUS_SUCCESS; a NULL
 * entry makes MINIRDR_CALL_THROUGH synthesise STATUS_NOT_IMPLEMENTED and
 * the whole SrvCall is marked Condition_Bad (which is what was happening
 * before and caused MRxCreateVNetRoot never to fire).  We don't need any
 * per-server state today so this is a no-op. */
NTSTATUS NTAPI
smb2rdr_SrvCallWinnerNotify(IN OUT PMRX_SRV_CALL SrvCall,
                            IN BOOLEAN ThisMinirdrIsTheWinner,
                            IN OUT PVOID RecommunicateContext)
{
    UNREFERENCED_PARAMETER(SrvCall);
    UNREFERENCED_PARAMETER(ThisMinirdrIsTheWinner);
    UNREFERENCED_PARAMETER(RecommunicateContext);
    DPRINT1("SMB2RDR: SrvCallWinnerNotify winner=%u\n",
            ThisMinirdrIsTheWinner);
    return STATUS_SUCCESS;
}

/* CreateVNetRoot is the hook where we actually bring up an SMB2 share
 * connection.  By this point rdbss has plumbed both the server name (via
 * SrvCall->pSrvCallName) and the full path (via NetRoot->pNetRootName,
 * which is "\server\share").  We strip the SrvCall prefix off the latter
 * to recover the share name, pack both into a length-prefixed UTF-16LE
 * payload, and drive an SMB2D_OP_CONNECT_SHARE upcall to the daemon.
 * On success the daemon hands back an opaque ULONG64 handle that we stash
 * on the V_NET_ROOT extension for subsequent op calldowns to reference. */
static NTSTATUS
smb2rdr_PackConnectShare(PMRX_SRV_CALL SrvCall,
                          PUNICODE_STRING FullNetRootName,
                          OUT PVOID *OutBuf,
                          OUT PULONG OutLen)
{
    USHORT serverBytes, shareBytes;
    USHORT srvPrefixBytes;
    PWCHAR srvChars, shareStart;
    ULONG totalBytes;
    PUCHAR buf;
    PUCHAR cursor;

    /* SrvCall->pSrvCallName is of the form "\server".  Strip the leading
     * backslash to get the bare hostname the way libsmb2 expects it. */
    if (SrvCall->pSrvCallName->Length < sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER;
    srvChars = SrvCall->pSrvCallName->Buffer + 1;
    serverBytes = (USHORT)(SrvCall->pSrvCallName->Length - sizeof(WCHAR));

    /* FullNetRootName ("\server\share") starts with the SrvCall prefix.
     * The share portion starts right after it, with its own leading
     * backslash that we also drop. */
    srvPrefixBytes = SrvCall->pSrvCallName->Length;
    if (FullNetRootName->Length <= srvPrefixBytes + sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER;
    shareStart = (PWCHAR)((PUCHAR)FullNetRootName->Buffer + srvPrefixBytes);
    if (*shareStart == L'\\') {
        shareStart++;
        shareBytes = FullNetRootName->Length - srvPrefixBytes - sizeof(WCHAR);
    } else {
        shareBytes = FullNetRootName->Length - srvPrefixBytes;
    }

    totalBytes = 2 * sizeof(USHORT) + serverBytes + shareBytes;
    buf = ExAllocatePoolWithTag(NonPagedPool, totalBytes, 'pucS');
    if (buf == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    cursor = buf;
    *(PUSHORT)cursor = serverBytes;
    cursor += sizeof(USHORT);
    RtlCopyMemory(cursor, srvChars, serverBytes);
    cursor += serverBytes;
    *(PUSHORT)cursor = shareBytes;
    cursor += sizeof(USHORT);
    RtlCopyMemory(cursor, shareStart, shareBytes);

    *OutBuf = buf;
    *OutLen = totalBytes;
    return STATUS_SUCCESS;
}

static VOID NTAPI
smb2rdr_CreateVNetRootInner(PVOID pContext)
{
    PMRX_CREATENETROOT_CONTEXT Context = pContext;
    PMRX_V_NET_ROOT pVNetRoot = (PMRX_V_NET_ROOT)Context->pVNetRoot;
    PMRX_NET_ROOT pNetRoot;
    PMRX_SRV_CALL pSrvCall;
    PSMB2RDR_V_NET_ROOT_EXTENSION ext;
    PVOID inBuf = NULL;
    ULONG inLen = 0;
    NTSTATUS status;
    NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS bridgeStatus;
    ULONG outActual = 0;
    ULONG64 handle = 0;

    pNetRoot = pVNetRoot->pNetRoot;
    pSrvCall = pNetRoot->pSrvCall;

    DPRINT1("SMB2RDR: MRxCreateVNetRoot (worker) srv=%wZ netroot=%wZ\n",
            pSrvCall->pSrvCallName, pNetRoot->pNetRootName);

    if (pNetRoot->Type != NET_ROOT_DISK && pNetRoot->Type != NET_ROOT_WILD) {
        status = STATUS_NOT_SUPPORTED;
        goto out;
    }

    ext = Smb2RdrGetVNetRootExtension(pVNetRoot);
    if (ext == NULL) {
        /* rdbss failed to allocate the extension (shouldn't happen because
         * we set MRxVNetRootSize and the MANAGE flag). */
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    status = smb2rdr_PackConnectShare(pSrvCall, pNetRoot->pNetRootName,
                                      &inBuf, &inLen);
    if (status != STATUS_SUCCESS)
        goto out;

    bridgeStatus = SmbRdrIssueUpcall(
        SMB2D_OP_CONNECT_SHARE,
        inBuf, inLen,
        &handle, (ULONG)sizeof(handle), &outActual,
        &daemonStatus,
        30 /* seconds */);

    if (bridgeStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: CreateVNetRoot bridge failed 0x%08lx\n",
                bridgeStatus);
        status = STATUS_BAD_NETWORK_PATH;
        goto out;
    }

    if (daemonStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: CreateVNetRoot daemon failed 0x%08lx\n",
                daemonStatus);
        status = daemonStatus;
        goto out;
    }

    if (outActual < sizeof(handle) || handle == 0) {
        DPRINT1("SMB2RDR: CreateVNetRoot bad daemon reply outlen=%lu "
                "handle=0x%llx\n", outActual, handle);
        status = STATUS_UNEXPECTED_NETWORK_ERROR;
        goto out;
    }

    ext->DaemonHandle = handle;
    pNetRoot->MRxNetRootState = MRX_NET_ROOT_STATE_GOOD;
    pNetRoot->DeviceType = FILE_DEVICE_DISK;

    DPRINT1("SMB2RDR: MRxCreateVNetRoot success handle=0x%llx\n", handle);
    status = STATUS_SUCCESS;

out:
    if (inBuf)
        ExFreePoolWithTag(inBuf, 'pucS');

    Context->VirtualNetRootStatus = status;
    if (pNetRoot->Context == NULL)
        Context->NetRootStatus = status;
    Context->Callback(Context);
}

NTSTATUS NTAPI
smb2rdr_CreateVNetRoot(IN OUT PMRX_CREATENETROOT_CONTEXT Context)
{
    NTSTATUS status;

    DPRINT1("SMB2RDR: MRxCreateVNetRoot\n");

    if (IoGetCurrentProcess() == RxGetRDBSSProcess()) {
        smb2rdr_CreateVNetRootInner(Context);
    } else {
        status = RxDispatchToWorkerThread(smb2rdr_dev, DelayedWorkQueue,
                                          smb2rdr_CreateVNetRootInner,
                                          Context);
        if (status != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: CreateVNetRoot RxDispatchToWorkerThread "
                    "-> 0x%08lx\n", status);
            Context->VirtualNetRootStatus = status;
            Context->NetRootStatus = status;
            Context->Callback(Context);
        }
    }

    /* rdbss requires STATUS_PENDING for this calldown regardless of
     * success or failure (the Callback has already been invoked). */
    return STATUS_PENDING;
}

/* Byte-identical parse to nfs41_ExtractNetRootName: given the full
 * canonicalised UNC path, emit NetRootName = first component after the
 * server part (i.e. the share), with RestOfName pointing at the remainder.
 * Input is of the form "\;letter:\server\share\path" or "\server\share\path"
 * depending on rdbss; SrvCall->pSrvCallName supplies the server chunk. */
VOID NTAPI
smb2rdr_ExtractNetRootName(IN PUNICODE_STRING FilePathName,
                           IN PMRX_SRV_CALL SrvCall,
                           OUT PUNICODE_STRING NetRootName,
                           OUT PUNICODE_STRING RestOfName OPTIONAL)
{
    ULONG length = FilePathName->Length;
    PWCH w = FilePathName->Buffer;
    PWCH wlimit = (PWCH)(((PCHAR)w) + length);
    PWCH wlow;

    w += (SrvCall->pSrvCallName->Length / sizeof(WCHAR));
    NetRootName->Buffer = wlow = w;
    for (;;)
    {
        if (w >= wlimit)
            break;
        if ((*w == OBJ_NAME_PATH_SEPARATOR) && (w != wlow))
            break;
        w++;
    }
    NetRootName->Length = NetRootName->MaximumLength =
        (USHORT)((PCHAR)w - (PCHAR)wlow);

    if (RestOfName != NULL)
    {
        RestOfName->Buffer = w;
        RestOfName->Length = RestOfName->MaximumLength =
            (USHORT)((PCHAR)wlimit - (PCHAR)w);
    }

    DPRINT1("SMB2RDR: ExtractNetRootName path=%wZ srv=%wZ -> root=%wZ\n",
            FilePathName, SrvCall->pSrvCallName, NetRootName);
}

NTSTATUS NTAPI
smb2rdr_FinalizeSrvCall(IN OUT PMRX_SRV_CALL SrvCall,
                        IN BOOLEAN Force)
{
    UNREFERENCED_PARAMETER(SrvCall);
    UNREFERENCED_PARAMETER(Force);
    DPRINT1("SMB2RDR: FinalizeSrvCall\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
smb2rdr_FinalizeNetRoot(IN OUT PMRX_NET_ROOT NetRoot,
                        IN PBOOLEAN Force)
{
    UNREFERENCED_PARAMETER(NetRoot);
    UNREFERENCED_PARAMETER(Force);
    DPRINT1("SMB2RDR: FinalizeNetRoot\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
smb2rdr_FinalizeVNetRoot(IN OUT PMRX_V_NET_ROOT VNetRoot,
                         IN PBOOLEAN Force)
{
    PSMB2RDR_V_NET_ROOT_EXTENSION ext = Smb2RdrGetVNetRootExtension(VNetRoot);
    ULONG64 handle;

    UNREFERENCED_PARAMETER(Force);

    if (ext != NULL && ext->DaemonHandle != 0) {
        handle = ext->DaemonHandle;
        ext->DaemonHandle = 0;
        DPRINT1("SMB2RDR: FinalizeVNetRoot handle=0x%llx\n", handle);

        /* Best-effort: ask the daemon to tear down its libsmb2 context.
         * If the bridge is down or the daemon is gone the kernel side
         * still unwinds cleanly — the handle reference is dropped either
         * way, and leaking a daemon-side context is recoverable at
         * smb2d restart.  SmbRdrIssueUpcall is self-gating via gUpcallInit
         * and simply returns STATUS_DEVICE_NOT_READY if the bridge is
         * already torn down. */
        {
            NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
            ULONG outActual = 0;
            (void)SmbRdrIssueUpcall(
                SMB2D_OP_DISCONNECT,
                &handle, (ULONG)sizeof(handle),
                NULL, 0, &outActual,
                &daemonStatus,
                5 /* seconds */);
        }
    } else {
        DPRINT1("SMB2RDR: FinalizeVNetRoot (no handle)\n");
    }

    return STATUS_SUCCESS;
}

/* ---------- MRxCreate / MRxCloseSrvOpen ----------
 *
 * MRxCreate is invoked by rdbss once it has already allocated an SRV_OPEN
 * and (possibly) an FCB for the path.  Our job is to translate the create
 * parameters into an OP_CREATE upcall, block on the daemon's reply, and
 * populate the FCB extension with the opaque daemon-side handle that
 * subsequent read/close/query-info calls will quote.
 *
 * For the share-root case (path length 0 after stripping the NetRoot
 * prefix), the daemon drives smb2_opendir(""); for non-root paths it
 * either honours FILE_DIRECTORY_FILE / FILE_NON_DIRECTORY_FILE hints or
 * falls back to smb2_stat() to disambiguate. */

static NTSTATUS NTAPI
smb2rdr_Create(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_SRV_OPEN SrvOpen = RxContext->pRelevantSrvOpen;
    PMRX_FCB Fcb = RxContext->pFcb;
    PSMB2RDR_V_NET_ROOT_EXTENSION vNetExt;
    PSMB2RDR_FCB_EXTENSION fcbExt;
    PNT_CREATE_PARAMETERS params;
    PUNICODE_STRING relName;
    PUCHAR inBuf = NULL;
    ULONG inLen;
    PWCHAR pathChars;
    USHORT pathBytes;
    OP_CREATE_OUT out;
    ULONG outActual = 0;
    NTSTATUS bridgeStatus;
    NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS status;

    if (SrvOpen == NULL || Fcb == NULL)
        return STATUS_INVALID_PARAMETER;

    vNetExt = Smb2RdrGetVNetRootExtension(SrvOpen->pVNetRoot);
    fcbExt  = Smb2RdrGetFcbExtension(Fcb);
    if (vNetExt == NULL || fcbExt == NULL ||
        vNetExt->DaemonHandle == 0)
    {
        DPRINT1("SMB2RDR: MRxCreate: no daemon share handle on vnet\n");
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    params = &RxContext->Create.NtCreateParameters;

    /* The relative path is whatever remains after the VNetRoot prefix
     * was stripped during canonicalisation.  rdbss stashes this in
     * Fcb->FcbTableEntry.Path (empty for the share root). */
    relName = &((PFCB)Fcb)->FcbTableEntry.Path;
    pathChars = relName->Buffer;
    pathBytes = relName->Length;

    /* Drop a single leading backslash: the daemon converts backslashes
     * to forward slashes but libsmb2's smb2_open wants "dir/file" rather
     * than "/dir/file". */
    if (pathBytes >= sizeof(WCHAR) && pathChars[0] == L'\\') {
        pathChars++;
        pathBytes -= sizeof(WCHAR);
    }

    DPRINT("SMB2RDR: MRxCreate path=\"%wZ\" opts=0x%lx disp=%lu "
           "access=0x%lx vnet=0x%llx\n",
           relName, params->CreateOptions, params->Disposition,
           params->DesiredAccess, vNetExt->DaemonHandle);

    inLen = (ULONG)sizeof(OP_CREATE_IN) + pathBytes;
    inBuf = ExAllocatePoolWithTag(NonPagedPool, inLen, 'rCcS');
    if (inBuf == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    {
        OP_CREATE_IN hdr;
        RtlZeroMemory(&hdr, sizeof(hdr));
        hdr.VNetHandle     = vNetExt->DaemonHandle;
        hdr.CreateOptions  = params->CreateOptions;
        hdr.Disposition    = params->Disposition;
        hdr.DesiredAccess  = params->DesiredAccess;
        hdr.FileAttributes = params->FileAttributes;
        hdr.ShareAccess    = params->ShareAccess;
        hdr.PathLen        = pathBytes;
        RtlCopyMemory(inBuf, &hdr, sizeof(hdr));
        if (pathBytes)
            RtlCopyMemory(inBuf + sizeof(hdr), pathChars, pathBytes);
    }

    RtlZeroMemory(&out, sizeof(out));
    bridgeStatus = SmbRdrIssueUpcall(
        SMB2D_OP_CREATE,
        inBuf, inLen,
        &out, (ULONG)sizeof(out), &outActual,
        &daemonStatus,
        30 /* seconds */);

    ExFreePoolWithTag(inBuf, 'rCcS');

    if (bridgeStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxCreate bridge failed 0x%08lx\n", bridgeStatus);
        return STATUS_UNEXPECTED_NETWORK_ERROR;
    }
    if (daemonStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxCreate daemon failed 0x%08lx\n", daemonStatus);
        return daemonStatus;
    }
    if (outActual < sizeof(out) || out.FileHandle == 0) {
        DPRINT1("SMB2RDR: MRxCreate short reply outlen=%lu handle=0x%llx\n",
                outActual, out.FileHandle);
        return STATUS_UNEXPECTED_NETWORK_ERROR;
    }

    /* Allocate a FOBX for the file object.  rdbss frees it on Cleanup. */
    if (!RxIsFcbAcquiredExclusive(Fcb)) {
        ASSERT(!RxIsFcbAcquiredShared(Fcb));
        RxAcquireExclusiveFcbResourceInMRx(Fcb);
    }
    RxContext->pFobx = RxCreateNetFobx(RxContext, SrvOpen);
    if (RxContext->pFobx == NULL) {
        /* Best-effort: ask the daemon to release the libsmb2 handle we
         * just opened; worst case the handle sits in the table until
         * the service is restarted. */
        OP_CLOSE_IN cin;
        ULONG closeActual = 0;
        NTSTATUS closeStatus = STATUS_UNSUCCESSFUL;
        RtlZeroMemory(&cin, sizeof(cin));
        cin.FileHandle  = out.FileHandle;
        cin.IsDirectory = out.IsDirectory;
        (void)SmbRdrIssueUpcall(SMB2D_OP_CLOSE, &cin, (ULONG)sizeof(cin),
                                 NULL, 0, &closeActual, &closeStatus, 5);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Teach rdbss about the file-vs-directory distinction and the
     * basic metadata we got back.  For the P0 slice we only need the
     * FCB to know it's a directory (so subsequent SetInfo/Query paths
     * pick the right branch).  QueryFileInfo, when implemented, will
     * overwrite the InitPacket values with authoritative numbers. */
    {
        FCB_INIT_PACKET initPkt;
        RX_FILE_TYPE storageType;
        LARGE_INTEGER zero;
        ULONG attrs = out.FileAttributes ? out.FileAttributes
                                         : FILE_ATTRIBUTE_NORMAL;
        ULONG numLinks = 1;

        zero.QuadPart = 0;
        RxFormInitPacket(initPkt,
                          &attrs,
                          &numLinks,
                          &zero, &zero, &zero, &zero,
                          &out.AllocationSize,
                          &out.EndOfFile,
                          &out.EndOfFile);

        storageType = out.IsDirectory ? FileTypeDirectory : FileTypeFile;
        RxFinishFcbInitialization(Fcb, RDBSS_STORAGE_NTC(storageType),
                                   &initPkt);
    }

    fcbExt->DaemonFileHandle = out.FileHandle;
    fcbExt->IsDirectory      = out.IsDirectory ? TRUE : FALSE;
    fcbExt->DeleteOnClose    = FALSE;

    /* Cache the share-relative path so MRxSetFileInfo (rename) and the
     * DeleteOnClose path in MRxCloseSrvOpen can reconstruct an OP_RENAME
     * / OP_UNLINK payload without depending on rdbss's FcbTableEntry,
     * which gets cleared out by RxRemoveNameNetFcb before the disposition
     * upcall fires.  Uses the relName copy already normalised above (leading
     * backslash stripped), matching the OP_CREATE path format so the daemon
     * sees consistent shape across all file ops. */
    if (fcbExt->Path.Buffer != NULL) {
        /* Should not happen — MRxCreate runs once per FCB — but defend. */
        ExFreePoolWithTag(fcbExt->Path.Buffer, 'pFcS');
        fcbExt->Path.Buffer = NULL;
        fcbExt->Path.Length = 0;
        fcbExt->Path.MaximumLength = 0;
    }
    if (pathBytes > 0) {
        PWCHAR pathCopy = ExAllocatePoolWithTag(PagedPool, pathBytes, 'pFcS');
        if (pathCopy != NULL) {
            RtlCopyMemory(pathCopy, pathChars, pathBytes);
            fcbExt->Path.Buffer        = pathCopy;
            fcbExt->Path.Length        = pathBytes;
            fcbExt->Path.MaximumLength = pathBytes;
        } else {
            /* Non-fatal — rename/unlink will refuse when Path is empty. */
            DPRINT1("SMB2RDR: MRxCreate path-stash alloc failed, "
                    "rename/unlink disabled on this fcb\n");
        }
    }

    RxContext->Create.ReturnedCreateInformation = out.Information;
#ifndef __REACTOS__
    RxContext->CurrentIrp->IoStatus.Information = out.Information;
#endif

    status = STATUS_SUCCESS;
    RxContext->CurrentIrp->IoStatus.Status = status;

    DPRINT("SMB2RDR: MRxCreate success fcb_handle=0x%llx info=%lu "
           "is_dir=%lu\n",
           out.FileHandle, out.Information, out.IsDirectory);
    return status;
}

static NTSTATUS NTAPI
smb2rdr_ShouldTryToCollapseThisOpen(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_SRV_OPEN SrvOpen;

    PAGED_CODE();

    if (RxContext->pRelevantSrvOpen == NULL)
        return STATUS_SUCCESS;

    SrvOpen = RxContext->pRelevantSrvOpen;

    if (SrvOpen->pVNetRoot != RxContext->Create.pVNetRoot)
        return STATUS_MORE_PROCESSING_REQUIRED;

    if (BooleanFlagOn(SrvOpen->CreateOptions ^
                      RxContext->Create.NtCreateParameters.CreateOptions,
                      FILE_OPEN_REPARSE_POINT))
    {
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
smb2rdr_CollapseOpen(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PMRX_SRV_OPEN SrvOpen = RxContext->pRelevantSrvOpen;

    PAGED_CODE();

    if (SrvOpen == NULL)
        return STATUS_INVALID_PARAMETER;

    if (!RxIsFcbAcquiredExclusive(Fcb)) {
        ASSERT(!RxIsFcbAcquiredShared(Fcb));
        RxAcquireExclusiveFcbResourceInMRx(Fcb);
    }

    RxContext->pFobx = RxCreateNetFobx(RxContext, SrvOpen);
    if (RxContext->pFobx == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
smb2rdr_CloseSrvOpen(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PMRX_SRV_OPEN SrvOpen = RxContext->pRelevantSrvOpen;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    PSMB2RDR_V_NET_ROOT_EXTENSION vNetExt =
        (SrvOpen != NULL) ? Smb2RdrGetVNetRootExtension(SrvOpen->pVNetRoot)
                          : NULL;
    ULONG64 handle;
    BOOLEAN isDir;
    BOOLEAN wantUnlink;
    OP_CLOSE_IN cin;
    ULONG outActual = 0;
    NTSTATUS bridgeStatus;
    NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;

    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0) {
        /* Nothing to close daemon-side — either MRxCreate never ran to
         * completion, or a prior CloseSrvOpen already cleared the slot
         * (rdbss can call us twice on the scavenger path). */
        DPRINT1("SMB2RDR: MRxCloseSrvOpen (no handle)\n");
        return STATUS_SUCCESS;
    }

    handle = fcbExt->DaemonFileHandle;
    isDir  = fcbExt->IsDirectory;
    wantUnlink = fcbExt->DeleteOnClose;
    /* Clear the slot up-front so a racing finalize doesn't double-close
     * even if the upcall path bounces (timeout, bridge not ready, etc.). */
    fcbExt->DaemonFileHandle = 0;
    fcbExt->DeleteOnClose    = FALSE;

    DPRINT("SMB2RDR: MRxCloseSrvOpen handle=0x%llx is_dir=%u delete=%u\n",
           handle, isDir, wantUnlink);

    RtlZeroMemory(&cin, sizeof(cin));
    cin.FileHandle  = handle;
    cin.IsDirectory = isDir ? 1u : 0u;

    bridgeStatus = SmbRdrIssueUpcall(
        SMB2D_OP_CLOSE,
        &cin, (ULONG)sizeof(cin),
        NULL, 0, &outActual,
        &daemonStatus,
        15 /* seconds */);

    if (bridgeStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxCloseSrvOpen bridge failed 0x%08lx\n",
                bridgeStatus);
        /* Still report SUCCESS to rdbss: the kernel side has already
         * dropped its reference, and leaking a daemon-side handle is
         * recoverable across a service restart. */
        return STATUS_SUCCESS;
    }
    if (daemonStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxCloseSrvOpen daemon reply 0x%08lx\n",
                daemonStatus);
    }

    /* If MRxSetFileInfo armed DeleteOnClose, fire an OP_UNLINK now that the
     * daemon has released its libsmb2 handle.  We deliberately swallow
     * errors from the unlink — NT semantics treat the disposition flag as
     * best-effort at close time, and failing the close would strand the
     * kernel-side FCB in an inconsistent state. */
    if (wantUnlink &&
        vNetExt != NULL && vNetExt->DaemonHandle != 0 &&
        fcbExt->Path.Buffer != NULL && fcbExt->Path.Length > 0)
    {
        ULONG unlinkLen = (ULONG)sizeof(OP_UNLINK_IN) + fcbExt->Path.Length;
        POP_UNLINK_IN unlinkIn =
            ExAllocatePoolWithTag(NonPagedPool, unlinkLen, 'UDcS');
        if (unlinkIn != NULL) {
            NTSTATUS unlinkDaemon = STATUS_UNSUCCESSFUL;
            NTSTATUS unlinkBridge;
            ULONG unlinkActual = 0;

            RtlZeroMemory(unlinkIn, sizeof(OP_UNLINK_IN));
            unlinkIn->VNetHandle  = vNetExt->DaemonHandle;
            unlinkIn->IsDirectory = isDir ? 1u : 0u;
            unlinkIn->PathLen     = fcbExt->Path.Length;
            RtlCopyMemory((PUCHAR)unlinkIn + sizeof(OP_UNLINK_IN),
                          fcbExt->Path.Buffer, fcbExt->Path.Length);

            unlinkBridge = SmbRdrIssueUpcall(
                SMB2D_OP_UNLINK,
                unlinkIn, unlinkLen,
                NULL, 0, &unlinkActual,
                &unlinkDaemon,
                15 /* seconds */);

            DPRINT1("SMB2RDR: CloseSrvOpen delete-on-close unlink "
                    "bridge=0x%08lx daemon=0x%08lx\n",
                    unlinkBridge, unlinkDaemon);

            ExFreePoolWithTag(unlinkIn, 'UDcS');
        } else {
            DPRINT1("SMB2RDR: CloseSrvOpen delete-on-close alloc failed\n");
        }
    } else if (wantUnlink) {
        DPRINT1("SMB2RDR: CloseSrvOpen delete-on-close skipped "
                "(no path or no vnet handle)\n");
    }

    return STATUS_SUCCESS;
}

/* ---------- MRxQueryDirectory ----------
 *
 * rdbss already canonicalised everything we need: Fobx->UnicodeQueryTemplate
 * is the filename filter (empty/"*" on the first call), Info.Buffer is the
 * (locked + mapped) user buffer to fill, and QueryDirectory.{InitialQuery,
 * RestartScan, ReturnSingleEntry} tell us how to pilot the iterator in the
 * daemon.  We drive SMB2D_OP_READDIR with a temporary non-paged staging
 * buffer big enough to hold the header + the user's buffer; the daemon
 * formats NT dir-info records straight into it, and we bounce the payload
 * into the user buffer in a single memcpy.  This deliberately trades one
 * extra copy for a tiny kernel-side footprint.
 */
static NTSTATUS NTAPI
smb2rdr_QueryDirectory(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PMRX_FOBX Fobx = RxContext->pFobx;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    FILE_INFORMATION_CLASS infoClass;
    PVOID userBuf;
    LONG userBufLen;
    PUNICODE_STRING filter;
    POP_READDIR_IN opIn = NULL;
    PUCHAR opOutBuf = NULL;
    POP_READDIR_OUT opOut;
    ULONG inLen;
    ULONG outCap;
    ULONG outActual = 0;
    NTSTATUS bridgeStatus;
    NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS status;

    if (RxContext->Info.Buffer == NULL)
        return STATUS_INVALID_USER_BUFFER;

    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0 || !fcbExt->IsDirectory) {
        DPRINT1("SMB2RDR: MRxQueryDirectory: no daemon dir handle\n");
        return STATUS_INVALID_HANDLE;
    }

    infoClass   = RxContext->Info.FileInformationClass;
    userBuf     = RxContext->Info.Buffer;
    userBufLen  = RxContext->Info.LengthRemaining;
    filter      = (Fobx != NULL) ? &Fobx->UnicodeQueryTemplate : NULL;

    if (userBufLen <= 0)
        return STATUS_BUFFER_TOO_SMALL;

    switch (infoClass) {
    case FileDirectoryInformation:
    case FileFullDirectoryInformation:
    case FileBothDirectoryInformation:
    case FileNamesInformation:
        break;
    default:
        DPRINT1("SMB2RDR: MRxQueryDirectory: unhandled info class %d\n",
                (int)infoClass);
        return STATUS_INVALID_INFO_CLASS;
    }

    /* Build upcall input.  The filter is length-prefixed inside the header
     * so the daemon can reconstruct it without extra parsing gymnastics. */
    {
        USHORT filterBytes = 0;
        if (filter != NULL && filter->Length > 0 && filter->Buffer != NULL)
            filterBytes = filter->Length;

        inLen = (ULONG)sizeof(OP_READDIR_IN) + filterBytes;
        opIn = ExAllocatePoolWithTag(NonPagedPool, inLen, 'RDcS');
        if (opIn == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        RtlZeroMemory(opIn, inLen);
        opIn->FileHandle        = fcbExt->DaemonFileHandle;
        opIn->InfoClass         = (ULONG)infoClass;
        opIn->MaxOutputLen      = (ULONG)userBufLen;
        opIn->RestartScan       = (RxContext->QueryDirectory.RestartScan ||
                                    RxContext->QueryDirectory.InitialQuery) ? 1 : 0;
        opIn->ReturnSingleEntry = RxContext->QueryDirectory.ReturnSingleEntry ? 1 : 0;
        opIn->FileIndex         = RxContext->QueryDirectory.IndexSpecified
                                  ? RxContext->QueryDirectory.FileIndex : 0;
        opIn->FileSpecLen       = filterBytes;
        if (filterBytes > 0) {
            RtlCopyMemory((PUCHAR)opIn + sizeof(OP_READDIR_IN),
                          filter->Buffer, filterBytes);
        }
    }

    /* Size the output staging buffer to fit the user's full request.  The
     * daemon side will cap what it emits at MaxOutputLen, so we never
     * overflow on the copy-out below. */
    outCap = (ULONG)sizeof(OP_READDIR_OUT) + (ULONG)userBufLen;
    opOutBuf = ExAllocatePoolWithTag(NonPagedPool, outCap, 'RDoS');
    if (opOutBuf == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlZeroMemory(opOutBuf, sizeof(OP_READDIR_OUT));
    opOut = (POP_READDIR_OUT)opOutBuf;

    DPRINT("SMB2RDR: MRxQueryDirectory handle=0x%llx class=%d buf=%d "
           "restart=%u initial=%u single=%u filter=%wZ\n",
           fcbExt->DaemonFileHandle, (int)infoClass, userBufLen,
           (unsigned)RxContext->QueryDirectory.RestartScan,
           (unsigned)RxContext->QueryDirectory.InitialQuery,
           (unsigned)RxContext->QueryDirectory.ReturnSingleEntry,
           filter);

    bridgeStatus = SmbRdrIssueUpcall(
        SMB2D_OP_READDIR,
        opIn, inLen,
        opOutBuf, outCap, &outActual,
        &daemonStatus,
        30 /* seconds */);

    if (bridgeStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxQueryDirectory bridge failed 0x%08lx\n",
                bridgeStatus);
        status = STATUS_UNEXPECTED_NETWORK_ERROR;
        goto out;
    }

    if (outActual < sizeof(OP_READDIR_OUT)) {
        /* Daemon-side failures (unknown handle, stat error, ...) come back
         * as a status-only downcall with no payload.  Surface the NTSTATUS
         * unchanged — for STATUS_NO_MORE_FILES the caller already expects
         * a zero-entry completion. */
        status = (daemonStatus != STATUS_SUCCESS) ? daemonStatus
                                                 : STATUS_UNEXPECTED_NETWORK_ERROR;
        DPRINT1("SMB2RDR: MRxQueryDirectory short reply outlen=%lu ds=0x%08lx\n",
                outActual, daemonStatus);
        goto out;
    }

    if (daemonStatus == STATUS_NO_MORE_FILES) {
        /* Iterator has been drained by a previous call; rdbss wants the
         * buffer to remain untouched and the terminating status. */
        RxContext->Info.LengthRemaining = userBufLen;
        status = STATUS_NO_MORE_FILES;
        goto out;
    }

    if (daemonStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxQueryDirectory daemon failed 0x%08lx\n",
                daemonStatus);
        status = daemonStatus;
        goto out;
    }

    if (opOut->BytesWritten > (ULONG)userBufLen ||
        sizeof(OP_READDIR_OUT) + opOut->BytesWritten > outActual)
    {
        DPRINT1("SMB2RDR: MRxQueryDirectory bad reply bw=%lu bufcap=%d "
                "actual=%lu\n",
                opOut->BytesWritten, userBufLen, outActual);
        status = STATUS_UNEXPECTED_NETWORK_ERROR;
        goto out;
    }

    if (opOut->EntryCount == 0) {
        /* End-of-directory with no entries produced this round. */
        RxContext->Info.LengthRemaining = userBufLen;
        status = STATUS_NO_MORE_FILES;
        goto out;
    }

    /* Ferry the packed records into the locked user buffer.  rdbss
     * has already mapped Info.Buffer at elevated IRQL; this copy is
     * kernel-mode to kernel-mode. */
    _SEH2_TRY {
        RtlCopyMemory(userBuf,
                      opOutBuf + sizeof(OP_READDIR_OUT),
                      opOut->BytesWritten);
    } _SEH2_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_INVALID_USER_BUFFER;
        goto out;
    } _SEH2_END;

    RxContext->Info.LengthRemaining = userBufLen - (LONG)opOut->BytesWritten;

    DPRINT("SMB2RDR: MRxQueryDirectory ok bytes=%lu entries=%lu flags=0x%lx\n",
           opOut->BytesWritten, opOut->EntryCount, opOut->Flags);

    status = STATUS_SUCCESS;

out:
    if (opIn) ExFreePoolWithTag(opIn, 'RDcS');
    if (opOutBuf) ExFreePoolWithTag(opOutBuf, 'RDoS');
    return status;
}

/* ---------- MRxLowIOSubmit[LOWIO_OP_READ] ----------
 *
 * rdbss has already locked the caller's buffer (RxLockUserBuffer) and
 * mapped it into system address space (RxNewMapUserBuffer called
 * MmGetSystemAddressForMdlSafe before the read was dispatched).  By the
 * time we land here, LowIoContext->ParamsFor.ReadWrite.Buffer is the
 * PMDL covering the destination pages and ByteCount is the request size
 * in bytes.  We cap per-upcall transfer at 1 MiB so the daemon's staging
 * pool allocation (OP_READ_OUT header + payload) stays bounded; larger
 * user reads loop over multiple OP_READ round-trips until the whole
 * range is filled or the daemon reports a short read (EOF).
 */
#ifndef SMB2RDR_READ_CHUNK_BYTES
#define SMB2RDR_READ_CHUNK_BYTES (1u << 20)
#endif

static NTSTATUS NTAPI
smb2rdr_Read(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    PLOWIO_CONTEXT LowIo = &RxContext->LowIoContext;
    PMDL mdl;
    PUCHAR dst;
    ULONG64 offset;
    ULONG   total;
    ULONG   done = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] no daemon handle\n");
        return STATUS_INVALID_HANDLE;
    }
    if (fcbExt->IsDirectory) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] on directory handle=0x%llx\n",
                fcbExt->DaemonFileHandle);
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    mdl    = LowIo->ParamsFor.ReadWrite.Buffer;
    offset = (ULONG64)LowIo->ParamsFor.ReadWrite.ByteOffset;
    total  = LowIo->ParamsFor.ReadWrite.ByteCount;

    if (total == 0) {
        RxContext->InformationToReturn = 0;
        RxContext->CurrentIrp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }
    if (mdl == NULL) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] NULL MDL\n");
        return STATUS_INVALID_USER_BUFFER;
    }

    dst = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (dst == NULL) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] MmGetSystemAddressForMdlSafe "
                "failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT("SMB2RDR: MRxLowIOSubmit[READ] fh=0x%llx offset=%llu len=%lu\n",
             fcbExt->DaemonFileHandle,
             (unsigned long long)offset, total);

    while (done < total) {
        OP_READ_IN  in;
        POP_READ_OUT ro;
        PUCHAR out;
        ULONG chunk, outCap, outActual = 0;
        ULONG bytesRead;
        NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
        NTSTATUS bridgeStatus;

        chunk = total - done;
        if (chunk > SMB2RDR_READ_CHUNK_BYTES)
            chunk = SMB2RDR_READ_CHUNK_BYTES;

        outCap = (ULONG)sizeof(OP_READ_OUT) + chunk;
        /* PagedPool is acceptable: the downcall copy runs at PASSIVE_LEVEL
         * inside SmbRdrDowncallIoctl, and the buffer is never touched
         * below DISPATCH_LEVEL.  Keeping it out of NonPaged avoids
         * pressuring the 1 MiB-per-read tag. */
        out = ExAllocatePoolWithTag(PagedPool, outCap, 'RDrS');
        if (out == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(&in, sizeof(in));
        in.FileHandle = fcbExt->DaemonFileHandle;
        in.Offset     = offset + done;
        in.Length     = chunk;

        bridgeStatus = SmbRdrIssueUpcall(
            SMB2D_OP_READ,
            &in, (ULONG)sizeof(in),
            out, outCap, &outActual,
            &daemonStatus,
            60 /* seconds */);

        if (bridgeStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] bridge failed 0x%08lx\n",
                    bridgeStatus);
            ExFreePoolWithTag(out, 'RDrS');
            if (done > 0) {
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_UNEXPECTED_NETWORK_ERROR;
            }
            break;
        }
        if (daemonStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] daemon failed 0x%08lx\n",
                    daemonStatus);
            ExFreePoolWithTag(out, 'RDrS');
            if (done > 0) {
                status = STATUS_SUCCESS;
            } else {
                status = daemonStatus;
            }
            break;
        }
        if (outActual < sizeof(OP_READ_OUT)) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] short reply outlen=%lu\n",
                    outActual);
            ExFreePoolWithTag(out, 'RDrS');
            if (done == 0)
                status = STATUS_UNEXPECTED_NETWORK_ERROR;
            break;
        }

        ro = (POP_READ_OUT)out;
        if (ro->BytesRead == 0) {
            /* End-of-file at current offset. */
            ExFreePoolWithTag(out, 'RDrS');
            break;
        }
        if (ro->BytesRead > chunk ||
            sizeof(OP_READ_OUT) + ro->BytesRead > outActual)
        {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[READ] bad reply br=%lu chunk=%lu "
                    "actual=%lu\n",
                    ro->BytesRead, chunk, outActual);
            ExFreePoolWithTag(out, 'RDrS');
            if (done == 0)
                status = STATUS_UNEXPECTED_NETWORK_ERROR;
            break;
        }

        bytesRead = ro->BytesRead;

        _SEH2_TRY {
            RtlCopyMemory(dst + done,
                          (PUCHAR)(ro + 1),
                          bytesRead);
        } _SEH2_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
            ExFreePoolWithTag(out, 'RDrS');
            status = STATUS_INVALID_USER_BUFFER;
            goto done_out;
        } _SEH2_END;

        DPRINT("SMB2RDR: Read chunk ok bytes=%lu total=%lu\n",
                 bytesRead, done + bytesRead);

        done += bytesRead;
        ExFreePoolWithTag(out, 'RDrS');

        if (bytesRead < chunk) {
            /* Short read — either EOF mid-chunk or server bounded us. */
            break;
        }
    }

done_out:
    /* Hand the final count back to rdbss.  A zero-byte completion is
     * STATUS_END_OF_FILE; a non-zero partial fill is STATUS_SUCCESS with
     * Information == done.  rdbss's RxLowIoReadShellCompletion remaps
     * the paging-IO case internally.  InformationToReturn and IoStatusBlock
     * share a union in RX_CONTEXT — setting the status block fills both. */
    if (done > 0)
        status = STATUS_SUCCESS;
    else if (status == STATUS_SUCCESS)
        status = STATUS_END_OF_FILE;

    RxContext->IoStatusBlock.Information = done;
    RxContext->IoStatusBlock.Status = status;
    return status;
}

/* ---------- MRxExtendForCache / MRxExtendForNonCache ----------
 *
 * rdbss invokes these during RxCommonWrite when the pending write would
 * extend the file past its current AllocationSize, and refuses to issue
 * the IRP if the slot returns STATUS_NOT_IMPLEMENTED.  The contract is
 * just "acknowledge the new end-of-file and hand back an allocation size
 * that's at least that large" — the actual on-disk extension happens
 * when smb2_pwrite() lands on the server.  We mirror the nfs41 miniport
 * here: set the FCB's StandardInfo if we tracked it (we don't yet), and
 * round the allocation size up by a small slack so we don't have to
 * re-visit this path on every byte.
 */
static ULONG NTAPI
smb2rdr_ExtendForCache(IN OUT PRX_CONTEXT RxContext,
                       IN OUT PLARGE_INTEGER NewFileSize,
                       OUT PLARGE_INTEGER NewAllocationSize)
{
    UNREFERENCED_PARAMETER(RxContext);

    NewAllocationSize->QuadPart = NewFileSize->QuadPart + 8192;
    return STATUS_SUCCESS;
}

/* ---------- MRxLowIOSubmit[LOWIO_OP_WRITE] ----------
 *
 * Mirror of the read path.  DO_DIRECT_IO on the device object means the
 * user buffer has already been probed+locked into an MDL by the time we
 * land here; LowIoContext->ParamsFor.ReadWrite.Buffer is that MDL.  We
 * copy out of the mapped system VA into a per-chunk OP_WRITE_IN payload
 * that gets shipped to the daemon inline.  Chunks are capped at 1 MiB so
 * the daemon's receive buffer (and its libsmb2 pwrite() scratch space)
 * stays bounded.  Short writes (server accepted < chunk) break the loop
 * and surface however many bytes actually made it; bridge/daemon errors
 * with a non-zero partial fill are also reported as success so rdbss
 * lets the caller retry at offset+done.
 */
#ifndef SMB2RDR_WRITE_CHUNK_BYTES
#define SMB2RDR_WRITE_CHUNK_BYTES (1u << 20)
#endif

static NTSTATUS NTAPI
smb2rdr_Write(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    PLOWIO_CONTEXT LowIo = &RxContext->LowIoContext;
    PMDL mdl;
    PUCHAR src;
    ULONG64 offset;
    ULONG   total;
    ULONG   done = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] no daemon handle\n");
        return STATUS_INVALID_HANDLE;
    }
    if (fcbExt->IsDirectory) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] on directory handle=0x%llx\n",
                fcbExt->DaemonFileHandle);
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    mdl    = LowIo->ParamsFor.ReadWrite.Buffer;
    offset = (ULONG64)LowIo->ParamsFor.ReadWrite.ByteOffset;
    total  = LowIo->ParamsFor.ReadWrite.ByteCount;

    if (total == 0) {
        RxContext->InformationToReturn = 0;
        RxContext->CurrentIrp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }
    if (mdl == NULL) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] NULL MDL\n");
        return STATUS_INVALID_USER_BUFFER;
    }

    src = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (src == NULL) {
        DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] MmGetSystemAddressForMdlSafe "
                "failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT("SMB2RDR: MRxLowIOSubmit[WRITE] fh=0x%llx offset=%llu len=%lu\n",
             fcbExt->DaemonFileHandle,
             (unsigned long long)offset, total);

    while (done < total) {
        POP_WRITE_IN in;
        OP_WRITE_OUT out;
        ULONG chunk, inLen, outActual = 0;
        NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
        NTSTATUS bridgeStatus;

        chunk = total - done;
        if (chunk > SMB2RDR_WRITE_CHUNK_BYTES)
            chunk = SMB2RDR_WRITE_CHUNK_BYTES;

        inLen = (ULONG)sizeof(OP_WRITE_IN) + chunk;
        /* PagedPool mirrors the READ path: upcall serialisation runs at
         * PASSIVE_LEVEL inside SmbRdrUpcallIoctl's caller (the daemon's
         * IOCTL thread), and the buffer is never touched below
         * DISPATCH_LEVEL. */
        in = ExAllocatePoolWithTag(PagedPool, inLen, 'WDrS');
        if (in == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(in, sizeof(*in));
        in->FileHandle = fcbExt->DaemonFileHandle;
        in->Offset     = offset + done;
        in->Length     = chunk;

        _SEH2_TRY {
            RtlCopyMemory((PUCHAR)(in + 1), src + done, chunk);
        } _SEH2_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
            ExFreePoolWithTag(in, 'WDrS');
            status = STATUS_INVALID_USER_BUFFER;
            goto done_out;
        } _SEH2_END;

        RtlZeroMemory(&out, sizeof(out));
        bridgeStatus = SmbRdrIssueUpcall(
            SMB2D_OP_WRITE,
            in, inLen,
            &out, (ULONG)sizeof(out), &outActual,
            &daemonStatus,
            60 /* seconds */);

        ExFreePoolWithTag(in, 'WDrS');

        if (bridgeStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] bridge failed 0x%08lx\n",
                    bridgeStatus);
            if (done > 0) {
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_UNEXPECTED_NETWORK_ERROR;
            }
            break;
        }
        if (daemonStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] daemon failed 0x%08lx\n",
                    daemonStatus);
            if (done > 0) {
                status = STATUS_SUCCESS;
            } else {
                status = daemonStatus;
            }
            break;
        }
        if (outActual < sizeof(out)) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] short reply outlen=%lu\n",
                    outActual);
            if (done == 0)
                status = STATUS_UNEXPECTED_NETWORK_ERROR;
            break;
        }

        if (out.BytesWritten == 0) {
            /* Server accepted nothing; stop to avoid an infinite loop. */
            break;
        }
        if (out.BytesWritten > chunk) {
            DPRINT1("SMB2RDR: MRxLowIOSubmit[WRITE] bad reply bw=%lu chunk=%lu\n",
                    out.BytesWritten, chunk);
            if (done == 0)
                status = STATUS_UNEXPECTED_NETWORK_ERROR;
            break;
        }

        DPRINT("SMB2RDR: Write chunk ok bytes=%lu total=%lu\n",
                 out.BytesWritten, done + out.BytesWritten);

        done += out.BytesWritten;

        if (out.BytesWritten < chunk) {
            /* Short write — surface a partial success to the caller so
             * it can decide whether to retry at offset+done. */
            break;
        }
    }

done_out:
    /* Hand the final count back to rdbss.  A zero-byte completion with a
     * non-success code propagates; otherwise success, and rdbss fills
     * IoStatusBlock.Information from RxContext->IoStatusBlock. */
    if (done > 0)
        status = STATUS_SUCCESS;

    RxContext->IoStatusBlock.Information = done;
    RxContext->IoStatusBlock.Status = status;
    return status;
}

/* ---------- MRxQueryFileInfo ----------
 *
 * NtQueryInformationFile lands here after rdbss has decoded the caller's
 * information class into RxContext->Info and mapped the user buffer into
 * Info.Buffer.  We issue a single OP_QUERY_FILE_INFO upcall so the daemon
 * can hand back a flat SMB2RDR_STAT (mirror of libsmb2's smb2_stat_64);
 * the kernel then synthesises each NT FILE_*_INFORMATION layout that
 * cmd.exe copy / shell consumers exercise.
 *
 * Info classes honoured:
 *   FileBasicInformation       - timestamps + attribute bits
 *   FileStandardInformation    - AllocationSize/EndOfFile/NumberOfLinks
 *   FileNetworkOpenInformation - flat composite cmd.exe issues first
 *   FileInternalInformation    - server file-id (smb2_ino)
 *   FileEaInformation          - EaSize = 0 (no EAs over SMB2 today)
 *   FileAllInformation         - aggregate of the above (partial)
 *
 * Other classes return STATUS_NOT_IMPLEMENTED or
 * STATUS_INVALID_INFO_CLASS so the caller can fall back to a more
 * minimal query.  Buffers smaller than the requested structure get
 * STATUS_BUFFER_OVERFLOW with InformationToReturn set to the full size.
 */

/* POSIX seconds -> NT FILETIME (100ns since 1601-01-01).  11644473600 is
 * the offset in seconds between the two epochs. */
static ULONGLONG
Smb2StatToFileTime(ULONGLONG sec, ULONGLONG nsec)
{
    return (sec + 11644473600ULL) * 10000000ULL + (nsec / 100ULL);
}

static VOID
Smb2FillBasicInfo(PFILE_BASIC_INFORMATION bi, const SMB2RDR_STAT *st)
{
    bi->CreationTime.QuadPart   =
        (LONGLONG)Smb2StatToFileTime(st->Btime, st->Btime_nsec);
    bi->LastAccessTime.QuadPart =
        (LONGLONG)Smb2StatToFileTime(st->Atime, st->Atime_nsec);
    bi->LastWriteTime.QuadPart  =
        (LONGLONG)Smb2StatToFileTime(st->Mtime, st->Mtime_nsec);
    bi->ChangeTime.QuadPart     =
        (LONGLONG)Smb2StatToFileTime(st->Ctime, st->Ctime_nsec);
    bi->FileAttributes = st->Attributes ? st->Attributes
                                        : FILE_ATTRIBUTE_NORMAL;
}

static VOID
Smb2FillStandardInfo(PFILE_STANDARD_INFORMATION si, const SMB2RDR_STAT *st)
{
    si->AllocationSize.QuadPart =
        (LONGLONG)((st->Size + 4095ULL) & ~4095ULL);
    si->EndOfFile.QuadPart      = (LONGLONG)st->Size;
    si->NumberOfLinks           = st->NLink ? st->NLink : 1;
    si->DeletePending           = FALSE;
    si->Directory               = (st->Attributes & FILE_ATTRIBUTE_DIRECTORY)
                                  ? TRUE : FALSE;
}

static VOID
Smb2FillNetworkOpenInfo(PFILE_NETWORK_OPEN_INFORMATION noi,
                        const SMB2RDR_STAT *st)
{
    noi->CreationTime.QuadPart   =
        (LONGLONG)Smb2StatToFileTime(st->Btime, st->Btime_nsec);
    noi->LastAccessTime.QuadPart =
        (LONGLONG)Smb2StatToFileTime(st->Atime, st->Atime_nsec);
    noi->LastWriteTime.QuadPart  =
        (LONGLONG)Smb2StatToFileTime(st->Mtime, st->Mtime_nsec);
    noi->ChangeTime.QuadPart     =
        (LONGLONG)Smb2StatToFileTime(st->Ctime, st->Ctime_nsec);
    noi->AllocationSize.QuadPart =
        (LONGLONG)((st->Size + 4095ULL) & ~4095ULL);
    noi->EndOfFile.QuadPart      = (LONGLONG)st->Size;
    noi->FileAttributes = st->Attributes ? st->Attributes
                                         : FILE_ATTRIBUTE_NORMAL;
}

static NTSTATUS NTAPI
smb2rdr_QueryFileInfo(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    FILE_INFORMATION_CLASS infoClass;
    PVOID userBuf;
    LONG userBufLen;
    OP_QUERY_FILE_INFO_IN in;
    OP_QUERY_FILE_INFO_OUT out;
    ULONG outActual = 0;
    NTSTATUS bridgeStatus;
    NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS status;
    ULONG needed = 0;

    if (RxContext->Info.Buffer == NULL)
        return STATUS_INVALID_USER_BUFFER;
    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0) {
        DPRINT1("SMB2RDR: MRxQueryFileInfo: no daemon handle\n");
        return STATUS_INVALID_HANDLE;
    }

    infoClass  = RxContext->Info.FileInformationClass;
    userBuf    = RxContext->Info.Buffer;
    userBufLen = RxContext->Info.LengthRemaining;

    if (userBufLen <= 0)
        return STATUS_BUFFER_TOO_SMALL;

    /* Classes we know are irrelevant for cmd.exe copy — fail them quickly
     * so rdbss/NtQueryInformationFile can fall back without round-tripping
     * through the daemon. */
    switch (infoClass) {
    case FileAlternateNameInformation:
    case FileStreamInformation:
    case FileCompressionInformation:
        DPRINT1("SMB2RDR: MRxQueryFileInfo class=%d returning NOT_IMPLEMENTED\n",
                (int)infoClass);
        return STATUS_NOT_IMPLEMENTED;
    case FileBasicInformation:
    case FileStandardInformation:
    case FileInternalInformation:
    case FileEaInformation:
    case FileNetworkOpenInformation:
    case FileAllInformation:
    case FileAttributeTagInformation:
        break;
    default:
        DPRINT1("SMB2RDR: MRxQueryFileInfo unhandled class=%d\n",
                (int)infoClass);
        return STATUS_INVALID_INFO_CLASS;
    }

    /* FileEaInformation doesn't need a round-trip — we never expose
     * extended attributes over SMB2, so the answer is always zero. */
    if (infoClass == FileEaInformation) {
        PFILE_EA_INFORMATION ei = (PFILE_EA_INFORMATION)userBuf;
        if ((ULONG)userBufLen < sizeof(FILE_EA_INFORMATION)) {
            RxContext->InformationToReturn = sizeof(FILE_EA_INFORMATION);
            return STATUS_BUFFER_OVERFLOW;
        }
        ei->EaSize = 0;
        RxContext->Info.LengthRemaining -= (LONG)sizeof(FILE_EA_INFORMATION);
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&out, sizeof(out));

    if (fcbExt->IsDirectory) {
        out.Stat.Type       = SMB2RDR_STAT_TYPE_DIRECTORY;
        out.Stat.NLink      = 1;
        out.Stat.Size       = 0;
        out.Stat.Attributes = FILE_ATTRIBUTE_DIRECTORY;
        goto HaveStat;
    }

    RtlZeroMemory(&in, sizeof(in));
    in.FileHandle = fcbExt->DaemonFileHandle;

    DPRINT("SMB2RDR: MRxQueryFileInfo class=%d fh=0x%llx buflen=%d\n",
             (int)infoClass, fcbExt->DaemonFileHandle, userBufLen);

    bridgeStatus = SmbRdrIssueUpcall(
        SMB2D_OP_QUERY_INFO,
        &in, (ULONG)sizeof(in),
        &out, (ULONG)sizeof(out), &outActual,
        &daemonStatus,
        30 /* seconds */);

    if (bridgeStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxQueryFileInfo bridge failed 0x%08lx\n",
                bridgeStatus);
        return STATUS_UNEXPECTED_NETWORK_ERROR;
    }
    if (daemonStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxQueryFileInfo daemon failed 0x%08lx\n",
                daemonStatus);
        return daemonStatus;
    }
    if (outActual < sizeof(out)) {
        DPRINT1("SMB2RDR: MRxQueryFileInfo short reply outlen=%lu\n",
                outActual);
        return STATUS_UNEXPECTED_NETWORK_ERROR;
    }

HaveStat:
    /* Stamp DIRECTORY if the daemon told us and the FCB agrees — old
     * smb2d builds may only set Type=1 without translating it to the NT
     * attribute bitmap. */
    if ((out.Stat.Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        fcbExt->IsDirectory)
    {
        out.Stat.Attributes |= FILE_ATTRIBUTE_DIRECTORY;
    }

    switch (infoClass) {
    case FileBasicInformation:
        needed = sizeof(FILE_BASIC_INFORMATION);
        if ((ULONG)userBufLen < needed) {
            RxContext->InformationToReturn = needed;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        Smb2FillBasicInfo((PFILE_BASIC_INFORMATION)userBuf, &out.Stat);
        RxContext->Info.LengthRemaining -= (LONG)needed;
        status = STATUS_SUCCESS;
        break;

    case FileStandardInformation:
        needed = sizeof(FILE_STANDARD_INFORMATION);
        if ((ULONG)userBufLen < needed) {
            RxContext->InformationToReturn = needed;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        Smb2FillStandardInfo((PFILE_STANDARD_INFORMATION)userBuf, &out.Stat);
        RxContext->Info.LengthRemaining -= (LONG)needed;
        status = STATUS_SUCCESS;
        break;

    case FileInternalInformation:
        needed = sizeof(FILE_INTERNAL_INFORMATION);
        if ((ULONG)userBufLen < needed) {
            RxContext->InformationToReturn = needed;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        ((PFILE_INTERNAL_INFORMATION)userBuf)->IndexNumber.QuadPart =
            (LONGLONG)out.Stat.Ino;
        RxContext->Info.LengthRemaining -= (LONG)needed;
        status = STATUS_SUCCESS;
        break;

    case FileNetworkOpenInformation:
        needed = sizeof(FILE_NETWORK_OPEN_INFORMATION);
        if ((ULONG)userBufLen < needed) {
            RxContext->InformationToReturn = needed;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        Smb2FillNetworkOpenInfo((PFILE_NETWORK_OPEN_INFORMATION)userBuf,
                                &out.Stat);
        RxContext->Info.LengthRemaining -= (LONG)needed;
        status = STATUS_SUCCESS;
        break;

    case FileAttributeTagInformation:
        needed = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
        if ((ULONG)userBufLen < needed) {
            RxContext->InformationToReturn = needed;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION ati =
                (PFILE_ATTRIBUTE_TAG_INFORMATION)userBuf;
            ati->FileAttributes = out.Stat.Attributes
                ? out.Stat.Attributes : FILE_ATTRIBUTE_NORMAL;
            ati->ReparseTag     = 0;
        }
        RxContext->Info.LengthRemaining -= (LONG)needed;
        status = STATUS_SUCCESS;
        break;

    case FileAllInformation: {
        /* FILE_ALL_INFORMATION is a flat struct.  We fill the leading four
         * fields (Basic/Standard/Internal/Ea); Access/Position/Mode/Alignment
         * are left zero, which is valid for a freshly-opened handle, and
         * NameInformation is not populated (callers that need the filename
         * round-trip through FileNameInformation separately). */
        needed = sizeof(FILE_ALL_INFORMATION);
        if ((ULONG)userBufLen < sizeof(FILE_BASIC_INFORMATION)) {
            /* Not enough room for even the first field — bail out. */
            RxContext->InformationToReturn = needed;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        {
            PFILE_ALL_INFORMATION ai = (PFILE_ALL_INFORMATION)userBuf;
            ULONG fill = ((ULONG)userBufLen >= needed) ? needed
                                                       : (ULONG)userBufLen;
            RtlZeroMemory(userBuf, fill);
            Smb2FillBasicInfo(&ai->BasicInformation, &out.Stat);
            if (fill >= (ULONG)(sizeof(FILE_BASIC_INFORMATION)
                              + sizeof(FILE_STANDARD_INFORMATION)))
                Smb2FillStandardInfo(&ai->StandardInformation, &out.Stat);
            if (fill >= (ULONG)(sizeof(FILE_BASIC_INFORMATION)
                              + sizeof(FILE_STANDARD_INFORMATION)
                              + sizeof(FILE_INTERNAL_INFORMATION)))
                ai->InternalInformation.IndexNumber.QuadPart =
                    (LONGLONG)out.Stat.Ino;
            /* EaInformation.EaSize already zeroed above.  Access/Position/
             * Mode/Alignment left zero. */
            if ((ULONG)userBufLen < needed) {
                RxContext->InformationToReturn = needed;
                RxContext->Info.LengthRemaining -= (LONG)fill;
                status = STATUS_BUFFER_OVERFLOW;
            } else {
                RxContext->Info.LengthRemaining -= (LONG)needed;
                status = STATUS_SUCCESS;
            }
        }
        break;
    }

    default:
        /* Shouldn't reach here — earlier switch filters unknown classes. */
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    DPRINT("SMB2RDR: QueryFileInfo class=%d size=%llu attr=0x%lx -> 0x%08lx\n",
             (int)infoClass,
             (unsigned long long)out.Stat.Size,
             out.Stat.Attributes, status);
    return status;
}

/* ---------- MRxFlush ----------
 *
 * FlushFileBuffers / NtFlushBuffersFile / cache-manager flushes land here.
 * We ship an OP_FSYNC upcall so the daemon can call libsmb2's smb2_fsync()
 * and have the server force an on-disk flush of the open handle.  Nothing
 * to flush (handle already closed, or opened as a directory) is a trivial
 * success — directories don't have SMB2 flush semantics, and a missing
 * daemon handle means there's no dirty server-side state to begin with.
 */
static NTSTATUS NTAPI
smb2rdr_Flush(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    OP_FSYNC_IN in;
    ULONG outActual = 0;
    NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS bridgeStatus;

    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0) {
        /* Nothing open on the daemon side — nothing to flush. */
        return STATUS_SUCCESS;
    }
    if (fcbExt->IsDirectory) {
        /* SMB2 has no directory-flush op; the server has nothing to sync. */
        return STATUS_SUCCESS;
    }

    DPRINT("SMB2RDR: MRxFlush fh=0x%llx\n",
             fcbExt->DaemonFileHandle);

    RtlZeroMemory(&in, sizeof(in));
    in.FileHandle = fcbExt->DaemonFileHandle;

    bridgeStatus = SmbRdrIssueUpcall(
        SMB2D_OP_FSYNC,
        &in, (ULONG)sizeof(in),
        NULL, 0, &outActual,
        &daemonStatus,
        30 /* seconds */);

    if (bridgeStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxFlush bridge failed 0x%08lx\n",
                bridgeStatus);
        return bridgeStatus;
    }
    if (daemonStatus != STATUS_SUCCESS) {
        DPRINT1("SMB2RDR: MRxFlush daemon failed 0x%08lx\n",
                daemonStatus);
        return daemonStatus;
    }

    DPRINT("SMB2RDR: Flush ok fh=0x%llx\n",
             fcbExt->DaemonFileHandle);
    return STATUS_SUCCESS;
}

/* ---------- MRxSetFileInfo ----------
 *
 * NtSetInformationFile lands here after rdbss has routed the caller's
 * class through RxCommonSetInformation -> {RxSetDispositionInfo,
 * RxSetRenameInfo, ...} -> RxpSetInfoMiniRdr.  We implement the subset
 * cmd.exe + Explorer exercise on a mounted SMB2 share:
 *
 *   FileRenameInformation       (10) - smb2_rename on the share context
 *   FileDispositionInformation  (13) - stash DeleteOnClose on the FCB,
 *                                      trigger smb2_unlink/smb2_rmdir from
 *                                      MRxCloseSrvOpen after smb2_close
 *   FileEndOfFileInformation    (20) - smb2_ftruncate on the file handle
 *   FileAllocationInformation   (19) - acknowledged no-op (SMB2 has no
 *                                      preallocation wire op)
 *
 * Other classes return STATUS_NOT_IMPLEMENTED so higher layers can fall
 * back or report a meaningful failure to user code.  FileBasicInformation
 * is left unimplemented today because Explorer will accept failure there
 * without blocking the copy/rename/delete dataflow; wiring it is a P2 item.
 */
static NTSTATUS NTAPI
smb2rdr_SetFileInfo(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PMRX_SRV_OPEN SrvOpen = RxContext->pRelevantSrvOpen;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    PSMB2RDR_V_NET_ROOT_EXTENSION vNetExt =
        (SrvOpen != NULL) ? Smb2RdrGetVNetRootExtension(SrvOpen->pVNetRoot)
                          : NULL;
    FILE_INFORMATION_CLASS infoClass;
    PVOID userBuf;
    LONG userBufLen;
    NTSTATUS status;

    if (fcbExt == NULL || fcbExt->DaemonFileHandle == 0) {
        DPRINT1("SMB2RDR: MRxSetFileInfo no daemon handle\n");
        return STATUS_INVALID_HANDLE;
    }

    infoClass  = RxContext->Info.FileInformationClass;
    userBuf    = RxContext->Info.Buffer;
    userBufLen = (LONG)RxContext->Info.Length;

    DPRINT("SMB2RDR: MRxSetFileInfo class=%d fh=0x%llx buflen=%d\n",
             (int)infoClass, fcbExt->DaemonFileHandle, userBufLen);

    switch (infoClass) {
    case FileDispositionInformation: {
        PFILE_DISPOSITION_INFORMATION dinfo;

        if (userBuf == NULL || userBufLen < (LONG)sizeof(*dinfo))
            return STATUS_INFO_LENGTH_MISMATCH;
        dinfo = (PFILE_DISPOSITION_INFORMATION)userBuf;
        fcbExt->DeleteOnClose = dinfo->DeleteFile ? TRUE : FALSE;
        DPRINT("SMB2RDR: SetFileInfo Disposition DeleteFile=%u\n",
                 dinfo->DeleteFile);
        /* Actual unlink fires in MRxCloseSrvOpen after the daemon close
         * has released the server-side handle.  NT semantics: disposition
         * is an FCB-level flag, deletion is deferred to last-close. */
        return STATUS_SUCCESS;
    }

    case FileAllocationInformation: {
        /* SMB2 has no preallocation op, and Windows is happy to treat the
         * hint as advisory when the underlying FS doesn't support it.
         * Accepting with no round-trip matches how most non-NTFS file
         * systems handle this class. */
        DPRINT("SMB2RDR: SetFileInfo Allocation accepted (no-op)\n");
        return STATUS_SUCCESS;
    }

    case FileEndOfFileInformation: {
        PFILE_END_OF_FILE_INFORMATION einfo;
        OP_FTRUNCATE_IN in;
        ULONG outActual = 0;
        NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
        NTSTATUS bridgeStatus;

        if (userBuf == NULL || userBufLen < (LONG)sizeof(*einfo))
            return STATUS_INFO_LENGTH_MISMATCH;
        einfo = (PFILE_END_OF_FILE_INFORMATION)userBuf;

        if (fcbExt->IsDirectory) {
            DPRINT1("SMB2RDR: SetFileInfo EOF on directory handle=0x%llx\n",
                    fcbExt->DaemonFileHandle);
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (einfo->EndOfFile.QuadPart < 0)
            return STATUS_INVALID_PARAMETER;

        RtlZeroMemory(&in, sizeof(in));
        in.FileHandle = fcbExt->DaemonFileHandle;
        in.NewSize    = (ULONGLONG)einfo->EndOfFile.QuadPart;

        DPRINT("SMB2RDR: SetFileInfo EOF fh=0x%llx new_size=%llu\n",
                 fcbExt->DaemonFileHandle,
                 (unsigned long long)in.NewSize);

        bridgeStatus = SmbRdrIssueUpcall(
            SMB2D_OP_FTRUNCATE,
            &in, (ULONG)sizeof(in),
            NULL, 0, &outActual,
            &daemonStatus,
            30 /* seconds */);

        if (bridgeStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: SetFileInfo EOF bridge failed 0x%08lx\n",
                    bridgeStatus);
            return STATUS_UNEXPECTED_NETWORK_ERROR;
        }
        if (daemonStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: SetFileInfo EOF daemon failed 0x%08lx\n",
                    daemonStatus);
            return daemonStatus;
        }
        DPRINT("SMB2RDR: SetFileInfo EOF ok\n");
        return STATUS_SUCCESS;
    }

    case FileRenameInformation: {
        PFILE_RENAME_INFORMATION rinfo;
        PWCHAR newPathChars;
        USHORT newPathBytes;
        ULONG totalLen;
        POP_RENAME_IN renameIn = NULL;
        ULONG outActual = 0;
        NTSTATUS daemonStatus = STATUS_UNSUCCESSFUL;
        NTSTATUS bridgeStatus;

        if (vNetExt == NULL || vNetExt->DaemonHandle == 0)
            return STATUS_DEVICE_NOT_CONNECTED;
        if (userBuf == NULL ||
            userBufLen < (LONG)FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName))
        {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        rinfo = (PFILE_RENAME_INFORMATION)userBuf;
        if (rinfo->RootDirectory != NULL) {
            /* rdbss only supplies paths through the OpenTargetDir route,
             * which never carries a RootDirectory handle.  If we get one,
             * it's a pass-through from user space we can't honour. */
            DPRINT1("SMB2RDR: SetFileInfo Rename rejected: RootDirectory!=NULL\n");
            return STATUS_INVALID_PARAMETER;
        }

        newPathChars = rinfo->FileName;
        if (rinfo->FileNameLength > 0xFFFF)
            return STATUS_INVALID_PARAMETER;
        newPathBytes = (USHORT)rinfo->FileNameLength;

        /* RxSetRenameInfo rewrites Info.Buffer so FileName holds the
         * OpenTargetDir FCB's FcbTableEntry.Path, which always starts with
         * a single leading backslash relative to the share root.  Strip
         * that single slash to match the OP_CREATE path format the daemon
         * already understands. */
        if (newPathBytes >= sizeof(WCHAR) && newPathChars[0] == L'\\') {
            newPathChars++;
            newPathBytes -= sizeof(WCHAR);
        }
        if (newPathBytes == 0) {
            DPRINT1("SMB2RDR: SetFileInfo Rename empty dest\n");
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (fcbExt->Path.Buffer == NULL || fcbExt->Path.Length == 0) {
            DPRINT1("SMB2RDR: SetFileInfo Rename source-path missing\n");
            return STATUS_INVALID_PARAMETER;
        }

        totalLen = (ULONG)sizeof(OP_RENAME_IN)
                 + (ULONG)fcbExt->Path.Length
                 + (ULONG)newPathBytes;
        renameIn = ExAllocatePoolWithTag(NonPagedPool, totalLen, 'RNcS');
        if (renameIn == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(renameIn, sizeof(OP_RENAME_IN));
        renameIn->VNetHandle      = vNetExt->DaemonHandle;
        renameIn->ReplaceIfExists = rinfo->ReplaceIfExists ? 1u : 0u;
        renameIn->OldPathLen      = fcbExt->Path.Length;
        renameIn->NewPathLen      = newPathBytes;
        RtlCopyMemory((PUCHAR)renameIn + sizeof(OP_RENAME_IN),
                      fcbExt->Path.Buffer, fcbExt->Path.Length);
        RtlCopyMemory((PUCHAR)renameIn + sizeof(OP_RENAME_IN)
                              + fcbExt->Path.Length,
                      newPathChars, newPathBytes);

        DPRINT("SMB2RDR: SetFileInfo Rename old=%wZ new_bytes=%u\n",
                 &fcbExt->Path, (unsigned)newPathBytes);

        bridgeStatus = SmbRdrIssueUpcall(
            SMB2D_OP_RENAME,
            renameIn, totalLen,
            NULL, 0, &outActual,
            &daemonStatus,
            30 /* seconds */);

        ExFreePoolWithTag(renameIn, 'RNcS');

        if (bridgeStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: SetFileInfo Rename bridge failed 0x%08lx\n",
                    bridgeStatus);
            return STATUS_UNEXPECTED_NETWORK_ERROR;
        }
        if (daemonStatus != STATUS_SUCCESS) {
            DPRINT1("SMB2RDR: SetFileInfo Rename daemon failed 0x%08lx\n",
                    daemonStatus);
            return daemonStatus;
        }

        /* Update the cached FCB path so a follow-up rename or delete sees
         * the new name rather than the pre-rename one.  Best-effort: if
         * the realloc fails we leave the stale name in place and the next
         * upcall will surface STATUS_OBJECT_NAME_NOT_FOUND, which is
         * recoverable by the caller. */
        {
            PWCHAR pathCopy =
                ExAllocatePoolWithTag(PagedPool, newPathBytes, 'pFcS');
            if (pathCopy != NULL) {
                RtlCopyMemory(pathCopy, newPathChars, newPathBytes);
                if (fcbExt->Path.Buffer != NULL)
                    ExFreePoolWithTag(fcbExt->Path.Buffer, 'pFcS');
                fcbExt->Path.Buffer        = pathCopy;
                fcbExt->Path.Length        = newPathBytes;
                fcbExt->Path.MaximumLength = newPathBytes;
            }
        }

        DPRINT("SMB2RDR: SetFileInfo Rename ok\n");
        return STATUS_SUCCESS;
    }

    case FileBasicInformation:
    case FileValidDataLengthInformation:
    case FileShortNameInformation:
    case FileLinkInformation:
    case FilePositionInformation:
    case FilePipeInformation:
    case FilePipeLocalInformation:
    case FilePipeRemoteInformation:
        /* Known NT classes we don't translate today.  STATUS_NOT_IMPLEMENTED
         * lets rdbss surface a clean failure upstream. */
        DPRINT1("SMB2RDR: MRxSetFileInfo class=%d NOT_IMPLEMENTED\n",
                (int)infoClass);
        status = STATUS_NOT_IMPLEMENTED;
        break;

    default:
        DPRINT1("SMB2RDR: MRxSetFileInfo unknown class=%d\n",
                (int)infoClass);
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return status;
}

/* ---------- ops table ---------- */

static NTSTATUS
smb2rdr_init_ops(void)
{
    ZeroAndInitializeNodeType(&smb2rdr_ops, RDBSS_NTC_MINIRDR_DISPATCH,
                              sizeof(MINIRDR_DISPATCH));

    smb2rdr_ops.MRxFlags = (RDBSS_MANAGE_NET_ROOT_EXTENSION
                          | RDBSS_MANAGE_V_NET_ROOT_EXTENSION
                          | RDBSS_MANAGE_FCB_EXTENSION
                          | RDBSS_MANAGE_FOBX_EXTENSION);

    smb2rdr_ops.MRxSrvCallSize  = 0;
    smb2rdr_ops.MRxNetRootSize  = 0;
    smb2rdr_ops.MRxVNetRootSize = sizeof(SMB2RDR_V_NET_ROOT_EXTENSION);
    smb2rdr_ops.MRxFcbSize      = sizeof(SMB2RDR_FCB_EXTENSION);
    smb2rdr_ops.MRxFobxSize     = 0;

    smb2rdr_ops.MRxCancel = NULL;

    smb2rdr_ops.MRxStart                = smb2rdr_Start;
    smb2rdr_ops.MRxStop                 = smb2rdr_Stop;
    smb2rdr_ops.MRxDevFcbXXXControlFile = smb2rdr_DevFcbXXXControlFile;

    smb2rdr_ops.MRxCreateSrvCall       = smb2rdr_CreateSrvCall;
    smb2rdr_ops.MRxSrvCallWinnerNotify = smb2rdr_SrvCallWinnerNotify;
    smb2rdr_ops.MRxCreateVNetRoot      = smb2rdr_CreateVNetRoot;
    smb2rdr_ops.MRxExtractNetRootName  = smb2rdr_ExtractNetRootName;
    smb2rdr_ops.MRxFinalizeSrvCall     = smb2rdr_FinalizeSrvCall;
    smb2rdr_ops.MRxFinalizeNetRoot     = smb2rdr_FinalizeNetRoot;
    smb2rdr_ops.MRxFinalizeVNetRoot    = smb2rdr_FinalizeVNetRoot;

    smb2rdr_ops.MRxCreate                     = smb2rdr_Create;
    smb2rdr_ops.MRxCollapseOpen               = smb2rdr_CollapseOpen;
    smb2rdr_ops.MRxShouldTryToCollapseThisOpen = smb2rdr_ShouldTryToCollapseThisOpen;
    smb2rdr_ops.MRxCloseSrvOpen      = smb2rdr_CloseSrvOpen;
    smb2rdr_ops.MRxFlush             = smb2rdr_Flush;
    smb2rdr_ops.MRxDeallocateForFcb  = smb2rdr_DeallocateForFcb;
    smb2rdr_ops.MRxDeallocateForFobx = smb2rdr_DeallocateForFobx;

    smb2rdr_ops.MRxQueryDirectory    = smb2rdr_QueryDirectory;
    smb2rdr_ops.MRxQueryVolumeInfo   = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxQueryFileInfo     = smb2rdr_QueryFileInfo;
    smb2rdr_ops.MRxSetFileInfo       = smb2rdr_SetFileInfo;

    smb2rdr_ops.MRxLowIOSubmit[LOWIO_OP_READ]   = smb2rdr_Read;
    smb2rdr_ops.MRxLowIOSubmit[LOWIO_OP_WRITE]  = smb2rdr_Write;

    smb2rdr_ops.MRxExtendForCache    = smb2rdr_ExtendForCache;
    smb2rdr_ops.MRxExtendForNonCache = smb2rdr_ExtendForCache;

    smb2rdr_ops.MRxTruncate       = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxZeroExtend     = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxAreFilesAliased = smb2rdr_AreFilesAliased;
    smb2rdr_ops.MRxQueryQuotaInfo = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxSetQuotaInfo   = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxSetVolumeInfo  = smb2rdr_Unimplemented;

    return STATUS_SUCCESS;
}

/* ---------- dispatch shim ---------- */

NTSTATUS NTAPI
smb2rdr_FsdDispatch(PDEVICE_OBJECT dev, PIRP Irp)
{
    if (dev != (PDEVICE_OBJECT)smb2rdr_dev) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return RxFsdDispatch((PRDBSS_DEVICE_OBJECT)dev, Irp);
}

/* ---------- driver lifecycle ---------- */

VOID NTAPI
smb2rdr_driver_unload(PDRIVER_OBJECT drv)
{
    PRX_CONTEXT RxContext;
    UNICODE_STRING shadow_name;

    SmbRdrShutdownUpcall();

    RxContext = RxCreateRxContext(NULL, smb2rdr_dev, RX_CONTEXT_FLAG_IN_FSP);
    if (RxContext) {
        RxStopMinirdr(RxContext, &RxContext->PostRequest);
        RxDereferenceAndDeleteRxContext(RxContext);
    }

    RtlInitUnicodeString(&shadow_name, SMB2RDR_SHADOW_DEVICE_NAME);
    IoDeleteSymbolicLink(&shadow_name);

    RxUnload(drv);
}

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    NTSTATUS status;
    UNICODE_STRING dev_name, shadow_name;
    PSMB2RDR_DEVICE_EXTENSION dev_exts;
    ULONG flags = 0;
    ULONG i;

    status = RxDriverEntry(drv, path);
    if (status != STATUS_SUCCESS)
        return status;

    SmbRdrInitUpcall();

    status = smb2rdr_init_ops();
    if (status != STATUS_SUCCESS)
        return status;

    RtlInitUnicodeString(&dev_name, SMB2RDR_DEVICE_NAME);
    SetFlag(flags, RX_REGISTERMINI_FLAG_DONT_PROVIDE_MAILSLOTS);

    status = RxRegisterMinirdr(&smb2rdr_dev, drv, &smb2rdr_ops, flags,
                               &dev_name,
                               sizeof(SMB2RDR_DEVICE_EXTENSION),
                               FILE_DEVICE_NETWORK_FILE_SYSTEM,
                               FILE_REMOTE_DEVICE);
    if (status != STATUS_SUCCESS)
        return status;

    dev_exts = (PSMB2RDR_DEVICE_EXTENSION)
        ((PBYTE)smb2rdr_dev + sizeof(RDBSS_DEVICE_OBJECT));
    RxDefineNode(dev_exts, SMB2RDR_DEVICE_EXTENSION);
    dev_exts->DeviceObject = smb2rdr_dev;

    RtlInitUnicodeString(&shadow_name, SMB2RDR_SHADOW_DEVICE_NAME);
    status = IoCreateSymbolicLink(&shadow_name, &dev_name);
    if (status != STATUS_SUCCESS) {
        RxUnregisterMinirdr(smb2rdr_dev);
        return status;
    }

    drv->DriverUnload = smb2rdr_driver_unload;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = (PDRIVER_DISPATCH)smb2rdr_FsdDispatch;

    /* IoCreateDevice (via RxRegisterMinirdr) left DO_DEVICE_INITIALIZING set
     * on our device; the I/O manager normally clears it only after
     * DriverEntry returns.  Clear it now so MUP's FSCTL_MUP_REGISTER_PROVIDER
     * -> NtOpenFile(\Device\smb2rdr) the rdbss start path triggers can reach
     * IopCheckDeviceAndDriver without tripping the "fix its AddDevice
     * routine" rejection in iomgr/file.c. */
    ClearFlag(smb2rdr_dev->DeviceObject.Flags, DO_DEVICE_INITIALIZING);

    /* Direct-I/O transfer mode: NtReadFile / NtWriteFile / NtQueryDirectoryFile
     * construct and probe+lock the user buffer's MDL in the originating
     * caller's thread context before the IRP reaches us.  Without this flag
     * Irp->MdlAddress arrives NULL for those major codes and rdbss's
     * RxLockUserBuffer ends up calling MmProbeAndLockPages on its own,
     * from whatever context the FSD happens to be running in.  That is how
     * every production Windows mini-redirector is wired and mirrors the
     * setup MUP uses for its own redirected-file device objects. */
    SetFlag(smb2rdr_dev->DeviceObject.Flags, DO_DIRECT_IO);

    /* Self-trigger the rdbss start handshake so FsRtlRegisterUncProvider
     * runs and smb2rdr shows up as a UNC provider to MUP without waiting
     * for a usermode daemon to IOCTL us.  RX_CONTEXT_FLAG_IN_FSP bypasses
     * RxStartMinirdr's auto-post; RX_CONTEXT_FLAG_WAIT lets it acquire
     * RxData.Resource + the prefix table lock synchronously. */
    {
        PRX_CONTEXT StartCtx;
        BOOLEAN PostRequest = FALSE;
        NTSTATUS StartStatus;
        ULONG CtxFlags = RX_CONTEXT_FLAG_IN_FSP | RX_CONTEXT_FLAG_WAIT;

        StartCtx = RxCreateRxContext(NULL, smb2rdr_dev, CtxFlags);
        if (StartCtx != NULL) {
            StartStatus = RxStartMinirdr(StartCtx, &PostRequest);
            DPRINT1("SMB2RDR: self-start RxStartMinirdr -> 0x%08lx (post=%u)\n",
                    StartStatus, PostRequest);
            RxDereferenceAndDeleteRxContext(StartCtx);
        } else {
            DPRINT1("SMB2RDR: self-start RxCreateRxContext returned NULL\n");
        }
    }

    return STATUS_SUCCESS;
}
