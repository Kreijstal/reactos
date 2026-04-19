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
 * daemon IOCTL.  MRxCreateSrvCall is the hook point where the future SMB2
 * server-negotiation upcall will land; today it logs the server name and
 * fails the calldown with STATUS_NOT_IMPLEMENTED.
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

/* ---------- connection-bringup stubs ----------
 *
 * These stubs exist only so that rdbss has a non-NULL entry in each MRx slot
 * it touches while canonicalising a UNC path.  No libsmb2 work happens here;
 * the usermode daemon's upcall will land in MRxCreateSrvCall as the hook
 * point.  Every callback returns STATUS_NOT_IMPLEMENTED (or STATUS_SUCCESS
 * where rdbss treats failure as fatal to the driver) after logging. */

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

NTSTATUS NTAPI
smb2rdr_CreateSrvCall(IN OUT PMRX_SRV_CALL SrvCall,
                      IN OUT PMRX_SRVCALL_CALLBACK_CONTEXT CallbackContext)
{
    /* This is the entry point the usermode SMB2 daemon will upcall into
     * once libsmb2 is wired up.  For now we log the server name, fail the
     * calldown with STATUS_NOT_IMPLEMENTED, and let rdbss unwind. */
    PMRX_SRVCALLDOWN_STRUCTURE Calldown;

    DPRINT1("SMB2RDR: MRxCreateSrvCall name=%wZ\n", SrvCall->pSrvCallName);

    Calldown = (PMRX_SRVCALLDOWN_STRUCTURE)CallbackContext->SrvCalldownStructure;
    CallbackContext->Status = STATUS_NOT_IMPLEMENTED;
    CallbackContext->RecommunicateContext = NULL;
    Calldown->CallBack(CallbackContext);

    /* rdbss requires MRxCreateSrvCall to return STATUS_PENDING on success
     * or failure; the real status travels via CallbackContext->Status. */
    return STATUS_PENDING;
}

NTSTATUS NTAPI
smb2rdr_CreateVNetRoot(IN OUT PMRX_CREATENETROOT_CONTEXT Context)
{
    DPRINT1("SMB2RDR: MRxCreateVNetRoot\n");
    Context->VirtualNetRootStatus = STATUS_NOT_IMPLEMENTED;
    Context->NetRootStatus = STATUS_NOT_IMPLEMENTED;
    Context->Callback(Context);
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
    UNREFERENCED_PARAMETER(VNetRoot);
    UNREFERENCED_PARAMETER(Force);
    DPRINT1("SMB2RDR: FinalizeVNetRoot\n");
    return STATUS_SUCCESS;
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
    smb2rdr_ops.MRxVNetRootSize = 0;
    smb2rdr_ops.MRxFcbSize      = 0;
    smb2rdr_ops.MRxFobxSize     = 0;

    smb2rdr_ops.MRxCancel = NULL;

    smb2rdr_ops.MRxStart                = smb2rdr_Start;
    smb2rdr_ops.MRxStop                 = smb2rdr_Stop;
    smb2rdr_ops.MRxDevFcbXXXControlFile = smb2rdr_Unimplemented;

    smb2rdr_ops.MRxCreateSrvCall       = smb2rdr_CreateSrvCall;
    smb2rdr_ops.MRxSrvCallWinnerNotify = NULL;
    smb2rdr_ops.MRxCreateVNetRoot      = smb2rdr_CreateVNetRoot;
    smb2rdr_ops.MRxExtractNetRootName  = smb2rdr_ExtractNetRootName;
    smb2rdr_ops.MRxFinalizeSrvCall     = smb2rdr_FinalizeSrvCall;
    smb2rdr_ops.MRxFinalizeNetRoot     = smb2rdr_FinalizeNetRoot;
    smb2rdr_ops.MRxFinalizeVNetRoot    = smb2rdr_FinalizeVNetRoot;

    smb2rdr_ops.MRxCreate            = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxCloseSrvOpen      = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxFlush             = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxDeallocateForFcb  = smb2rdr_Unimplemented;
    smb2rdr_ops.MRxDeallocateForFobx = smb2rdr_Unimplemented;

    smb2rdr_ops.MRxQueryDirectory    = smb2rdr_Unimplemented;
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
