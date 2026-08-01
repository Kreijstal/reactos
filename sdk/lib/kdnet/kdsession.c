/*
 * PROJECT:     ReactOS KD protocol
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Reliable inner KD packet session state
 */

#include <reactos/kdprotocol.h>

static void
KdPacketSessionClearResult(
    KD_PACKET_SESSION_RESULT *Result)
{
    Result->Event = KdPacketSessionEventNone;
    Result->ControlType = KD_PACKET_SESSION_NO_CONTROL;
    Result->ControlId = 0;
}

void
KdPacketSessionInitialize(
    KD_PACKET_SESSION *Session,
    uint32_t MaximumRetries)
{
    if (Session == NULL)
        return;

    Session->NextSendId = KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID;
    Session->NextReceiveId = KD_PACKET_INITIAL_ID;
    Session->PendingSendId = 0;
    Session->MaximumRetries = MaximumRetries;
    Session->RetriesRemaining = MaximumRetries;
    Session->WaitingForAcknowledge = 0;
}

KD_PACKET_STATUS
KdPacketSessionBeginSend(
    KD_PACKET_SESSION *Session,
    uint32_t *PacketId)
{
    if (Session == NULL || PacketId == NULL)
        return KdPacketStatusInvalidParameter;
    if (Session->WaitingForAcknowledge)
        return KdPacketStatusInvalidPacket;

    Session->PendingSendId = Session->NextSendId;
    Session->RetriesRemaining = Session->MaximumRetries;
    Session->WaitingForAcknowledge = 1;
    *PacketId = Session->PendingSendId;
    return KdPacketStatusSuccess;
}

static void
KdPacketSessionRequestRetry(
    KD_PACKET_SESSION *Session,
    KD_PACKET_SESSION_RESULT *Result)
{
    if (!Session->WaitingForAcknowledge)
        return;

    if (Session->RetriesRemaining == 0)
    {
        Result->Event = KdPacketSessionEventRetryLimit;
        return;
    }

    --Session->RetriesRemaining;
    Result->Event = KdPacketSessionEventResendLast;
}

KD_PACKET_STATUS
KdPacketSessionProcess(
    KD_PACKET_SESSION *Session,
    const KD_PACKET_VIEW *PacketView,
    KD_PACKET_SESSION_RESULT *Result)
{
    uint32_t PacketId;

    if (Session == NULL || PacketView == NULL || Result == NULL)
        return KdPacketStatusInvalidParameter;

    KdPacketSessionClearResult(Result);
    if (PacketView->Leader == KD_PACKET_LEADER_CONTROL)
    {
        switch (PacketView->Type)
        {
            case KD_PACKET_TYPE_ACKNOWLEDGE:
                if (!Session->WaitingForAcknowledge)
                    return KdPacketStatusSuccess;

                /* Some peers preserve the initial sync bit; others clear it. */
                PacketId = Session->PendingSendId;
                if (PacketView->Id != PacketId &&
                    PacketView->Id != (PacketId & ~KD_PACKET_SYNC_ID))
                {
                    return KdPacketStatusSuccess;
                }

                Session->NextSendId =
                    (Session->PendingSendId & ~KD_PACKET_SYNC_ID) ^ 1;
                Session->PendingSendId = 0;
                Session->WaitingForAcknowledge = 0;
                Session->RetriesRemaining = Session->MaximumRetries;
                Result->Event = KdPacketSessionEventSendAcknowledged;
                return KdPacketStatusSuccess;

            case KD_PACKET_TYPE_RESEND:
                KdPacketSessionRequestRetry(Session, Result);
                return KdPacketStatusSuccess;

            case KD_PACKET_TYPE_RESET:
                KdPacketSessionInitialize(Session, Session->MaximumRetries);
                Result->Event = KdPacketSessionEventReset;
                Result->ControlType = KD_PACKET_TYPE_RESET;
                Result->ControlId = 0;
                return KdPacketStatusSuccess;

            default:
                return KdPacketStatusInvalidPacket;
        }
    }

    if (PacketView->Leader != KD_PACKET_LEADER_DATA)
        return KdPacketStatusInvalidPacket;

    Result->ControlType = KD_PACKET_TYPE_ACKNOWLEDGE;
    Result->ControlId = PacketView->Id;
    PacketId = PacketView->Id & ~KD_PACKET_SYNC_ID;

    if ((PacketView->Id & KD_PACKET_SYNC_ID) != 0)
    {
        Session->NextReceiveId = PacketId ^ 1;
        Result->Event = KdPacketSessionEventDataReceived;
    }
    else if (PacketId == Session->NextReceiveId)
    {
        Session->NextReceiveId ^= 1;
        Result->Event = KdPacketSessionEventDataReceived;
    }
    else
    {
        /* Valid duplicate packets are acknowledged but not delivered twice. */
        Result->Event = KdPacketSessionEventDuplicateData;
    }

    return KdPacketStatusSuccess;
}

KD_PACKET_STATUS
KdPacketSessionTimeout(
    KD_PACKET_SESSION *Session,
    KD_PACKET_SESSION_RESULT *Result)
{
    if (Session == NULL || Result == NULL)
        return KdPacketStatusInvalidParameter;

    KdPacketSessionClearResult(Result);
    KdPacketSessionRequestRetry(Session, Result);
    return KdPacketStatusSuccess;
}

void
KdPacketSessionRejectMalformed(
    KD_PACKET_SESSION_RESULT *Result)
{
    if (Result == NULL)
        return;

    KdPacketSessionClearResult(Result);
    Result->ControlType = KD_PACKET_TYPE_RESEND;
    Result->ControlId = 0;
}
