/*
 * Unit test for bcrypt functions
 *
 * Copyright 2014 Bruno Jesus
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
 */

#include <stdio.h>
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <bcrypt.h>

#include "wine/test.h"

static NTSTATUS (WINAPI *pBCryptOpenAlgorithmProvider)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
static NTSTATUS (WINAPI *pBCryptCloseAlgorithmProvider)(BCRYPT_ALG_HANDLE, ULONG);
static NTSTATUS (WINAPI *pBCryptGetFipsAlgorithmMode)(BOOLEAN *);
static NTSTATUS (WINAPI *pBCryptCreateHash)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptHash)(BCRYPT_ALG_HANDLE, UCHAR *, ULONG, UCHAR *, ULONG, UCHAR *, ULONG);
static NTSTATUS (WINAPI *pBCryptHashData)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptFinishHash)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptDestroyHash)(BCRYPT_HASH_HANDLE);
static NTSTATUS (WINAPI *pBCryptGenRandom)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptGetProperty)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);
static NTSTATUS (WINAPI *pBCryptImportKeyPair)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptVerifySignature)(BCRYPT_KEY_HANDLE, void *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptDestroyKey)(BCRYPT_KEY_HANDLE);

/* Known-answer ECDSA-P256 / SHA-256 vector (message "ReactOS bcrypt KAT",
 * generated with OpenSSL prime256v1).  This exercises the mbedTLS-backed
 * asymmetric verify path used for HTTPS certificate-signature validation. */
static const BYTE ecdsa_p256_hash[32] =
{
    0xda,0x22,0x9c,0x87,0x15,0xee,0x2c,0x83,0x04,0xf2,0x98,0xfa,0xe5,0x55,0x43,0x2a,
    0x84,0x1a,0x5c,0x34,0xcc,0xda,0x9c,0xd1,0x92,0x6e,0x27,0xbf,0xc4,0x50,0xb3,0xa0
};
static const BYTE ecdsa_p256_x[32] =
{
    0x50,0xa4,0x19,0xd2,0x0a,0x52,0x18,0x52,0x49,0x28,0x4e,0x34,0x1e,0x7b,0xcd,0xda,
    0x9f,0xd5,0xb6,0x07,0x7a,0xd3,0xe3,0x22,0x4d,0x26,0xb3,0x44,0x71,0x5e,0x6c,0x7c
};
static const BYTE ecdsa_p256_y[32] =
{
    0x9e,0xd2,0x52,0xd1,0x80,0x86,0xaa,0x67,0xd0,0x4a,0x40,0xb1,0x92,0x1c,0x61,0x9a,
    0x67,0x32,0x7f,0x44,0xa6,0xa9,0x8e,0x93,0xd3,0xc2,0x2c,0xa0,0xd1,0x68,0xa4,0x1e
};
static const BYTE ecdsa_p256_sig[64] =  /* raw r || s */
{
    0x0a,0x3a,0x79,0xf0,0x68,0xe5,0x8f,0x96,0xd8,0x60,0x6a,0xa2,0xa1,0x33,0x0c,0xba,
    0x43,0x64,0x26,0x20,0x36,0x70,0xd2,0xb1,0x1c,0xb1,0x57,0xd0,0xf3,0x34,0x52,0xbb,
    0x2d,0x4c,0x9e,0x89,0x0b,0xef,0x81,0xfc,0xa4,0x53,0xb5,0x60,0x3e,0xdb,0x3c,0x6e,
    0x71,0xf1,0xef,0x13,0x64,0xed,0x67,0x31,0xd1,0x85,0xb7,0x64,0xe2,0x06,0x53,0xea
};

