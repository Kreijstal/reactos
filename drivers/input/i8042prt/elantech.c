/*
 * PROJECT:     ReactOS i8042 (ps/2 keyboard-mouse controller) driver
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/input/i8042prt/elantech.c
 * PURPOSE:     Elantech touchpad detection, absolute mode initialization
 *              and packet translation
 *
 * The device protocol (magic knock, sliced commands, register layout and
 * packet formats) follows the Linux v6.6 reference driver
 * drivers/input/mouse/elantech.c (Copyright 2007-2009 Arjan Opmeer).
 *
 * ReactOS has no absolute-pointer/HID-touchpad input stack, so absolute
 * mode packets are translated into relative MOUSE_INPUT_DATA events:
 * - one finger moves the cursor (relative deltas)
 * - a two finger vertical drag emits mouse wheel events
 * - physical button bits are passed through on every packet
 *
 * Everything here runs at DIRQL from the mouse interrupt service routine.
 */

/* INCLUDES ****************************************************************/

#include "i8042prt.h"

#include <debug.h>

/* INIT COMMAND ENGINE *****************************************************/

/*
 * The initialization dialogue (queries, register writes) is a flat table
 * of bytes to send, each of which is acknowledged by the device with an
 * interrupt delivering 0xFA, optionally followed by a 3 byte response.
 * The ISR advances an index through the current table; when a phase
 * completes, the next phase's table is built.
 */

static VOID
ElantechSeqStart(
	IN PI8042_MOUSE_EXTENSION DeviceExtension,
	IN ELANTECH_INIT_PHASE Phase)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	Etd->Phase = Phase;
	Etd->SeqLength = 0;
	Etd->SeqAcked = 0;
	Etd->ResponseLength = 0;
	Etd->ResponseIndex = 0;
}

static VOID
ElantechSeqPush(
	IN PELANTECH_EXTENSION Etd,
	IN UCHAR Byte)
{
	ASSERT(Etd->SeqLength < ELANTECH_SEQ_MAX);
	Etd->Seq[Etd->SeqLength++] = Byte;
}

/*
 * ps2_sliced_command(): E6, then the command byte 2 bits at a time,
 * each pair sent as the parameter of a Set Resolution (E8) command.
 */
static VOID
ElantechSeqPushSliced(
	IN PELANTECH_EXTENSION Etd,
	IN UCHAR Command)
{
	LONG Shift;

	ElantechSeqPush(Etd, PSMOUSE_CMD_SETSCALE11);
	for (Shift = 6; Shift >= 0; Shift -= 2)
	{
		ElantechSeqPush(Etd, PSMOUSE_CMD_SETRES);
		ElantechSeqPush(Etd, (Command >> Shift) & 3);
	}
}

/*
 * synaptics_send_cmd()/elantech_send_cmd(): a query returning 3 status
 * bytes. Hardware v3+ supports the fast F8 command form, v1/v2 (and the
 * initial detection, when the version is still unknown) use the sliced
 * form.
 */
static VOID
ElantechSeqPushQuery(
	IN PELANTECH_EXTENSION Etd,
	IN UCHAR Command)
{
	if (Etd->HwVersion >= 3)
	{
		ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
		ElantechSeqPush(Etd, Command);
	}
	else
		ElantechSeqPushSliced(Etd, Command);

	ElantechSeqPush(Etd, PSMOUSE_CMD_GETINFO);
	Etd->ResponseLength = 3;
}

/* elantech_write_reg() */
static VOID
ElantechSeqPushWriteReg(
	IN PELANTECH_EXTENSION Etd,
	IN UCHAR Reg,
	IN UCHAR Value)
{
	switch (Etd->HwVersion)
	{
		case 1:
			ElantechSeqPushSliced(Etd, ETP_REGISTER_WRITE);
			ElantechSeqPushSliced(Etd, Reg);
			ElantechSeqPushSliced(Etd, Value);
			break;

		case 2:
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, ETP_REGISTER_WRITE);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Reg);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Value);
			break;

		case 3:
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, ETP_REGISTER_READWRITE);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Reg);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Value);
			break;

		case 4:
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, ETP_REGISTER_READWRITE);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Reg);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, ETP_REGISTER_READWRITE);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Value);
			break;
	}

	ElantechSeqPush(Etd, PSMOUSE_CMD_SETSCALE11);
}

/* elantech_read_reg(), only used to read back register 0x10 (hw v1-v3) */
static VOID
ElantechSeqPushReadReg(
	IN PELANTECH_EXTENSION Etd,
	IN UCHAR Reg)
{
	switch (Etd->HwVersion)
	{
		case 1:
			ElantechSeqPushSliced(Etd, ETP_REGISTER_READ);
			ElantechSeqPushSliced(Etd, Reg);
			break;

		case 2:
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, ETP_REGISTER_READ);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Reg);
			break;

		default: /* 3 (v4 does not read back) */
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, ETP_REGISTER_READWRITE);
			ElantechSeqPush(Etd, ETP_PS2_CUSTOM_COMMAND);
			ElantechSeqPush(Etd, Reg);
			break;
	}

	ElantechSeqPush(Etd, PSMOUSE_CMD_GETINFO);
	Etd->ResponseLength = 3;
}

