/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * FILE:              lib/rtl/process.c
 * PURPOSE:           Process functions
 * PROGRAMMER:        Alex Ionescu (alex@relsoft.net)
 *                    Ariadne (ariadne@xs4all.nl)
 *                    Eric Kohl
 */

/* INCLUDES ****************************************************************/

#include <rtl.h>
#include <ndk/umfuncs.h>

#define NDEBUG
#include <debug.h>

/* INTERNAL FUNCTIONS *******************************************************/

NTSTATUS
NTAPI
RtlpMapFile(PUNICODE_STRING ImageFileName,
            ULONG Attributes,
            PHANDLE Section)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE hFile = NULL;
    IO_STATUS_BLOCK IoStatusBlock;

    /* Open the Image File */
    InitializeObjectAttributes(&ObjectAttributes,
                               ImageFileName,
                               Attributes & (OBJ_CASE_INSENSITIVE | OBJ_INHERIT),
                               NULL,
                               NULL);
    Status = ZwOpenFile(&hFile,
                        SYNCHRONIZE | FILE_EXECUTE | FILE_READ_DATA,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_DELETE | FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to read image file from disk, Status = 0x%08X\n", Status);
        return Status;
    }

    /* Now create a section for this image */
    Status = ZwCreateSection(Section,
                             SECTION_ALL_ACCESS,
                             NULL,
                             NULL,
                             PAGE_EXECUTE,
                             SEC_IMAGE,
                             hFile);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create section for image file, Status = 0x%08X\n", Status);
    }

    ZwClose(hFile);
    return Status;
}

/* FUNCTIONS ****************************************************************/

NTSTATUS
NTAPI
RtlpInitEnvironment(HANDLE ProcessHandle,
                    PPEB Peb,
                    PRTL_USER_PROCESS_PARAMETERS ProcessParameters)
{
    NTSTATUS Status;
    PVOID BaseAddress = NULL;
    SIZE_T EnviroSize;
    SIZE_T Size;
    PWCHAR Environment = NULL;
    DPRINT("RtlpInitEnvironment(ProcessHandle: %p, Peb: %p Params: %p)\n",
            ProcessHandle, Peb, ProcessParameters);

    /* Give the caller 1MB if he requested it */
    if (ProcessParameters->Flags & RTL_USER_PROCESS_PARAMETERS_RESERVE_1MB)
    {
        /* Give 1MB starting at 0x4 */
        BaseAddress = (PVOID)4;
        EnviroSize = (1024 * 1024) - 256;
        Status = ZwAllocateVirtualMemory(ProcessHandle,
                                         &BaseAddress,
                                         0,
                                         &EnviroSize,
                                         MEM_RESERVE,
                                         PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to reserve 1MB of space\n");
            return Status;
        }
    }

    /* Find the end of the Enviroment Block */
    if ((Environment = (PWCHAR)ProcessParameters->Environment))
    {
        while (*Environment++) while (*Environment++);

        /* Calculate the size of the block */
        EnviroSize = (ULONG)((ULONG_PTR)Environment -
                             (ULONG_PTR)ProcessParameters->Environment);

        /* Allocate and Initialize new Environment Block */
        Size = EnviroSize;
        Status = ZwAllocateVirtualMemory(ProcessHandle,
                                         &BaseAddress,
                                         0,
                                         &Size,
                                         MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to allocate Environment Block\n");
            return Status;
        }

        /* Write the Environment Block */
        ZwWriteVirtualMemory(ProcessHandle,
                             BaseAddress,
                             ProcessParameters->Environment,
                             EnviroSize,
                             NULL);

        /* Save pointer */
        ProcessParameters->Environment = BaseAddress;
    }

    /* Now allocate space for the Parameter Block */
    BaseAddress = NULL;
    Size = ProcessParameters->MaximumLength;
    Status = ZwAllocateVirtualMemory(ProcessHandle,
                                     &BaseAddress,
                                     0,
                                     &Size,
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to allocate Parameter Block\n");
        return Status;
    }

    /* Write the Parameter Block */
    Status = ZwWriteVirtualMemory(ProcessHandle,
                                  BaseAddress,
                                  ProcessParameters,
                                  ProcessParameters->Length,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to write the Parameter Block\n");
        return Status;
    }

    /* Write pointer to Parameter Block */
    Status = ZwWriteVirtualMemory(ProcessHandle,
                                  &Peb->ProcessParameters,
                                  &BaseAddress,
                                  sizeof(BaseAddress),
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to write pointer to Parameter Block\n");
        return Status;
    }

    /* Return */
    return STATUS_SUCCESS;
}

