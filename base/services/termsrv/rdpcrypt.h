/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Standard RDP security (RC4 + Server Proprietary Certificate)
 * COPYRIGHT:   Copyright 2026 ReactOS Terminal Services contributors
 *
 * Implements the "RDP Standard" cryptographic layer described by MS-RDPBCGR:
 *  - a Server Proprietary Certificate advertising a fixed RSA public key,
 *    signed with the well-known Terminal Services signing key,
 *  - RSA decryption of the client random exchanged in the Security Exchange
 *    PDU,
 *  - the MD5/SHA1 session-key derivation, and
 *  - RC4 decryption (with the 4096-packet key update) of the client-to-server
 *    stream.
 *
 * The listener advertises ENCRYPTION_LEVEL_LOW, so only the client-to-server
 * direction is encrypted; server-to-client output stays in the clear and needs
 * no security header (MS-RDPBCGR 5.3.2).
 */

#ifndef _TERMSRV_RDPCRYPT_H_
#define _TERMSRV_RDPCRYPT_H_

#include <windef.h>

#define TERMSRV_RDP_SERVER_RANDOM_LENGTH 32
#define TERMSRV_RDP_SESSION_KEY_LENGTH 16
#define TERMSRV_RDP_MAC_LENGTH 8

typedef struct _TERMSRV_RC4_STATE
{
    UCHAR S[256];
    UCHAR I;
    UCHAR J;
} TERMSRV_RC4_STATE;

typedef struct _TERMSRV_RDP_CRYPT
{
    BOOL Enabled;                                      /* key exchange completed */
    UCHAR ServerRandom[TERMSRV_RDP_SERVER_RANDOM_LENGTH];
    UCHAR ClientRandom[TERMSRV_RDP_SERVER_RANDOM_LENGTH];
    UCHAR MacKey[TERMSRV_RDP_SESSION_KEY_LENGTH];
    UCHAR InitialDecryptKey[TERMSRV_RDP_SESSION_KEY_LENGTH];   /* client encrypt key */
    UCHAR CurrentDecryptKey[TERMSRV_RDP_SESSION_KEY_LENGTH];
    TERMSRV_RC4_STATE Rc4Decrypt;
    ULONG DecryptPacketCount;
} TERMSRV_RDP_CRYPT;

/* Generate a fresh server random. Call once per connection before building the
 * MCS Connect Response. */
VOID
TermSrvCryptInit(
    _Out_ TERMSRV_RDP_CRYPT *Crypt);

/* Build the SC_SECURITY (TS_UD_SC_SEC1) GCC user-data block, including its
 * 4-byte user-data header, advertising 128-bit RC4 encryption at LOW level plus
 * the server random and Proprietary Certificate. Returns the number of bytes
 * written, or 0 on failure. */
SIZE_T
TermSrvCryptBuildServerSecurityBlock(
    _Out_writes_bytes_to_(BufferLength, return) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDP_CRYPT *Crypt);

/* Complete the key exchange from the Security Exchange PDU payload (the 4-byte
 * length determinant followed by the RSA-encrypted client random). Derives the
 * MAC and RC4 session keys and enables decryption. */
BOOL
TermSrvCryptCompleteKeyExchange(
    _Inout_ TERMSRV_RDP_CRYPT *Crypt,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength);

/* Decrypt a fully-received inbound PDU in place. Handles both slow-path
 * (TPKT/MCS Send Data) and fast-path input PDUs: when the security header (or
 * fast-path header) marks the PDU encrypted, the 8-byte MAC is stripped, the
 * payload is RC4-decrypted, and the length/flag fields are rewritten so the
 * existing plaintext parsers accept the result. No-op until the key exchange
 * has completed or when the PDU is not encrypted. *Length is updated in place. */
BOOL
TermSrvCryptUnwrapInbound(
    _Inout_ TERMSRV_RDP_CRYPT *Crypt,
    _Inout_updates_bytes_(*Length) UCHAR *Buffer,
    _Inout_ SIZE_T *Length);

#endif /* _TERMSRV_RDPCRYPT_H_ */
