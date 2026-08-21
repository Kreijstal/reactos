/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Verbatim subset of Linux drivers/net/wireless/ath/ath9k/
 *              ar9003_eeprom.h (v6.6) - the AR9300-family EEPROM/OTP
 *              constants and the on-media `struct ar9300_eeprom' layout.
 *
 *              Slice 3 of the AR9485 port (Phase 2a).  Everything between
 *              the VERBATIM markers below is byte-for-byte upstream; the
 *              only additions are this banner, the include guard, the
 *              AR_EEPROM_MODAL_SPURS constant lifted from upstream
 *              ath9k/eeprom.h:20 (ar9003_eeprom.h gets it transitively via
 *              hw.h, which we do not port), and the pshpack1/poppack pair
 *              that makes the upstream __packed attribute effective for
 *              both the GCC and the MSVC ReactOS builds.
 *
 *              sizeof(struct ar9300_eeprom) is load-bearing: it is the
 *              `mdata_size' handed to ar9300_eeprom_restore_internal(),
 *              which both gates the _CompressNone path (length must match
 *              exactly) and bounds the _CompressBlock writes.  A layout
 *              divergence here silently truncates the restore, so the
 *              struct is copied whole rather than trimmed to the prefix
 *              that carries the MAC address.
 */

#ifndef _AR9485_ATH9K_AR9003_EEPROM_MIN_H_
#define _AR9485_ATH9K_AR9003_EEPROM_MIN_H_

#include "../linux-compat.h"

/* Upstream ath9k/eeprom.h:20 - referenced by ar9300_modal_eep_header. */
#define AR_EEPROM_MODAL_SPURS   5

/* ---------------- VERBATIM ar9003_eeprom.h:22-94 ------------------- */

#define AR9300_EEP_VER               0xD000
#define AR9300_EEP_VER_MINOR_MASK    0xFFF
#define AR9300_EEP_MINOR_VER_1       0x1
#define AR9300_EEP_MINOR_VER         AR9300_EEP_MINOR_VER_1

/* 16-bit offset location start of calibration struct */
#define AR9300_EEP_START_LOC         256
#define AR9300_NUM_5G_CAL_PIERS      8
#define AR9300_NUM_2G_CAL_PIERS      3
#define AR9300_NUM_5G_20_TARGET_POWERS  8
#define AR9300_NUM_5G_40_TARGET_POWERS  8
#define AR9300_NUM_2G_CCK_TARGET_POWERS 2
#define AR9300_NUM_2G_20_TARGET_POWERS  3
#define AR9300_NUM_2G_40_TARGET_POWERS  3
/* #define AR9300_NUM_CTLS              21 */
#define AR9300_NUM_CTLS_5G           9
#define AR9300_NUM_CTLS_2G           12
#define AR9300_NUM_BAND_EDGES_5G     8
#define AR9300_NUM_BAND_EDGES_2G     4
#define AR9300_EEPMISC_WOW           0x02
#define AR9300_CUSTOMER_DATA_SIZE    20

#define AR9300_MAX_CHAINS            3
#define AR9300_ANT_16S               25
#define AR9300_FUTURE_MODAL_SZ       6

#define AR9300_PAPRD_RATE_MASK		0x01ffffff
#define AR9300_PAPRD_SCALE_1		0x0e000000
#define AR9300_PAPRD_SCALE_1_S		25
#define AR9300_PAPRD_SCALE_2		0x70000000
#define AR9300_PAPRD_SCALE_2_S		28

#define AR9300_EEP_ANTDIV_CONTROL_DEFAULT_VALUE 0xc9

/* Delta from which to start power to pdadc table */
/* This offset is used in both open loop and closed loop power control
 * schemes. In open loop power control, it is not really needed, but for
 * the "sake of consistency" it was kept. For certain AP designs, this
 * value is overwritten by the value in the flag "pwrTableOffset" just
 * before writing the pdadc vs pwr into the chip registers.
 */
