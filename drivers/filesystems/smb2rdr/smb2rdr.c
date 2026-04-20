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
 * underlying libsmb2 smb2fh/smb2dir.  One entry per FCB is sufficient for
 * the current non-collapsing open model: every CreateFile() produces a
 * fresh FCB (MRxShouldTryToCollapseThisOpen returns NOT_IMPLEMENTED), so
 * the FCB extension cleanly maps 1:1 to a daemon handle for the lifetime
 * of the open chain.  FOBX extension is unused today; when sharing opens
 * across collapsed FCBs is introduced, the per-handle state moves there. */
typedef struct _SMB2RDR_FCB_EXTENSION {
    ULONG64 DaemonFileHandle;   /* 0 if no daemon-side open */
    BOOLEAN IsDirectory;
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
 * FCB — there's nothing dynamic here for us to release.  MRxCloseSrvOpen
 * has already told the daemon to drop its libsmb2 handle, so the slot
 * sits at DaemonFileHandle == 0 by the time we get here. */
NTSTATUS NTAPI
smb2rdr_DeallocateForFcb(IN OUT PMRX_FCB pFcb)
{
    UNREFERENCED_PARAMETER(pFcb);
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
        DbgPrint("SMB2RDR: DevFcbXXXControlFile unhandled ioctl=0x%08lx\n",
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

static NTSTATUS
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

    DPRINT1("SMB2RDR: MRxCreate path=\"%wZ\" opts=0x%lx disp=%lu "
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

    RxContext->Create.ReturnedCreateInformation = out.Information;
#ifndef __REACTOS__
    RxContext->CurrentIrp->IoStatus.Information = out.Information;
#endif

    status = STATUS_SUCCESS;
    RxContext->CurrentIrp->IoStatus.Status = status;

    DPRINT1("SMB2RDR: MRxCreate success fcb_handle=0x%llx info=%lu "
            "is_dir=%lu\n",
            out.FileHandle, out.Information, out.IsDirectory);
    return status;
}

static NTSTATUS
smb2rdr_CloseSrvOpen(IN OUT PRX_CONTEXT RxContext)
{
    PMRX_FCB Fcb = RxContext->pFcb;
    PSMB2RDR_FCB_EXTENSION fcbExt = Smb2RdrGetFcbExtension(Fcb);
    ULONG64 handle;
    BOOLEAN isDir;
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
    /* Clear the slot up-front so a racing finalize doesn't double-close
     * even if the upcall path bounces (timeout, bridge not ready, etc.). */
    fcbExt->DaemonFileHandle = 0;

    DPRINT1("SMB2RDR: MRxCloseSrvOpen handle=0x%llx is_dir=%u\n",
            handle, isDir);

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
static NTSTATUS
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

    DPRINT1("SMB2RDR: MRxQueryDirectory handle=0x%llx class=%d buf=%d "
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

    DPRINT1("SMB2RDR: MRxQueryDirectory ok bytes=%lu entries=%lu flags=0x%lx\n",
            opOut->BytesWritten, opOut->EntryCount, opOut->Flags);

    status = STATUS_SUCCESS;

out:
    if (opIn) ExFreePoolWithTag(opIn, 'RDcS');
    if (opOutBuf) ExFreePoolWithTag(opOutBuf, 'RDoS');
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
    smb2rdr_ops.MRxCollapseOpen               = smb2rdr_Unimplemented;
    /* Returning failure here tells rdbss "don't collapse, open fresh".
     * rdbss requires this slot non-NULL or asserts in RxSearchForCollapsibleOpen. */
    smb2rdr_ops.MRxShouldTryToCollapseThisOpen = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxCloseSrvOpen      = smb2rdr_CloseSrvOpen;
    smb2rdr_ops.MRxFlush             = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxDeallocateForFcb  = smb2rdr_DeallocateForFcb;
    smb2rdr_ops.MRxDeallocateForFobx = smb2rdr_DeallocateForFobx;

    smb2rdr_ops.MRxQueryDirectory    = smb2rdr_QueryDirectory;
    smb2rdr_ops.MRxQueryVolumeInfo   = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxQueryFileInfo     = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxSetFileInfo       = smb2rdr_Unimplemented;

    smb2rdr_ops.MRxLowIOSubmit[LOWIO_OP_READ]   = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxLowIOSubmit[LOWIO_OP_WRITE]  = smb2rdr_Unimplemented;

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
