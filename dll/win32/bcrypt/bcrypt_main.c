/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include <wine/config.h>
#include <wine/port.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <ntsecapi.h>
#include <bcrypt.h>

#include <wine/debug.h>
#include <wine/unicode.h>
#include <wine/library.h>

#ifdef SONAME_LIBMBEDTLS
#include <mbedtls/md.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/rsa.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(bcrypt);

static HINSTANCE instance;

#if defined(HAVE_GNUTLS_HASH) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H)
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static void *libgnutls_handle;
#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gnutls_global_deinit);
MAKE_FUNCPTR(gnutls_global_init);
MAKE_FUNCPTR(gnutls_global_set_log_function);
MAKE_FUNCPTR(gnutls_global_set_log_level);
MAKE_FUNCPTR(gnutls_hash);
MAKE_FUNCPTR(gnutls_hash_deinit);
MAKE_FUNCPTR(gnutls_hash_init);
MAKE_FUNCPTR(gnutls_hmac);
MAKE_FUNCPTR(gnutls_hmac_deinit);
MAKE_FUNCPTR(gnutls_hmac_init);
MAKE_FUNCPTR(gnutls_perror);
#undef MAKE_FUNCPTR

static void gnutls_log( int level, const char *msg )
{
    TRACE( "<%d> %s", level, msg );
}

