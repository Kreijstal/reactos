/*
 * PROJECT:     ReactOS i8042 (ps/2 keyboard-mouse controller) driver
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/input/i8042prt/elantech.h
 * PURPOSE:     Elantech touchpad protocol constants and per-device state
 *
 * All protocol magic values (commands, register layout, packet signatures)
 * are transcribed from the Linux v6.6 reference driver
 * drivers/input/mouse/elantech.[ch] and drivers/input/mouse/libps2.c.
 */

#ifndef _I8042PRT_ELANTECH_H_
#define _I8042PRT_ELANTECH_H_

/* Standard PS/2 mouse command bytes used by the Elantech protocol */
#define PSMOUSE_CMD_SETSCALE11    0xE6
#define PSMOUSE_CMD_SETRES        0xE8
#define PSMOUSE_CMD_GETINFO       0xE9

/* Command values for Synaptics style (sliced) queries */
#define ETP_FW_ID_QUERY           0x00
#define ETP_FW_VERSION_QUERY      0x01
#define ETP_CAPABILITIES_QUERY    0x02
#define ETP_SAMPLE_QUERY          0x03
#define ETP_RESOLUTION_QUERY      0x04
#define ETP_ICBODY_QUERY          0x05

/* Command values for register reading or writing */
#define ETP_REGISTER_READ         0x10
#define ETP_REGISTER_WRITE        0x11
#define ETP_REGISTER_READWRITE    0x00

/* Hardware version 2+ custom PS/2 command value */
#define ETP_PS2_CUSTOM_COMMAND    0xF8

/* Register bitmasks for hardware version 1 */
#define ETP_R10_ABSOLUTE_MODE     0x04
#define ETP_R11_4_BYTE_MODE       0x02

/* Capability bitmasks */
#define ETP_CAP_HAS_ROCKER        0x04

/* v3 hardware has 2 kinds of packet types, v4 hardware has 3 */
#define ELANTECH_PACKET_UNKNOWN     0x01
#define ELANTECH_PACKET_DEBOUNCE    0x02
#define ELANTECH_PACKET_V3_HEAD     0x03
#define ELANTECH_PACKET_V3_TAIL     0x04
#define ELANTECH_PACKET_V4_HEAD     0x05
#define ELANTECH_PACKET_V4_MOTION   0x06
#define ELANTECH_PACKET_V4_STATUS   0x07
#define ELANTECH_PACKET_TRACKPOINT  0x08

/* Track up to 5 fingers for v4 hardware */
#define ETP_MAX_FINGERS           5

/* Weight value for v4 hardware motion packets */
#define ETP_WEIGHT_VALUE          5

/* Bit 12 of fw_version: pad is a clickpad (both buttons map to left) */
#define ELANTECH_FW_IS_BUTTONPAD(FwVersion) \
    (((FwVersion) & 0x001000) != 0)

/* New ICs (used to detect the presence of a real middle button) */
#define ELANTECH_FW_NEW_IC_SMBUS_HOST_NOTIFY(FwVersion) \
    ((((FwVersion) & 0x0F2000) == 0x0F2000) && (((FwVersion) & 0x0000FF) != 0))

/*
 * MOUSE_RESET_SUBSTATE value used while the Elantech init engine runs.
 * Must live in the I8042ReservedMinimum (>= 1000) range and not collide
 * with the 1001-1040 values used by the generic wheel/5-button detection.
 */
#define ELANTECH_INIT_SUBSTATE    1100

/*
 * Longest command sequence: a hardware v1 register write is three sliced
 * commands (9 bytes each) plus a trailing Set Scaling 1:1 = 28 bytes.
 */
#define ELANTECH_SEQ_MAX          32

/* Maximum number of register writes needed for absolute mode (hw v2) */
#define ELANTECH_MAX_ABS_REGS     3

typedef enum _ELANTECH_INIT_PHASE
{
    ElantechPhaseFwVersion,
    ElantechPhaseCapabilities,
    ElantechPhaseSample,
    ElantechPhaseSetAbsolute,
    ElantechPhaseReadBack
} ELANTECH_INIT_PHASE;

typedef struct _ELANTECH_EXTENSION
{
    /* Device information (mirrors Linux struct elantech_device_info) */
    ULONG FwVersion;
    UCHAR IcVersion;
    UCHAR HwVersion;
    UCHAR Capabilities[3];
    UCHAR Samples[3];
    BOOLEAN CrcEnabled;
    BOOLEAN ReportsPressure;
    BOOLEAN HasTrackpoint;
    BOOLEAN HasMiddleButton;
    BOOLEAN JumpyCursor;
    /* Sticky: a previous Elantech init attempt failed, use the legacy path */
    BOOLEAN Failed;

    /* Init command engine (table-driven, advanced from the ISR) */
    ELANTECH_INIT_PHASE Phase;
    UCHAR Seq[ELANTECH_SEQ_MAX];
    UCHAR SeqLength;
    UCHAR SeqAcked;
    UCHAR ResponseLength;
    UCHAR ResponseIndex;
    UCHAR Response[3];
    UCHAR AbsRegs[ELANTECH_MAX_ABS_REGS][2]; /* {register, value} pairs */
    UCHAR AbsRegCount;
    UCHAR AbsRegIndex;

    /* Packet accumulation */
    UCHAR Packet[6];
    UCHAR PacketIndex;
    UCHAR PacketSize;                        /* 4 (v1) or 6 (v2/v3/v4) */

    /* Byte parity table for the v1 packet check */
    UCHAR Parity[256];

    /*
     * Absolute-to-relative translation state. Y coordinates are kept in
     * "screen" orientation (positive = down), i.e. the negated raw value.
     */
    ULONG LastFingers;
    LONG LastX;
    LONG LastY;
    LONG AccumX;
    LONG AccumY;
    LONG ScrollAccum;
    ULONG SingleFingerReports;
    LONG DeltaDivisor;
    LONG ScrollThreshold;

    /* v3 two-finger head packet cache / v4 per-finger tracking */
    LONG FingerX[ETP_MAX_FINGERS];
    LONG FingerY[ETP_MAX_FINGERS];
    ULONG FingerBitmap;                      /* v4 active finger bitmap */
} ELANTECH_EXTENSION, *PELANTECH_EXTENSION;

#endif /* _I8042PRT_ELANTECH_H_ */