/*
 * @implemented
 *
 * Creates a process and its initial thread.
 *
 * NOTES:
 *  - The first thread is created suspended, so it needs a manual resume!!!
 *  - If ParentProcess is NULL, current process is used
 *  - ProcessParameters must be normalized
 *  - Attributes are object attribute flags used when opening the ImageFileName.
 *    Valid flags are OBJ_INHERIT and OBJ_CASE_INSENSITIVE.
 *
 * -Gunnar
 */
NTSTATUS
NTAPI
RtlCreateUserProcess(IN PUNICODE_STRING ImageFileName,
                     IN ULONG Attributes,
                     IN OUT PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
                     IN PSECURITY_DESCRIPTOR ProcessSecurityDescriptor OPTIONAL,
                     IN PSECURITY_DESCRIPTOR ThreadSecurityDescriptor OPTIONAL,
                     IN HANDLE ParentProcess OPTIONAL,
                     IN BOOLEAN InheritHandles,
                     IN HANDLE DebugPort OPTIONAL,
                     IN HANDLE ExceptionPort OPTIONAL,
                     OUT PRTL_USER_PROCESS_INFORMATION ProcessInfo)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    DPRINT("RtlCreateUserProcess: %wZ\n", ImageFileName);

#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    /*
     * Vista+ path: use NtCreateUserProcess which combines process and
     * thread creation into a single kernel call.
     */
    {
        PS_CREATE_INFO CreateInfo;
        ULONG ProcessCreateFlags = 0;
        ULONG AttrCount = 0;

        /*
         * We need up to 4 attributes:
         * [0] PS_ATTRIBUTE_IMAGE_NAME (required)
         * [1] PS_ATTRIBUTE_CLIENT_ID (output)
         * [2] PS_ATTRIBUTE_IMAGE_INFO (output)
         * [3] PS_ATTRIBUTE_PARENT_PROCESS (optional, if non-default)
         */
        ULONG_PTR AttrBuffer[sizeof(PS_ATTRIBUTE_LIST) / sizeof(ULONG_PTR) +
                             3 * (sizeof(PS_ATTRIBUTE) / sizeof(ULONG_PTR)) + 1];
        PPS_ATTRIBUTE_LIST AttrList = (PPS_ATTRIBUTE_LIST)AttrBuffer;

        /*
         * PS_ATTRIBUTE_IMAGE_NAME must be an NT-form path (\??\…, \SystemRoot\…,
         * or \Device\…) because the kernel's NtCreateUserProcess uses it with
         * ZwOpenFile, which doesn't understand DOS paths.  Some callers
         * (notably ntoskrnl!Phase3InitializationDiscard spawning SMSS) reuse
         * the same UNICODE_STRING Buffer for ImageFileName and
         * ProcessParameters->ImagePathName, so the post-normalize DOS-form
         * rewrites we do below would corrupt the kernel's view of the image
         * name.  Snapshot ImageFileName into a local buffer up-front so the
         * PS_ATTRIBUTE_IMAGE_NAME binding is immune to the later in-place
         * rewrites of the shared ProcessParameters buffer.
         */
        WCHAR LocalImageName[MAX_PATH + 1];
        USHORT LocalImageNameLen = ImageFileName->Length;
        if (LocalImageNameLen > sizeof(LocalImageName) - sizeof(WCHAR))
            LocalImageNameLen = sizeof(LocalImageName) - sizeof(WCHAR);
        RtlCopyMemory(LocalImageName, ImageFileName->Buffer, LocalImageNameLen);
        LocalImageName[LocalImageNameLen / sizeof(WCHAR)] = UNICODE_NULL;

        /* Clean out the current directory handle if we won't use it */
        if (!InheritHandles) ProcessParameters->CurrentDirectory.Handle = NULL;

        /* Use us as parent if none other specified */
        if (!ParentProcess) ParentProcess = NtCurrentProcess();

        /* Set process creation flags */
        if (InheritHandles)
            ProcessCreateFlags |= PROCESS_CREATE_FLAGS_INHERIT_HANDLES;

        /* Build attribute list */
        AttrCount = 0;

        /* Image name attribute (required) - pristine NT form, not affected by
         * the in-place rewrites of ProcessParameters below. */
        AttrList->Attributes[AttrCount].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
        AttrList->Attributes[AttrCount].Size = LocalImageNameLen;
        AttrList->Attributes[AttrCount].ValuePtr = LocalImageName;
        AttrList->Attributes[AttrCount].ReturnLength = NULL;
        AttrCount++;

        /* Client ID output attribute */
        AttrList->Attributes[AttrCount].Attribute = PS_ATTRIBUTE_CLIENT_ID;
        AttrList->Attributes[AttrCount].Size = sizeof(ProcessInfo->ClientId);
        AttrList->Attributes[AttrCount].ValuePtr = &ProcessInfo->ClientId;
        AttrList->Attributes[AttrCount].ReturnLength = NULL;
        AttrCount++;

        /* Image information output attribute */
        AttrList->Attributes[AttrCount].Attribute = PS_ATTRIBUTE_IMAGE_INFO;
        AttrList->Attributes[AttrCount].Size = sizeof(ProcessInfo->ImageInformation);
        AttrList->Attributes[AttrCount].ValuePtr = &ProcessInfo->ImageInformation;
        AttrList->Attributes[AttrCount].ReturnLength = NULL;
        AttrCount++;

        /* Parent process attribute if non-default */
        if (ParentProcess != NtCurrentProcess())
        {
            AttrList->Attributes[AttrCount].Attribute = PS_ATTRIBUTE_PARENT_PROCESS;
            AttrList->Attributes[AttrCount].Size = sizeof(HANDLE);
            AttrList->Attributes[AttrCount].ValuePtr = ParentProcess;
            AttrList->Attributes[AttrCount].ReturnLength = NULL;
            AttrCount++;
        }

        /* Set total length */
        AttrList->TotalLength = FIELD_OFFSET(PS_ATTRIBUTE_LIST, Attributes) +
                                AttrCount * sizeof(PS_ATTRIBUTE);

        /* Initialize the Object Attributes */
        InitializeObjectAttributes(&ObjectAttributes,
                                   NULL,
                                   0,
                                   NULL,
                                   ProcessSecurityDescriptor);

        /* Set up CreateInfo */
        RtlZeroMemory(&CreateInfo, sizeof(CreateInfo));
        CreateInfo.Size = sizeof(CreateInfo);
        CreateInfo.State = PsCreateInitialState;

        /* Normalize process parameters (converts relative Buffer offsets
         * back to absolute pointers so the fields below can be read). */
        RtlNormalizeProcessParams(ProcessParameters);

        /*
         * Windows invariant: the DOS-form fields of RTL_USER_PROCESS_PARAMETERS
         * (ImagePathName, CurrentDirectory.DosPath) really are DOS paths.
         * Downstream consumers - LDR_DATA_TABLE_ENTRY.FullDllName,
         * BaseComputeProcessDllPath (feeding SearchPathW), activation-context
         * resource probing, GetCurrentDirectoryW, inherited CWD for spawned
         * children, etc. - are DOS-path APIs and don't understand the native
         * "\??\" or "\SystemRoot\" prefix.  Callers (the NT6 kernel spawning
         * SMSS, SMSS spawning subsystems, kernel32 CreateProcessW inheriting
         * an NT-form CWD) legitimately reuse the same NT-form string for both
         * PS_ATTRIBUTE_IMAGE_NAME (kernel needs NT form to ZwOpenFile the
         * image) and ProcessParameters, which lets the NT form leak into the
         * child's user-mode DOS APIs.
         *
         * We restore the invariant here, after RtlNormalizeProcessParams so
         * that Buffer is an absolute pointer.  Two prefixes are handled:
         *
         *   "\??\"          -> stripped in place (4 WCHARs removed).
         *   "\SystemRoot\"  -> replaced by the runtime DOS expansion kept in
         *                     SharedUserData->NtSystemRoot (e.g. "X:\ReactOS"
         *                     on a livecd boot).  SharedUserData resolves to
         *                     a valid read-only mapping in both user mode
         *                     (0x7FFE0000) and kernel mode
         *                     (KI_USER_SHARED_DATA), and this function links
         *                     into ntoskrnl as well as ntdll.
         *
         * All rewrites are bounded by MaximumLength to avoid corruption; if
         * the DOS expansion would not fit, we leave the field alone rather
         * than truncate.
         */
#define STRIP_NT_PREFIX(us)                                                          \
        do {                                                                         \
            if ((us).Length >= 4 * sizeof(WCHAR) &&                                  \
                (us).Buffer[0] == L'\\' &&                                           \
                (us).Buffer[1] == L'?' &&                                            \
                (us).Buffer[2] == L'?' &&                                            \
                (us).Buffer[3] == L'\\')                                             \
            {                                                                        \
                USHORT _strip = 4 * sizeof(WCHAR);                                   \
                USHORT _newLen = (us).Length - _strip;                               \
                RtlMoveMemory((us).Buffer,                                           \
                              (PBYTE)(us).Buffer + _strip,                           \
                              _newLen + sizeof(WCHAR));                              \
                (us).Length = _newLen;                                               \
            }                                                                        \
        } while (0)

        /*
         * Replace leading "\SystemRoot\" (case-insensitive) with the DOS-form
         * system root from SharedUserData->NtSystemRoot.  The replacement
         * keeps the trailing backslash of the prefix, so we only substitute
         * the "\SystemRoot" portion (the 11-char substring before the final
         * backslash) with the expansion.
         */
#define SYSROOT_PREFIX_CHARS  11 /* length of L"\\SystemRoot" (no trailing '\\') */
#define EXPAND_SYSTEMROOT_PREFIX(us)                                                 \
        do {                                                                         \
            static const WCHAR _sysroot[SYSROOT_PREFIX_CHARS] =                      \
                { L'\\', L'S', L'y', L's', L't', L'e',                               \
                  L'm', L'R', L'o', L'o', L't' };                                    \
            BOOLEAN _match = FALSE;                                                  \
            if ((us).Length >= (SYSROOT_PREFIX_CHARS + 1) * sizeof(WCHAR) &&         \
                (us).Buffer[SYSROOT_PREFIX_CHARS] == L'\\')                          \
            {                                                                        \
                ULONG _i;                                                            \
                _match = TRUE;                                                       \
                for (_i = 0; _i < SYSROOT_PREFIX_CHARS; _i++)                        \
                {                                                                    \
                    WCHAR _a = (us).Buffer[_i];                                      \
                    WCHAR _b = _sysroot[_i];                                         \
                    if (_a >= L'A' && _a <= L'Z') _a = (WCHAR)(_a + (L'a' - L'A'));  \
                    if (_b >= L'A' && _b <= L'Z') _b = (WCHAR)(_b + (L'a' - L'A'));  \
                    if (_a != _b) { _match = FALSE; break; }                         \
                }                                                                    \
            }                                                                        \
            if (_match)                                                              \
            {                                                                        \
                PCWSTR _dos = SharedUserData->NtSystemRoot;                          \
                SIZE_T _dosChars = 0;                                                \
                while (_dos[_dosChars] != UNICODE_NULL &&                            \
                       _dosChars < RTL_NUMBER_OF(SharedUserData->NtSystemRoot))      \
                    _dosChars++;                                                     \
                if (_dosChars != 0)                                                  \
                {                                                                    \
                    SIZE_T _tail = (us).Length -                                     \
                                   SYSROOT_PREFIX_CHARS * sizeof(WCHAR);             \
                    SIZE_T _newBytes = _dosChars * sizeof(WCHAR) + _tail;            \
                    if (_newBytes + sizeof(WCHAR) <= (us).MaximumLength)             \
                    {                                                                \
                        RtlMoveMemory((PBYTE)(us).Buffer +                           \
                                          _dosChars * sizeof(WCHAR),                 \
                                      (PBYTE)(us).Buffer +                           \
                                          SYSROOT_PREFIX_CHARS * sizeof(WCHAR),      \
                                      _tail + sizeof(WCHAR));                        \
                        RtlCopyMemory((us).Buffer,                                   \
                                      _dos,                                          \
                                      _dosChars * sizeof(WCHAR));                    \
                        (us).Length = (USHORT)_newBytes;                             \
                    }                                                                \
                    else                                                             \
                    {                                                                \
                        DPRINT1("RtlCreateUserProcess: cannot expand \\SystemRoot"   \
                                " in %wZ (need %Iu, have %u)\n",                     \
                                &(us), _newBytes + sizeof(WCHAR),                    \
                                (us).MaximumLength);                                 \
                    }                                                                \
                }                                                                    \
            }                                                                        \
        } while (0)

        if (ProcessParameters->Flags & RTL_USER_PROCESS_PARAMETERS_NORMALIZED)
        {
            STRIP_NT_PREFIX(ProcessParameters->ImagePathName);
            STRIP_NT_PREFIX(ProcessParameters->CurrentDirectory.DosPath);
            EXPAND_SYSTEMROOT_PREFIX(ProcessParameters->ImagePathName);
            EXPAND_SYSTEMROOT_PREFIX(ProcessParameters->CurrentDirectory.DosPath);
        }
#undef STRIP_NT_PREFIX
#undef EXPAND_SYSTEMROOT_PREFIX
#undef SYSROOT_PREFIX_CHARS

        /* Call NtCreateUserProcess */
        Status = NtCreateUserProcess(&ProcessInfo->ProcessHandle,
                                     &ProcessInfo->ThreadHandle,
                                     PROCESS_ALL_ACCESS,
                                     THREAD_ALL_ACCESS,
                                     &ObjectAttributes,
                                     NULL,
                                     ProcessCreateFlags,
                                     THREAD_CREATE_FLAGS_CREATE_SUSPENDED,
                                     ProcessParameters,
                                     &CreateInfo,
                                     AttrList);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("NtCreateUserProcess failed, Status=0x%lx\n", Status);
            return Status;
        }

        return STATUS_SUCCESS;
    }
