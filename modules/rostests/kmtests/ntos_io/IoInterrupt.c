/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite Interrupt test
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define CheckSpinLock(Lock, Locked) do                  \
{                                                       \
    if (KmtIsMultiProcessorBuild)                       \
        ok_eq_ulongptr(*(Lock), (Locked) != 0);         \
    else if (KmtIsCheckedBuild)                         \
        ok_eq_bool(*(Lock) != 0, (Locked) != 0);        \
    else                                                \
        ok_eq_ulongptr(*(Lock), 0);                     \
} while (0)

typedef struct
{
    BOOLEAN ReturnValue;
    KIRQL ExpectedIrql;
    PKINTERRUPT Interrupt;
} TEST_CONTEXT, *PTEST_CONTEXT;

static KSYNCHRONIZE_ROUTINE SynchronizeRoutine;

static
BOOLEAN
NTAPI
SynchronizeRoutine(
    IN PVOID Context)
{
    PTEST_CONTEXT TestContext = Context;

    ok_irql(TestContext->ExpectedIrql);

    CheckSpinLock(TestContext->Interrupt->ActualLock, TRUE);

    return TestContext->ReturnValue;
}

static
VOID
TestSynchronizeExecution(VOID)
{
    KINTERRUPT Interrupt;
    TEST_CONTEXT TestContext;
    KIRQL SynchIrql;
    KIRQL OriginalIrql;
    KIRQL Irql;
    KSPIN_LOCK ActualLock;
    BOOLEAN Ret;

    if (skip(GetNTVersion() < _WIN32_WINNT_VISTA, "We can't create fake interrupts on Vista+.\n"))
        return;

    RtlFillMemory(&Interrupt, sizeof Interrupt, 0x55);
    Interrupt.ActualLock = &ActualLock;
    KeInitializeSpinLock(Interrupt.ActualLock);
    CheckSpinLock(Interrupt.ActualLock, FALSE);

    TestContext.Interrupt = &Interrupt;
    TestContext.ReturnValue = TRUE;

    for (TestContext.ReturnValue = 0; TestContext.ReturnValue <= 2; ++TestContext.ReturnValue)
    {
        for (OriginalIrql = PASSIVE_LEVEL; OriginalIrql <= HIGH_LEVEL; ++OriginalIrql)
        {
            /* TODO: don't hardcode this :| */
            if (OriginalIrql == 3 || (OriginalIrql >= 11 && OriginalIrql <= 26) || OriginalIrql == 30)
                continue;
            KeRaiseIrql(OriginalIrql, &Irql);
            for (SynchIrql = max(DISPATCH_LEVEL, OriginalIrql); SynchIrql <= HIGH_LEVEL; ++SynchIrql)
            {
                if (SynchIrql == 3 || (SynchIrql >= 11 && SynchIrql <= 26) || SynchIrql == 30)
                    continue;
                Interrupt.SynchronizeIrql = SynchIrql;
                ok_irql(OriginalIrql);
                CheckSpinLock(Interrupt.ActualLock, FALSE);
                TestContext.ExpectedIrql = SynchIrql;
                Ret = KeSynchronizeExecution(&Interrupt, SynchronizeRoutine, &TestContext);
                ok_eq_int(Ret, TestContext.ReturnValue);
                ok_irql(OriginalIrql);
                CheckSpinLock(Interrupt.ActualLock, FALSE);
                /* TODO: Check that all other fields of the interrupt are untouched */
            }
            KeLowerIrql(Irql);
        }
    }
}

static
VOID
TestConnectInterrupt(VOID)
{
    PKINTERRUPT InterruptObject;
    NTSTATUS Status;

    /* If the IoConnectInterrupt() fails, the interrupt object should be set to NULL */
    InterruptObject = KmtInvalidPointer;

    /* Test for invalid interrupt */
    Status = IoConnectInterrupt(&InterruptObject,
                                (PKSERVICE_ROUTINE)TestConnectInterrupt,
                                NULL,
                                NULL,
                                0,
                                0,
                                0,
                                LevelSensitive,
                                TRUE,
                                (KAFFINITY)-1,
                                FALSE);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_pointer(InterruptObject, NULL);
}

static KSERVICE_ROUTINE DummyServiceRoutine;

static
BOOLEAN
NTAPI
DummyServiceRoutine(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(ServiceContext);
    return FALSE;
}

