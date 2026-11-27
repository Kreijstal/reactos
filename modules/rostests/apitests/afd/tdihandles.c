/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the TDI objects of a socket: the handles
 *              IOCTL_AFD_GET_TDI_HANDLES gives, and which address object the
 *              TCP/IP helper sets an option of
 */

#include "precomp.h"

#include <ws2tcpip.h>
#include <tdiinfo.h>
#include <wsahelp.h>

#define MAX_ENTITIES 512

/* TTLs that are not the default */
#define TTL_BY_ENTITY 55
#define TTL_BY_SOCKET 44
#define TTL_BY_HELPER 77
#define TTL_BY_ACCEPTED 99

/* A control channel of \Device\Tcp */
static
HANDLE
OpenTcpControlChannel(void)
{
    NTSTATUS Status;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\Tcp");
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;

    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreateFile(&Handle,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    ok(Status == STATUS_SUCCESS, "Opening \\Device\\Tcp returned %lx\n", Status);
    return NT_SUCCESS(Status) ? Handle : NULL;
}

/* The TTL of the address object Entity names, or the one Handle is if it
 * is one; 0xFFFFFFFF if the query fails */
static
ULONG
QueryTtl(
    _In_ HANDLE Handle,
    _In_ TDIEntityID Entity)
{
    TCP_REQUEST_QUERY_INFORMATION_EX Request;
    ULONG Ttl = 0xFFFFFFFF;
    DWORD Returned = 0;
    BOOL Success;

    ZeroMemory(&Request, sizeof(Request));
    Request.ID.toi_entity = Entity;
    Request.ID.toi_class = INFO_CLASS_PROTOCOL;
    Request.ID.toi_type = INFO_TYPE_ADDRESS_OBJECT;
    Request.ID.toi_id = AO_OPTION_TTL;
    Success = DeviceIoControl(Handle, IOCTL_TCP_QUERY_INFORMATION_EX, &Request, sizeof(Request),
                              &Ttl, sizeof(Ttl), &Returned, NULL);
    ok(Success, "Querying the TTL failed with %lu\n", GetLastError());
    ok(Returned == sizeof(Ttl), "The TTL is %lu bytes\n", Returned);

    return Success ? (Ttl & 0xFF) : 0xFFFFFFFF;
}

static
BOOL
SetTtl(
    _In_ HANDLE Handle,
    _In_ TDIEntityID Entity,
    _In_ ULONG Ttl)
{
    UCHAR Buffer[FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG)];
    PTCP_REQUEST_SET_INFORMATION_EX Info = (PTCP_REQUEST_SET_INFORMATION_EX)Buffer;
    DWORD Returned;

    ZeroMemory(Buffer, sizeof(Buffer));
    Info->ID.toi_entity = Entity;
    Info->ID.toi_class = INFO_CLASS_PROTOCOL;
    Info->ID.toi_type = INFO_TYPE_ADDRESS_OBJECT;
    Info->ID.toi_id = AO_OPTION_TTL;
    Info->BufferSize = sizeof(ULONG);
    CopyMemory(Info->Buffer, &Ttl, sizeof(Ttl));

    return DeviceIoControl(Handle, IOCTL_TCP_SET_INFORMATION_EX, Buffer, sizeof(Buffer),
                           NULL, 0, &Returned, NULL);
}

/* The entities there are now */
static
ULONG
QueryEntities(
    _In_ HANDLE ControlChannel,
    _Out_writes_(MAX_ENTITIES) TDIEntityID *Entities)
{
    TCP_REQUEST_QUERY_INFORMATION_EX Request;
    DWORD Returned = 0;
    BOOL Success;

    ZeroMemory(&Request, sizeof(Request));
    Request.ID.toi_entity.tei_entity = GENERIC_ENTITY;
    Request.ID.toi_class = INFO_CLASS_GENERIC;
    Request.ID.toi_type = INFO_TYPE_PROVIDER;
    Request.ID.toi_id = ENTITY_LIST_ID;
    Success = DeviceIoControl(ControlChannel, IOCTL_TCP_QUERY_INFORMATION_EX, &Request, sizeof(Request),
                              Entities, MAX_ENTITIES * sizeof(TDIEntityID), &Returned, NULL);
    ok(Success, "Querying the entities failed with %lu\n", GetLastError());

    return Success ? Returned / sizeof(TDIEntityID) : 0;
}