static VOID
ElantechSeqSend(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	ASSERT(Etd->SeqLength != 0);
	DeviceExtension->MouseHook.IsrWritePort(
		DeviceExtension->MouseHook.CallContext,
		Etd->Seq[0]);
}

/* DETECTION / INIT ********************************************************/

/* elantech_detect(): the answer to the magic knock */
BOOLEAN
ElantechIsKnockResponse(
	IN CONST UCHAR Buffer[3])
{
	return (Buffer[0] == 0x3C && Buffer[1] == 0x03 &&
	        (Buffer[2] == 0xC8 || Buffer[2] == 0x00));
}

/* elantech_is_signature_valid() */
static BOOLEAN
ElantechIsSignatureValid(
	IN CONST UCHAR *Param)
{
	static const UCHAR Rates[] = { 200, 100, 80, 60, 40, 20, 10 };
	ULONG i;

	if (Param[0] == 0)
		return FALSE;

	if (Param[1] == 0)
		return TRUE;

	/*
	 * Some hw_version >= 4 models have a revision higher than 20.
	 * Meaning that Param[2] may be 10 or 20, skip the rates check
	 * for these.
	 */
	if ((Param[0] & 0x0F) >= 0x06 && (Param[1] & 0xAF) == 0x0F &&
	    Param[2] < 40)
		return TRUE;

	for (i = 0; i < RTL_NUMBER_OF(Rates); i++)
		if (Param[2] == Rates[i])
			return FALSE;

	return TRUE;
}

/* elantech_set_properties() + elantech_set_absolute_mode() register sets */
static BOOLEAN
ElantechSetProperties(
	IN PELANTECH_EXTENSION Etd)
{
	Etd->IcVersion = (UCHAR)((Etd->FwVersion & 0x0F0000) >> 16);

	/* Early versions of Elan touchpads don't obey the version rule */
	if (Etd->FwVersion < 0x020030 || Etd->FwVersion == 0x020600)
		Etd->HwVersion = 1;
	else
	{
		switch (Etd->IcVersion)
		{
			case 2:
			case 4:
				Etd->HwVersion = 2;
				break;
			case 5:
				Etd->HwVersion = 3;
				break;
			default:
				if (Etd->IcVersion >= 6 && Etd->IcVersion <= 15)
					Etd->HwVersion = 4;
				else
					return FALSE;
				break;
		}
	}

	/*
	 * This firmware suffers from misreporting coordinates when a touch
	 * action starts, causing the cursor to jump.
	 */
	Etd->JumpyCursor =
		(Etd->FwVersion == 0x020022 || Etd->FwVersion == 0x020600);

	Etd->ReportsPressure =
		(Etd->HwVersion > 1 && Etd->FwVersion >= 0x020800);

	/*
	 * The signatures of v3 and v4 packets change depending on the value
	 * of this hardware flag.
	 */
	Etd->CrcEnabled = (Etd->FwVersion & 0x4000) == 0x4000;

	Etd->HasMiddleButton =
		ELANTECH_FW_NEW_IC_SMBUS_HOST_NOTIFY(Etd->FwVersion) &&
		!ELANTECH_FW_IS_BUTTONPAD(Etd->FwVersion);

	Etd->PacketSize = (Etd->HwVersion > 1) ? 6 : 4;

	/*
	 * Relative-event translation tuning (ReactOS specific). The raw
	 * coordinate ranges are roughly 576x384 (v1), 1152x768 (v2) and
	 * 2000-3000 units wide for v3/v4, so scale the deltas down to
	 * mouse-like counts and pick a matching two-finger scroll step.
	 */
	switch (Etd->HwVersion)
	{
		case 1:
			Etd->DeltaDivisor = 2;
			Etd->ScrollThreshold = 15;
			break;
		case 2:
			Etd->DeltaDivisor = 4;
			Etd->ScrollThreshold = 30;
			break;
		default:
			Etd->DeltaDivisor = 8;
			Etd->ScrollThreshold = 60;
			break;
	}

	/* Absolute mode register set (elantech_set_absolute_mode) */
	switch (Etd->HwVersion)
	{
		case 1:
			Etd->AbsRegs[0][0] = 0x10; Etd->AbsRegs[0][1] = 0x16;
			Etd->AbsRegs[1][0] = 0x11; Etd->AbsRegs[1][1] = 0x8F;
			Etd->AbsRegCount = 2;
			break;

		case 2:
			/* Windows driver values */
			Etd->AbsRegs[0][0] = 0x10; Etd->AbsRegs[0][1] = 0x54;
			Etd->AbsRegs[1][0] = 0x11; Etd->AbsRegs[1][1] = 0x88;
			Etd->AbsRegs[2][0] = 0x21; Etd->AbsRegs[2][1] = 0x60;
			Etd->AbsRegCount = 3;
			break;

		case 3:
			/* Absolute mode + real hardware resolution */
			Etd->AbsRegs[0][0] = 0x10; Etd->AbsRegs[0][1] = 0x0B;
			Etd->AbsRegCount = 1;
			break;

		case 4:
			Etd->AbsRegs[0][0] = 0x07; Etd->AbsRegs[0][1] = 0x01;
			Etd->AbsRegCount = 1;
			break;
	}

	return TRUE;
}