static BOOL gnutls_initialize(void)
{
    int ret;

    if (!(libgnutls_handle = wine_dlopen( SONAME_LIBGNUTLS, RTLD_NOW, NULL, 0 )))
    {
        ERR_(winediag)( "failed to load libgnutls, no support for crypto hashes\n" );
        return FALSE;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = wine_dlsym( libgnutls_handle, #f, NULL, 0 ))) \
    { \
        ERR( "failed to load %s\n", #f ); \
        goto fail; \
    }

    LOAD_FUNCPTR(gnutls_global_deinit)
    LOAD_FUNCPTR(gnutls_global_init)
    LOAD_FUNCPTR(gnutls_global_set_log_function)
    LOAD_FUNCPTR(gnutls_global_set_log_level)
    LOAD_FUNCPTR(gnutls_hash);
    LOAD_FUNCPTR(gnutls_hash_deinit);
    LOAD_FUNCPTR(gnutls_hash_init);
    LOAD_FUNCPTR(gnutls_hmac);
    LOAD_FUNCPTR(gnutls_hmac_deinit);
    LOAD_FUNCPTR(gnutls_hmac_init);
    LOAD_FUNCPTR(gnutls_perror)
#undef LOAD_FUNCPTR

    if ((ret = pgnutls_global_init()) != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror( ret );
        goto fail;
    }

    if (TRACE_ON( bcrypt ))
    {
        pgnutls_global_set_log_level( 4 );
        pgnutls_global_set_log_function( gnutls_log );
    }

    return TRUE;

fail:
    wine_dlclose( libgnutls_handle, NULL, 0 );
    libgnutls_handle = NULL;
    return FALSE;
}

static void gnutls_uninitialize(void)
{
    pgnutls_global_deinit();
    wine_dlclose( libgnutls_handle, NULL, 0 );
    libgnutls_handle = NULL;
}
#elif defined(SONAME_LIBMBEDTLS) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H) && !defined(__REACTOS__)
WINE_DECLARE_DEBUG_CHANNEL(winediag);

void *libmbedtls_handle;

#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(mbedtls_md_init);
MAKE_FUNCPTR(mbedtls_md_setup);
MAKE_FUNCPTR(mbedtls_md_update);
MAKE_FUNCPTR(mbedtls_md_hmac_starts);
MAKE_FUNCPTR(mbedtls_md_hmac_finish);
MAKE_FUNCPTR(mbedtls_md_free);
MAKE_FUNCPTR(mbedtls_md5_init);
MAKE_FUNCPTR(mbedtls_md5_starts);
MAKE_FUNCPTR(mbedtls_md5_update);
MAKE_FUNCPTR(mbedtls_md5_finish);
MAKE_FUNCPTR(mbedtls_md5_free);
MAKE_FUNCPTR(mbedtls_sha1_init);
MAKE_FUNCPTR(mbedtls_sha1_starts);
MAKE_FUNCPTR(mbedtls_sha1_update);
MAKE_FUNCPTR(mbedtls_sha1_finish);
MAKE_FUNCPTR(mbedtls_sha1_free);
MAKE_FUNCPTR(mbedtls_sha256_init);
MAKE_FUNCPTR(mbedtls_sha256_starts);
MAKE_FUNCPTR(mbedtls_sha256_update);
MAKE_FUNCPTR(mbedtls_sha256_finish);
MAKE_FUNCPTR(mbedtls_sha256_free);
MAKE_FUNCPTR(mbedtls_sha512_init);
MAKE_FUNCPTR(mbedtls_sha512_starts);
MAKE_FUNCPTR(mbedtls_sha512_update);
MAKE_FUNCPTR(mbedtls_sha512_finish);
MAKE_FUNCPTR(mbedtls_sha512_free);
#undef MAKE_FUNCPTR

#define mbedtls_md_init             pmbedtls_md_init
#define mbedtls_md_setup            pmbedtls_md_setup
#define mbedtls_md_update           pmbedtls_md_update
#define mbedtls_md_hmac_starts      pmbedtls_md_hmac_starts
#define mbedtls_md_hmac_finish      pmbedtls_md_hmac_finish
#define mbedtls_md_free             pmbedtls_md_free
#define mbedtls_md5_init            pmbedtls_md5_init
#define mbedtls_md5_starts          pmbedtls_md5_starts
#define mbedtls_md5_update          pmbedtls_md5_update
#define mbedtls_md5_finish          pmbedtls_md5_finish
#define mbedtls_md5_free            pmbedtls_md5_free
#define mbedtls_sha1_init           pmbedtls_sha1_init
#define mbedtls_sha1_starts         pmbedtls_sha1_starts
#define mbedtls_sha1_update         pmbedtls_sha1_update
#define mbedtls_sha1_finish         pmbedtls_sha1_finish
#define mbedtls_sha1_free           pmbedtls_sha1_free
#define mbedtls_sha256_init         pmbedtls_sha256_init
#define mbedtls_sha256_starts       pmbedtls_sha256_starts
#define mbedtls_sha256_update       pmbedtls_sha256_update
#define mbedtls_sha256_finish       pmbedtls_sha256_finish
#define mbedtls_sha256_free         pmbedtls_sha256_free
#define mbedtls_sha512_init         pmbedtls_sha512_init
#define mbedtls_sha512_starts       pmbedtls_sha512_starts
#define mbedtls_sha512_update       pmbedtls_sha512_update
#define mbedtls_sha512_finish       pmbedtls_sha512_finish
#define mbedtls_sha512_free         pmbedtls_sha512_free

static BOOL mbedtls_initialize(void)
{
    if (!(libmbedtls_handle = wine_dlopen( SONAME_LIBMBEDTLS, RTLD_NOW, NULL, 0 )))
    {
        ERR_(winediag)( "failed to load libmbedtls, no support for crypto hashes\n" );
        return FALSE;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = wine_dlsym( libmbedtls_handle, #f, NULL, 0 ))) \
    { \
        ERR( "failed to load %s\n", #f ); \
        goto fail; \
    }

    LOAD_FUNCPTR(mbedtls_md_init)
    LOAD_FUNCPTR(mbedtls_md_setup)
    LOAD_FUNCPTR(mbedtls_md_update)
    LOAD_FUNCPTR(mbedtls_md_hmac_starts)
    LOAD_FUNCPTR(mbedtls_md_hmac_finish)
    LOAD_FUNCPTR(mbedtls_md_free);
    LOAD_FUNCPTR(mbedtls_md5_init)
    LOAD_FUNCPTR(mbedtls_md5_starts)
    LOAD_FUNCPTR(mbedtls_md5_update)
    LOAD_FUNCPTR(mbedtls_md5_finish)
    LOAD_FUNCPTR(mbedtls_md5_free);
    LOAD_FUNCPTR(mbedtls_sha1_init)
    LOAD_FUNCPTR(mbedtls_sha1_starts)
    LOAD_FUNCPTR(mbedtls_sha1_update)
    LOAD_FUNCPTR(mbedtls_sha1_finish)
    LOAD_FUNCPTR(mbedtls_sha1_free);
    LOAD_FUNCPTR(mbedtls_sha256_init)
    LOAD_FUNCPTR(mbedtls_sha256_starts)
    LOAD_FUNCPTR(mbedtls_sha256_update)
    LOAD_FUNCPTR(mbedtls_sha256_finish)
    LOAD_FUNCPTR(mbedtls_sha256_free);
    LOAD_FUNCPTR(mbedtls_sha512_init)
    LOAD_FUNCPTR(mbedtls_sha512_starts)
    LOAD_FUNCPTR(mbedtls_sha512_update)
    LOAD_FUNCPTR(mbedtls_sha512_finish)
    LOAD_FUNCPTR(mbedtls_sha512_free);
#undef LOAD_FUNCPTR

    return TRUE;

fail:
    wine_dlclose( libmbedtls_handle, NULL, 0 );
    libmbedtls_handle = NULL;
    return FALSE;
}

static void mbedtls_uninitialize(void)
{
    wine_dlclose( libmbedtls_handle, NULL, 0 );
    libmbedtls_handle = NULL;
}
#endif /* SONAME_LIBMBEDTLS && !HAVE_COMMONCRYPTO_COMMONDIGEST_H && !__REACTOS__ */

NTSTATUS WINAPI BCryptEnumAlgorithms(ULONG dwAlgOperations, ULONG *pAlgCount,
                                     BCRYPT_ALGORITHM_IDENTIFIER **ppAlgList, ULONG dwFlags)
{
    FIXME("%08x, %p, %p, %08x - stub\n", dwAlgOperations, pAlgCount, ppAlgList, dwFlags);

    *ppAlgList=NULL;
    *pAlgCount=0;

    return STATUS_NOT_IMPLEMENTED;
}

#define MAGIC_ALG  (('A' << 24) | ('L' << 16) | ('G' << 8) | '0')
#define MAGIC_HASH (('H' << 24) | ('A' << 16) | ('S' << 8) | 'H')
struct object
{
    ULONG magic;
};

enum alg_id
{
    ALG_ID_MD5,
    ALG_ID_RNG,
    ALG_ID_SHA1,
    ALG_ID_SHA256,
    ALG_ID_SHA384,
    ALG_ID_SHA512,
    ALG_ID_ECDSA_P256,
    ALG_ID_ECDSA_P384,
};

static const struct {
    ULONG hash_length;
    const WCHAR *alg_name;
} alg_props[] = {
    /* ALG_ID_MD5    */ { 16, BCRYPT_MD5_ALGORITHM },
    /* ALG_ID_RNG    */ {  0, BCRYPT_RNG_ALGORITHM },
    /* ALG_ID_SHA1   */ { 20, BCRYPT_SHA1_ALGORITHM },
    /* ALG_ID_SHA256 */ { 32, BCRYPT_SHA256_ALGORITHM },
    /* ALG_ID_SHA384 */ { 48, BCRYPT_SHA384_ALGORITHM },
    /* ALG_ID_SHA512 */ { 64, BCRYPT_SHA512_ALGORITHM },
    /* ALG_ID_ECDSA_P256 */ { 0, BCRYPT_ECDSA_P256_ALGORITHM },
    /* ALG_ID_ECDSA_P384 */ { 0, BCRYPT_ECDSA_P384_ALGORITHM },
};

struct algorithm
{
    struct object hdr;
    enum alg_id   id;
    BOOL hmac;
};

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE handle, UCHAR *buffer, ULONG count, ULONG flags)
{
    const DWORD supported_flags = BCRYPT_USE_SYSTEM_PREFERRED_RNG;
    struct algorithm *algorithm = handle;

    TRACE("%p, %p, %u, %08x - semi-stub\n", handle, buffer, count, flags);

    if (!algorithm)
    {
        /* It's valid to call without an algorithm if BCRYPT_USE_SYSTEM_PREFERRED_RNG
         * is set. In this case the preferred system RNG is used.
         */
        if (!(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
            return STATUS_INVALID_HANDLE;
    }
    else if (algorithm->hdr.magic != MAGIC_ALG || algorithm->id != ALG_ID_RNG)
        return STATUS_INVALID_HANDLE;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    if (flags & ~supported_flags)
        FIXME("unsupported flags %08x\n", flags & ~supported_flags);

    if (algorithm)
        FIXME("ignoring selected algorithm\n");

    /* When zero bytes are requested the function returns success too. */
    if (!count)
        return STATUS_SUCCESS;

    if (algorithm || (flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
    {
        if (RtlGenRandom(buffer, count))
            return STATUS_SUCCESS;
    }

    FIXME("called with unsupported parameters, returning error\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI BCryptOpenAlgorithmProvider( BCRYPT_ALG_HANDLE *handle, LPCWSTR id, LPCWSTR implementation, DWORD flags )
{
    struct algorithm *alg;
    enum alg_id alg_id;

    const DWORD supported_flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;

    TRACE( "%p, %s, %s, %08x\n", handle, wine_dbgstr_w(id), wine_dbgstr_w(implementation), flags );

    if (!handle || !id) return STATUS_INVALID_PARAMETER;
    if (flags & ~supported_flags)
    {
        FIXME( "unsupported flags %08x\n", flags & ~supported_flags);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!strcmpW( id, BCRYPT_SHA1_ALGORITHM )) alg_id = ALG_ID_SHA1;
    else if (!strcmpW( id, BCRYPT_MD5_ALGORITHM )) alg_id = ALG_ID_MD5;
    else if (!strcmpW( id, BCRYPT_RNG_ALGORITHM )) alg_id = ALG_ID_RNG;
    else if (!strcmpW( id, BCRYPT_SHA256_ALGORITHM )) alg_id = ALG_ID_SHA256;
    else if (!strcmpW( id, BCRYPT_SHA384_ALGORITHM )) alg_id = ALG_ID_SHA384;
    else if (!strcmpW( id, BCRYPT_SHA512_ALGORITHM )) alg_id = ALG_ID_SHA512;
    else if (!strcmpW( id, BCRYPT_ECDSA_P256_ALGORITHM )) alg_id = ALG_ID_ECDSA_P256;
    else if (!strcmpW( id, BCRYPT_ECDSA_P384_ALGORITHM )) alg_id = ALG_ID_ECDSA_P384;
    else
    {
        FIXME( "algorithm %s not supported\n", debugstr_w(id) );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (implementation && strcmpW( implementation, MS_PRIMITIVE_PROVIDER ))
    {
        FIXME( "implementation %s not supported\n", debugstr_w(implementation) );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(alg = HeapAlloc( GetProcessHeap(), 0, sizeof(*alg) ))) return STATUS_NO_MEMORY;
    alg->hdr.magic = MAGIC_ALG;
    alg->id        = alg_id;
    alg->hmac      = flags & BCRYPT_ALG_HANDLE_HMAC_FLAG;

    *handle = alg;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider( BCRYPT_ALG_HANDLE handle, DWORD flags )
{
    struct algorithm *alg = handle;

    TRACE( "%p, %08x\n", handle, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    HeapFree( GetProcessHeap(), 0, alg );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptGetFipsAlgorithmMode(BOOLEAN *enabled)
{
    FIXME("%p - semi-stub\n", enabled);

    if (!enabled)
        return STATUS_INVALID_PARAMETER;

    *enabled = FALSE;
    return STATUS_SUCCESS;
}

#ifdef HAVE_COMMONCRYPTO_COMMONDIGEST_H
struct hash
{
    struct object hdr;
    enum alg_id   alg_id;
    BOOL hmac;
    union
    {
        CC_MD5_CTX    md5_ctx;
        CC_SHA1_CTX   sha1_ctx;
        CC_SHA256_CTX sha256_ctx;
        CC_SHA512_CTX sha512_ctx;
        CCHmacContext hmac_ctx;
    } u;
};

static NTSTATUS hash_init( struct hash *hash )
{
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        CC_MD5_Init( &hash->u.md5_ctx );
        break;

    case ALG_ID_SHA1:
        CC_SHA1_Init( &hash->u.sha1_ctx );
        break;

    case ALG_ID_SHA256:
        CC_SHA256_Init( &hash->u.sha256_ctx );
        break;

    case ALG_ID_SHA384:
        CC_SHA384_Init( &hash->u.sha512_ctx );
        break;

    case ALG_ID_SHA512:
        CC_SHA512_Init( &hash->u.sha512_ctx );
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hmac_init( struct hash *hash, UCHAR *key, ULONG key_size )
{
    CCHmacAlgorithm cc_algorithm;
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        cc_algorithm = kCCHmacAlgMD5;
        break;

    case ALG_ID_SHA1:
        cc_algorithm = kCCHmacAlgSHA1;
        break;

    case ALG_ID_SHA256:
        cc_algorithm = kCCHmacAlgSHA256;
        break;

    case ALG_ID_SHA384:
        cc_algorithm = kCCHmacAlgSHA384;
        break;

    case ALG_ID_SHA512:
        cc_algorithm = kCCHmacAlgSHA512;
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    CCHmacInit( &hash->u.hmac_ctx, cc_algorithm, key, key_size );
    return STATUS_SUCCESS;
}


static NTSTATUS hash_update( struct hash *hash, UCHAR *input, ULONG size )
{
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        CC_MD5_Update( &hash->u.md5_ctx, input, size );
        break;

    case ALG_ID_SHA1:
        CC_SHA1_Update( &hash->u.sha1_ctx, input, size );
        break;

    case ALG_ID_SHA256:
        CC_SHA256_Update( &hash->u.sha256_ctx, input, size );
        break;

    case ALG_ID_SHA384:
        CC_SHA384_Update( &hash->u.sha512_ctx, input, size );
        break;

    case ALG_ID_SHA512:
        CC_SHA512_Update( &hash->u.sha512_ctx, input, size );
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hmac_update( struct hash *hash, UCHAR *input, ULONG size )
{
    CCHmacUpdate( &hash->u.hmac_ctx, input, size );
    return STATUS_SUCCESS;
}

static NTSTATUS hash_finish( struct hash *hash, UCHAR *output, ULONG size )
{
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        CC_MD5_Final( output, &hash->u.md5_ctx );
        break;

    case ALG_ID_SHA1:
        CC_SHA1_Final( output, &hash->u.sha1_ctx );
        break;

    case ALG_ID_SHA256:
        CC_SHA256_Final( output, &hash->u.sha256_ctx );
        break;

    case ALG_ID_SHA384:
        CC_SHA384_Final( output, &hash->u.sha512_ctx );
        break;

    case ALG_ID_SHA512:
        CC_SHA512_Final( output, &hash->u.sha512_ctx );
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        break;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hmac_finish( struct hash *hash, UCHAR *output, ULONG size )
{
    CCHmacFinal( &hash->u.hmac_ctx, output );

    return STATUS_SUCCESS;
}
#elif defined(HAVE_GNUTLS_HASH)
struct hash
{
    struct object    hdr;
    enum alg_id      alg_id;
    BOOL hmac;
    union
    {
        gnutls_hash_hd_t hash_handle;
        gnutls_hmac_hd_t hmac_handle;
    } u;
};

static NTSTATUS hash_init( struct hash *hash )
{
    gnutls_digest_algorithm_t alg;

    if (!libgnutls_handle) return STATUS_INTERNAL_ERROR;

    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        alg = GNUTLS_DIG_MD5;
        break;
    case ALG_ID_SHA1:
        alg = GNUTLS_DIG_SHA1;
        break;

    case ALG_ID_SHA256:
        alg = GNUTLS_DIG_SHA256;
        break;

    case ALG_ID_SHA384:
        alg = GNUTLS_DIG_SHA384;
        break;

    case ALG_ID_SHA512:
        alg = GNUTLS_DIG_SHA512;
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (pgnutls_hash_init( &hash->u.hash_handle, alg )) return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS hmac_init( struct hash *hash, UCHAR *key, ULONG key_size )
{
    gnutls_mac_algorithm_t alg;

    if (!libgnutls_handle) return STATUS_INTERNAL_ERROR;

    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        alg = GNUTLS_MAC_MD5;
        break;
    case ALG_ID_SHA1:
        alg = GNUTLS_MAC_SHA1;
        break;

    case ALG_ID_SHA256:
        alg = GNUTLS_MAC_SHA256;
        break;

    case ALG_ID_SHA384:
        alg = GNUTLS_MAC_SHA384;
        break;

    case ALG_ID_SHA512:
        alg = GNUTLS_MAC_SHA512;
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (pgnutls_hmac_init( &hash->u.hmac_handle, alg, key, key_size )) return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS hash_update( struct hash *hash, UCHAR *input, ULONG size )
{
    if (pgnutls_hash( hash->u.hash_handle, input, size )) return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS hmac_update( struct hash *hash, UCHAR *input, ULONG size )
{
    if (pgnutls_hmac( hash->u.hmac_handle, input, size )) return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS hash_finish( struct hash *hash, UCHAR *output, ULONG size )
{
    pgnutls_hash_deinit( hash->u.hash_handle, output );
    return STATUS_SUCCESS;
}

static NTSTATUS hmac_finish( struct hash *hash, UCHAR *output, ULONG size )
{
    pgnutls_hmac_deinit( hash->u.hmac_handle, output );
    return STATUS_SUCCESS;
}
#elif defined(SONAME_LIBMBEDTLS)
struct hash
{
    struct object hdr;
    BOOL hmac;
    enum alg_id   alg_id;
    union
    {
        mbedtls_md5_context    md5_ctx;
        mbedtls_sha1_context   sha1_ctx;
        mbedtls_sha256_context sha256_ctx;
        mbedtls_sha512_context sha512_ctx;
        mbedtls_md_context_t   hmac_ctx;
    } u;
};

static NTSTATUS hash_init( struct hash *hash )
{
#ifndef __REACTOS__
    if (!libmbedtls_handle) return STATUS_INTERNAL_ERROR;
#endif
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        mbedtls_md5_init(&hash->u.md5_ctx);
        mbedtls_md5_starts(&hash->u.md5_ctx);
        break;

    case ALG_ID_SHA1:
        mbedtls_sha1_init(&hash->u.sha1_ctx);
        mbedtls_sha1_starts(&hash->u.sha1_ctx);
        break;

    case ALG_ID_SHA256:
        mbedtls_sha256_init(&hash->u.sha256_ctx);
        mbedtls_sha256_starts(&hash->u.sha256_ctx, FALSE);
        break;

    case ALG_ID_SHA384:
    case ALG_ID_SHA512:
        mbedtls_sha512_init(&hash->u.sha512_ctx);
        mbedtls_sha512_starts(&hash->u.sha512_ctx, hash->alg_id==ALG_ID_SHA384);
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS hmac_init( struct hash *hash, UCHAR *key, ULONG key_size )
{
    const mbedtls_md_info_t *md_info;
    mbedtls_md_type_t md_type;
    int ret;
#ifndef __REACTOS__
    if (!libmbedtls_handle) return STATUS_INTERNAL_ERROR;
#endif
    mbedtls_md_init(&hash->u.hmac_ctx);
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        md_type = MBEDTLS_MD_MD5;
        break;

    case ALG_ID_SHA1:
        md_type = MBEDTLS_MD_SHA1;
        break;

    case ALG_ID_SHA256:
        md_type = MBEDTLS_MD_SHA256;
        break;

    case ALG_ID_SHA384:
        md_type = MBEDTLS_MD_SHA384;
        break;

    case ALG_ID_SHA512:
        md_type = MBEDTLS_MD_SHA512;
        break;

    default:
        ERR("unhandled id %u\n", hash->alg_id);
        return STATUS_NOT_IMPLEMENTED;
    }
    if ((md_info = mbedtls_md_info_from_type(md_type)) == NULL)
    {
        mbedtls_md_free(&hash->u.hmac_ctx);
        return STATUS_INTERNAL_ERROR;
    }

    if ((ret = mbedtls_md_setup(&hash->u.hmac_ctx, md_info, 1)) != 0)
    {
        mbedtls_md_free(&hash->u.hmac_ctx);
        return STATUS_INTERNAL_ERROR;
    }

    mbedtls_md_hmac_starts(&hash->u.hmac_ctx, key, key_size);

    return STATUS_SUCCESS;
}

static NTSTATUS hash_update( struct hash *hash, UCHAR *input, ULONG size )
{
#ifndef __REACTOS__
    if (!libmbedtls_handle) return STATUS_INTERNAL_ERROR;
#endif
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        mbedtls_md5_update(&hash->u.md5_ctx, input, size);
        break;

    case ALG_ID_SHA1:
        mbedtls_sha1_update(&hash->u.sha1_ctx, input, size);
        break;

    case ALG_ID_SHA256:
        mbedtls_sha256_update(&hash->u.sha256_ctx, input, size);
        break;

    case ALG_ID_SHA384:
    case ALG_ID_SHA512:
        mbedtls_sha512_update(&hash->u.sha512_ctx, input, size);
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS hmac_update( struct hash *hash, UCHAR *input, ULONG size )
{
#ifndef __REACTOS__
    if (!libmbedtls_handle) return STATUS_INTERNAL_ERROR;
#endif
    mbedtls_md_update(&hash->u.hmac_ctx, input, size);

    return STATUS_SUCCESS;
}

static NTSTATUS hash_finish( struct hash *hash, UCHAR *output, ULONG size )
{
#ifndef __REACTOS__
    if (!libmbedtls_handle) return STATUS_INTERNAL_ERROR;
#endif
    switch (hash->alg_id)
    {
    case ALG_ID_MD5:
        mbedtls_md5_finish(&hash->u.md5_ctx, output);
        mbedtls_md5_free(&hash->u.md5_ctx);
        break;

    case ALG_ID_SHA1:
        mbedtls_sha1_finish(&hash->u.sha1_ctx, output);
        mbedtls_sha1_free(&hash->u.sha1_ctx);
        break;

    case ALG_ID_SHA256:
        mbedtls_sha256_finish(&hash->u.sha256_ctx, output);
        mbedtls_sha256_free(&hash->u.sha256_ctx);
        break;

    case ALG_ID_SHA384:
    case ALG_ID_SHA512:
        mbedtls_sha512_finish(&hash->u.sha512_ctx, output);
        mbedtls_sha512_free(&hash->u.sha512_ctx);
        break;

    default:
        ERR( "unhandled id %u\n", hash->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS hmac_finish( struct hash *hash, UCHAR *output, ULONG size )
{
#ifndef __REACTOS__
    if (!libmbedtls_handle) return STATUS_INTERNAL_ERROR;
#endif
    mbedtls_md_hmac_finish(&hash->u.hmac_ctx, output);
    mbedtls_md_free(&hash->u.hmac_ctx);

    return STATUS_SUCCESS;
}
#endif

#ifdef SONAME_LIBMBEDTLS
/*
 * Asymmetric (ECDSA / RSA) public-key support, backed by mbedTLS.
 *
 * This wires up the CNG asymmetric-verify primitives used by crypt32's
 * CryptVerifyCertificateSignatureEx (and hence HTTPS certificate-chain
 * validation) to the mbedTLS library already vendored and linked into
 * bcrypt.  Only the public-key import + signature-verify path is
 * implemented, which is all cert-signature validation needs.
 */

#define MAGIC_KEY  (('K' << 24) | ('E' << 16) | ('Y' << 8) | '0')

enum key_type
{
    KEY_TYPE_ECDSA,
    KEY_TYPE_RSA,
};

struct key
{
    struct object hdr;
    enum key_type type;
    union
    {
        mbedtls_ecp_keypair ecc;   /* grp + public point Q */
        mbedtls_rsa_context rsa;
    } u;
};

/* Map a BCRYPT alg pseudo-handle or an opened struct algorithm* to an
 * mbedTLS ECP group id.  Returns FALSE if the handle is not a recognised
 * ECDSA algorithm. */
static BOOL alg_handle_to_ecp_group( BCRYPT_ALG_HANDLE handle, mbedtls_ecp_group_id *grp_id )
{
    if (handle == BCRYPT_ECDSA_P256_ALG_HANDLE)
    {
        *grp_id = MBEDTLS_ECP_DP_SECP256R1;
        return TRUE;
    }
    if (handle == BCRYPT_ECDSA_P384_ALG_HANDLE)
    {
        *grp_id = MBEDTLS_ECP_DP_SECP384R1;
        return TRUE;
    }

    /* A real opened provider handle. */
    if (handle && !((ULONG_PTR)handle & ~(ULONG_PTR)0xffff))
        return FALSE;

    {
        const struct algorithm *alg = handle;
        if (!alg || alg->hdr.magic != MAGIC_ALG) return FALSE;
        if (alg->id == ALG_ID_ECDSA_P256) { *grp_id = MBEDTLS_ECP_DP_SECP256R1; return TRUE; }
        if (alg->id == ALG_ID_ECDSA_P384) { *grp_id = MBEDTLS_ECP_DP_SECP384R1; return TRUE; }
    }
    return FALSE;
}

static NTSTATUS key_import_ecc( BCRYPT_ALG_HANDLE alg_handle, struct key *key,
                                UCHAR *input, ULONG input_len )
{
    BCRYPT_ECCKEY_BLOB *blob = (BCRYPT_ECCKEY_BLOB *)input;
    mbedtls_ecp_group_id grp_id;
    ULONG magic_p256_ok, magic_p384_ok;
    ULONG cbKey;
    int ret;

    if (input_len < sizeof(*blob)) return STATUS_INVALID_PARAMETER;

    if (!alg_handle_to_ecp_group( alg_handle, &grp_id ))
    {
        WARN( "unrecognised ECDSA algorithm handle %p\n", alg_handle );
        return STATUS_NOT_SUPPORTED;
    }

    /* Validate the blob magic matches the requested curve. */
    magic_p256_ok = (blob->dwMagic == BCRYPT_ECDSA_PUBLIC_P256_MAGIC &&
                     grp_id == MBEDTLS_ECP_DP_SECP256R1);
    magic_p384_ok = (blob->dwMagic == BCRYPT_ECDSA_PUBLIC_P384_MAGIC &&
                     grp_id == MBEDTLS_ECP_DP_SECP384R1);
    if (!magic_p256_ok && !magic_p384_ok)
    {
        WARN( "ECC blob magic %08x does not match curve\n", blob->dwMagic );
        return STATUS_INVALID_PARAMETER;
    }

    cbKey = blob->cbKey;
    if (!cbKey || input_len < sizeof(*blob) + cbKey * 2)
        return STATUS_INVALID_PARAMETER;

    mbedtls_ecp_keypair_init( &key->u.ecc );

    if ((ret = mbedtls_ecp_group_load( &key->u.ecc.grp, grp_id )) != 0)
        goto error;

    /* X || Y, each cbKey bytes big-endian, follow the blob header. */
    if ((ret = mbedtls_mpi_read_binary( &key->u.ecc.Q.X, input + sizeof(*blob), cbKey )) != 0)
        goto error;
    if ((ret = mbedtls_mpi_read_binary( &key->u.ecc.Q.Y, input + sizeof(*blob) + cbKey, cbKey )) != 0)
        goto error;
    if ((ret = mbedtls_mpi_lset( &key->u.ecc.Q.Z, 1 )) != 0)
        goto error;

    if ((ret = mbedtls_ecp_check_pubkey( &key->u.ecc.grp, &key->u.ecc.Q )) != 0)
        goto error;

    key->type = KEY_TYPE_ECDSA;
    return STATUS_SUCCESS;

error:
    WARN( "mbedtls ECC import failed: -%#x\n", -ret );
    mbedtls_ecp_keypair_free( &key->u.ecc );
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS key_import_rsa( struct key *key, UCHAR *input, ULONG input_len )
{
    BCRYPT_RSAKEY_BLOB *blob = (BCRYPT_RSAKEY_BLOB *)input;
    const UCHAR *exp, *modulus;
    int ret;

    if (input_len < sizeof(*blob)) return STATUS_INVALID_PARAMETER;
    if (blob->Magic != BCRYPT_RSAPUBLIC_MAGIC) return STATUS_INVALID_PARAMETER;
    if (input_len < sizeof(*blob) + blob->cbPublicExp + blob->cbModulus)
        return STATUS_INVALID_PARAMETER;

    /* BCRYPT_RSAKEY_BLOB stores PublicExponent then Modulus, both big-endian. */
    exp = input + sizeof(*blob);
    modulus = exp + blob->cbPublicExp;

    mbedtls_rsa_init( &key->u.rsa, MBEDTLS_RSA_PKCS_V15, 0 );

    if ((ret = mbedtls_rsa_import_raw( &key->u.rsa,
                                       modulus, blob->cbModulus,
                                       NULL, 0, NULL, 0, NULL, 0,
                                       exp, blob->cbPublicExp )) != 0)
        goto error;

    if ((ret = mbedtls_rsa_complete( &key->u.rsa )) != 0)
        goto error;

    key->type = KEY_TYPE_RSA;
    return STATUS_SUCCESS;

error:
    WARN( "mbedtls RSA import failed: -%#x\n", -ret );
    mbedtls_rsa_free( &key->u.rsa );
    return STATUS_INVALID_PARAMETER;
}

/* Map a BCRYPT hash algorithm id string to an mbedTLS md type. */
static mbedtls_md_type_t md_type_from_algid( const WCHAR *alg_id )
{
    if (!alg_id) return MBEDTLS_MD_NONE;
    if (!strcmpW( alg_id, BCRYPT_SHA1_ALGORITHM ))   return MBEDTLS_MD_SHA1;
    if (!strcmpW( alg_id, BCRYPT_SHA256_ALGORITHM )) return MBEDTLS_MD_SHA256;
    if (!strcmpW( alg_id, BCRYPT_SHA384_ALGORITHM )) return MBEDTLS_MD_SHA384;
    if (!strcmpW( alg_id, BCRYPT_SHA512_ALGORITHM )) return MBEDTLS_MD_SHA512;
    if (!strcmpW( alg_id, BCRYPT_MD5_ALGORITHM ))    return MBEDTLS_MD_MD5;
    return MBEDTLS_MD_NONE;
}
#endif /* SONAME_LIBMBEDTLS */

#define OBJECT_LENGTH_MD5       274
#define OBJECT_LENGTH_SHA1      278
#define OBJECT_LENGTH_SHA256    286
#define OBJECT_LENGTH_SHA384    382
#define OBJECT_LENGTH_SHA512    382

static NTSTATUS generic_alg_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!strcmpW( prop, BCRYPT_HASH_LENGTH ))
    {
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        if(buf)
            *(ULONG*)buf = alg_props[id].hash_length;
        return STATUS_SUCCESS;
    }

    if (!strcmpW( prop, BCRYPT_ALGORITHM_NAME ))
    {
        *ret_size = (strlenW(alg_props[id].alg_name)+1)*sizeof(WCHAR);
        if (size < *ret_size)
            return STATUS_BUFFER_TOO_SMALL;
        if(buf)
            memcpy(buf, alg_props[id].alg_name, *ret_size);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_alg_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    NTSTATUS status;
    ULONG value;

    status = generic_alg_property( id, prop, buf, size, ret_size );
    if (status != STATUS_NOT_IMPLEMENTED)
        return status;

    switch (id)
    {
    case ALG_ID_MD5:
        if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH ))
        {
            value = OBJECT_LENGTH_MD5;
            break;
        }
        FIXME( "unsupported md5 algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_RNG:
        if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH )) return STATUS_NOT_SUPPORTED;
        FIXME( "unsupported rng algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_SHA1:
        if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH ))
        {
            value = OBJECT_LENGTH_SHA1;
            break;
        }
        FIXME( "unsupported sha1 algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_SHA256:
        if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH ))
        {
            value = OBJECT_LENGTH_SHA256;
            break;
        }
        FIXME( "unsupported sha256 algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_SHA384:
        if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH ))
        {
            value = OBJECT_LENGTH_SHA384;
            break;
        }
        FIXME( "unsupported sha384 algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_SHA512:
        if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH ))
        {
            value = OBJECT_LENGTH_SHA512;
            break;
        }
        FIXME( "unsupported sha512 algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    default:
        FIXME( "unsupported algorithm %u\n", id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (size < sizeof(ULONG))
    {
        *ret_size = sizeof(ULONG);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (buf) *(ULONG *)buf = value;
    *ret_size = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS get_hash_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    NTSTATUS status;

    status = generic_alg_property( id, prop, buf, size, ret_size );
    if (status == STATUS_NOT_IMPLEMENTED)
        FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return status;
}

NTSTATUS WINAPI BCryptGetProperty( BCRYPT_HANDLE handle, LPCWSTR prop, UCHAR *buffer, ULONG count, ULONG *res, ULONG flags )
{
    struct object *object = handle;

    TRACE( "%p, %s, %p, %u, %p, %08x\n", handle, wine_dbgstr_w(prop), buffer, count, res, flags );

    if (!object) return STATUS_INVALID_HANDLE;
    if (!prop || !res) return STATUS_INVALID_PARAMETER;

    switch (object->magic)
    {
    case MAGIC_ALG:
    {
        const struct algorithm *alg = (const struct algorithm *)object;
        return get_alg_property( alg->id, prop, buffer, count, res );
    }
    case MAGIC_HASH:
    {
        const struct hash *hash = (const struct hash *)object;
        return get_hash_property( hash->alg_id, prop, buffer, count, res );
    }
    default:
        WARN( "unknown magic %08x\n", object->magic );
        return STATUS_INVALID_HANDLE;
    }
}

NTSTATUS WINAPI BCryptCreateHash( BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE *handle, UCHAR *object, ULONG objectlen,
                                  UCHAR *secret, ULONG secretlen, ULONG flags )
{
    struct algorithm *alg = algorithm;
    struct hash *hash;
    NTSTATUS status;

    TRACE( "%p, %p, %p, %u, %p, %u, %08x - stub\n", algorithm, handle, object, objectlen,
           secret, secretlen, flags );
    if (flags)
    {
        FIXME( "unimplemented flags %08x\n", flags );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (object) FIXME( "ignoring object buffer\n" );

    if (!(hash = HeapAlloc( GetProcessHeap(), 0, sizeof(*hash) ))) return STATUS_NO_MEMORY;
    hash->hdr.magic = MAGIC_HASH;
    hash->alg_id    = alg->id;
    hash->hmac      = alg->hmac;

    if (hash->hmac)
    {
        status = hmac_init( hash, secret, secretlen );
    }
    else
    {
        status = hash_init( hash );
    }

    if (status != STATUS_SUCCESS)
    {
        HeapFree( GetProcessHeap(), 0, hash );
        return status;
    }

    *handle = hash;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDestroyHash( BCRYPT_HASH_HANDLE handle )
{
    struct hash *hash = handle;

    TRACE( "%p\n", handle );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    HeapFree( GetProcessHeap(), 0, hash );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptHashData( BCRYPT_HASH_HANDLE handle, UCHAR *input, ULONG size, ULONG flags )
{
    struct hash *hash = handle;

    TRACE( "%p, %p, %u, %08x\n", handle, input, size, flags );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!input) return STATUS_SUCCESS;

    if (hash->hmac)
    {
        return hmac_update( hash, input, size );
    }
    else
    {
        return hash_update( hash, input, size );
    }
}

NTSTATUS WINAPI BCryptFinishHash( BCRYPT_HASH_HANDLE handle, UCHAR *output, ULONG size, ULONG flags )
{
    struct hash *hash = handle;

    TRACE( "%p, %p, %u, %08x\n", handle, output, size, flags );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!output) return STATUS_INVALID_PARAMETER;

    if (hash->hmac)
    {
        return hmac_finish( hash, output, size );
    }
    else
    {
        return hash_finish( hash, output, size );
    }
}

NTSTATUS WINAPI BCryptHash( BCRYPT_ALG_HANDLE algorithm, UCHAR *secret, ULONG secretlen,
                            UCHAR *input, ULONG inputlen, UCHAR *output, ULONG outputlen )
{
    NTSTATUS status;
    BCRYPT_HASH_HANDLE handle;

    TRACE( "%p, %p, %u, %p, %u, %p, %u\n", algorithm, secret, secretlen,
           input, inputlen, output, outputlen );

    status = BCryptCreateHash( algorithm, &handle, NULL, 0, secret, secretlen, 0);
    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    status = BCryptHashData( handle, input, inputlen, 0 );
    if (status != STATUS_SUCCESS)
    {
        BCryptDestroyHash( handle );
        return status;
    }

    status = BCryptFinishHash( handle, output, outputlen, 0 );
    if (status != STATUS_SUCCESS)
    {
        BCryptDestroyHash( handle );
        return status;
    }

    return BCryptDestroyHash( handle );
}

NTSTATUS WINAPI BCryptImportKeyPair( BCRYPT_ALG_HANDLE algorithm, BCRYPT_KEY_HANDLE decrypt_key, LPCWSTR type,
                                     BCRYPT_KEY_HANDLE *ret_key, UCHAR *input, ULONG input_len, ULONG flags )
{
#ifdef SONAME_LIBMBEDTLS
    struct key *key;
    NTSTATUS status;

    TRACE( "%p, %p, %s, %p, %p, %u, %08x\n", algorithm, decrypt_key, wine_dbgstr_w(type),
           ret_key, input, input_len, flags );

    if (!ret_key || !type || !input) return STATUS_INVALID_PARAMETER;
    if (decrypt_key)
    {
        FIXME( "decryption of key blob not supported\n" );
        return STATUS_NOT_SUPPORTED;
    }

    if (!(key = HeapAlloc( GetProcessHeap(), 0, sizeof(*key) ))) return STATUS_NO_MEMORY;
    key->hdr.magic = MAGIC_KEY;

    if (!strcmpW( type, BCRYPT_ECCPUBLIC_BLOB ))
        status = key_import_ecc( algorithm, key, input, input_len );
    else if (!strcmpW( type, BCRYPT_RSAPUBLIC_BLOB ))
        status = key_import_rsa( key, input, input_len );
    else
    {
        FIXME( "unsupported key blob type %s\n", debugstr_w(type) );
        status = STATUS_NOT_SUPPORTED;
    }

    if (status != STATUS_SUCCESS)
    {
        HeapFree( GetProcessHeap(), 0, key );
        return status;
    }

    *ret_key = key;
    return STATUS_SUCCESS;
#else
    FIXME( "%p, %p, %s, %p, %p, %u, %08x - no crypto backend\n", algorithm, decrypt_key,
           wine_dbgstr_w(type), ret_key, input, input_len, flags );
    return STATUS_NOT_IMPLEMENTED;
#endif
}

NTSTATUS WINAPI BCryptVerifySignature( BCRYPT_KEY_HANDLE handle, void *padding, UCHAR *hash, ULONG hash_len,
                                       UCHAR *signature, ULONG sig_len, ULONG flags )
{
#ifdef SONAME_LIBMBEDTLS
    struct key *key = handle;
    int ret;

    TRACE( "%p, %p, %p, %u, %p, %u, %08x\n", handle, padding, hash, hash_len,
           signature, sig_len, flags );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (!hash || !signature) return STATUS_INVALID_PARAMETER;

    if (key->type == KEY_TYPE_ECDSA)
    {
        mbedtls_mpi r, s;
        ULONG half = sig_len / 2;
        NTSTATUS status = STATUS_INVALID_SIGNATURE;

        /* CNG passes a raw fixed-width r || s pair (not DER). */
        if (!sig_len || (sig_len & 1))
            return STATUS_INVALID_SIGNATURE;

        mbedtls_mpi_init( &r );
        mbedtls_mpi_init( &s );

        if (mbedtls_mpi_read_binary( &r, signature, half ) == 0 &&
            mbedtls_mpi_read_binary( &s, signature + half, half ) == 0)
        {
            ret = mbedtls_ecdsa_verify( &key->u.ecc.grp, hash, hash_len, &key->u.ecc.Q, &r, &s );
            status = ret ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
        }

        mbedtls_mpi_free( &r );
        mbedtls_mpi_free( &s );
        return status;
    }

    if (key->type == KEY_TYPE_RSA)
    {
        BCRYPT_PKCS1_PADDING_INFO *pad = padding;
        mbedtls_md_type_t md_type;

        if (flags & BCRYPT_PAD_PSS)
        {
            BCRYPT_PSS_PADDING_INFO *pss = padding;
            if (!pss) return STATUS_INVALID_PARAMETER;
            md_type = md_type_from_algid( pss->pszAlgId );
            if (md_type == MBEDTLS_MD_NONE) return STATUS_NOT_SUPPORTED;
            mbedtls_rsa_set_padding( &key->u.rsa, MBEDTLS_RSA_PKCS_V21, md_type );
            ret = mbedtls_rsa_rsassa_pss_verify( &key->u.rsa, NULL, NULL, MBEDTLS_RSA_PUBLIC,
                                                 md_type, hash_len, hash, signature );
            return ret ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
        }

        if (flags & BCRYPT_PAD_PKCS1)
        {
            md_type = pad ? md_type_from_algid( pad->pszAlgId ) : MBEDTLS_MD_NONE;
            /* For PKCS#1 v1.5 an unspecified/NONE md still verifies against the
             * raw DigestInfo; mbedTLS supports MBEDTLS_MD_NONE in that case. */
            mbedtls_rsa_set_padding( &key->u.rsa, MBEDTLS_RSA_PKCS_V15, md_type );
            ret = mbedtls_rsa_pkcs1_verify( &key->u.rsa, NULL, NULL, MBEDTLS_RSA_PUBLIC,
                                            md_type, hash_len, hash, signature );
            return ret ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
        }

        FIXME( "unsupported RSA padding flags %08x\n", flags );
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_INVALID_HANDLE;
#else
    FIXME( "%p, %p, %p, %u, %p, %u, %08x - no crypto backend\n", handle, padding, hash,
           hash_len, signature, sig_len, flags );
    return STATUS_NOT_IMPLEMENTED;
#endif
}

NTSTATUS WINAPI BCryptDestroyKey( BCRYPT_KEY_HANDLE handle )
{
#ifdef SONAME_LIBMBEDTLS
    struct key *key = handle;

    TRACE( "%p\n", handle );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;

    if (key->type == KEY_TYPE_ECDSA)
        mbedtls_ecp_keypair_free( &key->u.ecc );
    else if (key->type == KEY_TYPE_RSA)
        mbedtls_rsa_free( &key->u.rsa );

    HeapFree( GetProcessHeap(), 0, key );
    return STATUS_SUCCESS;
#else
    FIXME( "%p - no crypto backend\n", handle );
    return STATUS_NOT_IMPLEMENTED;
#endif
}

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        instance = hinst;
        DisableThreadLibraryCalls( hinst );
#if defined(HAVE_GNUTLS_HASH) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H)
        gnutls_initialize();
#elif defined(SONAME_LIBMBEDTLS) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H) && !defined(__REACTOS__)
        mbedtls_initialize();
#endif
        break;

    case DLL_PROCESS_DETACH:
        if (reserved) break;
#if defined(HAVE_GNUTLS_HASH) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H)
        gnutls_uninitialize();
#elif defined(SONAME_LIBMBEDTLS) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H) && !defined(__REACTOS__)
        mbedtls_uninitialize();
#endif
        break;
    }
    return TRUE;
}