#define AR9300_PWR_TABLE_OFFSET  0

/* Noise power data definitions
 * units are: 4 x dBm - NOISE_PWR_DATA_OFFSET
 * (e.g. -25 = (-25/4 - 90) = -96.25 dBm)
 * range (for 6 signed bits) is (-32 to 31) + offset => -122dBm to -59dBm
 * resolution (2 bits) is 0.25dBm
 */
#define NOISE_PWR_DATA_OFFSET	-90
#define NOISE_PWR_DBM_2_INT(_p)	((((_p) + 3) >> 2) + NOISE_PWR_DATA_OFFSET)
#define N2DBM(_p)		NOISE_PWR_DBM_2_INT(_p)

/* byte addressable */
#define AR9300_EEPROM_SIZE (16*1024)

#define AR9300_BASE_ADDR_4K 0xfff
#define AR9300_BASE_ADDR 0x3ff
#define AR9300_BASE_ADDR_512 0x1ff

/* AR5416_EEPMISC_BIG_ENDIAN not set indicates little endian */
#define AR9300_EEPMISC_LITTLE_ENDIAN 0

#define AR9300_OTP_BASE(_ah) \
		((AR_SREV_9340(_ah) || AR_SREV_9550(_ah)) ? 0x30000 : 0x14000)
#define AR9300_OTP_STATUS(_ah) \
		((AR_SREV_9340(_ah) || AR_SREV_9550(_ah)) ? 0x31018 : 0x15f18)
#define AR9300_OTP_STATUS_TYPE		0x7
#define AR9300_OTP_STATUS_VALID		0x4
#define AR9300_OTP_STATUS_ACCESS_BUSY	0x2
#define AR9300_OTP_STATUS_SM_BUSY	0x1
#define AR9300_OTP_READ_DATA(_ah) \
		((AR_SREV_9340(_ah) || AR_SREV_9550(_ah)) ? 0x3101c : 0x15f1c)

/* ---------------- VERBATIM ar9003_eeprom.h:168-357 ----------------- */

/* The upstream structures below are all __packed.  ReactOS builds with
 * both GCC and MSVC; __packed expands to the GCC attribute (see
 * linux-compat.h) and is a no-op under MSVC, so wrap the block in the
 * SDK's pshpack1/poppack pair to get identical layout either way. */
#include <pshpack1.h>

struct eepFlags {
	u8 opFlags;
	u8 eepMisc;
} __packed;

enum CompressAlgorithm {
	_CompressNone = 0,
	_CompressLzma,
	_CompressPairs,
	_CompressBlock,
	_Compress4,
	_Compress5,
	_Compress6,
	_Compress7,
};

struct ar9300_base_eep_hdr {
	__le16 regDmn[2];
	/* 4 bits tx and 4 bits rx */
	u8 txrxMask;
	struct eepFlags opCapFlags;
	u8 rfSilent;
	u8 blueToothOptions;
	u8 deviceCap;
	/* takes lower byte in eeprom location */
	u8 deviceType;
	/* offset in dB to be added to beginning
	 * of pdadc table in calibration
	 */
	int8_t pwrTableOffset;
	u8 params_for_tuning_caps[2];
	/*
	 * bit0 - enable tx temp comp
	 * bit1 - enable tx volt comp
	 * bit2 - enable fastClock - default to 1
	 * bit3 - enable doubling - default to 1
	 * bit4 - enable internal regulator - default to 1
	 */
	u8 featureEnable;
	/* misc flags: bit0 - turn down drivestrength */
	u8 miscConfiguration;
	u8 eepromWriteEnableGpio;
	u8 wlanDisableGpio;
	u8 wlanLedGpio;
	u8 rxBandSelectGpio;
	u8 txrxgain;
	/* SW controlled internal regulator fields */
	__le32 swreg;
} __packed;