/*
 * Abort the Elantech dialogue and re-run the whole mouse detection.
 * The sticky Failed flag makes the knock check take the plain PS/2
 * path on the next pass, so a device that answered the knock but then
 * failed still ends up as a working generic mouse.
 */
static VOID
ElantechFallback(
	IN PI8042_MOUSE_EXTENSION DeviceExtension,
	IN PCSTR Reason)
{
	DPRINT1("Elantech init failed (%s), falling back to generic PS/2\n",
		Reason);

	DeviceExtension->ElantechData.Failed = TRUE;
	DeviceExtension->MouseType = GenericPS2;
	DeviceExtension->MouseResetState = ExpectingReset;
	DeviceExtension->MouseHook.IsrWritePort(
		DeviceExtension->MouseHook.CallContext,
		MOU_CMD_RESET);
}

/*
 * All Elantech specific work is done; the touchpad is in absolute mode.
 * Hand control back to the generic reset machine tail which programs the
 * sample rate and resolution and finally enables data reporting (F4).
 */
static VOID
ElantechInitDone(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	Etd->PacketIndex = 0;

	DeviceExtension->MouseAttributes.MouseIdentifier =
		WHEELMOUSE_I8042_HARDWARE;
	DeviceExtension->MouseAttributes.NumberOfButtons =
		Etd->HasMiddleButton ? 3 : 2;

	DPRINT1("Elantech touchpad in absolute mode (%u byte packets)\n",
		Etd->PacketSize);

	DeviceExtension->MouseResetState = ExpectingSetSamplingRateACK;
	DeviceExtension->MouseHook.IsrWritePort(
		DeviceExtension->MouseHook.CallContext,
		0xF3);
}

static VOID
ElantechStartSetAbsolute(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	if (Etd->AbsRegIndex < Etd->AbsRegCount)
	{
		ElantechSeqStart(DeviceExtension, ElantechPhaseSetAbsolute);
		ElantechSeqPushWriteReg(Etd,
			Etd->AbsRegs[Etd->AbsRegIndex][0],
			Etd->AbsRegs[Etd->AbsRegIndex][1]);
		ElantechSeqSend(DeviceExtension);
		return;
	}

	if (Etd->HwVersion != 4)
	{
		/*
		 * Read back register 0x10; hw v1 must have the absolute
		 * mode bit set, and v2 hardware may not be ready until the
		 * value is read back (elantech_set_absolute_mode).
		 */
		ElantechSeqStart(DeviceExtension, ElantechPhaseReadBack);
		ElantechSeqPushReadReg(Etd, 0x10);
		ElantechSeqSend(DeviceExtension);
		return;
	}

	/* v4 has no register 0x10 to read back */
	ElantechInitDone(DeviceExtension);
}

static VOID
ElantechPhaseComplete(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	switch (Etd->Phase)
	{
		case ElantechPhaseFwVersion:
			/*
			 * Check that the reported firmware version is sane;
			 * Logitech mice are known to answer the magic knock
			 * too (elantech_detect).
			 */
			if (!ElantechIsSignatureValid(Etd->Response))
			{
				ElantechFallback(DeviceExtension,
					"invalid firmware version signature");
				return;
			}

			Etd->FwVersion = ((ULONG)Etd->Response[0] << 16) |
			                 ((ULONG)Etd->Response[1] << 8) |
			                 Etd->Response[2];

			if (!ElantechSetProperties(Etd))
			{
				ElantechFallback(DeviceExtension,
					"unknown hardware version");
				return;
			}

			DPRINT1("Elantech touchpad: hw version %u, fw version 0x%06lx\n",
				Etd->HwVersion, Etd->FwVersion);

			DeviceExtension->MouseType = Elantech;

			ElantechSeqStart(DeviceExtension, ElantechPhaseCapabilities);
			ElantechSeqPushQuery(Etd, ETP_CAPABILITIES_QUERY);
			ElantechSeqSend(DeviceExtension);
			return;

		case ElantechPhaseCapabilities:
			RtlCopyMemory(Etd->Capabilities, Etd->Response,
				sizeof(Etd->Capabilities));
			/* The MSB indicates the presence of a trackpoint */
			Etd->HasTrackpoint =
				(Etd->Capabilities[0] & 0x80) == 0x80;

			DPRINT1("Elantech capabilities: 0x%02x 0x%02x 0x%02x\n",
				Etd->Capabilities[0], Etd->Capabilities[1],
				Etd->Capabilities[2]);

			if (Etd->HwVersion != 1)
			{
				ElantechSeqStart(DeviceExtension, ElantechPhaseSample);
				ElantechSeqPushQuery(Etd, ETP_SAMPLE_QUERY);
				ElantechSeqSend(DeviceExtension);
				return;
			}

			ElantechStartSetAbsolute(DeviceExtension);
			return;

		case ElantechPhaseSample:
			RtlCopyMemory(Etd->Samples, Etd->Response,
				sizeof(Etd->Samples));

			if (Etd->Samples[1] == 0x74 && Etd->HwVersion == 3)
			{
				/*
				 * This module has a bug which makes absolute
				 * mode unusable (elantech_query_info), so use
				 * the standard PS/2 protocol instead.
				 */
				ElantechFallback(DeviceExtension,
					"module with broken absolute mode");
				return;
			}

			ElantechStartSetAbsolute(DeviceExtension);
			return;

		case ElantechPhaseSetAbsolute:
			Etd->AbsRegIndex++;
			ElantechStartSetAbsolute(DeviceExtension);
			return;

		case ElantechPhaseReadBack:
			if (Etd->HwVersion == 1 &&
			    !(Etd->Response[0] & ETP_R10_ABSOLUTE_MODE))
			{
				ElantechFallback(DeviceExtension,
					"touchpad refuses to switch to absolute mode");
				return;
			}
			ElantechInitDone(DeviceExtension);
			return;
	}
}

