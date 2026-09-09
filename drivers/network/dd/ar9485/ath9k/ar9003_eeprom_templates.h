/*
 * PROJECT:     ReactOS AR9485 Wi-Fi miniport
 * PURPOSE:     AR9300-family EEPROM reference templates
 * COPYRIGHT:   Copyright (c) 2010-2011 Atheros Communications, Inc.
 *              ISC-licensed, as ath9k's ar9003_eeprom.c
 *
 * VERBATIM from linux/drivers/net/wireless/ath/ath9k/ar9003_eeprom.c:46-2955
 * (v6.12-rc6, c426456857fa).  Only the LE16/LE32 defines below are added; not
 * one template byte is edited.
 *
 * WHY THIS EXISTS
 * ---------------
 * The compressed EEPROM image on an AR9300-family card is a DIFF, not a whole
 * image.  ar9300_uncompress_block() walks (offset, length, bytes) triples and
 * writes only what the card actually carries; every field the card does not
 * override is expected to already hold the reference template's value.
 *
 * Phase 2a restored the image over ZEROED memory and documented that as safe,
 * which it was for its one consumer: a per-card MAC is always carried by the
 * diff, so it cannot come from the template.  hw_board.c changed that.  It
 * reads antCtrlCommon, antCtrlCommon2, antCtrlChain[] and xpaBiasLvl, none of
 * which this card's diff carries -- so on 2026-08-26 all four read back 0, and
 * ar9003_hw_ant_ctrl_apply() wrote those zeros over a working initval default:
 * AR_PHY_SWITCH_CHAIN_0 went 0x000002a0 -> 0x00000000 on the metal.
 *
 * A zeroed base is not a neutral starting point.  It is a wrong one that only
 * looks neutral because the first consumer happened not to read those fields.
 *
 * THE MAC HAZARD, WHICH IS REAL
 * -----------------------------
 * ar9300_default.macAddr is the placeholder {0, 2, 3, 4, 5, 6}.  Seeding it
 * means a card whose diff omits the MAC now yields a plausible-looking wrong
 * address instead of all zeroes.  ar9485_is_valid_mac() already rejects that
 * exact constant -- it was written for this moment, and its comment says so.
 * Do not remove that check.
 */

#ifndef _AR9003_EEPROM_TEMPLATES_H_
#define _AR9003_EEPROM_TEMPLATES_H_

/* VERBATIM from ar9003_eeprom.c:27-41, the macro block the templates are
 * written in terms of. */
#define LE16(x) cpu_to_le16(x)
#define LE32(x) cpu_to_le32(x)

/* Local defines to distinguish between extension and control CTL's */
#define EXT_ADDITIVE (0x8000)
#define CTL_11A_EXT (CTL_11A | EXT_ADDITIVE)
#define CTL_11G_EXT (CTL_11G | EXT_ADDITIVE)
#define CTL_11B_EXT (CTL_11B | EXT_ADDITIVE)

#define CTL(_tpower, _flag) ((_tpower) | ((_flag) << 6))

/* VERBATIM from ath9k's eeprom.h:76-83,109,125-126.  The templates are the
 * only thing in this port that needs them. */
#define CTL_11A                 0
#define CTL_11B                 1
#define CTL_11G                 2
#define CTL_2GHT20              5
#define CTL_5GHT20              6
#define CTL_2GHT40              7
#define CTL_5GHT40              8

#define FREQ2FBIN(x, y)         (u8)((y) ? ((x) - 2300) : (((x) - 4800) / 5))
#define AR5416_OPFLAGS_11A      0x01
#define AR5416_OPFLAGS_11G      0x02

static const struct ar9300_eeprom ar9300_default = {
	.eepromVersion = 2,
	.templateVersion = 2,
	.macAddr = {0, 2, 3, 4, 5, 6},
	.custData = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		     0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	.baseEepHeader = {
		.regDmn = { LE16(0), LE16(0x1f) },
		.txrxMask =  0x77, /* 4 bits tx and 4 bits rx */
		.opCapFlags = {
			.opFlags = AR5416_OPFLAGS_11G | AR5416_OPFLAGS_11A,
			.eepMisc = AR9300_EEPMISC_LITTLE_ENDIAN,
		},
		.rfSilent = 0,
		.blueToothOptions = 0,
		.deviceCap = 0,
		.deviceType = 5, /* takes lower byte in eeprom location */
		.pwrTableOffset = AR9300_PWR_TABLE_OFFSET,
		.params_for_tuning_caps = {0, 0},
		.featureEnable = 0x0c,
		 /*
		  * bit0 - enable tx temp comp - disabled
		  * bit1 - enable tx volt comp - disabled
		  * bit2 - enable fastClock - enabled
		  * bit3 - enable doubling - enabled
		  * bit4 - enable internal regulator - disabled
		  * bit5 - enable pa predistortion - disabled
		  */
		.miscConfiguration = 0, /* bit0 - turn down drivestrength */
		.eepromWriteEnableGpio = 3,
		.wlanDisableGpio = 0,
		.wlanLedGpio = 8,
		.rxBandSelectGpio = 0xff,
		.txrxgain = 0,
		.swreg = 0,
	 },
	.modalHeader2G = {
	/* ar9300_modal_eep_header  2g */
		/* 4 idle,t1,t2,b(4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
		.antCtrlCommon2 = LE32(0x22222),

		/*
		 * antCtrlChain[AR9300_MAX_CHAINS]; 6 idle, t, r,
		 * rx1, rx12, b (2 bits each)
		 */
		.antCtrlChain = { LE16(0x150), LE16(0x150), LE16(0x150) },

		/*
		 * xatten1DB[AR9300_MAX_CHAINS];  3 xatten1_db
		 * for ar9280 (0xa20c/b20c 5:0)
		 */
		.xatten1DB = {0, 0, 0},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for ar9280 (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0, 0, 0},
		.tempSlope = 36,
		.voltSlope = 0,

		/*
		 * spurChans[OSPREY_EEPROM_MODAL_SPURS]; spur
		 * channels in usual fbin coding format
		 */
		.spurChans = {0, 0, 0, 0, 0},

		/*
		 * noiseFloorThreshCh[AR9300_MAX_CHAINS]; 3 Check
		 * if the register is per chain
		 */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2c,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0cf0e0e0),
		.papdRateMaskHt40 = LE32(0x6cf0e0e0),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	 },
	.base_ext1 = {
		.ant_div_control = 0,
		.future = {0, 0},
		.tempslopextension = {0, 0, 0, 0, 0, 0, 0, 0}
	},
	.calFreqPier2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1),
	 },
	/* ar9300_cal_data_per_freq_op_loop 2g */
	.calPierData2G = {
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
	 },
	.calTarget_freqbin_Cck = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2484, 1),
	 },
	.calTarget_freqbin_2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTarget_freqbin_2GHT20 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTarget_freqbin_2GHT40 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTargetPowerCck = {
		 /* 1L-5L,5S,11L,11S */
		 { {36, 36, 36, 36} },
		 { {36, 36, 36, 36} },
	},
	.calTargetPower2G = {
		 /* 6-24,36,48,54 */
		 { {32, 32, 28, 24} },
		 { {32, 32, 28, 24} },
		 { {32, 32, 28, 24} },
	},
	.calTargetPower2GHT20 = {
		{ {32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20} },
		{ {32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20} },
		{ {32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20} },
	},
	.calTargetPower2GHT40 = {
		{ {32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20} },
		{ {32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20} },
		{ {32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20} },
	},
	.ctlIndex_2G =  {
		0x11, 0x12, 0x15, 0x17, 0x41, 0x42,
		0x45, 0x47, 0x31, 0x32, 0x35, 0x37,
	},
	.ctl_freqbin_2G = {
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2457, 1),
			FREQ2FBIN(2462, 1)
		},
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},

		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},
		{
			FREQ2FBIN(2422, 1),
			FREQ2FBIN(2427, 1),
			FREQ2FBIN(2447, 1),
			FREQ2FBIN(2452, 1)
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(2484, 1),
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
		},

		{
			/* Data[9].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[9].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[9].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[10].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[10].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[10].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[11].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[11].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[11].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[11].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		}
	 },
	.ctlPowerData_2G = {
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1) } },

		 { { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
	 },
	.modalHeader5G = {
		/* 4 idle,t1,t2,b (4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2,ra2l2,ra12 */
		.antCtrlCommon2 = LE32(0x22222),
		 /* antCtrlChain 6 idle, t,r,rx1,rx12,b (2 bits each) */
		.antCtrlChain = {
			LE16(0x000), LE16(0x000), LE16(0x000),
		},
		 /* xatten1DB 3 xatten1_db for AR9280 (0xa20c/b20c 5:0) */
		.xatten1DB = {0, 0, 0},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for merlin (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0, 0, 0},
		.tempSlope = 68,
		.voltSlope = 0,
		/* spurChans spur channels in usual fbin coding format */
		.spurChans = {0, 0, 0, 0, 0},
		/* noiseFloorThreshCh Check if the register is per chain */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2d,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0c80c080),
		.papdRateMaskHt40 = LE32(0x0080c080),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	 },
	.base_ext2 = {
		.tempSlopeLow = 0,
		.tempSlopeHigh = 0,
		.xatten1DBLow = {0, 0, 0},
		.xatten1MarginLow = {0, 0, 0},
		.xatten1DBHigh = {0, 0, 0},
		.xatten1MarginHigh = {0, 0, 0}
	},
	.calFreqPier5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	},
	.calPierData5G = {
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},

	},
	.calTarget_freqbin_5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT20 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT40 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	 },
	.calTargetPower5G = {
		/* 6-24,36,48,54 */
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
		{ {20, 20, 20, 10} },
	 },
	.calTargetPower5GHT20 = {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
	 },
	.calTargetPower5GHT40 =  {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
		{ {20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0} },
	 },
	.ctlIndex_5G =  {
		0x10, 0x16, 0x18, 0x40, 0x46,
		0x48, 0x30, 0x36, 0x38
	},
	.ctl_freqbin_5G =  {
		{
			/* Data[0].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[0].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[0].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[0].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[0].ctlEdges[4].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[0].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[0].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[0].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},
		{
			/* Data[1].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[1].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[1].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[1].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[1].ctlEdges[4].bChannel */ FREQ2FBIN(5520, 0),
			/* Data[1].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[1].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[1].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[2].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[2].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[2].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[2].ctlEdges[3].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[2].ctlEdges[4].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[2].ctlEdges[5].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[2].ctlEdges[6].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[2].ctlEdges[7].bChannel */ FREQ2FBIN(5755, 0)
		},

		{
			/* Data[3].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[3].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[3].ctlEdges[2].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[3].ctlEdges[3].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[3].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[3].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[3].ctlEdges[6].bChannel */ 0xFF,
			/* Data[3].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[4].ctlEdges[4].bChannel */ 0xFF,
			/* Data[4].ctlEdges[5].bChannel */ 0xFF,
			/* Data[4].ctlEdges[6].bChannel */ 0xFF,
			/* Data[4].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[5].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[5].ctlEdges[4].bChannel */ FREQ2FBIN(5590, 0),
			/* Data[5].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[5].ctlEdges[6].bChannel */ 0xFF,
			/* Data[5].ctlEdges[7].bChannel */ 0xFF
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[6].ctlEdges[2].bChannel */ FREQ2FBIN(5220, 0),
			/* Data[6].ctlEdges[3].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[6].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[6].ctlEdges[5].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[6].ctlEdges[6].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[6].ctlEdges[7].bChannel */ FREQ2FBIN(5745, 0)
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[7].ctlEdges[4].bChannel */ FREQ2FBIN(5560, 0),
			/* Data[7].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[7].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[7].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[8].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[8].ctlEdges[4].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[8].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[8].ctlEdges[6].bChannel */ FREQ2FBIN(5755, 0),
			/* Data[8].ctlEdges[7].bChannel */ FREQ2FBIN(5795, 0)
		}
	 },
	.ctlPowerData_5G = {
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
			}
		},
	 }
};

