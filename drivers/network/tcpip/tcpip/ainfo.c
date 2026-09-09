/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/ainfo.c
 * PURPOSE:     Per-socket information.
 * PROGRAMMER:  Cameron Gutman
 */

#include "precomp.h"

TDI_STATUS SetAddressFileInfo(TDIObjectID *ID,
                              PADDRESS_FILE AddrFile,
                              PVOID Buffer,
                              UINT BufferSize)
{
    switch (ID->toi_id)
    {
      case AO_OPTION_TTL:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->TTL = *((PUCHAR)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_IP_DONTFRAGMENT:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->DF = *((PUINT)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_BROADCAST:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->BCast = *((PUINT)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_IP_HDRINCL:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->HeaderIncl = *((PUINT)Buffer);
         UnlockObject(AddrFile);

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

         LockObject(AddrFile);
         AddrFile->UnicastIfIndex = IfIndex;
         UnlockObject(AddrFile);

         return TDI_SUCCESS;
      }

      default:
         DbgPrint("Unimplemented option %x\n", ID->toi_id);

         return TDI_INVALID_REQUEST;
    }
}

TDI_STATUS GetAddressFileInfo(TDIObjectID *ID,
                              PADDRESS_FILE AddrFile,
                              PVOID Buffer,
                              PUINT BufferSize)
{
    UINT Value;

    switch (ID->toi_id)
    {
      case AO_OPTION_TTL:
         LockObject(AddrFile);
         Value = AddrFile->TTL;
         UnlockObject(AddrFile);
         break;

      case AO_OPTION_IP_DONTFRAGMENT:
         LockObject(AddrFile);
         Value = AddrFile->DF;
         UnlockObject(AddrFile);
         break;

      case AO_OPTION_BROADCAST:
         LockObject(AddrFile);
         Value = AddrFile->BCast;
         UnlockObject(AddrFile);
         break;

      case AO_OPTION_IP_HDRINCL:
         LockObject(AddrFile);
         Value = AddrFile->HeaderIncl;
         UnlockObject(AddrFile);
         break;

      case AO_OPTION_IP_UCASTIF:
         LockObject(AddrFile);
         Value = AddrFile->UnicastIfIndex;
         UnlockObject(AddrFile);
         break;

      default:
         DbgPrint("Unimplemented option %x\n", ID->toi_id);

         return TDI_INVALID_REQUEST;
    }

    if (*BufferSize < sizeof(UINT))
    {
        *BufferSize = sizeof(UINT);
        return TDI_BUFFER_TOO_SMALL;
    }

    *((PUINT)Buffer) = Value;
    *BufferSize = sizeof(UINT);

    return TDI_SUCCESS;
}