VOID
ElantechStartDetection(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	ULONG i;

	/* Only reached when no previous attempt failed */
	ASSERT(!Etd->Failed);
	RtlZeroMemory(Etd, sizeof(*Etd));

	/* Byte parity table for the v1 packet check (elantech_setup_ps2) */
	Etd->Parity[0] = 1;
	for (i = 1; i < 256; i++)
		Etd->Parity[i] = Etd->Parity[i & (i - 1)] ^ 1;

	DPRINT1("Elantech magic knock answered, querying firmware version\n");

	/*
	 * The version query must use the sliced form: the hardware version
	 * is not known yet (elantech_detect uses synaptics_send_cmd).
	 */
	ElantechSeqStart(DeviceExtension, ElantechPhaseFwVersion);
	ElantechSeqPushQuery(Etd, ETP_FW_VERSION_QUERY);
	DeviceExtension->MouseResetState = ELANTECH_INIT_SUBSTATE;
	ElantechSeqSend(DeviceExtension);
}

BOOLEAN
ElantechInitIsr(
	IN PI8042_MOUSE_EXTENSION DeviceExtension,
	IN UCHAR Value)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	if (Etd->SeqAcked < Etd->SeqLength)
	{
		if (Value != MOUSE_ACK)
		{
			ElantechFallback(DeviceExtension, "command not ACKed");
			return TRUE;
		}

		Etd->SeqAcked++;
		if (Etd->SeqAcked < Etd->SeqLength)
		{
			DeviceExtension->MouseHook.IsrWritePort(
				DeviceExtension->MouseHook.CallContext,
				Etd->Seq[Etd->SeqAcked]);
		}
		else if (Etd->ResponseLength == 0)
		{
			ElantechPhaseComplete(DeviceExtension);
		}
		return TRUE;
	}

	/* Collecting a query/status response */
	Etd->Response[Etd->ResponseIndex++] = Value;
	if (Etd->ResponseIndex >= Etd->ResponseLength)
		ElantechPhaseComplete(DeviceExtension);
	return TRUE;
}

/* EVENT TRANSLATION *******************************************************/

static VOID
ElantechQueueEvent(
	IN PI8042_MOUSE_EXTENSION DeviceExtension,
	IN LONG LastX,
	IN LONG LastY,
	IN LONG WheelUnits,
	IN UCHAR ButtonByte,
	IN BOOLEAN Clickpad)
{
	PMOUSE_INPUT_DATA MouseInput;

	MouseInput = DeviceExtension->MouseBuffer +
	             DeviceExtension->MouseInBuffer;

	MouseInput->Buttons = 0;
	MouseInput->RawButtons = 0;
	MouseInput->Flags = MOUSE_MOVE_RELATIVE;
	MouseInput->LastX = LastX;
	MouseInput->LastY = LastY;

	if (Clickpad)
	{
		/* Both physical buttons of a clickpad map to the left one */
		if (ButtonByte & 0x03)
			MouseInput->RawButtons |= MOUSE_LEFT_BUTTON_DOWN;
	}
	else
	{
		if (ButtonByte & 0x01)
			MouseInput->RawButtons |= MOUSE_LEFT_BUTTON_DOWN;
		if (ButtonByte & 0x02)
			MouseInput->RawButtons |= MOUSE_RIGHT_BUTTON_DOWN;
		if (ButtonByte & 0x04)
			MouseInput->RawButtons |= MOUSE_MIDDLE_BUTTON_DOWN;
	}

	if (WheelUnits != 0)
	{
		MouseInput->RawButtons |= MOUSE_WHEEL;
		MouseInput->ButtonData = (USHORT)(WheelUnits * WHEEL_DELTA);
	}

	i8042MouHandleButtons(
		DeviceExtension,
		MOUSE_LEFT_BUTTON_DOWN |
		MOUSE_RIGHT_BUTTON_DOWN |
		MOUSE_MIDDLE_BUTTON_DOWN);
	DeviceExtension->MouseHook.QueueMousePacket(
		DeviceExtension->MouseHook.CallContext);
}

/*
 * Translate an absolute touch state into relative events.
 * X is the raw coordinate (grows to the right), Y is kept in screen
 * orientation (grows downwards, i.e. the negated raw value).
 */