struct ar9300_modal_eep_header {
	/* 4 idle, t1, t2, b (4 bits per setting) */
	__le32 antCtrlCommon;
	/* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
	__le32 antCtrlCommon2;
	/* 6 idle, t, r, rx1, rx12, b (2 bits each) */
	__le16 antCtrlChain[AR9300_MAX_CHAINS];
	/* 3 xatten1_db for AR9280 (0xa20c/b20c 5:0) */
	u8 xatten1DB[AR9300_MAX_CHAINS];
	/* 3  xatten1_margin for merlin (0xa20c/b20c 16:12 */
	u8 xatten1Margin[AR9300_MAX_CHAINS];
	int8_t tempSlope;
	int8_t voltSlope;
	/* spur channels in usual fbin coding format */
	u8 spurChans[AR_EEPROM_MODAL_SPURS];
	/* 3  Check if the register is per chain */
	int8_t noiseFloorThreshCh[AR9300_MAX_CHAINS];
	u8 reserved[11];
	int8_t quick_drop;
	u8 xpaBiasLvl;
	u8 txFrameToDataStart;
	u8 txFrameToPaOn;
	u8 txClip;
	int8_t antennaGain;
	u8 switchSettling;
	int8_t adcDesiredSize;
	u8 txEndToXpaOff;
	u8 txEndToRxOn;
	u8 txFrameToXpaOn;
	u8 thresh62;
	__le32 papdRateMaskHt20;
	__le32 papdRateMaskHt40;
	__le16 switchcomspdt;
	u8 xlna_bias_strength;
	u8 futureModal[7];
} __packed;

struct ar9300_cal_data_per_freq_op_loop {
	int8_t refPower;
	/* pdadc voltage at power measurement */
	u8 voltMeas;
	/* pcdac used for power measurement   */
	u8 tempMeas;
	/* range is -60 to -127 create a mapping equation 1db resolution */
	int8_t rxNoisefloorCal;
	/*range is same as noisefloor */
	int8_t rxNoisefloorPower;
	/* temp measured when noisefloor cal was performed */
	u8 rxTempMeas;
} __packed;

struct cal_tgt_pow_legacy {
	u8 tPow2x[4];
} __packed;

struct cal_tgt_pow_ht {
	u8 tPow2x[14];
} __packed;

struct cal_ctl_data_2g {
	u8 ctlEdges[AR9300_NUM_BAND_EDGES_2G];
} __packed;

struct cal_ctl_data_5g {
	u8 ctlEdges[AR9300_NUM_BAND_EDGES_5G];
} __packed;

#define MAX_BASE_EXTENSION_FUTURE 2

struct ar9300_BaseExtension_1 {
	u8 ant_div_control;
	u8 future[MAX_BASE_EXTENSION_FUTURE];
	/*
	 * misc_enable:
	 *
	 * BIT 0   - TX Gain Cap enable.
	 * BIT 1   - Uncompressed Checksum enable.
	 * BIT 2/3 - MinCCApwr enable 2g/5g.
	 */
	u8 misc_enable;
	int8_t tempslopextension[8];
	int8_t quick_drop_low;
	int8_t quick_drop_high;
} __packed;

struct ar9300_BaseExtension_2 {
	int8_t    tempSlopeLow;
	int8_t    tempSlopeHigh;
	u8   xatten1DBLow[AR9300_MAX_CHAINS];
	u8   xatten1MarginLow[AR9300_MAX_CHAINS];
	u8   xatten1DBHigh[AR9300_MAX_CHAINS];
	u8   xatten1MarginHigh[AR9300_MAX_CHAINS];
} __packed;

struct ar9300_eeprom {
	u8 eepromVersion;
	u8 templateVersion;
	u8 macAddr[6];
	u8 custData[AR9300_CUSTOMER_DATA_SIZE];

	struct ar9300_base_eep_hdr baseEepHeader;