/* Known-answer RSA-2048 / SHA-256 PKCS#1 v1.5 vector (same message). */
static const BYTE rsa_2048_exp[3] = { 0x01,0x00,0x01 };
static const BYTE rsa_2048_mod[256] =
{
    0x8f,0x2d,0x6e,0xfe,0x75,0x97,0xf9,0x6c,0x37,0xcb,0xf1,0xbd,0x9d,0x67,0x7d,0x5c,
    0x7c,0x91,0x7d,0x81,0xe5,0xb7,0x33,0x1b,0xdb,0xdc,0xc4,0x26,0x32,0xd7,0x0f,0xfd,
    0xe1,0x91,0x6e,0x6b,0xe8,0xe9,0x2e,0x54,0x61,0x58,0x34,0xff,0x9f,0xf2,0x86,0xcb,
    0x56,0x48,0x3f,0x7b,0x39,0xab,0x67,0x01,0x7b,0x71,0x34,0x41,0xe5,0xdb,0x90,0x63,
    0xad,0xbf,0xb4,0x25,0x3a,0x3d,0xa1,0xfe,0x98,0x77,0x1b,0x29,0xda,0xd0,0x46,0x5b,
    0x92,0x7a,0x1a,0x9e,0x15,0x55,0x51,0x66,0x22,0x1c,0x0e,0x99,0x7c,0xd5,0xb5,0xf8,
    0x3b,0xbe,0x57,0x58,0xa4,0x82,0x43,0x19,0xa1,0x54,0x66,0x2a,0x5b,0x0b,0x07,0x08,
    0xe8,0xa6,0xba,0x9a,0x08,0xb2,0xd3,0xaa,0x5d,0x12,0x63,0xf6,0x9b,0x70,0x83,0x6f,
    0xde,0x88,0xed,0x9d,0x2c,0x7a,0x28,0xad,0xd2,0x51,0x14,0xe9,0xb0,0x03,0x4f,0x6a,
    0x0c,0xb9,0xba,0x25,0x40,0xc1,0x56,0xcf,0xec,0xa7,0x18,0x7f,0x6a,0x2a,0xb5,0x71,
    0x37,0x93,0x2a,0x62,0xe3,0x93,0xc7,0x6d,0xbf,0x8b,0x82,0xa2,0x41,0x24,0x26,0xd8,
    0x2f,0x60,0x59,0x3e,0x82,0xfb,0x3e,0x3d,0xb3,0x41,0x0a,0x60,0x15,0x99,0x83,0x83,
    0xd3,0x5a,0xad,0x08,0x50,0x76,0xb5,0x05,0x28,0x24,0xc6,0x9b,0xda,0x3b,0x82,0x47,
    0x29,0x40,0x4d,0x67,0x4a,0xf5,0x77,0x6e,0xc1,0x2d,0x6d,0xc0,0x9f,0x01,0x33,0xc9,
    0x19,0x30,0xcf,0x76,0x09,0x32,0x34,0x48,0x8e,0xf6,0xaf,0x9e,0x2a,0xcb,0x31,0x31,
    0x1f,0x4d,0xca,0x37,0x11,0xe4,0x6f,0x7b,0x16,0xf9,0xb1,0x02,0xf9,0x3f,0xba,0xbd
};
static const BYTE rsa_2048_sig[256] =
{
    0x62,0x66,0x5f,0x5f,0xd0,0x6d,0x2d,0x75,0x86,0xb4,0x72,0xd8,0x16,0x55,0xd6,0x19,
    0xec,0x47,0x0c,0x28,0xdd,0x26,0x0d,0xa5,0x3f,0x8b,0x06,0x3c,0xc8,0xb3,0x00,0x19,
    0xab,0x2f,0xcf,0xe1,0x92,0x60,0x27,0x68,0x30,0x38,0x19,0x8a,0x02,0xdf,0x97,0x46,
    0x80,0xd0,0x55,0x94,0x28,0xc5,0x50,0x5b,0x51,0xc6,0x57,0x24,0xfb,0xca,0x91,0x98,
    0x11,0x93,0x6a,0xc0,0xfb,0xdb,0xc1,0xcd,0x01,0xcf,0xdb,0x13,0xde,0x8e,0x0b,0xbf,
    0x5b,0x55,0xbe,0xe1,0x7f,0x76,0x1d,0x66,0x9d,0xb7,0xee,0x19,0xa4,0x40,0x8f,0xf1,
    0x0f,0x9f,0x5f,0x41,0xc4,0xb1,0x1b,0x11,0x34,0x2f,0x72,0xc3,0xe6,0x33,0x22,0x60,
    0x25,0xa8,0x85,0x0d,0x04,0xce,0x43,0x01,0x1d,0xd3,0x4d,0xc6,0xf7,0x7a,0x27,0xb0,
    0x6e,0xee,0xf3,0xf7,0xdd,0xfc,0xea,0xaa,0x5a,0x1e,0x76,0xc1,0x47,0x8c,0x25,0x37,
    0x8c,0xb5,0xed,0xc7,0x89,0x16,0x1d,0x75,0x76,0xb5,0xbf,0x6a,0xf6,0x7a,0xcb,0xb8,
    0x9f,0x7d,0x16,0x69,0x73,0x98,0x52,0x5c,0xd2,0x5b,0xaf,0x0b,0x07,0x75,0x24,0x82,
    0x11,0x2e,0xfd,0x65,0x86,0xa5,0xf4,0x65,0xc6,0x1b,0xa3,0x20,0xba,0x23,0xa5,0x2b,
    0x35,0x68,0xb7,0x3b,0x39,0x73,0xf1,0xbd,0xaa,0x61,0xf9,0x42,0x62,0x27,0xda,0xf3,
    0xe4,0x55,0xb6,0x07,0x3d,0x13,0x10,0x5d,0x4f,0xdf,0xb2,0x47,0xaf,0xfc,0x42,0xb1,
    0x05,0xbe,0x9d,0x10,0xc7,0x15,0x35,0x05,0x15,0xcf,0x67,0x25,0x82,0xac,0x13,0xdf,
    0x09,0x57,0x60,0x63,0xaf,0xe2,0x44,0x87,0x63,0xd1,0xe1,0x22,0xbf,0x4f,0xc6,0xe3
};