static VOID
ElantechReport(
	IN PI8042_MOUSE_EXTENSION DeviceExtension,
	IN ULONG Fingers,
	IN LONG X,
	IN LONG Y,
	IN UCHAR ButtonByte)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	LONG Dx = 0, Dy = 0, Wheel = 0;
	BOOLEAN Clickpad;

	Clickpad = (Etd->HwVersion >= 3) &&
	           ELANTECH_FW_IS_BUTTONPAD(Etd->FwVersion);

	if (Fingers != Etd->LastFingers)
	{
		/*
		 * Finger count edge: reset the reference position and
		 * suppress motion for this packet (jitter filter).
		 */
		Etd->LastFingers = Fingers;
		Etd->LastX = X;
		Etd->LastY = Y;
		Etd->AccumX = 0;
		Etd->AccumY = 0;
		Etd->ScrollAccum = 0;
		if (Fingers != 1)
			Etd->SingleFingerReports = 0;
	}
	else if (Fingers == 1)
	{
		if (Etd->JumpyCursor && Etd->SingleFingerReports < 2)
		{
			/*
			 * The first 2 one-finger reports of this firmware
			 * are bogus (elantech_report_absolute_v1)
			 */
			Etd->SingleFingerReports++;
			Etd->LastX = X;
			Etd->LastY = Y;
		}
		else
		{
			Etd->AccumX += X - Etd->LastX;
			Etd->AccumY += Y - Etd->LastY;
			Etd->LastX = X;
			Etd->LastY = Y;
			Dx = Etd->AccumX / Etd->DeltaDivisor;
			Dy = Etd->AccumY / Etd->DeltaDivisor;
			Etd->AccumX -= Dx * Etd->DeltaDivisor;
			Etd->AccumY -= Dy * Etd->DeltaDivisor;
		}
	}
	else if (Fingers == 2)
	{
		/* Two finger vertical drag scrolls, fingers up = wheel up */
		Etd->ScrollAccum += Etd->LastY - Y;
		Etd->LastX = X;
		Etd->LastY = Y;
		Wheel = Etd->ScrollAccum / Etd->ScrollThreshold;
		Etd->ScrollAccum -= Wheel * Etd->ScrollThreshold;
	}
	else
	{
		/* 0 or 3+ fingers: only track the position */
		Etd->LastX = X;
		Etd->LastY = Y;
	}

	ElantechQueueEvent(DeviceExtension, Dx, Dy, Wheel, ButtonByte, Clickpad);
}

/* PACKET CHECKS ***********************************************************/

/* elantech_packet_check_v1() */
static BOOLEAN
ElantechPacketCheckV1(
	IN PELANTECH_EXTENSION Etd)
{
	PUCHAR Packet = Etd->Packet;
	UCHAR P1, P2, P3;

	/* Parity bits are placed differently */
	if (Etd->FwVersion < 0x020000)
	{
		/* byte 0:  D   U  p1  p2   1  p3   R   L */
		P1 = (Packet[0] & 0x20) >> 5;
		P2 = (Packet[0] & 0x10) >> 4;
	}
	else
	{
		/* byte 0: n1  n0  p2  p1   1  p3   R   L */
		P1 = (Packet[0] & 0x10) >> 4;
		P2 = (Packet[0] & 0x20) >> 5;
	}

	P3 = (Packet[0] & 0x04) >> 2;

	return Etd->Parity[Packet[1]] == P1 &&
	       Etd->Parity[Packet[2]] == P2 &&
	       Etd->Parity[Packet[3]] == P3;
}

/* elantech_debounce_check_v2() */
static BOOLEAN
ElantechDebounceCheckV2(
	IN PELANTECH_EXTENSION Etd)
{
	static const UCHAR DebouncePacket[] =
		{ 0x84, 0xFF, 0xFF, 0x02, 0xFF, 0xFF };

	/* RtlEqualMemory is a memcmp macro, which i386 GCC emits as a libcall
	 * kernel drivers cannot link; RtlCompareMemory is a real export */
	return RtlCompareMemory(Etd->Packet, DebouncePacket,
	                        sizeof(DebouncePacket)) == sizeof(DebouncePacket);
}

/* elantech_packet_check_v2() */
static BOOLEAN
ElantechPacketCheckV2(
	IN PELANTECH_EXTENSION Etd)
{
	PUCHAR Packet = Etd->Packet;

	if (Etd->ReportsPressure)
		return (Packet[0] & 0x0C) == 0x04 &&
		       (Packet[3] & 0x0F) == 0x02;

	if ((Packet[0] & 0xC0) == 0x80)
		return (Packet[0] & 0x0C) == 0x0C &&
		       (Packet[3] & 0x0E) == 0x08;

	return (Packet[0] & 0x3C) == 0x3C &&
	       (Packet[1] & 0xF0) == 0x00 &&
	       (Packet[3] & 0x3E) == 0x38 &&
	       (Packet[4] & 0xF0) == 0x00;
}

