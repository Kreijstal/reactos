#ifndef __WLANTYPES_H__
#define __WLANTYPES_H__

#define DOT11_SSID_MAX_LENGTH 32

typedef enum _DOT11_BSS_TYPE {
    dot11_BSS_type_infrastructure = 1,
    dot11_BSS_type_independent    = 2,
    dot11_BSS_type_any            = 3
} DOT11_BSS_TYPE, *PDOT11_BSS_TYPE;

/* The full DOT11_AUTH_ALGORITHM / DOT11_CIPHER_ALGORITHM enums and the
 * DOT11_SSID struct live here so windot11.h (which only declares the OID
 * and structure surface for NDIS 6 miniports) can reference them via the
 * usual <wlantypes.h> include.  This matches the layout the Windows SDK
 * ships, which mingw-w64 mirrors. */

#ifdef __WIDL__
typedef [v1_enum] enum _DOT11_AUTH_ALGORITHM {
#else
typedef enum _DOT11_AUTH_ALGORITHM {
#endif
    DOT11_AUTH_ALGO_80211_OPEN         = 1,
    DOT11_AUTH_ALGO_80211_SHARED_KEY   = 2,
    DOT11_AUTH_ALGO_WPA                = 3,
    DOT11_AUTH_ALGO_WPA_PSK            = 4,
    DOT11_AUTH_ALGO_WPA_NONE           = 5,
    DOT11_AUTH_ALGO_RSNA               = 6,
    DOT11_AUTH_ALGO_RSNA_PSK           = 7,
    DOT11_AUTH_ALGO_WPA3               = 8,
    DOT11_AUTH_ALGO_WPA3_SAE           = 9,
    DOT11_AUTH_ALGO_OWE                = 10,
    DOT11_AUTH_ALGO_WPA3_ENT           = 11,
    DOT11_AUTH_ALGO_IHV_START          = 0x80000000,
    DOT11_AUTH_ALGO_IHV_END            = 0xffffffff
} DOT11_AUTH_ALGORITHM;
typedef DOT11_AUTH_ALGORITHM *PDOT11_AUTH_ALGORITHM;

#ifdef __WIDL__
typedef [v1_enum] enum _DOT11_CIPHER_ALGORITHM {
#else
typedef enum _DOT11_CIPHER_ALGORITHM {
#endif
    DOT11_CIPHER_ALGO_NONE            = 0x00,
    DOT11_CIPHER_ALGO_WEP40           = 0x01,
    DOT11_CIPHER_ALGO_TKIP            = 0x02,
    DOT11_CIPHER_ALGO_CCMP            = 0x04,
    DOT11_CIPHER_ALGO_WEP104          = 0x05,
    DOT11_CIPHER_ALGO_BIP             = 0x06,
    DOT11_CIPHER_ALGO_GCMP            = 0x08,
    DOT11_CIPHER_ALGO_GCMP_256        = 0x09,
    DOT11_CIPHER_ALGO_CCMP_256        = 0x0a,
    DOT11_CIPHER_ALGO_BIP_GMAC_128    = 0x0b,
    DOT11_CIPHER_ALGO_BIP_GMAC_256    = 0x0c,
    DOT11_CIPHER_ALGO_BIP_CMAC_256    = 0x0d,
    DOT11_CIPHER_ALGO_WPA_USE_GROUP   = 0x100,
    DOT11_CIPHER_ALGO_RSN_USE_GROUP   = 0x100,
    DOT11_CIPHER_ALGO_WEP             = 0x101,
    DOT11_CIPHER_ALGO_IHV_START       = 0x80000000,
    DOT11_CIPHER_ALGO_IHV_END         = 0xffffffff
} DOT11_CIPHER_ALGORITHM;
typedef DOT11_CIPHER_ALGORITHM *PDOT11_CIPHER_ALGORITHM;

typedef struct _DOT11_SSID {
    ULONG uSSIDLength;
    UCHAR ucSSID[DOT11_SSID_MAX_LENGTH];
} DOT11_SSID, *PDOT11_SSID;

typedef struct _DOT11_AUTH_CIPHER_PAIR {
    DOT11_AUTH_ALGORITHM   AuthAlgoId;
    DOT11_CIPHER_ALGORITHM CipherAlgoId;
} DOT11_AUTH_CIPHER_PAIR, *PDOT11_AUTH_CIPHER_PAIR;

#endif