static void test_ecdsa_verify(void)
{
    BCRYPT_ECCKEY_BLOB *blob;
    BCRYPT_KEY_HANDLE key = NULL;
    BYTE buf[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BYTE bad_sig[64];
    NTSTATUS status;

    if (!pBCryptImportKeyPair || !pBCryptVerifySignature || !pBCryptDestroyKey)
    {
        win_skip("asymmetric BCrypt functions not available\n");
        return;
    }

    blob = (BCRYPT_ECCKEY_BLOB *)buf;
    blob->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob->cbKey = 32;
    memcpy(buf + sizeof(*blob), ecdsa_p256_x, 32);
    memcpy(buf + sizeof(*blob) + 32, ecdsa_p256_y, 32);

    status = pBCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, NULL, BCRYPT_ECCPUBLIC_BLOB,
                                  &key, buf, sizeof(*blob) + 64, 0);
    ok(status == STATUS_SUCCESS, "ECDSA import failed: %08x\n", status);
    if (status) return;

    status = pBCryptVerifySignature(key, NULL, (PUCHAR)ecdsa_p256_hash, sizeof(ecdsa_p256_hash),
                                    (PUCHAR)ecdsa_p256_sig, sizeof(ecdsa_p256_sig), 0);
    ok(status == STATUS_SUCCESS, "ECDSA verify (valid) failed: %08x\n", status);

    /* A tampered signature must fail. */
    memcpy(bad_sig, ecdsa_p256_sig, sizeof(bad_sig));
    bad_sig[0] ^= 0xff;
    status = pBCryptVerifySignature(key, NULL, (PUCHAR)ecdsa_p256_hash, sizeof(ecdsa_p256_hash),
                                    bad_sig, sizeof(bad_sig), 0);
    ok(status == STATUS_INVALID_SIGNATURE, "ECDSA verify (bad sig) returned: %08x\n", status);

    pBCryptDestroyKey(key);
}