/* elantech_packet_check_v3() */
static UCHAR
ElantechPacketCheckV3(
	IN PELANTECH_EXTENSION Etd)
{
	static const UCHAR DebouncePacket[] =
		{ 0xC4, 0xFF, 0xFF, 0x02, 0xFF, 0xFF };
	PUCHAR Packet = Etd->Packet;

	/*
	 * Check debounce first, it has the same signature in byte 0
	 * and byte 3 as PACKET_V3_HEAD.
	 */
	if (RtlCompareMemory(Packet, DebouncePacket,
	                     sizeof(DebouncePacket)) == sizeof(DebouncePacket))
		return ELANTECH_PACKET_DEBOUNCE;

	if (Etd->CrcEnabled)
	{
		if ((Packet[3] & 0x09) == 0x08)
			return ELANTECH_PACKET_V3_HEAD;
		if ((Packet[3] & 0x09) == 0x09)
			return ELANTECH_PACKET_V3_TAIL;
	}
	else
	{
		if ((Packet[0] & 0x0C) == 0x04 && (Packet[3] & 0xCF) == 0x02)
			return ELANTECH_PACKET_V3_HEAD;
		if ((Packet[0] & 0x0C) == 0x0C && (Packet[3] & 0xCE) == 0x0C)
			return ELANTECH_PACKET_V3_TAIL;
		if ((Packet[3] & 0x0F) == 0x06)
			return ELANTECH_PACKET_TRACKPOINT;
	}

	return ELANTECH_PACKET_UNKNOWN;
}

/* elantech_packet_check_v4() */
static UCHAR
ElantechPacketCheckV4(
	IN PELANTECH_EXTENSION Etd)
{
	PUCHAR Packet = Etd->Packet;
	UCHAR PacketType = Packet[3] & 0x03;
	BOOLEAN SanityCheck;

	if (Etd->HasTrackpoint && (Packet[3] & 0x0F) == 0x06)
		return ELANTECH_PACKET_TRACKPOINT;

	/*
	 * Sanity check based on the constant bits of a packet. The constant
	 * bits change depending on the value of the hardware flag
	 * 'crc_enabled' and the version of the IC body, but are the same
	 * for every packet, regardless of the type.
	 */
	if (Etd->CrcEnabled)
		SanityCheck = ((Packet[3] & 0x08) == 0x00);
	else if (Etd->IcVersion == 7 && Etd->Samples[1] == 0x2A)
		SanityCheck = ((Packet[3] & 0x1C) == 0x10);
	else
		SanityCheck = ((Packet[0] & 0x08) == 0x00 &&
		               (Packet[3] & 0x1C) == 0x10);

	if (!SanityCheck)
		return ELANTECH_PACKET_UNKNOWN;

	switch (PacketType)
	{
		case 0:
			return ELANTECH_PACKET_V4_STATUS;
		case 1:
			return ELANTECH_PACKET_V4_HEAD;
		case 2:
			return ELANTECH_PACKET_V4_MOTION;
	}

	return ELANTECH_PACKET_UNKNOWN;
}

/* PACKET DECODING *********************************************************/

/* elantech_report_trackpoint() */
static VOID
ElantechReportTrackpoint(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	/*
	 * byte 0:  0   0  sx  sy   0   M   R   L
	 * byte 1: ~sx  0   0   0   0   0   0   0
	 * byte 2: ~sy  0   0   0   0   0   0   0
	 * byte 3:  0   0 ~sy ~sx   0   1   1   0
	 * byte 4: x7  x6  x5  x4  x3  x2  x1  x0
	 * byte 5: y7  y6  y5  y4  y3  y2  y1  y0
	 *
	 * x and y are written in two's complement spread over 9 bits
	 * with sx/sy the relative top bit and x7..x0 and y7..y0 the
	 * lower bits.
	 */
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	PUCHAR Packet = Etd->Packet;
	LONG X, Y;
	ULONG T;

	T = (ULONG)Packet[0] | ((ULONG)Packet[1] << 8) |
	    ((ULONG)Packet[2] << 16) | ((ULONG)Packet[3] << 24);

	switch (T & ~7ul)
	{
		case 0x06000030ul:
		case 0x16008020ul:
		case 0x26800010ul:
		case 0x36808000ul:
			/*
			 * This firmware misreports coordinates for trackpoint
			 * occasionally. Discard packets outside of [-127, 127]
			 * range to prevent cursor jumps.
			 */
			if (Packet[4] == 0x80 || Packet[5] == 0x80 ||
			    Packet[1] >> 7 == Packet[4] >> 7 ||
			    Packet[2] >> 7 == Packet[5] >> 7)
			{
				TRACE_(I8042PRT, "Elantech: discarding trackpoint packet\n");
				break;
			}
			X = (LONG)Packet[4] - (LONG)((Packet[1] ^ 0x80) << 1);
			Y = (LONG)((Packet[2] ^ 0x80) << 1) - (LONG)Packet[5];

			/* Positive Y is already downwards here */
			ElantechQueueEvent(DeviceExtension, X, Y, 0,
			                   Packet[0] & 0x07, FALSE);
			break;

		default:
			TRACE_(I8042PRT, "Elantech: unexpected trackpoint packet\n");
			break;
	}
}

