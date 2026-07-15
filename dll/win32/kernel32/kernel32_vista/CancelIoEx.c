
#include "k32_vista.h"

/*
 * @implemented
 */
BOOL
WINAPI
CancelIoEx(IN HANDLE hFile,
           IN LPOVERLAPPED lpOverlapped OPTIONAL)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    Status = NtCancelIoFileEx(hFile,
                              (PIO_STATUS_BLOCK)lpOverlapped,
                              &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