static const struct ar9300_eeprom ar9300_x113 = {
	.eepromVersion = 2,
	.templateVersion = 6,
	.macAddr = {0x00, 0x03, 0x7f, 0x0, 0x0, 0x0},
	.custData = {"x113-023-f0000"},
	.baseEepHeader = {
		.regDmn = { LE16(0), LE16(0x1f) },
		.txrxMask =  0x77, /* 4 bits tx and 4 bits rx */
		.opCapFlags = {
			.opFlags = AR5416_OPFLAGS_11A,
			.eepMisc = AR9300_EEPMISC_LITTLE_ENDIAN,
		},
		.rfSilent = 0,
		.blueToothOptions = 0,
		.deviceCap = 0,
		.deviceType = 5, /* takes lower byte in eeprom location */
		.pwrTableOffset = AR9300_PWR_TABLE_OFFSET,
		.params_for_tuning_caps = {0, 0},
		.featureEnable = 0x0d,
		 /*
		  * bit0 - enable tx temp comp - disabled
		  * bit1 - enable tx volt comp - disabled
		  * bit2 - enable fastClock - enabled
		  * bit3 - enable doubling - enabled
		  * bit4 - enable internal regulator - disabled
		  * bit5 - enable pa predistortion - disabled
		  */
		.miscConfiguration = 0, /* bit0 - turn down drivestrength */
		.eepromWriteEnableGpio = 6,
		.wlanDisableGpio = 0,
		.wlanLedGpio = 8,
		.rxBandSelectGpio = 0xff,
		.txrxgain = 0x21,
		.swreg = 0,
	 },
	.modalHeader2G = {
	/* ar9300_modal_eep_header  2g */
		/* 4 idle,t1,t2,b(4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
		.antCtrlCommon2 = LE32(0x44444),

		/*
		 * antCtrlChain[AR9300_MAX_CHAINS]; 6 idle, t, r,
		 * rx1, rx12, b (2 bits each)
		 */
		.antCtrlChain = { LE16(0x150), LE16(0x150), LE16(0x150) },

		/*
		 * xatten1DB[AR9300_MAX_CHAINS];  3 xatten1_db
		 * for ar9280 (0xa20c/b20c 5:0)
		 */
		.xatten1DB = {0, 0, 0},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for ar9280 (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0, 0, 0},
		.tempSlope = 25,
		.voltSlope = 0,

		/*
		 * spurChans[OSPREY_EEPROM_MODAL_SPURS]; spur
		 * channels in usual fbin coding format
		 */
		.spurChans = {FREQ2FBIN(2464, 1), 0, 0, 0, 0},

		/*
		 * noiseFloorThreshCh[AR9300_MAX_CHAINS]; 3 Check
		 * if the register is per chain
		 */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2c,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0c80c080),
		.papdRateMaskHt40 = LE32(0x0080c080),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	 },
	 .base_ext1 = {
		.ant_div_control = 0,
		.future = {0, 0},
		.tempslopextension = {0, 0, 0, 0, 0, 0, 0, 0}
	 },
	.calFreqPier2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1),
	 },
	/* ar9300_cal_data_per_freq_op_loop 2g */
	.calPierData2G = {
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
	 },
	.calTarget_freqbin_Cck = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2472, 1),
	 },
	.calTarget_freqbin_2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTarget_freqbin_2GHT20 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTarget_freqbin_2GHT40 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTargetPowerCck = {
		 /* 1L-5L,5S,11L,11S */
		 { {34, 34, 34, 34} },
		 { {34, 34, 34, 34} },
	},
	.calTargetPower2G = {
		 /* 6-24,36,48,54 */
		 { {34, 34, 32, 32} },
		 { {34, 34, 32, 32} },
		 { {34, 34, 32, 32} },
	},
	.calTargetPower2GHT20 = {
		{ {32, 32, 32, 32, 32, 28, 32, 32, 30, 28, 0, 0, 0, 0} },
		{ {32, 32, 32, 32, 32, 28, 32, 32, 30, 28, 0, 0, 0, 0} },
		{ {32, 32, 32, 32, 32, 28, 32, 32, 30, 28, 0, 0, 0, 0} },
	},
	.calTargetPower2GHT40 = {
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0} },
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0} },
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0} },
	},
	.ctlIndex_2G =  {
		0x11, 0x12, 0x15, 0x17, 0x41, 0x42,
		0x45, 0x47, 0x31, 0x32, 0x35, 0x37,
	},
	.ctl_freqbin_2G = {
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2457, 1),
			FREQ2FBIN(2462, 1)
		},
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},

		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},
		{
			FREQ2FBIN(2422, 1),
			FREQ2FBIN(2427, 1),
			FREQ2FBIN(2447, 1),
			FREQ2FBIN(2452, 1)
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(2484, 1),
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
		},

		{
			/* Data[9].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[9].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[9].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[10].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[10].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[10].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[11].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[11].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[11].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[11].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		}
	 },
	.ctlPowerData_2G = {
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1) } },

		 { { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
	 },
	.modalHeader5G = {
		/* 4 idle,t1,t2,b (4 bits per setting) */
		.antCtrlCommon = LE32(0x220),
		/* 4 ra1l1, ra2l1, ra1l2,ra2l2,ra12 */
		.antCtrlCommon2 = LE32(0x11111),
		 /* antCtrlChain 6 idle, t,r,rx1,rx12,b (2 bits each) */
		.antCtrlChain = {
			LE16(0x150), LE16(0x150), LE16(0x150),
		},
		 /* xatten1DB 3 xatten1_db for AR9280 (0xa20c/b20c 5:0) */
		.xatten1DB = {0, 0, 0},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for merlin (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0, 0, 0},
		.tempSlope = 68,
		.voltSlope = 0,
		/* spurChans spur channels in usual fbin coding format */
		.spurChans = {FREQ2FBIN(5500, 0), 0, 0, 0, 0},
		/* noiseFloorThreshCh Check if the register is per chain */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0xf,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2d,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0cf0e0e0),
		.papdRateMaskHt40 = LE32(0x6cf0e0e0),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	 },
	.base_ext2 = {
		.tempSlopeLow = 72,
		.tempSlopeHigh = 105,
		.xatten1DBLow = {0, 0, 0},
		.xatten1MarginLow = {0, 0, 0},
		.xatten1DBHigh = {0, 0, 0},
		.xatten1MarginHigh = {0, 0, 0}
	 },
	.calFreqPier5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5785, 0)
	},
	.calPierData5G = {
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},

	},
	.calTarget_freqbin_5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5785, 0)
	},
	.calTarget_freqbin_5GHT20 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT40 = {
		FREQ2FBIN(5190, 0),
		FREQ2FBIN(5230, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5410, 0),
		FREQ2FBIN(5510, 0),
		FREQ2FBIN(5670, 0),
		FREQ2FBIN(5755, 0),
		FREQ2FBIN(5825, 0)
	 },
	.calTargetPower5G = {
		/* 6-24,36,48,54 */
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
		{ {42, 40, 40, 34} },
	 },
	.calTargetPower5GHT20 = {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20} },
		{ {40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20} },
		{ {40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20} },
		{ {40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20} },
		{ {40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20} },
		{ {40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20} },
		{ {38, 38, 38, 38, 32, 28, 38, 38, 32, 28, 38, 38, 32, 26} },
		{ {36, 36, 36, 36, 32, 28, 36, 36, 32, 28, 36, 36, 32, 26} },
	 },
	.calTargetPower5GHT40 =  {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24} },
		{ {40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24} },
		{ {40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24} },
		{ {40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24} },
		{ {40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24} },
		{ {40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24} },
		{ {36, 36, 36, 36, 30, 26, 36, 36, 30, 26, 36, 36, 30, 24} },
		{ {34, 34, 34, 34, 30, 26, 34, 34, 30, 26, 34, 34, 30, 24} },
	 },
	.ctlIndex_5G =  {
		0x10, 0x16, 0x18, 0x40, 0x46,
		0x48, 0x30, 0x36, 0x38
	},
	.ctl_freqbin_5G =  {
		{
			/* Data[0].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[0].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[0].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[0].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[0].ctlEdges[4].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[0].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[0].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[0].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},
		{
			/* Data[1].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[1].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[1].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[1].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[1].ctlEdges[4].bChannel */ FREQ2FBIN(5520, 0),
			/* Data[1].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[1].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[1].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[2].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[2].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[2].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[2].ctlEdges[3].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[2].ctlEdges[4].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[2].ctlEdges[5].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[2].ctlEdges[6].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[2].ctlEdges[7].bChannel */ FREQ2FBIN(5755, 0)
		},

		{
			/* Data[3].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[3].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[3].ctlEdges[2].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[3].ctlEdges[3].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[3].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[3].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[3].ctlEdges[6].bChannel */ 0xFF,
			/* Data[3].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[4].ctlEdges[4].bChannel */ 0xFF,
			/* Data[4].ctlEdges[5].bChannel */ 0xFF,
			/* Data[4].ctlEdges[6].bChannel */ 0xFF,
			/* Data[4].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[5].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[5].ctlEdges[4].bChannel */ FREQ2FBIN(5590, 0),
			/* Data[5].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[5].ctlEdges[6].bChannel */ 0xFF,
			/* Data[5].ctlEdges[7].bChannel */ 0xFF
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[6].ctlEdges[2].bChannel */ FREQ2FBIN(5220, 0),
			/* Data[6].ctlEdges[3].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[6].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[6].ctlEdges[5].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[6].ctlEdges[6].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[6].ctlEdges[7].bChannel */ FREQ2FBIN(5745, 0)
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[7].ctlEdges[4].bChannel */ FREQ2FBIN(5560, 0),
			/* Data[7].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[7].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[7].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[8].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[8].ctlEdges[4].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[8].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[8].ctlEdges[6].bChannel */ FREQ2FBIN(5755, 0),
			/* Data[8].ctlEdges[7].bChannel */ FREQ2FBIN(5795, 0)
		}
	 },
	.ctlPowerData_5G = {
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
			}
		},
	 }
};