#else
    /*
     * Pre-Vista path: separate NtCreateProcess + NtCreateThread calls.
     */
    {
        HANDLE hSection;
        PROCESS_BASIC_INFORMATION ProcessBasicInfo;
        UNICODE_STRING DebugString = RTL_CONSTANT_STRING(L"\\WindowsSS");

        /* Map and Load the File */
        Status = RtlpMapFile(ImageFileName,
                             Attributes,
                             &hSection);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not map process image\n");
            return Status;
        }

        /* Clean out the current directory handle if we won't use it */
        if (!InheritHandles) ProcessParameters->CurrentDirectory.Handle = NULL;

        /* Use us as parent if none other specified */
        if (!ParentProcess) ParentProcess = NtCurrentProcess();

        /* Initialize the Object Attributes */
        InitializeObjectAttributes(&ObjectAttributes,
                                   NULL,
                                   0,
                                   NULL,
                                   ProcessSecurityDescriptor);

        /*
         * If FLG_ENABLE_CSRDEBUG is used, then CSRSS is created under the
         * watch of WindowsSS
         */
        if ((RtlGetNtGlobalFlags() & FLG_ENABLE_CSRDEBUG) &&
            (wcsstr(ImageFileName->Buffer, L"csrss")))
        {
            ObjectAttributes.ObjectName = &DebugString;
        }

        /* Create Kernel Process Object */
        Status = ZwCreateProcess(&ProcessInfo->ProcessHandle,
                                 PROCESS_ALL_ACCESS,
                                 &ObjectAttributes,
                                 ParentProcess,
                                 InheritHandles,
                                 hSection,
                                 DebugPort,
                                 ExceptionPort);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not create Kernel Process Object\n");
            ZwClose(hSection);
            return Status;
        }

        /* Get some information on the image */
        Status = ZwQuerySection(hSection,
                                SectionImageInformation,
                                &ProcessInfo->ImageInformation,
                                sizeof(SECTION_IMAGE_INFORMATION),
                                NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not query Section Info\n");
            ZwClose(ProcessInfo->ProcessHandle);
            ZwClose(hSection);
            return Status;
        }

        /* Get some information about the process */
        Status = ZwQueryInformationProcess(ProcessInfo->ProcessHandle,
                                           ProcessBasicInformation,
                                           &ProcessBasicInfo,
                                           sizeof(ProcessBasicInfo),
                                           NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not query Process Info\n");
            ZwClose(ProcessInfo->ProcessHandle);
            ZwClose(hSection);
            return Status;
        }

        /* Duplicate the standard handles */
        Status = STATUS_SUCCESS;
        _SEH2_TRY
        {
            if (ProcessParameters->StandardInput)
            {
                Status = ZwDuplicateObject(ParentProcess,
                                           ProcessParameters->StandardInput,
                                           ProcessInfo->ProcessHandle,
                                           &ProcessParameters->StandardInput,
                                           0,
                                           0,
                                           DUPLICATE_SAME_ACCESS |
                                           DUPLICATE_SAME_ATTRIBUTES);
                if (!NT_SUCCESS(Status))
                {
                    _SEH2_LEAVE;
                }
            }

            if (ProcessParameters->StandardOutput)
            {
                Status = ZwDuplicateObject(ParentProcess,
                                           ProcessParameters->StandardOutput,
                                           ProcessInfo->ProcessHandle,
                                           &ProcessParameters->StandardOutput,
                                           0,
                                           0,
                                           DUPLICATE_SAME_ACCESS |
                                           DUPLICATE_SAME_ATTRIBUTES);
                if (!NT_SUCCESS(Status))
                {
                    _SEH2_LEAVE;
                }
            }

            if (ProcessParameters->StandardError)
            {
                Status = ZwDuplicateObject(ParentProcess,
                                           ProcessParameters->StandardError,
                                           ProcessInfo->ProcessHandle,
                                           &ProcessParameters->StandardError,
                                           0,
                                           0,
                                           DUPLICATE_SAME_ACCESS |
                                           DUPLICATE_SAME_ATTRIBUTES);
                if (!NT_SUCCESS(Status))
                {
                    _SEH2_LEAVE;
                }
            }
        }
        _SEH2_FINALLY
        {
            if (!NT_SUCCESS(Status))
            {
                ZwClose(ProcessInfo->ProcessHandle);
                ZwClose(hSection);
            }
        }
        _SEH2_END;

        if (!NT_SUCCESS(Status))
            return Status;

        /* Create Process Environment */
        Status = RtlpInitEnvironment(ProcessInfo->ProcessHandle,
                                     ProcessBasicInfo.PebBaseAddress,
                                     ProcessParameters);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not Create Process Environment\n");
            ZwClose(ProcessInfo->ProcessHandle);
            ZwClose(hSection);
            return Status;
        }

        /* Create the first Thread */
        Status = RtlCreateUserThread(ProcessInfo->ProcessHandle,
                                     ThreadSecurityDescriptor,
                                     TRUE,
                                     ProcessInfo->ImageInformation.ZeroBits,
                                     ProcessInfo->ImageInformation.MaximumStackSize,
                                     ProcessInfo->ImageInformation.CommittedStackSize,
                                     ProcessInfo->ImageInformation.TransferAddress,
                                     ProcessBasicInfo.PebBaseAddress,
                                     &ProcessInfo->ThreadHandle,
                                     &ProcessInfo->ClientId);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not Create Thread\n");
            ZwClose(ProcessInfo->ProcessHandle);
            ZwClose(hSection); /* Don't try to optimize this on top! */
            return Status;
        }

        /* Close the Section Handle and return */
        ZwClose(hSection);
        return STATUS_SUCCESS;
    }
