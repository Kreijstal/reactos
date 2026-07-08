/*
 * PROJECT:     ReactOS HD Audio codec function driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Common definitions for the generic HD Audio codec driver
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#ifndef _HDAUDIO_FN_DRIVER_H_
#define _HDAUDIO_FN_DRIVER_H_

#include <ntddk.h>
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>
#include <hdaudio.h>

#define HDA_POOL_TAG 'aDDH'

/* provided by the stdunk library */
PVOID
operator new(
    size_t size,
    POOL_TYPE pool_type,
    ULONG tag);

/* not part of the generated ntddk.h, only of ntifs.h */
extern "C"
NTKERNELAPI
PDEVICE_OBJECT
NTAPI
IoGetLowerDeviceObject(
    _In_ PDEVICE_OBJECT DeviceObject);

/* miniport factory prototype (same shape as the ac97 sample) */
typedef NTSTATUS (*PFNCREATEMINIPORT)(
    OUT PUNKNOWN *Unknown,
    IN REFCLSID ClassId,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType);

/*
 * HD Audio verb / parameter encodings.
 * Transcribed from the Intel High Definition Audio specification 1.0a,
 * cross-checked against Linux v6.6 include/sound/hda_verbs.h.
 */

/* 12-bit verbs (8-bit payload) */
#define HDA_VERB_GET_STREAM_FORMAT      0xa00
#define HDA_VERB_GET_AMP_GAIN_MUTE      0xb00
#define HDA_VERB_GET_PARAMETERS         0xf00
#define HDA_VERB_GET_CONNECT_SEL        0xf01
#define HDA_VERB_GET_CONNECT_LIST       0xf02
#define HDA_VERB_GET_POWER_STATE        0xf05
#define HDA_VERB_GET_PIN_WIDGET_CONTROL 0xf07
#define HDA_VERB_GET_CONFIG_DEFAULT     0xf1c
#define HDA_VERB_SET_CONNECT_SEL        0x701
#define HDA_VERB_SET_POWER_STATE        0x705
#define HDA_VERB_SET_CHANNEL_STREAMID   0x706
#define HDA_VERB_SET_PIN_WIDGET_CONTROL 0x707
#define HDA_VERB_SET_EAPD_BTLENABLE     0x70c

/* 4-bit verbs (16-bit payload) */
#define HDA_VERB16_SET_STREAM_FORMAT    0x2
#define HDA_VERB16_SET_AMP_GAIN_MUTE    0x3

/* Parameters (payload of GET_PARAMETERS) */
#define HDA_PAR_VENDOR_ID           0x00
#define HDA_PAR_NODE_COUNT          0x04
#define HDA_PAR_FUNCTION_TYPE       0x05
#define HDA_PAR_AUDIO_FG_CAP        0x08
#define HDA_PAR_AUDIO_WIDGET_CAP    0x09
#define HDA_PAR_PCM                 0x0a
#define HDA_PAR_STREAM              0x0b
#define HDA_PAR_PIN_CAP             0x0c
#define HDA_PAR_AMP_IN_CAP          0x0d
#define HDA_PAR_CONNLIST_LEN        0x0e
#define HDA_PAR_POWER_STATE         0x0f
#define HDA_PAR_AMP_OUT_CAP         0x12

/* Function group types */
#define HDA_GRP_AUDIO_FUNCTION      0x01

/* Audio widget capabilities */
#define HDA_WCAP_STEREO             (1u << 0)
#define HDA_WCAP_IN_AMP             (1u << 1)
#define HDA_WCAP_OUT_AMP            (1u << 2)
#define HDA_WCAP_AMP_OVRD           (1u << 3)
#define HDA_WCAP_FORMAT_OVRD        (1u << 4)
#define HDA_WCAP_CONN_LIST          (1u << 8)
#define HDA_WCAP_DIGITAL            (1u << 9)
#define HDA_WCAP_POWER              (1u << 10)
#define HDA_WCAP_TYPE(wcaps)        (((wcaps) >> 20) & 0xf)

/* Widget types */
#define HDA_WID_AUD_OUT             0x0
#define HDA_WID_AUD_IN              0x1
#define HDA_WID_AUD_MIX             0x2
#define HDA_WID_AUD_SEL             0x3
#define HDA_WID_PIN                 0x4
#define HDA_WID_POWER               0x5
#define HDA_WID_VOL_KNB             0x6
#define HDA_WID_BEEP                0x7
#define HDA_WID_VENDOR              0xf

/* Pin capabilities */
#define HDA_PINCAP_PRES_DETECT      (1u << 2)
#define HDA_PINCAP_HP_DRV           (1u << 3)
#define HDA_PINCAP_OUT              (1u << 4)
#define HDA_PINCAP_IN               (1u << 5)
#define HDA_PINCAP_EAPD             (1u << 16)

/* Amplifier capabilities (AMP_IN_CAP / AMP_OUT_CAP) */
#define HDA_AMPCAP_OFFSET(caps)     (((caps) >> 0) & 0x7f)
#define HDA_AMPCAP_NUM_STEPS(caps)  (((caps) >> 8) & 0x7f)
#define HDA_AMPCAP_STEP_SIZE(caps)  (((caps) >> 16) & 0x7f)
#define HDA_AMPCAP_MUTE             (1u << 31)

