/*
 * PROJECT:     ReactOS KDNET transport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Small allocation-free SHA-256 implementation
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct _KDNET_SHA256_CONTEXT
{
    uint32_t State[8];
    uint64_t ByteCount;
    uint8_t Buffer[64];
} KDNET_SHA256_CONTEXT;

void
KdNetSha256Initialize(
    KDNET_SHA256_CONTEXT *Context);

void
KdNetSha256Update(
    KDNET_SHA256_CONTEXT *Context,
    const uint8_t *Input,
    size_t InputLength);

void
KdNetSha256Finish(
    KDNET_SHA256_CONTEXT *Context,
    uint8_t Digest[32]);