static KMESSAGE_SERVICE_ROUTINE DummyMessageServiceRoutine;

static
BOOLEAN
NTAPI
DummyMessageServiceRoutine(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext,
    _In_ ULONG MessageId)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(ServiceContext);
    UNREFERENCED_PARAMETER(MessageId);
    return FALSE;
}

/* A disk's PDO, the port's unit: it has no interrupt of its own */
static
PDEVICE_OBJECT
ReferenceDiskPdo(VOID)
{
    static const GUID DiskInterface = { 0x53F56307, 0xB6BF, 0x11D0, { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };
    PZZWSTR SymbolicLinkList;
    UNICODE_STRING Link;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT TopDevice, Pdo = NULL;
    NTSTATUS Status;

    Status = IoGetDeviceInterfaces(&DiskInterface, NULL, 0, &SymbolicLinkList);
    if (!NT_SUCCESS(Status))
        return NULL;

    if (*SymbolicLinkList != UNICODE_NULL)
    {
        RtlInitUnicodeString(&Link, SymbolicLinkList);
        Status = IoGetDeviceObjectPointer(&Link, FILE_READ_ATTRIBUTES, &FileObject, &TopDevice);
        if (NT_SUCCESS(Status))
        {
            Pdo = IoGetDeviceAttachmentBaseRef(TopDevice);
            ObDereferenceObject(FileObject);
        }
    }

    ExFreePool(SymbolicLinkList);
    return Pdo;
}

typedef NTSTATUS (NTAPI *PIO_CONNECT_INTERRUPT_EX)(PIO_CONNECT_INTERRUPT_PARAMETERS);

static
VOID
TestConnectInterruptEx(VOID)
{
    static UNICODE_STRING RoutineName = RTL_CONSTANT_STRING(L"IoConnectInterruptEx");
    PIO_CONNECT_INTERRUPT_EX pIoConnectInterruptEx;
    IO_CONNECT_INTERRUPT_PARAMETERS Parameters;
    PDEVICE_OBJECT Pdo;
    PKINTERRUPT InterruptObject;
    NTSTATUS Status;

    pIoConnectInterruptEx = (PIO_CONNECT_INTERRUPT_EX)MmGetSystemRoutineAddress(&RoutineName);
    if (skip(pIoConnectInterruptEx != NULL, "IoConnectInterruptEx is missing\n"))
        return;

    /* Unknown version */
    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Version = 0x7F;
    Status = pIoConnectInterruptEx(&Parameters);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER_1);

    Pdo = ReferenceDiskPdo();
    if (skip(Pdo != NULL, "No disk PDO\n"))
        return;

    /* Line based: the unit has no interrupt */
    InterruptObject = KmtInvalidPointer;
    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Version = CONNECT_LINE_BASED;
    Parameters.LineBased.PhysicalDeviceObject = Pdo;
    Parameters.LineBased.InterruptObject = &InterruptObject;
    Parameters.LineBased.ServiceRoutine = DummyServiceRoutine;
    Status = pIoConnectInterruptEx(&Parameters);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulong(Parameters.Version, CONNECT_LINE_BASED);
    if (NT_SUCCESS(Status) && InterruptObject && InterruptObject != KmtInvalidPointer)
        IoDisconnectInterrupt(InterruptObject);

    /* Message based, neither messages nor a line to fall back to */
    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Version = CONNECT_MESSAGE_BASED;
    Parameters.MessageBased.PhysicalDeviceObject = Pdo;
    InterruptObject = KmtInvalidPointer;
    Parameters.MessageBased.ConnectionContext.InterruptObject = &InterruptObject;
    Parameters.MessageBased.MessageServiceRoutine = DummyMessageServiceRoutine;
    Parameters.MessageBased.FallBackServiceRoutine = DummyServiceRoutine;
    Status = pIoConnectInterruptEx(&Parameters);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_ulong(Parameters.Version, CONNECT_MESSAGE_BASED);
    ok_eq_pointer(Parameters.MessageBased.ConnectionContext.InterruptObject, &InterruptObject);

    ObDereferenceObject(Pdo);
}

START_TEST(IoInterrupt)
{
    TestSynchronizeExecution();
    TestConnectInterrupt();
    TestConnectInterruptEx();
}