static void test_rsa_verify(void)
{
    static const WCHAR sha256[] = {'S','H','A','2','5','6',0};
    BCRYPT_PKCS1_PADDING_INFO pad;
    BCRYPT_RSAKEY_BLOB *blob;
    BCRYPT_KEY_HANDLE key = NULL;
    BYTE buf[sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(rsa_2048_exp) + sizeof(rsa_2048_mod)];
    BYTE bad_sig[256];
    NTSTATUS status;

    if (!pBCryptImportKeyPair || !pBCryptVerifySignature || !pBCryptDestroyKey)
    {
        win_skip("asymmetric BCrypt functions not available\n");
        return;
    }

    blob = (BCRYPT_RSAKEY_BLOB *)buf;
    blob->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    blob->BitLength = 2048;
    blob->cbPublicExp = sizeof(rsa_2048_exp);
    blob->cbModulus = sizeof(rsa_2048_mod);
    blob->cbPrime1 = 0;
    blob->cbPrime2 = 0;
    memcpy(buf + sizeof(*blob), rsa_2048_exp, sizeof(rsa_2048_exp));
    memcpy(buf + sizeof(*blob) + sizeof(rsa_2048_exp), rsa_2048_mod, sizeof(rsa_2048_mod));

    status = pBCryptImportKeyPair(BCRYPT_RSA_ALG_HANDLE, NULL, BCRYPT_RSAPUBLIC_BLOB,
                                  &key, buf, sizeof(buf), 0);
    ok(status == STATUS_SUCCESS, "RSA import failed: %08x\n", status);
    if (status) return;

    pad.pszAlgId = sha256;
    status = pBCryptVerifySignature(key, &pad, (PUCHAR)ecdsa_p256_hash, sizeof(ecdsa_p256_hash),
                                    (PUCHAR)rsa_2048_sig, sizeof(rsa_2048_sig), BCRYPT_PAD_PKCS1);
    ok(status == STATUS_SUCCESS, "RSA verify (valid) failed: %08x\n", status);

    memcpy(bad_sig, rsa_2048_sig, sizeof(bad_sig));
    bad_sig[0] ^= 0xff;
    status = pBCryptVerifySignature(key, &pad, (PUCHAR)ecdsa_p256_hash, sizeof(ecdsa_p256_hash),
                                    bad_sig, sizeof(bad_sig), BCRYPT_PAD_PKCS1);
    ok(status == STATUS_INVALID_SIGNATURE, "RSA verify (bad sig) returned: %08x\n", status);

    pBCryptDestroyKey(key);
}