/* The one entity of the type Type that is in After and not in Before */
static
BOOL
FindNewEntity(
    _In_reads_(BeforeCount) const TDIEntityID *Before,
    _In_ ULONG BeforeCount,
    _In_reads_(AfterCount) const TDIEntityID *After,
    _In_ ULONG AfterCount,
    _In_ ULONG Type,
    _Out_ TDIEntityID *Entity)
{
    ULONG i, j, Found = 0;

    for (i = 0; i < AfterCount; i++)
    {
        if (After[i].tei_entity != Type)
            continue;
        for (j = 0; j < BeforeCount; j++)
        {
            if (Before[j].tei_entity == After[i].tei_entity &&
                Before[j].tei_instance == After[i].tei_instance)
            {
                break;
            }
        }
        if (j == BeforeCount)
        {
            *Entity = After[i];
            Found++;
        }
    }

    ok(Found == 1, "%lu new entities of type %lx\n", Found, Type);
    return Found == 1;
}

/* A socket bound to 127.0.0.1, and the entity of its address object */
static
SOCKET
BoundSocket(
    _In_ HANDLE ControlChannel,
    _In_ int SocketType,
    _Out_ struct sockaddr_in *Address,
    _Out_ TDIEntityID *Entity)
{
    static TDIEntityID Before[MAX_ENTITIES], After[MAX_ENTITIES];
    ULONG BeforeCount, AfterCount;
    SOCKET Socket;

    BeforeCount = QueryEntities(ControlChannel, Before);
    Socket = LoopbackSocket(SocketType, Address);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;
    AfterCount = QueryEntities(ControlChannel, After);
    if (!FindNewEntity(Before, BeforeCount, After, AfterCount,
                       SocketType == SOCK_STREAM ? CO_TL_ENTITY : CL_TL_ENTITY, Entity))
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }

    return Socket;
}

static
NTSTATUS
GetTdiHandles(
    _In_ SOCKET Socket,
    _In_ ULONG Flags,
    _Out_ AFD_TDI_HANDLE_DATA *Handles)
{
    Handles->TdiAddressHandle = (HANDLE)(ULONG_PTR)0xDEADBEEF;
    Handles->TdiConnectionHandle = (HANDLE)(ULONG_PTR)0xDEADBEEF;
    return AfdRequest((HANDLE)Socket, IOCTL_AFD_GET_TDI_HANDLES, &Flags, sizeof(Flags),
                      Handles, sizeof(*Handles), NULL);
}

/* A set or query of an address object option by its entity: the TTL set is
 * the one queried */
static
VOID
TestQueryByEntity(
    _In_ HANDLE ControlChannel)
{
    SOCKET Socket;
    struct sockaddr_in Address;
    TDIEntityID Entity;
    BOOL Success;

    Socket = BoundSocket(ControlChannel, SOCK_STREAM, &Address, &Entity);
    if (Socket == INVALID_SOCKET)
        return;

    Success = SetTtl(ControlChannel, Entity, TTL_BY_ENTITY);
    ok(Success, "Setting the TTL failed with %lu\n", GetLastError());
    ok_eq_ulong(QueryTtl(ControlChannel, Entity), (ULONG)TTL_BY_ENTITY);

    closesocket(Socket);
}