static const struct ar9300_eeprom ar9300_h112 = {
	.eepromVersion = 2,
	.templateVersion = 3,
	.macAddr = {0x00, 0x03, 0x7f, 0x0, 0x0, 0x0},
	.custData = {"h112-241-f0000"},
	.baseEepHeader = {
		.regDmn = { LE16(0), LE16(0x1f) },
		.txrxMask =  0x77, /* 4 bits tx and 4 bits rx */
		.opCapFlags = {
			.opFlags = AR5416_OPFLAGS_11G | AR5416_OPFLAGS_11A,
			.eepMisc = AR9300_EEPMISC_LITTLE_ENDIAN,
		},
		.rfSilent = 0,
		.blueToothOptions = 0,
		.deviceCap = 0,
		.deviceType = 5, /* takes lower byte in eeprom location */
		.pwrTableOffset = AR9300_PWR_TABLE_OFFSET,
		.params_for_tuning_caps = {0, 0},
		.featureEnable = 0x0d,
		/*
		 * bit0 - enable tx temp comp - disabled
		 * bit1 - enable tx volt comp - disabled
		 * bit2 - enable fastClock - enabled
		 * bit3 - enable doubling - enabled
		 * bit4 - enable internal regulator - disabled
		 * bit5 - enable pa predistortion - disabled
		 */
		.miscConfiguration = 0, /* bit0 - turn down drivestrength */
		.eepromWriteEnableGpio = 6,
		.wlanDisableGpio = 0,
		.wlanLedGpio = 8,
		.rxBandSelectGpio = 0xff,
		.txrxgain = 0x10,
		.swreg = 0,
	},
	.modalHeader2G = {
		/* ar9300_modal_eep_header  2g */
		/* 4 idle,t1,t2,b(4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
		.antCtrlCommon2 = LE32(0x44444),

		/*
		 * antCtrlChain[AR9300_MAX_CHAINS]; 6 idle, t, r,
		 * rx1, rx12, b (2 bits each)
		 */
		.antCtrlChain = { LE16(0x150), LE16(0x150), LE16(0x150) },

		/*
		 * xatten1DB[AR9300_MAX_CHAINS];  3 xatten1_db
		 * for ar9280 (0xa20c/b20c 5:0)
		 */
		.xatten1DB = {0, 0, 0},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for ar9280 (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0, 0, 0},
		.tempSlope = 25,
		.voltSlope = 0,

		/*
		 * spurChans[OSPREY_EEPROM_MODAL_SPURS]; spur
		 * channels in usual fbin coding format
		 */
		.spurChans = {FREQ2FBIN(2464, 1), 0, 0, 0, 0},

		/*
		 * noiseFloorThreshCh[AR9300_MAX_CHAINS]; 3 Check
		 * if the register is per chain
		 */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2c,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0c80c080),
		.papdRateMaskHt40 = LE32(0x0080c080),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	},
	.base_ext1 = {
		.ant_div_control = 0,
		.future = {0, 0},
		.tempslopextension = {0, 0, 0, 0, 0, 0, 0, 0}
	},
	.calFreqPier2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2462, 1),
	},
	/* ar9300_cal_data_per_freq_op_loop 2g */
	.calPierData2G = {
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
	},
	.calTarget_freqbin_Cck = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2472, 1),
	},
	.calTarget_freqbin_2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	},
	.calTarget_freqbin_2GHT20 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	},
	.calTarget_freqbin_2GHT40 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	},
	.calTargetPowerCck = {
		/* 1L-5L,5S,11L,11S */
		{ {34, 34, 34, 34} },
		{ {34, 34, 34, 34} },
	},
	.calTargetPower2G = {
		/* 6-24,36,48,54 */
		{ {34, 34, 32, 32} },
		{ {34, 34, 32, 32} },
		{ {34, 34, 32, 32} },
	},
	.calTargetPower2GHT20 = {
		{ {32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 28, 28, 28, 24} },
		{ {32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 28, 28, 28, 24} },
		{ {32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 28, 28, 28, 24} },
	},
	.calTargetPower2GHT40 = {
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 26, 26, 26, 22} },
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 26, 26, 26, 22} },
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 26, 26, 26, 22} },
	},
	.ctlIndex_2G =  {
		0x11, 0x12, 0x15, 0x17, 0x41, 0x42,
		0x45, 0x47, 0x31, 0x32, 0x35, 0x37,
	},
	.ctl_freqbin_2G = {
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2457, 1),
			FREQ2FBIN(2462, 1)
		},
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},

		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},
		{
			FREQ2FBIN(2422, 1),
			FREQ2FBIN(2427, 1),
			FREQ2FBIN(2447, 1),
			FREQ2FBIN(2452, 1)
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(2484, 1),
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
		},

		{
			/* Data[9].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[9].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[9].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[10].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[10].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[10].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[11].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[11].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[11].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[11].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		}
	},
	.ctlPowerData_2G = {
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1) } },

		{ { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		{ { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
	},
	.modalHeader5G = {
		/* 4 idle,t1,t2,b (4 bits per setting) */
		.antCtrlCommon = LE32(0x220),
		/* 4 ra1l1, ra2l1, ra1l2,ra2l2,ra12 */
		.antCtrlCommon2 = LE32(0x44444),
		/* antCtrlChain 6 idle, t,r,rx1,rx12,b (2 bits each) */
		.antCtrlChain = {
			LE16(0x150), LE16(0x150), LE16(0x150),
		},
		/* xatten1DB 3 xatten1_db for AR9280 (0xa20c/b20c 5:0) */
		.xatten1DB = {0, 0, 0},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for merlin (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0, 0, 0},
		.tempSlope = 45,
		.voltSlope = 0,
		/* spurChans spur channels in usual fbin coding format */
		.spurChans = {0, 0, 0, 0, 0},
		/* noiseFloorThreshCh Check if the register is per chain */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2d,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0cf0e0e0),
		.papdRateMaskHt40 = LE32(0x6cf0e0e0),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	},
	.base_ext2 = {
		.tempSlopeLow = 40,
		.tempSlopeHigh = 50,
		.xatten1DBLow = {0, 0, 0},
		.xatten1MarginLow = {0, 0, 0},
		.xatten1DBHigh = {0, 0, 0},
		.xatten1MarginHigh = {0, 0, 0}
	},
	.calFreqPier5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5785, 0)
	},
	.calPierData5G = {
		{
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
		},
		{
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
		},
		{
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
		},

	},
	.calTarget_freqbin_5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT20 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT40 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTargetPower5G = {
		/* 6-24,36,48,54 */
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
	},
	.calTargetPower5GHT20 = {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 20, 20, 20, 16} },
		{ {30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 20, 20, 20, 16} },
		{ {30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 18, 18, 18, 16} },
		{ {30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 18, 18, 18, 16} },
		{ {30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 16, 16, 16, 14} },
		{ {30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 16, 16, 16, 14} },
		{ {30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 14, 14, 14, 12} },
		{ {30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 14, 14, 14, 12} },
	},
	.calTargetPower5GHT40 =  {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 18, 18, 18, 14} },
		{ {28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 18, 18, 18, 14} },
		{ {28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 16, 16, 16, 12} },
		{ {28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 16, 16, 16, 12} },
		{ {28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 14, 14, 14, 10} },
		{ {28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 14, 14, 14, 10} },
		{ {28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 12, 12, 12, 8} },
		{ {28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 12, 12, 12, 8} },
	},
	.ctlIndex_5G =  {
		0x10, 0x16, 0x18, 0x40, 0x46,
		0x48, 0x30, 0x36, 0x38
	},
	.ctl_freqbin_5G =  {
		{
			/* Data[0].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[0].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[0].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[0].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[0].ctlEdges[4].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[0].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[0].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[0].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},
		{
			/* Data[1].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[1].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[1].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[1].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[1].ctlEdges[4].bChannel */ FREQ2FBIN(5520, 0),
			/* Data[1].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[1].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[1].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[2].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[2].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[2].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[2].ctlEdges[3].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[2].ctlEdges[4].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[2].ctlEdges[5].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[2].ctlEdges[6].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[2].ctlEdges[7].bChannel */ FREQ2FBIN(5755, 0)
		},

		{
			/* Data[3].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[3].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[3].ctlEdges[2].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[3].ctlEdges[3].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[3].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[3].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[3].ctlEdges[6].bChannel */ 0xFF,
			/* Data[3].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[4].ctlEdges[4].bChannel */ 0xFF,
			/* Data[4].ctlEdges[5].bChannel */ 0xFF,
			/* Data[4].ctlEdges[6].bChannel */ 0xFF,
			/* Data[4].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[5].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[5].ctlEdges[4].bChannel */ FREQ2FBIN(5590, 0),
			/* Data[5].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[5].ctlEdges[6].bChannel */ 0xFF,
			/* Data[5].ctlEdges[7].bChannel */ 0xFF
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[6].ctlEdges[2].bChannel */ FREQ2FBIN(5220, 0),
			/* Data[6].ctlEdges[3].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[6].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[6].ctlEdges[5].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[6].ctlEdges[6].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[6].ctlEdges[7].bChannel */ FREQ2FBIN(5745, 0)
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[7].ctlEdges[4].bChannel */ FREQ2FBIN(5560, 0),
			/* Data[7].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[7].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[7].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[8].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[8].ctlEdges[4].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[8].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[8].ctlEdges[6].bChannel */ FREQ2FBIN(5755, 0),
			/* Data[8].ctlEdges[7].bChannel */ FREQ2FBIN(5795, 0)
		}
	},
	.ctlPowerData_5G = {
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
			}
		},
	}
};