#endif /* NTDDI_VERSION >= NTDDI_LONGHORN */
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlEncodePointer(IN PVOID Pointer)
{
    ULONG Cookie;
    NTSTATUS Status;

    Status = ZwQueryInformationProcess(NtCurrentProcess(),
                                       ProcessCookie,
                                       &Cookie,
                                       sizeof(Cookie),
                                       NULL);
    if(!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to receive the process cookie! Status: 0x%lx\n", Status);
        return Pointer;
    }

    return (PVOID)((ULONG_PTR)Pointer ^ Cookie);
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlDecodePointer(IN PVOID Pointer)
{
    return RtlEncodePointer(Pointer);
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlEncodeSystemPointer(IN PVOID Pointer)
{
    return (PVOID)((ULONG_PTR)Pointer ^ SharedUserData->Cookie);
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlDecodeSystemPointer(IN PVOID Pointer)
{
    return RtlEncodeSystemPointer(Pointer);
}

/*
 * @implemented
 *
 * NOTES:
 *   Implementation based on the documentation from:
 *   http://www.geoffchappell.com/studies/windows/win32/ntdll/api/rtl/peb/setprocessiscritical.htm
 */
NTSTATUS
__cdecl
RtlSetProcessIsCritical(IN BOOLEAN NewValue,
                        OUT PBOOLEAN OldValue OPTIONAL,
                        IN BOOLEAN NeedBreaks)
{
    ULONG BreakOnTermination;

    /* Initialize to FALSE */
    if (OldValue) *OldValue = FALSE;

    /* Fail, if the critical breaks flag is required but is not set */
    if ((NeedBreaks) &&
        !(NtCurrentPeb()->NtGlobalFlag & FLG_ENABLE_SYSTEM_CRIT_BREAKS))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Check if the caller wants the old value */
    if (OldValue)
    {
        /* Query and return the old break on termination flag for the process */
        ZwQueryInformationProcess(NtCurrentProcess(),
                                  ProcessBreakOnTermination,
                                  &BreakOnTermination,
                                  sizeof(ULONG),
                                  NULL);
        *OldValue = (BOOLEAN)BreakOnTermination;
    }

    /* Set the break on termination flag for the process */
    BreakOnTermination = NewValue;
    return ZwSetInformationProcess(NtCurrentProcess(),
                                   ProcessBreakOnTermination,
                                   &BreakOnTermination,
                                   sizeof(ULONG));
}

/*
 * @implemented
 *
 * Win6+ ntdll process-termination entry point.  Earlier code paths used
 * kernel32!ExitProcess, which performs the same dance inline; binaries
 * built against Win6+ ucrt/Qt6 sometimes call this directly via ntdll,
 * which historically would hit a no-op stub on ReactOS and leave the
 * process running silent.
 */
VOID
NTAPI
RtlExitUserProcess(_In_ ULONG ExitStatus)
{
    /* Serialize with concurrent loader/PEB updates. */
    RtlAcquirePebLock();

    _SEH2_TRY
    {
        /* Tear down every other thread first so DLL_PROCESS_DETACH runs
         * in a single-threaded process (Windows guarantees this). */
        NtTerminateProcess(NULL, ExitStatus);

        /* Walk the loaded-module list invoking DLL_PROCESS_DETACH. */
        LdrShutdownProcess();

        /* Terminate ourselves; this call does not return. */
        NtTerminateProcess(NtCurrentProcess(), ExitStatus);
    }
    _SEH2_FINALLY
    {
        RtlReleasePebLock();
    }
    _SEH2_END;
}

ULONG
NTAPI
RtlGetCurrentProcessorNumber(VOID)
{
    /* Forward to kernel */
    return NtGetCurrentProcessorNumber();
}

_IRQL_requires_max_(APC_LEVEL)
ULONG
NTAPI
RtlRosGetAppcompatVersion(VOID)
{
    /* Get the current PEB */
    PPEB Peb = RtlGetCurrentPeb();
    if (Peb == NULL)
    {
        /* Default to Server 2003 */
        return _WIN32_WINNT_WS03;
    }

    /* Calculate OS version from PEB fields */
    return (Peb->OSMajorVersion << 8) | Peb->OSMinorVersion;
}