/* The handles are the caller's, of the socket's own objects */
static
VOID
TestTdiHandles(
    _In_ HANDLE ControlChannel)
{
    NTSTATUS Status;
    AFD_TDI_HANDLE_DATA Handles;
    SOCKET Unbound, Listener, Client, Accepted;
    struct sockaddr_in Address, ClientAddress;
    TDIEntityID Entity, ClientEntity, Other = { CO_TL_ENTITY, 0x7FFF };
    INT Ttl;
    int Result;

    /* A socket that is not bound has no TDI objects yet */
    Unbound = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Unbound != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Unbound != INVALID_SOCKET)
    {
        Status = GetTdiHandles(Unbound, AFD_ADDRESS_HANDLE | AFD_CONNECTION_HANDLE, &Handles);
        ok(Status == STATUS_SUCCESS, "IOCTL_AFD_GET_TDI_HANDLES returned %lx\n", Status);
        ok(Handles.TdiAddressHandle == NULL, "Address handle %p\n", Handles.TdiAddressHandle);
        ok(Handles.TdiConnectionHandle == NULL, "Connection handle %p\n", Handles.TdiConnectionHandle);
        closesocket(Unbound);
    }

    /* A listener has its address object, set through Winsock */
    Listener = BoundSocket(ControlChannel, SOCK_STREAM, &Address, &Entity);
    if (Listener == INVALID_SOCKET)
        return;
    Result = listen(Listener, 1);
    ok(Result == 0, "listen failed with %d\n", WSAGetLastError());
    Ttl = TTL_BY_SOCKET;
    Result = setsockopt(Listener, IPPROTO_IP, IP_TTL, (const char *)&Ttl, sizeof(Ttl));
    ok(Result == 0, "setsockopt(IP_TTL) failed with %d\n", WSAGetLastError());
    ok_eq_ulong(QueryTtl(ControlChannel, Entity), (ULONG)TTL_BY_SOCKET);

    Status = GetTdiHandles(Listener, AFD_ADDRESS_HANDLE | AFD_CONNECTION_HANDLE, &Handles);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_GET_TDI_HANDLES returned %lx\n", Status);
    ok(Handles.TdiConnectionHandle == NULL, "Connection handle %p\n", Handles.TdiConnectionHandle);
    if (NT_SUCCESS(Status))
    {
        /* Whatever entity a query on it names, it is for the listener's */
        ok_eq_ulong(QueryTtl(Handles.TdiAddressHandle, Other), (ULONG)TTL_BY_SOCKET);
        ok(CloseHandle(Handles.TdiAddressHandle), "CloseHandle failed with %lu\n", GetLastError());
    }

    /* A connected socket has both, an accepted one its connection */
    Client = BoundSocket(ControlChannel, SOCK_STREAM, &ClientAddress, &ClientEntity);
    if (Client == INVALID_SOCKET)
    {
        closesocket(Listener);
        return;
    }
    Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "connect failed with %d\n", WSAGetLastError());
    Accepted = accept(Listener, NULL, NULL);
    ok(Accepted != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());

    Status = GetTdiHandles(Client, AFD_ADDRESS_HANDLE | AFD_CONNECTION_HANDLE, &Handles);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_GET_TDI_HANDLES returned %lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        ok(Handles.TdiConnectionHandle != NULL, "No connection handle\n");
        ok_eq_ulong(QueryTtl(Handles.TdiAddressHandle, Other), QueryTtl(ControlChannel, ClientEntity));
        ok(CloseHandle(Handles.TdiAddressHandle), "CloseHandle failed with %lu\n", GetLastError());
        ok(CloseHandle(Handles.TdiConnectionHandle), "CloseHandle failed with %lu\n", GetLastError());
    }

    if (Accepted != INVALID_SOCKET)
    {
        Status = GetTdiHandles(Accepted, AFD_CONNECTION_HANDLE, &Handles);
        ok(Status == STATUS_SUCCESS, "IOCTL_AFD_GET_TDI_HANDLES returned %lx\n", Status);
        ok(Handles.TdiAddressHandle == (HANDLE)(ULONG_PTR)0xDEADBEEF, "The address handle was written\n");
        if (NT_SUCCESS(Status))
        {
            ok(Handles.TdiConnectionHandle != NULL, "No connection handle\n");

            /* Its address object options are its connection's: it has the
             * listener's, set after the listener listened... */
            ok_eq_ulong(QueryTtl(Handles.TdiConnectionHandle, Other), (ULONG)TTL_BY_SOCKET);

            /* ...and one set on it is for it alone */
            Ttl = TTL_BY_ACCEPTED;
            Result = setsockopt(Accepted, IPPROTO_IP, IP_TTL, (const char *)&Ttl, sizeof(Ttl));
            ok(Result == 0, "setsockopt(IP_TTL) failed with %d\n", WSAGetLastError());
            ok_eq_ulong(QueryTtl(Handles.TdiConnectionHandle, Other), (ULONG)TTL_BY_ACCEPTED);
            ok_eq_ulong(QueryTtl(ControlChannel, Entity), (ULONG)TTL_BY_SOCKET);

            ok(CloseHandle(Handles.TdiConnectionHandle), "CloseHandle failed with %lu\n", GetLastError());
        }
        closesocket(Accepted);
    }

    closesocket(Client);
    closesocket(Listener);
}