	struct ar9300_modal_eep_header modalHeader2G;
	struct ar9300_BaseExtension_1 base_ext1;
	u8 calFreqPier2G[AR9300_NUM_2G_CAL_PIERS];
	struct ar9300_cal_data_per_freq_op_loop
	 calPierData2G[AR9300_MAX_CHAINS][AR9300_NUM_2G_CAL_PIERS];
	u8 calTarget_freqbin_Cck[AR9300_NUM_2G_CCK_TARGET_POWERS];
	u8 calTarget_freqbin_2G[AR9300_NUM_2G_20_TARGET_POWERS];
	u8 calTarget_freqbin_2GHT20[AR9300_NUM_2G_20_TARGET_POWERS];
	u8 calTarget_freqbin_2GHT40[AR9300_NUM_2G_40_TARGET_POWERS];
	struct cal_tgt_pow_legacy
	 calTargetPowerCck[AR9300_NUM_2G_CCK_TARGET_POWERS];
	struct cal_tgt_pow_legacy
	 calTargetPower2G[AR9300_NUM_2G_20_TARGET_POWERS];
	struct cal_tgt_pow_ht
	 calTargetPower2GHT20[AR9300_NUM_2G_20_TARGET_POWERS];
	struct cal_tgt_pow_ht
	 calTargetPower2GHT40[AR9300_NUM_2G_40_TARGET_POWERS];
	u8 ctlIndex_2G[AR9300_NUM_CTLS_2G];
	u8 ctl_freqbin_2G[AR9300_NUM_CTLS_2G][AR9300_NUM_BAND_EDGES_2G];
	struct cal_ctl_data_2g ctlPowerData_2G[AR9300_NUM_CTLS_2G];
	struct ar9300_modal_eep_header modalHeader5G;
	struct ar9300_BaseExtension_2 base_ext2;
	u8 calFreqPier5G[AR9300_NUM_5G_CAL_PIERS];
	struct ar9300_cal_data_per_freq_op_loop
	 calPierData5G[AR9300_MAX_CHAINS][AR9300_NUM_5G_CAL_PIERS];
	u8 calTarget_freqbin_5G[AR9300_NUM_5G_20_TARGET_POWERS];
	u8 calTarget_freqbin_5GHT20[AR9300_NUM_5G_20_TARGET_POWERS];
	u8 calTarget_freqbin_5GHT40[AR9300_NUM_5G_40_TARGET_POWERS];
	struct cal_tgt_pow_legacy
	 calTargetPower5G[AR9300_NUM_5G_20_TARGET_POWERS];
	struct cal_tgt_pow_ht
	 calTargetPower5GHT20[AR9300_NUM_5G_20_TARGET_POWERS];
	struct cal_tgt_pow_ht
	 calTargetPower5GHT40[AR9300_NUM_5G_40_TARGET_POWERS];
	u8 ctlIndex_5G[AR9300_NUM_CTLS_5G];
	u8 ctl_freqbin_5G[AR9300_NUM_CTLS_5G][AR9300_NUM_BAND_EDGES_5G];
	struct cal_ctl_data_5g ctlPowerData_5G[AR9300_NUM_CTLS_5G];
} __packed;

#include <poppack.h>

/* ---------------- end verbatim ------------------------------------- */

/* Layout tripwire: the AR9485 compressed-block length bound in
 * ar9300_eeprom_restore_internal() is EEPROM_DATA_LEN_9485 == 1088, which
 * upstream sizes against this very struct.  If a future edit perturbs the
 * packing, fail the build rather than mis-parse the OTP at runtime. */
C_ASSERT(sizeof(struct ar9300_eeprom) == 1088);

/* Byte offset of the permanent MAC inside the restored image: it follows
 * eepromVersion (1 byte) and templateVersion (1 byte). */
#define AR9300_EEPROM_MACADDR_OFFSET   2

#endif /* _AR9485_ATH9K_AR9003_EEPROM_MIN_H_ */