static const struct ar9300_eeprom ar9300_x112 = {
	.eepromVersion = 2,
	.templateVersion = 5,
	.macAddr = {0x00, 0x03, 0x7f, 0x0, 0x0, 0x0},
	.custData = {"x112-041-f0000"},
	.baseEepHeader = {
		.regDmn = { LE16(0), LE16(0x1f) },
		.txrxMask =  0x77, /* 4 bits tx and 4 bits rx */
		.opCapFlags = {
			.opFlags = AR5416_OPFLAGS_11G | AR5416_OPFLAGS_11A,
			.eepMisc = AR9300_EEPMISC_LITTLE_ENDIAN,
		},
		.rfSilent = 0,
		.blueToothOptions = 0,
		.deviceCap = 0,
		.deviceType = 5, /* takes lower byte in eeprom location */
		.pwrTableOffset = AR9300_PWR_TABLE_OFFSET,
		.params_for_tuning_caps = {0, 0},
		.featureEnable = 0x0d,
		/*
		 * bit0 - enable tx temp comp - disabled
		 * bit1 - enable tx volt comp - disabled
		 * bit2 - enable fastclock - enabled
		 * bit3 - enable doubling - enabled
		 * bit4 - enable internal regulator - disabled
		 * bit5 - enable pa predistortion - disabled
		 */
		.miscConfiguration = 0, /* bit0 - turn down drivestrength */
		.eepromWriteEnableGpio = 6,
		.wlanDisableGpio = 0,
		.wlanLedGpio = 8,
		.rxBandSelectGpio = 0xff,
		.txrxgain = 0x0,
		.swreg = 0,
	},
	.modalHeader2G = {
		/* ar9300_modal_eep_header  2g */
		/* 4 idle,t1,t2,b(4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
		.antCtrlCommon2 = LE32(0x22222),

		/*
		 * antCtrlChain[ar9300_max_chains]; 6 idle, t, r,
		 * rx1, rx12, b (2 bits each)
		 */
		.antCtrlChain = { LE16(0x10), LE16(0x10), LE16(0x10) },

		/*
		 * xatten1DB[AR9300_max_chains];  3 xatten1_db
		 * for ar9280 (0xa20c/b20c 5:0)
		 */
		.xatten1DB = {0x1b, 0x1b, 0x1b},

		/*
		 * xatten1Margin[ar9300_max_chains]; 3 xatten1_margin
		 * for ar9280 (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0x15, 0x15, 0x15},
		.tempSlope = 50,
		.voltSlope = 0,

		/*
		 * spurChans[OSPrey_eeprom_modal_sPURS]; spur
		 * channels in usual fbin coding format
		 */
		.spurChans = {FREQ2FBIN(2464, 1), 0, 0, 0, 0},

		/*
		 * noiseFloorThreshch[ar9300_max_cHAINS]; 3 Check
		 * if the register is per chain
		 */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2c,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0c80c080),
		.papdRateMaskHt40 = LE32(0x0080c080),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	},
	.base_ext1 = {
		.ant_div_control = 0,
		.future = {0, 0},
		.tempslopextension = {0, 0, 0, 0, 0, 0, 0, 0}
	},
	.calFreqPier2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1),
	},
	/* ar9300_cal_data_per_freq_op_loop 2g */
	.calPierData2G = {
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
	},
	.calTarget_freqbin_Cck = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2472, 1),
	},
	.calTarget_freqbin_2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	},
	.calTarget_freqbin_2GHT20 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	},
	.calTarget_freqbin_2GHT40 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	},
	.calTargetPowerCck = {
		/* 1L-5L,5S,11L,11s */
		{ {38, 38, 38, 38} },
		{ {38, 38, 38, 38} },
	},
	.calTargetPower2G = {
		/* 6-24,36,48,54 */
		{ {38, 38, 36, 34} },
		{ {38, 38, 36, 34} },
		{ {38, 38, 34, 32} },
	},
	.calTargetPower2GHT20 = {
		{ {36, 36, 36, 36, 36, 34, 34, 32, 30, 28, 28, 28, 28, 26} },
		{ {36, 36, 36, 36, 36, 34, 36, 34, 32, 30, 30, 30, 28, 26} },
		{ {36, 36, 36, 36, 36, 34, 34, 32, 30, 28, 28, 28, 28, 26} },
	},
	.calTargetPower2GHT40 = {
		{ {36, 36, 36, 36, 34, 32, 32, 30, 28, 26, 26, 26, 26, 24} },
		{ {36, 36, 36, 36, 34, 32, 34, 32, 30, 28, 28, 28, 28, 24} },
		{ {36, 36, 36, 36, 34, 32, 32, 30, 28, 26, 26, 26, 26, 24} },
	},
	.ctlIndex_2G =  {
		0x11, 0x12, 0x15, 0x17, 0x41, 0x42,
		0x45, 0x47, 0x31, 0x32, 0x35, 0x37,
	},
	.ctl_freqbin_2G = {
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2457, 1),
			FREQ2FBIN(2462, 1)
		},
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},

		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},
		{
			FREQ2FBIN(2422, 1),
			FREQ2FBIN(2427, 1),
			FREQ2FBIN(2447, 1),
			FREQ2FBIN(2452, 1)
		},

		{
			/* Data[4].ctledges[0].bchannel */ FREQ2FBIN(2412, 1),
			/* Data[4].ctledges[1].bchannel */ FREQ2FBIN(2417, 1),
			/* Data[4].ctledges[2].bchannel */ FREQ2FBIN(2472, 1),
			/* Data[4].ctledges[3].bchannel */ FREQ2FBIN(2484, 1),
		},

		{
			/* Data[5].ctledges[0].bchannel */ FREQ2FBIN(2412, 1),
			/* Data[5].ctledges[1].bchannel */ FREQ2FBIN(2417, 1),
			/* Data[5].ctledges[2].bchannel */ FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[6].ctledges[0].bchannel */ FREQ2FBIN(2412, 1),
			/* Data[6].ctledges[1].bchannel */ FREQ2FBIN(2417, 1),
			FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[7].ctledges[0].bchannel */ FREQ2FBIN(2422, 1),
			/* Data[7].ctledges[1].bchannel */ FREQ2FBIN(2427, 1),
			/* Data[7].ctledges[2].bchannel */ FREQ2FBIN(2447, 1),
			/* Data[7].ctledges[3].bchannel */ FREQ2FBIN(2462, 1),
		},

		{
			/* Data[8].ctledges[0].bchannel */ FREQ2FBIN(2412, 1),
			/* Data[8].ctledges[1].bchannel */ FREQ2FBIN(2417, 1),
			/* Data[8].ctledges[2].bchannel */ FREQ2FBIN(2472, 1),
		},

		{
			/* Data[9].ctledges[0].bchannel */ FREQ2FBIN(2412, 1),
			/* Data[9].ctledges[1].bchannel */ FREQ2FBIN(2417, 1),
			/* Data[9].ctledges[2].bchannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[10].ctledges[0].bchannel */ FREQ2FBIN(2412, 1),
			/* Data[10].ctledges[1].bchannel */ FREQ2FBIN(2417, 1),
			/* Data[10].ctledges[2].bchannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[11].ctledges[0].bchannel */ FREQ2FBIN(2422, 1),
			/* Data[11].ctledges[1].bchannel */ FREQ2FBIN(2427, 1),
			/* Data[11].ctledges[2].bchannel */ FREQ2FBIN(2447, 1),
			/* Data[11].ctledges[3].bchannel */ FREQ2FBIN(2462, 1),
		}
	},
	.ctlPowerData_2G = {
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1) } },

		{ { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		{ { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		{ { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
		{ { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
	},
	.modalHeader5G = {
		/* 4 idle,t1,t2,b (4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2,ra2l2,ra12 */
		.antCtrlCommon2 = LE32(0x22222),
		/* antCtrlChain 6 idle, t,r,rx1,rx12,b (2 bits each) */
		.antCtrlChain = {
			LE16(0x0), LE16(0x0), LE16(0x0),
		},
		/* xatten1DB 3 xatten1_db for ar9280 (0xa20c/b20c 5:0) */
		.xatten1DB = {0x13, 0x19, 0x17},

		/*
		 * xatten1Margin[ar9300_max_chains]; 3 xatten1_margin
		 * for merlin (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0x19, 0x19, 0x19},
		.tempSlope = 70,
		.voltSlope = 15,
		/* spurChans spur channels in usual fbin coding format */
		.spurChans = {0, 0, 0, 0, 0},
		/* noiseFloorThreshch check if the register is per chain */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2d,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0cf0e0e0),
		.papdRateMaskHt40 = LE32(0x6cf0e0e0),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	},
	.base_ext2 = {
		.tempSlopeLow = 72,
		.tempSlopeHigh = 105,
		.xatten1DBLow = {0x10, 0x14, 0x10},
		.xatten1MarginLow = {0x19, 0x19 , 0x19},
		.xatten1DBHigh = {0x1d, 0x20, 0x24},
		.xatten1MarginHigh = {0x10, 0x10, 0x10}
	},
	.calFreqPier5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5785, 0)
	},
	.calPierData5G = {
		{
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
		},
		{
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
		},
		{
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
			{0, 0, 0, 0, 0},
		},

	},
	.calTarget_freqbin_5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT20 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT40 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5725, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTargetPower5G = {
		/* 6-24,36,48,54 */
		{ {32, 32, 28, 26} },
		{ {32, 32, 28, 26} },
		{ {32, 32, 28, 26} },
		{ {32, 32, 26, 24} },
		{ {32, 32, 26, 24} },
		{ {32, 32, 24, 22} },
		{ {30, 30, 24, 22} },
		{ {30, 30, 24, 22} },
	},
	.calTargetPower5GHT20 = {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {32, 32, 32, 32, 28, 26, 32, 28, 26, 24, 24, 24, 22, 22} },
		{ {32, 32, 32, 32, 28, 26, 32, 28, 26, 24, 24, 24, 22, 22} },
		{ {32, 32, 32, 32, 28, 26, 32, 28, 26, 24, 24, 24, 22, 22} },
		{ {32, 32, 32, 32, 28, 26, 32, 26, 24, 22, 22, 22, 20, 20} },
		{ {32, 32, 32, 32, 28, 26, 32, 26, 24, 22, 20, 18, 16, 16} },
		{ {32, 32, 32, 32, 28, 26, 32, 24, 20, 16, 18, 16, 14, 14} },
		{ {30, 30, 30, 30, 28, 26, 30, 24, 20, 16, 18, 16, 14, 14} },
		{ {30, 30, 30, 30, 28, 26, 30, 24, 20, 16, 18, 16, 14, 14} },
	},
	.calTargetPower5GHT40 =  {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {32, 32, 32, 30, 28, 26, 30, 28, 26, 24, 24, 24, 22, 22} },
		{ {32, 32, 32, 30, 28, 26, 30, 28, 26, 24, 24, 24, 22, 22} },
		{ {32, 32, 32, 30, 28, 26, 30, 28, 26, 24, 24, 24, 22, 22} },
		{ {32, 32, 32, 30, 28, 26, 30, 26, 24, 22, 22, 22, 20, 20} },
		{ {32, 32, 32, 30, 28, 26, 30, 26, 24, 22, 20, 18, 16, 16} },
		{ {32, 32, 32, 30, 28, 26, 30, 22, 20, 16, 18, 16, 14, 14} },
		{ {30, 30, 30, 30, 28, 26, 30, 22, 20, 16, 18, 16, 14, 14} },
		{ {30, 30, 30, 30, 28, 26, 30, 22, 20, 16, 18, 16, 14, 14} },
	},
	.ctlIndex_5G =  {
		0x10, 0x16, 0x18, 0x40, 0x46,
		0x48, 0x30, 0x36, 0x38
	},
	.ctl_freqbin_5G =  {
		{
			/* Data[0].ctledges[0].bchannel */ FREQ2FBIN(5180, 0),
			/* Data[0].ctledges[1].bchannel */ FREQ2FBIN(5260, 0),
			/* Data[0].ctledges[2].bchannel */ FREQ2FBIN(5280, 0),
			/* Data[0].ctledges[3].bchannel */ FREQ2FBIN(5500, 0),
			/* Data[0].ctledges[4].bchannel */ FREQ2FBIN(5600, 0),
			/* Data[0].ctledges[5].bchannel */ FREQ2FBIN(5700, 0),
			/* Data[0].ctledges[6].bchannel */ FREQ2FBIN(5745, 0),
			/* Data[0].ctledges[7].bchannel */ FREQ2FBIN(5825, 0)
		},
		{
			/* Data[1].ctledges[0].bchannel */ FREQ2FBIN(5180, 0),
			/* Data[1].ctledges[1].bchannel */ FREQ2FBIN(5260, 0),
			/* Data[1].ctledges[2].bchannel */ FREQ2FBIN(5280, 0),
			/* Data[1].ctledges[3].bchannel */ FREQ2FBIN(5500, 0),
			/* Data[1].ctledges[4].bchannel */ FREQ2FBIN(5520, 0),
			/* Data[1].ctledges[5].bchannel */ FREQ2FBIN(5700, 0),
			/* Data[1].ctledges[6].bchannel */ FREQ2FBIN(5745, 0),
			/* Data[1].ctledges[7].bchannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[2].ctledges[0].bchannel */ FREQ2FBIN(5190, 0),
			/* Data[2].ctledges[1].bchannel */ FREQ2FBIN(5230, 0),
			/* Data[2].ctledges[2].bchannel */ FREQ2FBIN(5270, 0),
			/* Data[2].ctledges[3].bchannel */ FREQ2FBIN(5310, 0),
			/* Data[2].ctledges[4].bchannel */ FREQ2FBIN(5510, 0),
			/* Data[2].ctledges[5].bchannel */ FREQ2FBIN(5550, 0),
			/* Data[2].ctledges[6].bchannel */ FREQ2FBIN(5670, 0),
			/* Data[2].ctledges[7].bchannel */ FREQ2FBIN(5755, 0)
		},

		{
			/* Data[3].ctledges[0].bchannel */ FREQ2FBIN(5180, 0),
			/* Data[3].ctledges[1].bchannel */ FREQ2FBIN(5200, 0),
			/* Data[3].ctledges[2].bchannel */ FREQ2FBIN(5260, 0),
			/* Data[3].ctledges[3].bchannel */ FREQ2FBIN(5320, 0),
			/* Data[3].ctledges[4].bchannel */ FREQ2FBIN(5500, 0),
			/* Data[3].ctledges[5].bchannel */ FREQ2FBIN(5700, 0),
			/* Data[3].ctledges[6].bchannel */ 0xFF,
			/* Data[3].ctledges[7].bchannel */ 0xFF,
		},

		{
			/* Data[4].ctledges[0].bchannel */ FREQ2FBIN(5180, 0),
			/* Data[4].ctledges[1].bchannel */ FREQ2FBIN(5260, 0),
			/* Data[4].ctledges[2].bchannel */ FREQ2FBIN(5500, 0),
			/* Data[4].ctledges[3].bchannel */ FREQ2FBIN(5700, 0),
			/* Data[4].ctledges[4].bchannel */ 0xFF,
			/* Data[4].ctledges[5].bchannel */ 0xFF,
			/* Data[4].ctledges[6].bchannel */ 0xFF,
			/* Data[4].ctledges[7].bchannel */ 0xFF,
		},

		{
			/* Data[5].ctledges[0].bchannel */ FREQ2FBIN(5190, 0),
			/* Data[5].ctledges[1].bchannel */ FREQ2FBIN(5270, 0),
			/* Data[5].ctledges[2].bchannel */ FREQ2FBIN(5310, 0),
			/* Data[5].ctledges[3].bchannel */ FREQ2FBIN(5510, 0),
			/* Data[5].ctledges[4].bchannel */ FREQ2FBIN(5590, 0),
			/* Data[5].ctledges[5].bchannel */ FREQ2FBIN(5670, 0),
			/* Data[5].ctledges[6].bchannel */ 0xFF,
			/* Data[5].ctledges[7].bchannel */ 0xFF
		},

		{
			/* Data[6].ctledges[0].bchannel */ FREQ2FBIN(5180, 0),
			/* Data[6].ctledges[1].bchannel */ FREQ2FBIN(5200, 0),
			/* Data[6].ctledges[2].bchannel */ FREQ2FBIN(5220, 0),
			/* Data[6].ctledges[3].bchannel */ FREQ2FBIN(5260, 0),
			/* Data[6].ctledges[4].bchannel */ FREQ2FBIN(5500, 0),
			/* Data[6].ctledges[5].bchannel */ FREQ2FBIN(5600, 0),
			/* Data[6].ctledges[6].bchannel */ FREQ2FBIN(5700, 0),
			/* Data[6].ctledges[7].bchannel */ FREQ2FBIN(5745, 0)
		},

		{
			/* Data[7].ctledges[0].bchannel */ FREQ2FBIN(5180, 0),
			/* Data[7].ctledges[1].bchannel */ FREQ2FBIN(5260, 0),
			/* Data[7].ctledges[2].bchannel */ FREQ2FBIN(5320, 0),
			/* Data[7].ctledges[3].bchannel */ FREQ2FBIN(5500, 0),
			/* Data[7].ctledges[4].bchannel */ FREQ2FBIN(5560, 0),
			/* Data[7].ctledges[5].bchannel */ FREQ2FBIN(5700, 0),
			/* Data[7].ctledges[6].bchannel */ FREQ2FBIN(5745, 0),
			/* Data[7].ctledges[7].bchannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[8].ctledges[0].bchannel */ FREQ2FBIN(5190, 0),
			/* Data[8].ctledges[1].bchannel */ FREQ2FBIN(5230, 0),
			/* Data[8].ctledges[2].bchannel */ FREQ2FBIN(5270, 0),
			/* Data[8].ctledges[3].bchannel */ FREQ2FBIN(5510, 0),
			/* Data[8].ctledges[4].bchannel */ FREQ2FBIN(5550, 0),
			/* Data[8].ctledges[5].bchannel */ FREQ2FBIN(5670, 0),
			/* Data[8].ctledges[6].bchannel */ FREQ2FBIN(5755, 0),
			/* Data[8].ctledges[7].bchannel */ FREQ2FBIN(5795, 0)
		}
	},
	.ctlPowerData_5G = {
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
			}
		},
	}
};