/* SET_AMP_GAIN_MUTE payload bits */
#define HDA_AMP_MUTE                (1u << 7)
#define HDA_AMP_GAIN_MASK           0x7f
#define HDA_AMP_SET_INDEX_SHIFT     8
#define HDA_AMP_SET_RIGHT           (1u << 12)
#define HDA_AMP_SET_LEFT            (1u << 13)
#define HDA_AMP_SET_INPUT           (1u << 14)
#define HDA_AMP_SET_OUTPUT          (1u << 15)

/* Pin widget control bits */
#define HDA_PINCTL_IN_EN            (1u << 5)
#define HDA_PINCTL_OUT_EN           (1u << 6)
#define HDA_PINCTL_HP_EN            (1u << 7)

/* EAPD/BTL enable bits */
#define HDA_EAPDBTL_EAPD            (1u << 1)

/* Power states */
#define HDA_PWRST_D0                0x00
#define HDA_PWRST_D3                0x03

/* Configuration default fields */
#define HDA_DEFCFG_DEVICE(cfg)      (((cfg) >> 20) & 0xf)
#define HDA_DEFCFG_PORT_CONN(cfg)   (((cfg) >> 30) & 0x3)

/* Default device types */
#define HDA_JACK_LINE_OUT           0x0
#define HDA_JACK_SPEAKER            0x1
#define HDA_JACK_HP_OUT             0x2
#define HDA_JACK_MIC_IN             0xa
#define HDA_JACK_LINE_IN            0x8

/* Port connectivity */
#define HDA_PORT_CONN_JACK          0x0
#define HDA_PORT_CONN_NONE          0x1
#define HDA_PORT_CONN_FIXED         0x2
#define HDA_PORT_CONN_BOTH          0x3

/* Connection list */
#define HDA_CLIST_LEN_MASK          0x7f
#define HDA_CLIST_LONG              (1u << 7)

#define HDA_MAX_PATH_LENGTH         8
#define HDA_MAX_CONNECTIONS         32
#define HDA_MAX_WIDGETS             128

/* most negative value the volume property can carry */
#define HDA_PROP_MOST_NEGATIVE      ((LONG)0x80000000)

typedef struct _HDA_WIDGET
{
    UCHAR Nid;
    ULONG Caps;         /* audio widget capabilities */
    ULONG PinCaps;      /* pin capabilities (pins only) */
    ULONG DefConfig;    /* configuration default (pins only) */
    USHORT ConnCount;
    UCHAR Connections[HDA_MAX_CONNECTIONS];
} HDA_WIDGET, *PHDA_WIDGET;

/*****************************************************************************
 * IHDACodec
 *****************************************************************************
 * Private interface between the adapter common (codec) object and the
 * wave/topology miniports.
 */
#undef INTERFACE
#define INTERFACE IHDACodec

DEFINE_GUID(IID_IHDACodec,
    0x8dbb63b1, 0x24c2, 0x4e6a, 0xb1, 0xac, 0x93, 0x11, 0x5c, 0x67, 0x40, 0x22);

DECLARE_INTERFACE_(IHDACodec, IUnknown)
{
    DEFINE_ABSTRACT_UNKNOWN()

    STDMETHOD_(NTSTATUS, Init)( THIS_
        IN PDEVICE_OBJECT DeviceObject) PURE;

    STDMETHOD_(PHDAUDIO_BUS_INTERFACE, GetBusInterface)( THIS ) PURE;

    /* program converter stream/channel and format for the render path */
    STDMETHOD_(NTSTATUS, SetRenderStream)( THIS_
        IN UCHAR StreamId,
        IN USHORT ConverterFormat) PURE;

    /* does the render path have a controllable output amplifier? */
    STDMETHOD_(BOOLEAN, HasVolumeControl)( THIS ) PURE;

    /* volume range in 16.16 fixed point dB */
    STDMETHOD_(void, GetVolumeRange)( THIS_
        OUT PLONG Minimum,
        OUT PLONG Maximum,
        OUT PLONG Step) PURE;

    STDMETHOD_(NTSTATUS, SetVolume)( THIS_
        IN ULONG Channel,
        IN LONG Level) PURE;

    STDMETHOD_(NTSTATUS, GetVolume)( THIS_
        IN ULONG Channel,
        OUT PLONG Level) PURE;

    STDMETHOD_(NTSTATUS, SetMute)( THIS_
        IN BOOL Mute) PURE;

    STDMETHOD_(NTSTATUS, GetMute)( THIS_
        OUT PBOOL Mute) PURE;
};

typedef IHDACodec *PHDACODEC;

/*****************************************************************************
 * Object creation functions
 */
NTSTATUS NewHDACodec(
    OUT PUNKNOWN *Unknown,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType);

NTSTATUS CreateMiniportWaveCyclicHDA(
    OUT PUNKNOWN *Unknown,
    IN REFCLSID ClassId,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType);

NTSTATUS CreateMiniportTopologyHDA(
    OUT PUNKNOWN *Unknown,
    IN REFCLSID ClassId,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType);

#endif /* _HDAUDIO_FN_DRIVER_H_ */