/* WSH_NOTIFY_BIND tells the TCP/IP helper the socket's address object: an
 * option set after it is for that address object, though another socket
 * has bound since */
static
VOID
TestHelperBind(
    _In_ HANDLE ControlChannel)
{
    HMODULE Helper;
    PWSH_OPEN_SOCKET2 pWSHOpenSocket2;
    PWSH_NOTIFY pWSHNotify;
    PWSH_SET_SOCKET_INFORMATION pWSHSetSocketInformation;
    SOCKET Socket, Later;
    struct sockaddr_in Address, LaterAddress;
    TDIEntityID Entity, LaterEntity;
    AFD_TDI_HANDLE_DATA Handles;
    INT Family = AF_INET, Type = SOCK_STREAM, Protocol = IPPROTO_TCP, Ttl;
    UNICODE_STRING DeviceName;
    PVOID Context = NULL;
    DWORD Events;
    NTSTATUS Status;
    INT Error;

    Helper = LoadLibraryW(L"wshtcpip.dll");
    ok(Helper != NULL, "Loading wshtcpip failed with %lu\n", GetLastError());
    if (!Helper)
        return;
    pWSHOpenSocket2 = (PWSH_OPEN_SOCKET2)GetProcAddress(Helper, "WSHOpenSocket2");
    pWSHNotify = (PWSH_NOTIFY)GetProcAddress(Helper, "WSHNotify");
    pWSHSetSocketInformation = (PWSH_SET_SOCKET_INFORMATION)GetProcAddress(Helper, "WSHSetSocketInformation");
    ok(pWSHOpenSocket2 && pWSHNotify && pWSHSetSocketInformation, "wshtcpip lacks an export\n");
    if (!pWSHOpenSocket2 || !pWSHNotify || !pWSHSetSocketInformation)
        goto Unload;

    /* The socket binds, then another one does, then the helper hears of the
     * first bind */
    Socket = BoundSocket(ControlChannel, SOCK_STREAM, &Address, &Entity);
    if (Socket == INVALID_SOCKET)
        goto Unload;
    Later = BoundSocket(ControlChannel, SOCK_DGRAM, &LaterAddress, &LaterEntity);
    if (Later == INVALID_SOCKET)
    {
        closesocket(Socket);
        goto Unload;
    }
    Status = GetTdiHandles(Socket, AFD_ADDRESS_HANDLE, &Handles);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_GET_TDI_HANDLES returned %lx\n", Status);

    Error = pWSHOpenSocket2(&Family, &Type, &Protocol, 0, 0, &DeviceName, &Context, &Events);
    ok(Error == NO_ERROR, "WSHOpenSocket2 failed with %d\n", Error);
    if (Error == NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, DeviceName.Buffer);

        Error = pWSHNotify(Context, Socket, Handles.TdiAddressHandle, NULL, WSH_NOTIFY_BIND);
        ok(Error == NO_ERROR, "WSH_NOTIFY_BIND failed with %d\n", Error);
        Ttl = TTL_BY_HELPER;
        Error = pWSHSetSocketInformation(Context, Socket, Handles.TdiAddressHandle, NULL,
                                         IPPROTO_IP, IP_TTL, (PCHAR)&Ttl, sizeof(Ttl));
        ok(Error == NO_ERROR, "Setting IP_TTL failed with %d\n", Error);

        ok_eq_ulong(QueryTtl(ControlChannel, Entity), (ULONG)TTL_BY_HELPER);
        ok(QueryTtl(ControlChannel, LaterEntity) != TTL_BY_HELPER,
           "The TTL was set on the address object of the socket bound later\n");

        pWSHNotify(Context, Socket, Handles.TdiAddressHandle, NULL, WSH_NOTIFY_CLOSE);
    }

    if (NT_SUCCESS(Status))
        CloseHandle(Handles.TdiAddressHandle);
    closesocket(Later);
    closesocket(Socket);

Unload:
    FreeLibrary(Helper);
}

START_TEST(tdihandles)
{
    WSADATA WsaData;
    HANDLE ControlChannel;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed with %d\n", Result);
    if (Result)
        return;

    ControlChannel = OpenTcpControlChannel();
    if (ControlChannel)
    {
        TestQueryByEntity(ControlChannel);
        TestTdiHandles(ControlChannel);
        TestHelperBind(ControlChannel);
        CloseHandle(ControlChannel);
    }

    WSACleanup();
}