static void test_BCryptGenRandom(void)
{
    NTSTATUS ret;
    UCHAR buffer[256];

    ret = pBCryptGenRandom(NULL, NULL, 0, 0);
    ok(ret == STATUS_INVALID_HANDLE, "Expected STATUS_INVALID_HANDLE, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, 0, 0);
    ok(ret == STATUS_INVALID_HANDLE, "Expected STATUS_INVALID_HANDLE, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, sizeof(buffer), 0);
    ok(ret == STATUS_INVALID_HANDLE, "Expected STATUS_INVALID_HANDLE, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, sizeof(buffer),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG|BCRYPT_RNG_USE_ENTROPY_IN_BUFFER);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, NULL, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_INVALID_PARAMETER, "Expected STATUS_INVALID_PARAMETER, got 0x%x\n", ret);

    /* Zero sized buffer should work too */
    ret = pBCryptGenRandom(NULL, buffer, 0, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);

    /* Test random number generation - It's impossible for a sane RNG to return 8 zeros */
    memset(buffer, 0, 16);
    ret = pBCryptGenRandom(NULL, buffer, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);
    ok(memcmp(buffer, buffer + 8, 8), "Expected a random number, got 0\n");
}

static void test_BCryptGetFipsAlgorithmMode(void)
{
    static const WCHAR policyKeyVistaW[] = {
        'S','y','s','t','e','m','\\',
        'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
        'C','o','n','t','r','o','l','\\',
        'L','s','a','\\',
        'F','I','P','S','A','l','g','o','r','i','t','h','m','P','o','l','i','c','y',0};
    static const WCHAR policyValueVistaW[] = {'E','n','a','b','l','e','d',0};
    static const WCHAR policyKeyXPW[] = {
        'S','y','s','t','e','m','\\',
        'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
        'C','o','n','t','r','o','l','\\',
        'L','s','a',0};
    static const WCHAR policyValueXPW[] = {
        'F','I','P','S','A','l','g','o','r','i','t','h','m','P','o','l','i','c','y',0};
    HKEY hkey = NULL;
    BOOLEAN expected;
    BOOLEAN enabled;
    DWORD value, count[2] = {sizeof(value), sizeof(value)};
    NTSTATUS ret;

    if (RegOpenKeyW(HKEY_LOCAL_MACHINE, policyKeyVistaW, &hkey) == ERROR_SUCCESS &&
        RegQueryValueExW(hkey, policyValueVistaW, NULL, NULL, (void *)&value, &count[0]) == ERROR_SUCCESS)
    {
        expected = !!value;
    }
      else if (RegOpenKeyW(HKEY_LOCAL_MACHINE, policyKeyXPW, &hkey) == ERROR_SUCCESS &&
               RegQueryValueExW(hkey, policyValueXPW, NULL, NULL, (void *)&value, &count[0]) == ERROR_SUCCESS)
    {
        expected = !!value;
    }
    else
    {
        expected = FALSE;
todo_wine
        ok(0, "Neither XP or Vista key is present\n");
    }
    RegCloseKey(hkey);

    ret = pBCryptGetFipsAlgorithmMode(&enabled);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);
    ok(enabled == expected, "expected result %d, got %d\n", expected, enabled);

    ret = pBCryptGetFipsAlgorithmMode(NULL);
    ok(ret == STATUS_INVALID_PARAMETER, "Expected STATUS_INVALID_PARAMETER, got 0x%x\n", ret);
}

static void format_hash(const UCHAR *bytes, ULONG size, char *buf)
{
    ULONG i;
    buf[0] = '\0';
    for (i = 0; i < size; i++)
    {
        sprintf(buf + i * 2, "%02x", bytes[i]);
    }
    return;
}

static int strcmp_wa(const WCHAR *strw, const char *stra)
{
    WCHAR buf[512];
    MultiByteToWideChar(CP_ACP, 0, stra, -1, buf, sizeof(buf)/sizeof(buf[0]));
    return lstrcmpW(strw, buf);
}

#define test_hash_length(a,b) _test_hash_length(__LINE__,a,b)
static void _test_hash_length(unsigned line, void *handle, ULONG exlen)
{
    ULONG len = 0xdeadbeef, size = 0xdeadbeef;
    NTSTATUS status;

    status = pBCryptGetProperty(handle, BCRYPT_HASH_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok_(__FILE__,line)(status == STATUS_SUCCESS, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(size == sizeof(len), "got %u\n", size);
    ok_(__FILE__,line)(len == exlen, "len = %u, expected %u\n", len, exlen);
}

#define test_alg_name(a,b) _test_alg_name(__LINE__,a,b)
static void _test_alg_name(unsigned line, void *handle, const char *exname)
{
    ULONG size = 0xdeadbeef;
    UCHAR buf[256];
    const WCHAR *name = (const WCHAR*)buf;
    NTSTATUS status;

    status = pBCryptGetProperty(handle, BCRYPT_ALGORITHM_NAME, buf, sizeof(buf), &size, 0);
    ok_(__FILE__,line)(status == STATUS_SUCCESS, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(size == (strlen(exname)+1)*sizeof(WCHAR), "got %u\n", size);
    ok_(__FILE__,line)(!strcmp_wa(name, exname), "alg name = %s, expected %s\n", wine_dbgstr_w(name), exname);
}

static void test_sha1(void)
{
    static const char expected[] = "961fa64958818f767707072755d7018dcd278e94";
    static const char expected_hmac[] = "2472cf65d0e090618d769d3e46f0d9446cf212da";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha1[20], sha1_hmac[20];
    ULONG size, len;
    char str[41];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(NULL, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_HANDLE, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, NULL, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), NULL, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, NULL, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, 0, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(len == 0xdeadbeef, "got %u\n", len);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len , sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len != 0xdeadbeef, "len not set\n");
    ok(size == sizeof(len), "got %u\n", size);

    test_hash_length(alg, 20);
    test_alg_name(alg, "SHA1");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 20);
    test_alg_name(hash, "SHA1");

    memset(sha1, 0, sizeof(sha1));
    ret = pBCryptFinishHash(hash, sha1, sizeof(sha1), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha1, sizeof(sha1), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 20);
    test_alg_name(hash, "SHA1");

    memset(sha1_hmac, 0, sizeof(sha1_hmac));
    ret = pBCryptFinishHash(hash, sha1_hmac, sizeof(sha1_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha1_hmac, sizeof(sha1_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_sha256(void)
{
    static const char expected[] =
        "ceb73749c899693706ede1e30c9929b3fd5dd926163831c2fb8bd41e6efb1126";
    static const char expected_hmac[] =
        "34c1aa473a4468a91d06e7cdbc75bc4f93b830ccfc2a47ffd74e8e6ed29e4c72";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha256[32], sha256_hmac[32];
    ULONG size, len;
    char str[65];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(NULL, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_HANDLE, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, NULL, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), NULL, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, NULL, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, 0, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(len == 0xdeadbeef, "got %u\n", len);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len , sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len != 0xdeadbeef, "len not set\n");
    ok(size == sizeof(len), "got %u\n", size);

    test_hash_length(alg, 32);
    test_alg_name(alg, "SHA256");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 32);
    test_alg_name(hash, "SHA256");

    memset(sha256, 0, sizeof(sha256));
    ret = pBCryptFinishHash(hash, sha256, sizeof(sha256), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha256, sizeof(sha256), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 32);
    test_alg_name(hash, "SHA256");

    memset(sha256_hmac, 0, sizeof(sha256_hmac));
    ret = pBCryptFinishHash(hash, sha256_hmac, sizeof(sha256_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha256_hmac, sizeof(sha256_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_sha384(void)
{
    static const char expected[] =
        "62b21e90c9022b101671ba1f808f8631a8149f0f12904055839a35c1ca78ae5363eed1e743a692d70e0504b0cfd12ef9";
    static const char expected_hmac[] =
        "4b3e6d6ff2da121790ab7e7b9247583e3a7eed2db5bd4dabc680303b1608f37dfdc836d96a704c03283bc05b4f6c5eb8";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha384[48], sha384_hmac[48];
    ULONG size, len;
    char str[97];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA384_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(NULL, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_HANDLE, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, NULL, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), NULL, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, NULL, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, 0, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(len == 0xdeadbeef, "got %u\n", len);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len , sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len != 0xdeadbeef, "len not set\n");
    ok(size == sizeof(len), "got %u\n", size);

    test_hash_length(alg, 48);
    test_alg_name(alg, "SHA384");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 48);
    test_alg_name(hash, "SHA384");

    memset(sha384, 0, sizeof(sha384));
    ret = pBCryptFinishHash(hash, sha384, sizeof(sha384), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha384, sizeof(sha384), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA384_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 48);
    test_alg_name(hash, "SHA384");

    memset(sha384_hmac, 0, sizeof(sha384_hmac));
    ret = pBCryptFinishHash(hash, sha384_hmac, sizeof(sha384_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha384_hmac, sizeof(sha384_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_sha512(void)
{
    static const char expected[] =
        "d55ced17163bf5386f2cd9ff21d6fd7fe576a915065c24744d09cfae4ec84ee1e"
        "f6ef11bfbc5acce3639bab725b50a1fe2c204f8c820d6d7db0df0ecbc49c5ca";
    static const char expected_hmac[] =
        "415fb6b10018ca03b38a1b1399c42ac0be5e8aceddb9a73103f5e543bf2d888f2"
        "eecf91373941f9315dd730a77937fa92444450fbece86f409d9cb5ec48c6513";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha512[64], sha512_hmac[64];
    ULONG size, len;
    char str[129];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(NULL, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_HANDLE, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, NULL, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), NULL, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, NULL, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, 0, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(len == 0xdeadbeef, "got %u\n", len);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len , sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len != 0xdeadbeef, "len not set\n");
    ok(size == sizeof(len), "got %u\n", size);

    test_hash_length(alg, 64);
    test_alg_name(alg, "SHA512");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 64);
    test_alg_name(hash, "SHA512");

    memset(sha512, 0, sizeof(sha512));
    ret = pBCryptFinishHash(hash, sha512, sizeof(sha512), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha512, sizeof(sha512), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 64);
    test_alg_name(hash, "SHA512");

    memset(sha512_hmac, 0, sizeof(sha512_hmac));
    ret = pBCryptFinishHash(hash, sha512_hmac, sizeof(sha512_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha512_hmac, sizeof(sha512_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

}

static void test_md5(void)
{
    static const char expected[] =
        "e2a3e68d23ce348b8f68b3079de3d4c9";
    static const char expected_hmac[] =
        "7bda029b93fa8d817fcc9e13d6bdf092";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], md5[16], md5_hmac[16];
    ULONG size, len;
    char str[65];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(NULL, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_HANDLE, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, NULL, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), NULL, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, NULL, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, 0, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(len == 0xdeadbeef, "got %u\n", len);
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len , sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len != 0xdeadbeef, "len not set\n");
    ok(size == sizeof(len), "got %u\n", size);

    test_hash_length(alg, 16);
    test_alg_name(alg, "MD5");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 16);
    test_alg_name(hash, "MD5");

    memset(md5, 0, sizeof(md5));
    ret = pBCryptFinishHash(hash, md5, sizeof(md5), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5, sizeof(md5), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 16);
    test_alg_name(hash, "MD5");

    memset(md5_hmac, 0, sizeof(md5_hmac));
    ret = pBCryptFinishHash(hash, md5_hmac, sizeof(md5_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5_hmac, sizeof(md5_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_BcryptHash(void)
{
    static const char expected[] =
        "e2a3e68d23ce348b8f68b3079de3d4c9";
    static const char expected_hmac[] =
        "7bda029b93fa8d817fcc9e13d6bdf092";
    BCRYPT_ALG_HANDLE alg;
    UCHAR md5[16], md5_hmac[16];
    char str[65];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_hash_length(alg, 16);
    test_alg_name(alg, "MD5");

    memset(md5, 0, sizeof(md5));
    ret = pBCryptHash(alg, NULL, 0, (UCHAR *)"test", sizeof("test"), md5, sizeof(md5));
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5, sizeof(md5), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    memset(md5_hmac, 0, sizeof(md5_hmac));
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    ret = pBCryptHash(alg, (UCHAR *)"key", sizeof("key"), (UCHAR *)"test", sizeof("test"), md5_hmac, sizeof(md5_hmac));
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5_hmac, sizeof(md5_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_rng(void)
{
    BCRYPT_ALG_HANDLE alg;
    ULONG size, len;
    UCHAR buf[16];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_RNG_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_NOT_SUPPORTED, "got %08x\n", ret);

    test_alg_name(alg, "RNG");

    memset(buf, 0, 16);
    ret = pBCryptGenRandom(alg, buf, 8, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(memcmp(buf, buf + 8, 8), "got zeroes\n");

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

START_TEST(bcrypt)
{
    HMODULE module;

    module = LoadLibraryA("bcrypt.dll");
    if (!module)
    {
        win_skip("bcrypt.dll not found\n");
        return;
    }

    pBCryptOpenAlgorithmProvider = (void *)GetProcAddress(module, "BCryptOpenAlgorithmProvider");
    pBCryptCloseAlgorithmProvider = (void *)GetProcAddress(module, "BCryptCloseAlgorithmProvider");
    pBCryptGetFipsAlgorithmMode = (void *)GetProcAddress(module, "BCryptGetFipsAlgorithmMode");
    pBCryptCreateHash = (void *)GetProcAddress(module, "BCryptCreateHash");
    pBCryptHash = (void *)GetProcAddress(module, "BCryptHash");
    pBCryptHashData = (void *)GetProcAddress(module, "BCryptHashData");
    pBCryptFinishHash = (void *)GetProcAddress(module, "BCryptFinishHash");
    pBCryptDestroyHash = (void *)GetProcAddress(module, "BCryptDestroyHash");
    pBCryptGenRandom = (void *)GetProcAddress(module, "BCryptGenRandom");
    pBCryptGetProperty = (void *)GetProcAddress(module, "BCryptGetProperty");
    pBCryptImportKeyPair = (void *)GetProcAddress(module, "BCryptImportKeyPair");
    pBCryptVerifySignature = (void *)GetProcAddress(module, "BCryptVerifySignature");
    pBCryptDestroyKey = (void *)GetProcAddress(module, "BCryptDestroyKey");

    test_BCryptGenRandom();
    test_BCryptGetFipsAlgorithmMode();
    test_sha1();
    test_sha256();
    test_sha384();
    test_sha512();
    test_md5();
    test_rng();
    test_ecdsa_verify();
    test_rsa_verify();

    if (pBCryptHash) /* >= Win 10 */
        test_BcryptHash();
    else
        win_skip("BCryptHash is not available\n");

    FreeLibrary(module);
}