/* elantech_report_absolute_v1() */
static VOID
ElantechProcessPacketV1(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	PUCHAR Packet = Etd->Packet;
	ULONG Fingers;
	LONG X = 0, Y = 0;

	if (!ElantechPacketCheckV1(Etd))
	{
		TRACE_(I8042PRT, "Elantech v1 packet failed parity check\n");
		return;
	}

	if (Etd->FwVersion < 0x020000)
	{
		/*
		 * byte 0:  D   U  p1  p2   1  p3   R   L
		 * byte 1:  f   0  th  tw  x9  x8  y9  y8
		 */
		Fingers = ((Packet[1] & 0x80) >> 7) +
		          ((Packet[1] & 0x30) >> 4);
	}
	else
	{
		/*
		 * byte 0: n1  n0  p2  p1   1  p3   R   L
		 * byte 1:  0   0   0   0  x9  x8  y9  y8
		 */
		Fingers = (Packet[0] & 0xC0) >> 6;
	}

	/*
	 * byte 2: x7  x6  x5  x4  x3  x2  x1  x0
	 * byte 3: y7  y6  y5  y4  y3  y2  y1  y0
	 */
	if (Fingers)
	{
		X = ((Packet[1] & 0x0C) << 6) | Packet[2];
		Y = -(LONG)(((Packet[1] & 0x03) << 8) | Packet[3]);
	}

	ElantechReport(DeviceExtension, Fingers, X, Y, Packet[0] & 0x03);
}

/* elantech_report_absolute_v2() */
static VOID
ElantechProcessPacketV2(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	PUCHAR Packet = Etd->Packet;
	ULONG Fingers;
	LONG X = 0, Y = 0;

	/* The hardware is in debounce status, ignore the whole packet */
	if (ElantechDebounceCheckV2(Etd))
		return;

	if (!ElantechPacketCheckV2(Etd))
	{
		TRACE_(I8042PRT, "Elantech v2 packet failed check\n");
		return;
	}

	/* byte 0: n1  n0   .   .   .   .   R   L */
	Fingers = (Packet[0] & 0xC0) >> 6;

	if (Fingers == 3 && (Packet[3] & 0x80))
	{
		/* byte 3:  n4  .   w1  w0   .   .   .   . */
		Fingers = 4;
	}

	if (Fingers == 2)
	{
		/*
		 * The coordinate of each finger is reported separately with
		 * a lower resolution for two finger touches:
		 * byte 0:  .   .  ay8 ax8  .   .   .   .
		 * byte 1: ax7 ax6 ax5 ax4 ax3 ax2 ax1 ax0
		 * byte 2: ay7 ay6 ay5 ay4 ay3 ay2 ay1 ay0
		 */
		X = (LONG)(((((ULONG)Packet[0] & 0x10) << 4) | Packet[1]) << 2);
		Y = -(LONG)(((((ULONG)Packet[0] & 0x20) << 3) | Packet[2]) << 2);
	}
	else if (Fingers != 0)
	{
		/*
		 * byte 1:  .   .   .   .  x11 x10 x9  x8
		 * byte 2: x7  x6  x5  x4  x4  x2  x1  x0
		 * byte 4:  .   .   .   .  y11 y10 y9  y8
		 * byte 5: y7  y6  y5  y4  y3  y2  y1  y0
		 */
		X = ((Packet[1] & 0x0F) << 8) | Packet[2];
		Y = -(LONG)(((Packet[4] & 0x0F) << 8) | Packet[5]);
	}

	ElantechReport(DeviceExtension, Fingers, X, Y, Packet[0] & 0x03);
}

/* elantech_report_absolute_v3() */
static VOID
ElantechProcessPacketV3(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	PUCHAR Packet = Etd->Packet;
	UCHAR PacketType;
	UCHAR ButtonMask;
	ULONG Fingers;
	LONG X = 0, Y = 0;

	PacketType = ElantechPacketCheckV3(Etd);
	switch (PacketType)
	{
		case ELANTECH_PACKET_UNKNOWN:
			TRACE_(I8042PRT, "Elantech v3 packet failed check\n");
			return;

		case ELANTECH_PACKET_DEBOUNCE:
			return;

		case ELANTECH_PACKET_TRACKPOINT:
			ElantechReportTrackpoint(DeviceExtension);
			return;

		default:
			break;
	}

	/* byte 0: n1  n0   .   .   .   .   R   L */
	Fingers = (Packet[0] & 0xC0) >> 6;

	if (Fingers == 2)
	{
		if (PacketType == ELANTECH_PACKET_V3_HEAD)
		{
			/* Cache the first finger, wait for the tail packet */
			Etd->FingerX[0] = ((Packet[1] & 0x0F) << 8) | Packet[2];
			Etd->FingerY[0] =
				-(LONG)(((Packet[4] & 0x0F) << 8) | Packet[5]);
			return;
		}

		/* ELANTECH_PACKET_V3_TAIL: report using the first finger */
		X = Etd->FingerX[0];
		Y = Etd->FingerY[0];
	}
	else if (Fingers != 0)
	{
		X = ((Packet[1] & 0x0F) << 8) | Packet[2];
		Y = -(LONG)(((Packet[4] & 0x0F) << 8) | Packet[5]);
	}

	ButtonMask = Etd->HasMiddleButton ? 0x07 : 0x03;
	ElantechReport(DeviceExtension, Fingers, X, Y, Packet[0] & ButtonMask);
}

