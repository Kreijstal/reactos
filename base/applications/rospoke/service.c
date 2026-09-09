/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Installing, starting and -- the point of the exercise --
 *              unloading the driver
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * The service is created here rather than shipped in an INF on purpose.  A
 * registry entry that loads this driver is a permanent hole; making somebody
 * type `rospoke --install` keeps it an act rather than a default.
 *
 * `--stop` is not housekeeping, it is the hot-reload button: because rospoke is
 * a legacy driver its image really does leave memory, so copying a new
 * rospoke.sys over the old one and running `--reload` picks up the new code
 * without a boot.
 */

#include "pokecli.h"

#define POKE_DEFAULT_IMAGE_PATH "System32\\drivers\\rospoke.sys"

void
PokePrintLastError(const char *What)
{
    DWORD Error = GetLastError();
    char Message[256];

    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, Error, 0, Message, sizeof(Message), NULL) == 0)
    {
        _snprintf(Message, sizeof(Message), "(no description)");
        Message[sizeof(Message) - 1] = '\0';
    }
    else
    {
        size_t Length = strlen(Message);
        while (Length > 0 && (Message[Length - 1] == '\r' || Message[Length - 1] == '\n'))
            Message[--Length] = '\0';
    }

    fprintf(stderr, "rospoke: %s failed: %lu %s\n", What, Error, Message);
}

static
SC_HANDLE
PokeOpenManager(DWORD Access)
{
    SC_HANDLE Manager = OpenSCManagerA(NULL, NULL, Access);

    if (Manager == NULL)
        PokePrintLastError("OpenSCManager");

    return Manager;
}

int
PokeServiceInstall(const char *ImagePath)
{
    SC_HANDLE Manager, Service;
    const char *Path = ImagePath ? ImagePath : POKE_DEFAULT_IMAGE_PATH;

    Manager = PokeOpenManager(SC_MANAGER_CREATE_SERVICE);
    if (Manager == NULL)
        return POKE_EXIT_DRIVER;

    Service = CreateServiceA(Manager,
                             ROSPOKE_SERVICE_NAME_A,
                             "ReactOS hardware bring-up poke driver",
                             SERVICE_ALL_ACCESS,
                             SERVICE_KERNEL_DRIVER,
                             SERVICE_DEMAND_START,
                             SERVICE_ERROR_NORMAL,
                             Path,
                             NULL, NULL, NULL, NULL, NULL);
    if (Service == NULL)
    {
        if (GetLastError() == ERROR_SERVICE_EXISTS)
        {
            printf("rospoke: service already installed\n");
            CloseServiceHandle(Manager);
            return POKE_EXIT_OK;
        }
        PokePrintLastError("CreateService");
        CloseServiceHandle(Manager);
        return POKE_EXIT_DRIVER;
    }

    printf("rospoke: installed, ImagePath=%s\n", Path);
    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    return POKE_EXIT_OK;
}

int
PokeServiceRemove(void)
{
    SC_HANDLE Manager, Service;
    int Result = POKE_EXIT_OK;

    Manager = PokeOpenManager(SC_MANAGER_CONNECT);
    if (Manager == NULL)
        return POKE_EXIT_DRIVER;

    Service = OpenServiceA(Manager, ROSPOKE_SERVICE_NAME_A, DELETE);
    if (Service == NULL)
    {
        PokePrintLastError("OpenService");
        CloseServiceHandle(Manager);
        return POKE_EXIT_DRIVER;
    }

    if (!DeleteService(Service))
    {
        PokePrintLastError("DeleteService");
        Result = POKE_EXIT_DRIVER;
    }
    else
    {
        printf("rospoke: service deleted\n");
    }

    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    return Result;
}

int
PokeServiceStart(void)
{
    SC_HANDLE Manager, Service;
    int Result = POKE_EXIT_OK;

    Manager = PokeOpenManager(SC_MANAGER_CONNECT);
    if (Manager == NULL)
        return POKE_EXIT_DRIVER;

    Service = OpenServiceA(Manager, ROSPOKE_SERVICE_NAME_A, SERVICE_START);
    if (Service == NULL)
    {
        PokePrintLastError("OpenService");
        CloseServiceHandle(Manager);
        return POKE_EXIT_DRIVER;
    }

    if (!StartServiceA(Service, 0, NULL))
    {
        if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
        {
            printf("rospoke: already running\n");
        }
        else
        {
            PokePrintLastError("StartService");
            Result = POKE_EXIT_DRIVER;
        }
    }
    else
    {
        printf("rospoke: started\n");
    }

    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    return Result;
}

int
PokeServiceStop(void)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS Status;
    int Result = POKE_EXIT_OK;

    Manager = PokeOpenManager(SC_MANAGER_CONNECT);
    if (Manager == NULL)
        return POKE_EXIT_DRIVER;

    Service = OpenServiceA(Manager, ROSPOKE_SERVICE_NAME_A, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (Service == NULL)
    {
        PokePrintLastError("OpenService");
        CloseServiceHandle(Manager);
        return POKE_EXIT_DRIVER;
    }

    if (!ControlService(Service, SERVICE_CONTROL_STOP, &Status))
    {
        if (GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        {
            printf("rospoke: not running\n");
        }
        else
        {
            /*
             * If this ever starts returning ERROR_INVALID_SERVICE_CONTROL the
             * image has stopped being a legacy driver and can no longer be
             * unloaded -- see the note in DriverEntry.  It would mean edits to
             * rospoke.sys silently do nothing until a reboot.
             */
            PokePrintLastError("ControlService(STOP)");
            Result = POKE_EXIT_DRIVER;
        }
    }
    else
    {
        printf("rospoke: stopped and unloaded\n");
    }

    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    return Result;
}

int
PokeServiceStatus(void)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS Status;
    int Result = POKE_EXIT_OK;

    Manager = PokeOpenManager(SC_MANAGER_CONNECT);
    if (Manager == NULL)
        return POKE_EXIT_DRIVER;

    Service = OpenServiceA(Manager, ROSPOKE_SERVICE_NAME_A, SERVICE_QUERY_STATUS);
    if (Service == NULL)
    {
        printf("rospoke: not installed\n");
        CloseServiceHandle(Manager);
        return POKE_EXIT_OK;
    }

    if (!QueryServiceStatus(Service, &Status))
    {
        PokePrintLastError("QueryServiceStatus");
        Result = POKE_EXIT_DRIVER;
    }
    else
    {
        const char *State;

        switch (Status.dwCurrentState)
        {
            case SERVICE_STOPPED:       State = "stopped"; break;
            case SERVICE_START_PENDING: State = "start pending"; break;
            case SERVICE_STOP_PENDING:  State = "stop pending"; break;
            case SERVICE_RUNNING:       State = "running"; break;
            default:                    State = "unknown"; break;
        }
        printf("rospoke: installed, %s\n", State);
    }

    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    return Result;
}
