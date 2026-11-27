/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/ainfo.c
 * PURPOSE:     Per-socket information.
 * PROGRAMMER:  Cameron Gutman
 */

#include "precomp.h"

/* The options of a new address object */
VOID InitializeAddressOptions(PADDRESS_OPTIONS Options)
{
    Options->TTL = 128;
    Options->DF = 0;
    Options->BCast = 1;
    Options->HeaderIncl = 1;
    Options->UnicastIfIndex = 0;
}

/* Sets option ID of Options, whose owner the caller has locked */
static
TDI_STATUS SetAddressOption(TDIObjectID *ID,
                            PADDRESS_OPTIONS Options,
                            PVOID Buffer,
                            UINT BufferSize)
{
    switch (ID->toi_id)
    {
      case AO_OPTION_TTL:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         Options->TTL = *((PUCHAR)Buffer);
         return TDI_SUCCESS;

      case AO_OPTION_IP_DONTFRAGMENT:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         Options->DF = *((PUINT)Buffer);
         return TDI_SUCCESS;

      case AO_OPTION_BROADCAST:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         Options->BCast = *((PUINT)Buffer);
         return TDI_SUCCESS;

      case AO_OPTION_IP_HDRINCL:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         Options->HeaderIncl = *((PUINT)Buffer);
         return TDI_SUCCESS;

      case AO_OPTION_IP_UCASTIF:
      {
         UINT IfIndex;

         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         /* The interface index arrives in host byte order (wshtcpip converts
          * the network byte order value Windows uses for IP_UNICAST_IF) */
         IfIndex = *((PUINT)Buffer);

         /* An index of zero clears the binding; anything else must name an
          * interface that exists right now, exactly like Windows which fails
          * the option instead of silently sending out of another adapter */
         if ((IfIndex != 0) && (GetInterfaceByIndex(IfIndex) == NULL))
             return TDI_INVALID_PARAMETER;

         Options->UnicastIfIndex = IfIndex;
         return TDI_SUCCESS;
      }

      default:
         DbgPrint("Unimplemented option %x\n", ID->toi_id);

         return TDI_INVALID_REQUEST;
    }
}

/* The value of option ID of Options, whose owner the caller has locked */
static
TDI_STATUS GetAddressOption(TDIObjectID *ID,
                            PADDRESS_OPTIONS Options,
                            PUINT Value)
{
    switch (ID->toi_id)
    {
      case AO_OPTION_TTL:
         *Value = Options->TTL;
         return TDI_SUCCESS;

      case AO_OPTION_IP_DONTFRAGMENT:
         *Value = Options->DF;
         return TDI_SUCCESS;

      case AO_OPTION_BROADCAST:
         *Value = Options->BCast;
         return TDI_SUCCESS;

      case AO_OPTION_IP_HDRINCL:
         *Value = Options->HeaderIncl;
         return TDI_SUCCESS;

      case AO_OPTION_IP_UCASTIF:
         *Value = Options->UnicastIfIndex;
         return TDI_SUCCESS;

      default:
         DbgPrint("Unimplemented option %x\n", ID->toi_id);

         return TDI_INVALID_REQUEST;
    }
}

/* Answers an option's Value in the caller's buffer, which Buffer describes */
static
TDI_STATUS CopyOutAddressOption(UINT Value,
                                PNDIS_BUFFER Buffer,
                                PUINT BufferSize)
{
    if (*BufferSize < sizeof(UINT))
    {
        *BufferSize = sizeof(UINT);
        return TDI_BUFFER_TOO_SMALL;
    }

    return InfoCopyOut((PCHAR)&Value, sizeof(Value), Buffer, BufferSize);
}

TDI_STATUS SetAddressFileInfo(TDIObjectID *ID,
                              PADDRESS_FILE AddrFile,
                              PVOID Buffer,
                              UINT BufferSize)
{
    TDI_STATUS Status;

    LockObject(AddrFile);
    Status = SetAddressOption(ID, &AddrFile->Options, Buffer, BufferSize);
    UnlockObject(AddrFile);

    return Status;
}

TDI_STATUS GetAddressFileInfo(TDIObjectID *ID,
                              PADDRESS_FILE AddrFile,
                              PNDIS_BUFFER Buffer,
                              PUINT BufferSize)
{
    TDI_STATUS Status;
    UINT Value;

    LockObject(AddrFile);
    Status = GetAddressOption(ID, &AddrFile->Options, &Value);
    UnlockObject(AddrFile);

    if (Status != TDI_SUCCESS)
        return Status;

    return CopyOutAddressOption(Value, Buffer, BufferSize);
}

/* An address object option set on a connection is the connection's own:
 * its address file can be a listener's, which many connections share */
TDI_STATUS SetConnectionAddressInfo(TDIObjectID *ID,
                                    PCONNECTION_ENDPOINT Connection,
                                    PVOID Buffer,
                                    UINT BufferSize)
{
    TDI_STATUS Status;

    LockObject(Connection);
    Status = SetAddressOption(ID, &Connection->AddressOptions, Buffer, BufferSize);
    UnlockObject(Connection);

    return Status;
}

TDI_STATUS GetConnectionAddressInfo(TDIObjectID *ID,
                                    PCONNECTION_ENDPOINT Connection,
                                    PNDIS_BUFFER Buffer,
                                    PUINT BufferSize)
{
    TDI_STATUS Status;
    UINT Value;

    LockObject(Connection);
    Status = GetAddressOption(ID, &Connection->AddressOptions, &Value);
    UnlockObject(Connection);

    if (Status != TDI_SUCCESS)
        return Status;

    return CopyOutAddressOption(Value, Buffer, BufferSize);
}