static const struct ar9300_eeprom ar9300_h116 = {
	.eepromVersion = 2,
	.templateVersion = 4,
	.macAddr = {0x00, 0x03, 0x7f, 0x0, 0x0, 0x0},
	.custData = {"h116-041-f0000"},
	.baseEepHeader = {
		.regDmn = { LE16(0), LE16(0x1f) },
		.txrxMask =  0x33, /* 4 bits tx and 4 bits rx */
		.opCapFlags = {
			.opFlags = AR5416_OPFLAGS_11G | AR5416_OPFLAGS_11A,
			.eepMisc = AR9300_EEPMISC_LITTLE_ENDIAN,
		},
		.rfSilent = 0,
		.blueToothOptions = 0,
		.deviceCap = 0,
		.deviceType = 5, /* takes lower byte in eeprom location */
		.pwrTableOffset = AR9300_PWR_TABLE_OFFSET,
		.params_for_tuning_caps = {0, 0},
		.featureEnable = 0x0d,
		 /*
		  * bit0 - enable tx temp comp - disabled
		  * bit1 - enable tx volt comp - disabled
		  * bit2 - enable fastClock - enabled
		  * bit3 - enable doubling - enabled
		  * bit4 - enable internal regulator - disabled
		  * bit5 - enable pa predistortion - disabled
		  */
		.miscConfiguration = 0, /* bit0 - turn down drivestrength */
		.eepromWriteEnableGpio = 6,
		.wlanDisableGpio = 0,
		.wlanLedGpio = 8,
		.rxBandSelectGpio = 0xff,
		.txrxgain = 0x10,
		.swreg = 0,
	 },
	.modalHeader2G = {
	/* ar9300_modal_eep_header  2g */
		/* 4 idle,t1,t2,b(4 bits per setting) */
		.antCtrlCommon = LE32(0x110),
		/* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
		.antCtrlCommon2 = LE32(0x44444),

		/*
		 * antCtrlChain[AR9300_MAX_CHAINS]; 6 idle, t, r,
		 * rx1, rx12, b (2 bits each)
		 */
		.antCtrlChain = { LE16(0x10), LE16(0x10), LE16(0x10) },

		/*
		 * xatten1DB[AR9300_MAX_CHAINS];  3 xatten1_db
		 * for ar9280 (0xa20c/b20c 5:0)
		 */
		.xatten1DB = {0x1f, 0x1f, 0x1f},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for ar9280 (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0x12, 0x12, 0x12},
		.tempSlope = 25,
		.voltSlope = 0,

		/*
		 * spurChans[OSPREY_EEPROM_MODAL_SPURS]; spur
		 * channels in usual fbin coding format
		 */
		.spurChans = {FREQ2FBIN(2464, 1), 0, 0, 0, 0},

		/*
		 * noiseFloorThreshCh[AR9300_MAX_CHAINS]; 3 Check
		 * if the register is per chain
		 */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2c,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0c80C080),
		.papdRateMaskHt40 = LE32(0x0080C080),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	 },
	 .base_ext1 = {
		.ant_div_control = 0,
		.future = {0, 0},
		.tempslopextension = {0, 0, 0, 0, 0, 0, 0, 0}
	 },
	.calFreqPier2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2462, 1),
	 },
	/* ar9300_cal_data_per_freq_op_loop 2g */
	.calPierData2G = {
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
		{ {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0} },
	 },
	.calTarget_freqbin_Cck = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2472, 1),
	 },
	.calTarget_freqbin_2G = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTarget_freqbin_2GHT20 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTarget_freqbin_2GHT40 = {
		FREQ2FBIN(2412, 1),
		FREQ2FBIN(2437, 1),
		FREQ2FBIN(2472, 1)
	 },
	.calTargetPowerCck = {
		 /* 1L-5L,5S,11L,11S */
		 { {34, 34, 34, 34} },
		 { {34, 34, 34, 34} },
	},
	.calTargetPower2G = {
		 /* 6-24,36,48,54 */
		 { {34, 34, 32, 32} },
		 { {34, 34, 32, 32} },
		 { {34, 34, 32, 32} },
	},
	.calTargetPower2GHT20 = {
		{ {32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 0, 0, 0, 0} },
		{ {32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 0, 0, 0, 0} },
		{ {32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 0, 0, 0, 0} },
	},
	.calTargetPower2GHT40 = {
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0} },
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0} },
		{ {30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0} },
	},
	.ctlIndex_2G =  {
		0x11, 0x12, 0x15, 0x17, 0x41, 0x42,
		0x45, 0x47, 0x31, 0x32, 0x35, 0x37,
	},
	.ctl_freqbin_2G = {
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2457, 1),
			FREQ2FBIN(2462, 1)
		},
		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},

		{
			FREQ2FBIN(2412, 1),
			FREQ2FBIN(2417, 1),
			FREQ2FBIN(2462, 1),
			0xFF,
		},
		{
			FREQ2FBIN(2422, 1),
			FREQ2FBIN(2427, 1),
			FREQ2FBIN(2447, 1),
			FREQ2FBIN(2452, 1)
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(2484, 1),
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			FREQ2FBIN(2472, 1),
			0,
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
		},

		{
			/* Data[9].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[9].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[9].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[10].ctlEdges[0].bChannel */ FREQ2FBIN(2412, 1),
			/* Data[10].ctlEdges[1].bChannel */ FREQ2FBIN(2417, 1),
			/* Data[10].ctlEdges[2].bChannel */ FREQ2FBIN(2472, 1),
			0
		},

		{
			/* Data[11].ctlEdges[0].bChannel */ FREQ2FBIN(2422, 1),
			/* Data[11].ctlEdges[1].bChannel */ FREQ2FBIN(2427, 1),
			/* Data[11].ctlEdges[2].bChannel */ FREQ2FBIN(2447, 1),
			/* Data[11].ctlEdges[3].bChannel */ FREQ2FBIN(2462, 1),
		}
	 },
	.ctlPowerData_2G = {
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1) } },

		 { { CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },

		 { { CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
		 { { CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1) } },
	 },
	.modalHeader5G = {
		/* 4 idle,t1,t2,b (4 bits per setting) */
		.antCtrlCommon = LE32(0x220),
		/* 4 ra1l1, ra2l1, ra1l2,ra2l2,ra12 */
		.antCtrlCommon2 = LE32(0x44444),
		 /* antCtrlChain 6 idle, t,r,rx1,rx12,b (2 bits each) */
		.antCtrlChain = {
			LE16(0x150), LE16(0x150), LE16(0x150),
		},
		 /* xatten1DB 3 xatten1_db for AR9280 (0xa20c/b20c 5:0) */
		.xatten1DB = {0x19, 0x19, 0x19},

		/*
		 * xatten1Margin[AR9300_MAX_CHAINS]; 3 xatten1_margin
		 * for merlin (0xa20c/b20c 16:12
		 */
		.xatten1Margin = {0x14, 0x14, 0x14},
		.tempSlope = 70,
		.voltSlope = 0,
		/* spurChans spur channels in usual fbin coding format */
		.spurChans = {0, 0, 0, 0, 0},
		/* noiseFloorThreshCh Check if the register is per chain */
		.noiseFloorThreshCh = {-1, 0, 0},
		.reserved = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.quick_drop = 0,
		.xpaBiasLvl = 0,
		.txFrameToDataStart = 0x0e,
		.txFrameToPaOn = 0x0e,
		.txClip = 3, /* 4 bits tx_clip, 4 bits dac_scale_cck */
		.antennaGain = 0,
		.switchSettling = 0x2d,
		.adcDesiredSize = -30,
		.txEndToXpaOff = 0,
		.txEndToRxOn = 0x2,
		.txFrameToXpaOn = 0xe,
		.thresh62 = 28,
		.papdRateMaskHt20 = LE32(0x0cf0e0e0),
		.papdRateMaskHt40 = LE32(0x6cf0e0e0),
		.switchcomspdt = 0,
		.xlna_bias_strength = 0,
		.futureModal = {
			0, 0, 0, 0, 0, 0, 0,
		},
	 },
	.base_ext2 = {
		.tempSlopeLow = 35,
		.tempSlopeHigh = 50,
		.xatten1DBLow = {0, 0, 0},
		.xatten1MarginLow = {0, 0, 0},
		.xatten1DBHigh = {0, 0, 0},
		.xatten1MarginHigh = {0, 0, 0}
	 },
	.calFreqPier5G = {
		FREQ2FBIN(5160, 0),
		FREQ2FBIN(5220, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5785, 0)
	},
	.calPierData5G = {
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},
			{
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
				{0, 0, 0, 0, 0},
			},

	},
	.calTarget_freqbin_5G = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5600, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT20 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5825, 0)
	},
	.calTarget_freqbin_5GHT40 = {
		FREQ2FBIN(5180, 0),
		FREQ2FBIN(5240, 0),
		FREQ2FBIN(5320, 0),
		FREQ2FBIN(5400, 0),
		FREQ2FBIN(5500, 0),
		FREQ2FBIN(5700, 0),
		FREQ2FBIN(5745, 0),
		FREQ2FBIN(5825, 0)
	 },
	.calTargetPower5G = {
		/* 6-24,36,48,54 */
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
		{ {30, 30, 28, 24} },
	 },
	.calTargetPower5GHT20 = {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 0, 0, 0, 0} },
		{ {30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 0, 0, 0, 0} },
		{ {30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 0, 0, 0, 0} },
		{ {30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 0, 0, 0, 0} },
		{ {30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 0, 0, 0, 0} },
		{ {30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 0, 0, 0, 0} },
		{ {30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 0, 0, 0, 0} },
		{ {30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 0, 0, 0, 0} },
	 },
	.calTargetPower5GHT40 =  {
		/*
		 * 0_8_16,1-3_9-11_17-19,
		 * 4,5,6,7,12,13,14,15,20,21,22,23
		 */
		{ {28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 0, 0, 0, 0} },
		{ {28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 0, 0, 0, 0} },
		{ {28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 0, 0, 0, 0} },
		{ {28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 0, 0, 0, 0} },
		{ {28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 0, 0, 0, 0} },
		{ {28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 0, 0, 0, 0} },
		{ {28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 0, 0, 0, 0} },
		{ {28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 0, 0, 0, 0} },
	 },
	.ctlIndex_5G =  {
		0x10, 0x16, 0x18, 0x40, 0x46,
		0x48, 0x30, 0x36, 0x38
	},
	.ctl_freqbin_5G =  {
		{
			/* Data[0].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[0].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[0].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[0].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[0].ctlEdges[4].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[0].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[0].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[0].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},
		{
			/* Data[1].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[1].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[1].ctlEdges[2].bChannel */ FREQ2FBIN(5280, 0),
			/* Data[1].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[1].ctlEdges[4].bChannel */ FREQ2FBIN(5520, 0),
			/* Data[1].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[1].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[1].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[2].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[2].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[2].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[2].ctlEdges[3].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[2].ctlEdges[4].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[2].ctlEdges[5].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[2].ctlEdges[6].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[2].ctlEdges[7].bChannel */ FREQ2FBIN(5755, 0)
		},

		{
			/* Data[3].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[3].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[3].ctlEdges[2].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[3].ctlEdges[3].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[3].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[3].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[3].ctlEdges[6].bChannel */ 0xFF,
			/* Data[3].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[4].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[4].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[4].ctlEdges[2].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[4].ctlEdges[3].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[4].ctlEdges[4].bChannel */ 0xFF,
			/* Data[4].ctlEdges[5].bChannel */ 0xFF,
			/* Data[4].ctlEdges[6].bChannel */ 0xFF,
			/* Data[4].ctlEdges[7].bChannel */ 0xFF,
		},

		{
			/* Data[5].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[5].ctlEdges[1].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[5].ctlEdges[2].bChannel */ FREQ2FBIN(5310, 0),
			/* Data[5].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[5].ctlEdges[4].bChannel */ FREQ2FBIN(5590, 0),
			/* Data[5].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[5].ctlEdges[6].bChannel */ 0xFF,
			/* Data[5].ctlEdges[7].bChannel */ 0xFF
		},

		{
			/* Data[6].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[6].ctlEdges[1].bChannel */ FREQ2FBIN(5200, 0),
			/* Data[6].ctlEdges[2].bChannel */ FREQ2FBIN(5220, 0),
			/* Data[6].ctlEdges[3].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[6].ctlEdges[4].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[6].ctlEdges[5].bChannel */ FREQ2FBIN(5600, 0),
			/* Data[6].ctlEdges[6].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[6].ctlEdges[7].bChannel */ FREQ2FBIN(5745, 0)
		},

		{
			/* Data[7].ctlEdges[0].bChannel */ FREQ2FBIN(5180, 0),
			/* Data[7].ctlEdges[1].bChannel */ FREQ2FBIN(5260, 0),
			/* Data[7].ctlEdges[2].bChannel */ FREQ2FBIN(5320, 0),
			/* Data[7].ctlEdges[3].bChannel */ FREQ2FBIN(5500, 0),
			/* Data[7].ctlEdges[4].bChannel */ FREQ2FBIN(5560, 0),
			/* Data[7].ctlEdges[5].bChannel */ FREQ2FBIN(5700, 0),
			/* Data[7].ctlEdges[6].bChannel */ FREQ2FBIN(5745, 0),
			/* Data[7].ctlEdges[7].bChannel */ FREQ2FBIN(5825, 0)
		},

		{
			/* Data[8].ctlEdges[0].bChannel */ FREQ2FBIN(5190, 0),
			/* Data[8].ctlEdges[1].bChannel */ FREQ2FBIN(5230, 0),
			/* Data[8].ctlEdges[2].bChannel */ FREQ2FBIN(5270, 0),
			/* Data[8].ctlEdges[3].bChannel */ FREQ2FBIN(5510, 0),
			/* Data[8].ctlEdges[4].bChannel */ FREQ2FBIN(5550, 0),
			/* Data[8].ctlEdges[5].bChannel */ FREQ2FBIN(5670, 0),
			/* Data[8].ctlEdges[6].bChannel */ FREQ2FBIN(5755, 0),
			/* Data[8].ctlEdges[7].bChannel */ FREQ2FBIN(5795, 0)
		}
	 },
	.ctlPowerData_5G = {
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
				CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
			}
		},
		{
			{
				CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1),
				CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1),
			}
		},
	 }
};

static const struct ar9300_eeprom *ar9300_eep_templates[] = {
	&ar9300_default,
	&ar9300_x112,
	&ar9300_h116,
	&ar9300_h112,
	&ar9300_x113,
};

static const struct ar9300_eeprom *ar9003_eeprom_struct_find_by_id(int id)
{
	int it;

	for (it = 0; it < ARRAY_SIZE(ar9300_eep_templates); it++)
		if (ar9300_eep_templates[it]->templateVersion == id)
			return ar9300_eep_templates[it];
	return NULL;
}

#endif /* _AR9003_EEPROM_TEMPLATES_H_ */