static VOID
ElantechReportV4State(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	UCHAR ButtonMask;
	ULONG Fingers = 0;
	ULONG Primary = ETP_MAX_FINGERS;
	ULONG i;
	LONG X = 0, Y = 0;

	for (i = 0; i < ETP_MAX_FINGERS; i++)
	{
		if (Etd->FingerBitmap & (1ul << i))
		{
			Fingers++;
			if (Primary == ETP_MAX_FINGERS)
				Primary = i;
		}
	}

	if (Primary != ETP_MAX_FINGERS)
	{
		X = Etd->FingerX[Primary];
		Y = Etd->FingerY[Primary];
	}

	ButtonMask = Etd->HasMiddleButton ? 0x07 : 0x03;
	ElantechReport(DeviceExtension, Fingers, X, Y,
	               Etd->Packet[0] & ButtonMask);
}

/* elantech_report_absolute_v4() */
static VOID
ElantechProcessPacketV4(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;
	PUCHAR Packet = Etd->Packet;
	LONG Id, Sid, Weight;

	switch (ElantechPacketCheckV4(Etd))
	{
		case ELANTECH_PACKET_TRACKPOINT:
			ElantechReportTrackpoint(DeviceExtension);
			return;

		case ELANTECH_PACKET_V4_STATUS:
			/* New finger bitmap (process_packet_status_v4) */
			Etd->FingerBitmap = Packet[1] & 0x1F;
			ElantechReportV4State(DeviceExtension);
			return;

		case ELANTECH_PACKET_V4_HEAD:
			/* Absolute position of one finger (process_packet_head_v4) */
			Id = (LONG)((Packet[3] & 0xE0) >> 5) - 1;
			if (Id < 0 || Id >= ETP_MAX_FINGERS)
				return;

			Etd->FingerX[Id] = ((Packet[1] & 0x0F) << 8) | Packet[2];
			Etd->FingerY[Id] =
				-(LONG)(((Packet[4] & 0x0F) << 8) | Packet[5]);
			Etd->FingerBitmap |= 1ul << Id;
			ElantechReportV4State(DeviceExtension);
			return;

		case ELANTECH_PACKET_V4_MOTION:
			/* Two's complement deltas (process_packet_motion_v4) */
			Id = (LONG)((Packet[0] & 0xE0) >> 5) - 1;
			if (Id < 0 || Id >= ETP_MAX_FINGERS)
				return;

			Sid = (LONG)((Packet[3] & 0xE0) >> 5) - 1;
			Weight = (Packet[0] & 0x10) ? ETP_WEIGHT_VALUE : 1;

			Etd->FingerX[Id] += (LONG)(SCHAR)Packet[1] * Weight;
			/* FingerY is negated raw, same delta sign as Linux */
			Etd->FingerY[Id] -= (LONG)(SCHAR)Packet[2] * Weight;

			if (Sid >= 0 && Sid < ETP_MAX_FINGERS)
			{
				Etd->FingerX[Sid] += (LONG)(SCHAR)Packet[4] * Weight;
				Etd->FingerY[Sid] -= (LONG)(SCHAR)Packet[5] * Weight;
			}
			ElantechReportV4State(DeviceExtension);
			return;

		default:
			TRACE_(I8042PRT, "Elantech v4 packet failed check\n");
			return;
	}
}

static VOID
ElantechProcessPacket(
	IN PI8042_MOUSE_EXTENSION DeviceExtension)
{
	switch (DeviceExtension->ElantechData.HwVersion)
	{
		case 1:
			ElantechProcessPacketV1(DeviceExtension);
			break;
		case 2:
			ElantechProcessPacketV2(DeviceExtension);
			break;
		case 3:
			ElantechProcessPacketV3(DeviceExtension);
			break;
		case 4:
			ElantechProcessPacketV4(DeviceExtension);
			break;
		default:
			ASSERT(FALSE);
			break;
	}
}

/*
 * Byte-wise packet accumulator, called from the mouse ISR for every data
 * byte once the touchpad is initialized.
 */
VOID
ElantechMouHandle(
	IN PI8042_MOUSE_EXTENSION DeviceExtension,
	IN UCHAR Output)
{
	PELANTECH_EXTENSION Etd = &DeviceExtension->ElantechData;

	if (DeviceExtension->MouseState == MouseIdle)
	{
		/*
		 * Start of a packet. This also resynchronizes after an
		 * input timeout: i8042MouInputTestTimeout() resets
		 * MouseState to MouseIdle when bytes come in too slowly.
		 */
		Etd->PacketIndex = 0;
		DeviceExtension->MouseState = XMovement;
	}

	ASSERT(Etd->PacketIndex < Etd->PacketSize);
	Etd->Packet[Etd->PacketIndex++] = Output;
	if (Etd->PacketIndex < Etd->PacketSize)
		return;

	DeviceExtension->MouseState = MouseIdle;
	ElantechProcessPacket(DeviceExtension);
}
