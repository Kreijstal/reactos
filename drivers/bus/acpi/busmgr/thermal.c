/*
 *  thermal.c - ACPI Thermal Zone Driver (active cooling only)
 *
 *  Minimal active-cooling ("turn the fans on when it gets hot") support for
 *  the ReactOS ACPI bus manager, modeled after the Linux ACPI thermal driver
 *  (drivers/acpi/thermal.c):
 *
 *  Copyright (C) 2004 Luming Yu <luming.yu@intel.com>
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  Deliberately limited scope:
 *
 *   - Only *active* cooling (_ACx/_ALx -> fan power resources) is driven.
 *     Passive cooling (_PSV/_TC1/_TC2/_TSP throttling) and critical shutdown
 *     (_CRT/_HOT) are only reported, never acted upon: mis-reading a trip
 *     point must never be able to shut the machine down or throttle it.
 *   - Everything runs on one dedicated PASSIVE_LEVEL system thread, so AML
 *     evaluation (which on real hardware means EC transactions that can take
 *     hundreds of milliseconds) never blocks a PnP or notify path.
 *   - If a machine has no thermal zones, no _ACx trip points or no usable
 *     fans (the QEMU i440fx/q35 case), nothing at all is started and the
 *     driver stays completely silent past one boot-time summary line.
 */

#include <precomp.h>

#define NDEBUG
#include <debug.h>

#define _COMPONENT		ACPI_THERMAL_COMPONENT
ACPI_MODULE_NAME		("acpi_thermal")

#define ACPI_THERMAL_TAG		'TPCA'

/* ACPI allows _AC0.._AC9 / _AL0.._AL9 */
#define ACPI_THERMAL_MAX_ACTIVE		10

/* Hysteresis applied when dropping back below a trip point, in deciKelvin */
#define ACPI_THERMAL_HYSTERESIS		20	/* 2.0 K */

/* Polling bounds, in milliseconds.  _TZP is in deciseconds. */
#define ACPI_THERMAL_DEFAULT_POLL_MS	3000
#define ACPI_THERMAL_MIN_POLL_MS	1000
#define ACPI_THERMAL_MAX_POLL_MS	30000

/*
 * How far the temperature has to move from the last logged value before we
 * print it again.  This is a distance, not a bucket boundary, so a reading
 * hovering around one value can never chatter on the debug port.
 */
#define ACPI_THERMAL_LOG_DELTA		50	/* 5.0 K */

/* Absolute zero in deciKelvin; _TMP/_ACx/... are all deciKelvin */
#define ACPI_THERMAL_ZERO_C		2732

/*
 * Sanity window for any temperature we read out of AML.  Anything outside
 * this is treated as "the firmware/EC is not ready" rather than as a real
 * reading, so a bogus 0 or 0xFFFFFFFF can never engage or disengage a fan.
 */
#define ACPI_THERMAL_MIN_VALID		2000	/* -73.2 C */
#define ACPI_THERMAL_MAX_VALID		4732	/* 200.0 C */

/* --------------------------------------------------------------------------
                                  Structures
   -------------------------------------------------------------------------- */

/*
 * A cooling device (a PNP0C0B fan) referenced from some zone's _ALx list.
 * Fans are tracked globally, not per zone, because one fan may legitimately
 * appear in several _ALx lists of one zone (_AL0 and _AL1 both listing the
 * same fan is the common "one fan, several thresholds" layout) and in the
 * lists of several zones.  A fan is switched off only when the last trip
 * point referencing it has been released.
 */
struct acpi_thermal_fan {
	struct acpi_thermal_fan	*next;
	ACPI_HANDLE		handle;
	struct acpi_handle_list	power;		/* _PR0 resources */
	BOOLEAN			has_power_res;	/* _PR0 present and non-empty */
	BOOLEAN			has_ps;		/* _PS0/_PS3 fallback present */
	BOOLEAN			is_on;		/* we have driven it on */
	LONG			references;	/* trip points holding it on */
	char			name[5];
};

struct acpi_thermal_trip {
	unsigned long long	temperature;	/* _ACx, deciKelvin */
	struct acpi_thermal_fan	*fans[ACPI_MAX_HANDLES];
	UINT32			fan_count;
	BOOLEAN			valid;
	BOOLEAN			engaged;
};

struct acpi_thermal_zone {
	struct acpi_thermal_zone *next;
	ACPI_HANDLE		handle;
	struct acpi_device	*device;
	struct acpi_thermal_trip active[ACPI_THERMAL_MAX_ACTIVE];
	UINT32			active_count;	/* usable _ACx trip points */
	ULONG			poll_ms;
	unsigned long long	critical;	/* _CRT, 0 if absent */
	unsigned long long	hot;		/* _HOT, 0 if absent */
	unsigned long long	passive;	/* _PSV, 0 if absent */
	unsigned long long	temperature;	/* last valid _TMP */
	LONG			logged_temp;	/* last logged _TMP, -1 = none */
	volatile LONG		trips_dirty;	/* Notify(0x81) asked for a re-read */
	BOOLEAN			temperature_valid;
	BOOLEAN			read_failed;	/* _TMP failure already logged */
	BOOLEAN			critical_logged;
	BOOLEAN			notify_installed;
	char			name[5];
};

/*
 * Global state.  All of it is built during acpi_thermal_init() (single
 * threaded, PASSIVE_LEVEL, before the poll thread exists) and afterwards
 * only mutated by the poll thread, so no additional locking is needed;
 * the notify handler merely kicks an event.
 */
static struct acpi_thermal_zone	*acpi_thermal_zones;
static struct acpi_thermal_fan	*acpi_thermal_fans;
static UINT32			acpi_thermal_zone_count;	/* zones seen */
static UINT32			acpi_thermal_trip_count;	/* usable _ACx trips */
static ULONG			acpi_thermal_poll_ms = ACPI_THERMAL_DEFAULT_POLL_MS;

static KEVENT			acpi_thermal_wake;
static KEVENT			acpi_thermal_stopped;
static BOOLEAN			acpi_thermal_running;
static volatile LONG		acpi_thermal_stop;

static int acpi_thermal_add (struct acpi_device *device);
static int acpi_thermal_remove (struct acpi_device *device, int type);

static struct acpi_driver acpi_thermal_driver = {
    {0,0},
    ACPI_THERMAL_DRIVER_NAME,
    ACPI_THERMAL_CLASS,
    0,
    0,
    ACPI_THERMAL_HID,
    {acpi_thermal_add,acpi_thermal_remove}
};

/* --------------------------------------------------------------------------
                                    Helpers
   -------------------------------------------------------------------------- */

static void
acpi_thermal_get_name (
	ACPI_HANDLE		handle,
	char			*buffer /* [5] */)
{
	ACPI_BUFFER		name = { 5, buffer };

	buffer[0] = '?';
	buffer[1] = 0;

	if (ACPI_FAILURE(AcpiGetName(handle, ACPI_SINGLE_NAME, &name))) {
		buffer[0] = '?';
		buffer[1] = 0;
	}
	buffer[4] = 0;
}

static BOOLEAN
acpi_thermal_temp_valid (
	unsigned long long	value)
{
	return (value >= ACPI_THERMAL_MIN_VALID && value <= ACPI_THERMAL_MAX_VALID);
}

/* deciKelvin -> whole degrees Celsius, for logging only */
static LONG
acpi_thermal_celsius (
	unsigned long long	dk)
{
	return ((LONG)dk - ACPI_THERMAL_ZERO_C) / 10;
}

/* Tenths of a degree Celsius remainder, for logging only */
static LONG
acpi_thermal_celsius_frac (
	unsigned long long	dk)
{
	LONG			r = ((LONG)dk - ACPI_THERMAL_ZERO_C) % 10;

	return (r < 0) ? -r : r;
}

/*
 * Evaluate a temperature-valued object.  Returns TRUE only if the object
 * exists, evaluates to an integer and that integer is plausible.
 */
static BOOLEAN
acpi_thermal_get_temp (
	ACPI_HANDLE		handle,
	const char		*method,
	unsigned long long	*value)
{
	ACPI_STATUS		status;
	unsigned long long	raw = 0;

	status = acpi_evaluate_integer(handle, (ACPI_STRING)method, NULL, &raw);
	if (ACPI_FAILURE(status))
		return FALSE;

	if (!acpi_thermal_temp_valid(raw))
		return FALSE;

	*value = raw;
	return TRUE;
}

/* --------------------------------------------------------------------------
                               Cooling Devices
   -------------------------------------------------------------------------- */

static struct acpi_thermal_fan *
acpi_thermal_find_fan (
	ACPI_HANDLE		handle)
{
	struct acpi_thermal_fan	*fan;

	for (fan = acpi_thermal_fans; fan; fan = fan->next) {
		if (fan->handle == handle)
			return fan;
	}

	return NULL;
}

/*
 * Register a cooling device referenced from an _ALx package.  Returns NULL
 * if the device cannot actually be switched by us, in which case the caller
 * simply ignores it.
 */
static struct acpi_thermal_fan *
acpi_thermal_add_fan (
	ACPI_HANDLE		handle)
{
	struct acpi_thermal_fan	*fan;
	ACPI_HANDLE		method;
	ACPI_STATUS		status;

	fan = acpi_thermal_find_fan(handle);
	if (fan)
		return fan;

	fan = ExAllocatePoolWithTag(NonPagedPool, sizeof(*fan), ACPI_THERMAL_TAG);
	if (!fan)
		return NULL;

	RtlZeroMemory(fan, sizeof(*fan));
	fan->handle = handle;
	acpi_thermal_get_name(handle, fan->name);

	/*
	 * The normal case: the fan is switched by powering its _PR0 power
	 * resources up and down.
	 */
	status = acpi_evaluate_reference(handle, "_PR0", NULL, &fan->power);
	if (ACPI_SUCCESS(status) && fan->power.count)
		fan->has_power_res = TRUE;
	else
		fan->power.count = 0;

	/*
	 * Fallback for firmware that exposes the fan as a plain power-managed
	 * device without listing power resources.  Only used when both _PS0
	 * and _PS3 exist, so we can always undo what we did.
	 */
	if (!fan->has_power_res) {
		if (ACPI_SUCCESS(AcpiGetHandle(handle, "_PS0", &method)) &&
		    ACPI_SUCCESS(AcpiGetHandle(handle, "_PS3", &method)))
			fan->has_ps = TRUE;
	}

	if (!fan->has_power_res && !fan->has_ps) {
		/* Nothing we can drive - an always-on fan, most likely. */
		ExFreePoolWithTag(fan, ACPI_THERMAL_TAG);
		return NULL;
	}

	fan->next = acpi_thermal_fans;
	acpi_thermal_fans = fan;

	return fan;
}

/*
 * Actually switch a cooling device.  Never called from a notify or PnP
 * path - only from the thermal poll thread at PASSIVE_LEVEL.
 */
static void
acpi_thermal_set_fan (
	struct acpi_thermal_fan	*fan,
	BOOLEAN			on)
{
	ACPI_STATUS		status;
	UINT32			i;
	UINT32			failed = 0;

	if (fan->is_on == on)
		return;

	if (fan->has_power_res) {
		for (i = 0; i < fan->power.count; i++) {
			status = AcpiEvaluateObject(fan->power.handles[i],
				on ? "_ON" : "_OFF", NULL, NULL);
			if (ACPI_FAILURE(status))
				failed++;
		}
	}
	else {
		status = AcpiEvaluateObject(fan->handle,
			on ? "_PS0" : "_PS3", NULL, NULL);
		if (ACPI_FAILURE(status))
			failed++;
	}

	if (failed) {
		/*
		 * Log once per fan per direction change, not once per poll:
		 * an EC that has stopped answering would otherwise flood the
		 * debug port.  We still record the requested state so we do
		 * not retry on every single poll.
		 */
		DPRINT1("ACPI thermal: fan [%s] %s failed on %u resource(s)\n",
			fan->name, on ? "_ON" : "_OFF", failed);
	}

	fan->is_on = on;
}

static void
acpi_thermal_fan_reference (
	struct acpi_thermal_fan	*fan)
{
	fan->references++;
	if (fan->references == 1)
		acpi_thermal_set_fan(fan, TRUE);
}

static void
acpi_thermal_fan_dereference (
	struct acpi_thermal_fan	*fan)
{
	if (fan->references > 0)
		fan->references--;

	if (fan->references == 0)
		acpi_thermal_set_fan(fan, FALSE);
}

/* --------------------------------------------------------------------------
                                 Trip Points
   -------------------------------------------------------------------------- */

/*
 * Read (or re-read, on Notify 0x81) the _ACx/_ALx trip points of a zone.
 * _ALx is only read the first time a trip point becomes valid: rebuilding
 * the cooling-device lists while fans are engaged would lose the reference
 * counts, and firmware is not allowed to change _ALx at runtime anyway.
 */
static void
acpi_thermal_get_trip_points (
	struct acpi_thermal_zone *tz,
	BOOLEAN			first_time)
{
	char			method[5];
	UINT32			i, j;
	unsigned long long	temp;
	struct acpi_handle_list	list;
	struct acpi_thermal_fan	*fan;

	tz->active_count = 0;

	for (i = 0; i < ACPI_THERMAL_MAX_ACTIVE; i++) {
		struct acpi_thermal_trip *trip = &tz->active[i];

		method[0] = '_'; method[1] = 'A'; method[2] = 'C';
		method[3] = (char)('0' + i); method[4] = 0;

		if (!acpi_thermal_get_temp(tz->handle, method, &temp)) {
			/*
			 * A trip point that used to exist and now does not is
			 * simply disabled; any fan it holds is released by the
			 * caller's next evaluation pass.
			 */
			if (!first_time && trip->valid && !trip->engaged)
				trip->valid = FALSE;
			else if (first_time)
				trip->valid = FALSE;
			continue;
		}

		trip->temperature = temp;

		if (!first_time && trip->valid) {
			/* Threshold updated in place; keep the fan bindings. */
			tz->active_count++;
			continue;
		}

		/* Resolve _ALx once. */
		method[1] = 'A'; method[2] = 'L';
		RtlZeroMemory(&list, sizeof(list));

		if (ACPI_FAILURE(acpi_evaluate_reference(tz->handle,
				method, NULL, &list)) || !list.count)
			continue;

		trip->fan_count = 0;
		for (j = 0; j < list.count && j < ACPI_MAX_HANDLES; j++) {
			fan = acpi_thermal_add_fan(list.handles[j]);
			if (fan)
				trip->fans[trip->fan_count++] = fan;
		}

		if (!trip->fan_count)
			continue;

		trip->valid = TRUE;
		trip->engaged = FALSE;
		tz->active_count++;
	}
}

static void
acpi_thermal_engage (
	struct acpi_thermal_zone *tz,
	UINT32			index)
{
	struct acpi_thermal_trip *trip = &tz->active[index];
	UINT32			i;

	for (i = 0; i < trip->fan_count; i++)
		acpi_thermal_fan_reference(trip->fans[i]);

	trip->engaged = TRUE;

	DPRINT1("ACPI thermal: [%s] %ld.%ldC >= _AC%u (%ld.%ldC), cooling ON\n",
		tz->name,
		acpi_thermal_celsius(tz->temperature),
		acpi_thermal_celsius_frac(tz->temperature),
		index,
		acpi_thermal_celsius(trip->temperature),
		acpi_thermal_celsius_frac(trip->temperature));
}

static void
acpi_thermal_disengage (
	struct acpi_thermal_zone *tz,
	UINT32			index)
{
	struct acpi_thermal_trip *trip = &tz->active[index];
	UINT32			i;

	for (i = 0; i < trip->fan_count; i++)
		acpi_thermal_fan_dereference(trip->fans[i]);

	trip->engaged = FALSE;

	DPRINT1("ACPI thermal: [%s] %ld.%ldC < _AC%u (%ld.%ldC), cooling OFF\n",
		tz->name,
		acpi_thermal_celsius(tz->temperature),
		acpi_thermal_celsius_frac(tz->temperature),
		index,
		acpi_thermal_celsius(trip->temperature),
		acpi_thermal_celsius_frac(trip->temperature));
}

/* --------------------------------------------------------------------------
                                Evaluation Pass
   -------------------------------------------------------------------------- */

static void
acpi_thermal_check (
	struct acpi_thermal_zone *tz)
{
	unsigned long long	temp = 0;
	UINT32			i;
	LONG			delta;

	if (InterlockedExchange(&tz->trips_dirty, 0)) {
		acpi_thermal_get_trip_points(tz, FALSE);
		DPRINT1("ACPI thermal: [%s] trip points re-read, %u active\n",
			tz->name, tz->active_count);
	}

	if (!acpi_thermal_get_temp(tz->handle, "_TMP", &temp)) {
		/*
		 * Read failures are logged exactly once.  A zone we cannot
		 * read is left exactly as it is: any fan already engaged
		 * stays engaged, which is the safe direction.
		 */
		if (!tz->read_failed) {
			tz->read_failed = TRUE;
			DPRINT1("ACPI thermal: [%s] _TMP unreadable, zone idle\n",
				tz->name);
		}
		return;
	}

	if (tz->read_failed) {
		tz->read_failed = FALSE;
		DPRINT1("ACPI thermal: [%s] _TMP readable again\n", tz->name);
	}

	tz->temperature = temp;
	tz->temperature_valid = TRUE;

	/*
	 * Report a crossing of _CRT once.  We deliberately do NOT shut the
	 * machine down: a mis-parsed trip point must not be able to power
	 * off a working system, and the hardware THERMTRIP is the real
	 * last line of defence.
	 */
	if (tz->critical && temp >= tz->critical && !tz->critical_logged) {
		tz->critical_logged = TRUE;
		DPRINT1("ACPI thermal: [%s] CRITICAL temperature %ld.%ldC reached (_CRT %ldC)\n",
			tz->name,
			acpi_thermal_celsius(temp), acpi_thermal_celsius_frac(temp),
			acpi_thermal_celsius(tz->critical));
	}
	else if (tz->critical && temp < tz->critical) {
		tz->critical_logged = FALSE;
	}

	for (i = 0; i < ACPI_THERMAL_MAX_ACTIVE; i++) {
		struct acpi_thermal_trip *trip = &tz->active[i];

		if (!trip->valid) {
			/* Trip point vanished on a re-read: release its fans. */
			if (trip->engaged)
				acpi_thermal_disengage(tz, i);
			continue;
		}

		if (!trip->engaged) {
			if (temp >= trip->temperature)
				acpi_thermal_engage(tz, i);
		}
		else {
			if (temp + ACPI_THERMAL_HYSTERESIS <= trip->temperature)
				acpi_thermal_disengage(tz, i);
		}
	}

	/*
	 * Throttled temperature logging: one line per 5 K of movement away
	 * from the last logged value, never one per poll.  In the steady
	 * state this prints nothing at all - each DPRINT costs roughly 10 ms
	 * on a serial/KDNET debug port.
	 */
	if (tz->logged_temp < 0) {
		tz->logged_temp = (LONG)temp;
	}
	else {
		delta = (LONG)temp - tz->logged_temp;
		if (delta < 0)
			delta = -delta;

		if (delta >= ACPI_THERMAL_LOG_DELTA) {
			tz->logged_temp = (LONG)temp;
			DPRINT1("ACPI thermal: [%s] %ld.%ldC\n", tz->name,
				acpi_thermal_celsius(temp),
				acpi_thermal_celsius_frac(temp));
		}
	}
}

/* --------------------------------------------------------------------------
                                 Poll Thread
   -------------------------------------------------------------------------- */

static VOID NTAPI
acpi_thermal_poll_thread (
	PVOID			context)
{
	struct acpi_thermal_zone *tz;
	LARGE_INTEGER		timeout;

	UNREFERENCED_PARAMETER(context);

	timeout.QuadPart = -((LONGLONG)acpi_thermal_poll_ms * 10000);

	while (!acpi_thermal_stop) {
		for (tz = acpi_thermal_zones; tz; tz = tz->next)
			acpi_thermal_check(tz);

		if (acpi_thermal_stop)
			break;

		/*
		 * Wait for the poll interval, or until a Notify(0x80/0x81)
		 * on any zone kicks us early.
		 */
		KeWaitForSingleObject(&acpi_thermal_wake, Executive,
			KernelMode, FALSE, &timeout);
	}

	KeSetEvent(&acpi_thermal_stopped, IO_NO_INCREMENT, FALSE);

	PsTerminateSystemThread(STATUS_SUCCESS);
}

/* --------------------------------------------------------------------------
                                    Notify
   -------------------------------------------------------------------------- */

/*
 * Runs on an ACPICA notify worker.  Does no AML evaluation at all - it
 * only records what has to be re-read and wakes the poll thread.
 */
static void
acpi_thermal_notify (
	ACPI_HANDLE		handle,
	UINT32			event,
	void			*data)
{
	struct acpi_thermal_zone *tz = (struct acpi_thermal_zone *) data;

	UNREFERENCED_PARAMETER(handle);

	if (!tz)
		return;

	switch (event) {
	case ACPI_THERMAL_NOTIFY_TEMPERATURE:
		break;

	case ACPI_THERMAL_NOTIFY_THRESHOLDS:
		/*
		 * Trip points changed.  Re-reading them means running AML,
		 * so it is only flagged here and done by the poll thread.
		 */
		InterlockedExchange(&tz->trips_dirty, 1);
		break;

	default:
		return;
	}

	if (acpi_thermal_running)
		KeSetEvent(&acpi_thermal_wake, IO_NO_INCREMENT, FALSE);
}

/* --------------------------------------------------------------------------
                                Driver Interface
   -------------------------------------------------------------------------- */

static int
acpi_thermal_add (
	struct acpi_device	*device)
{
	struct acpi_thermal_zone *tz;
	unsigned long long	value = 0;
	ACPI_STATUS		status;

	ACPI_FUNCTION_TRACE("acpi_thermal_add");

	if (!device)
		return_VALUE(-1);

	tz = ExAllocatePoolWithTag(NonPagedPool, sizeof(*tz), ACPI_THERMAL_TAG);
	if (!tz)
		return_VALUE(-12);

	acpi_thermal_zone_count++;

	RtlZeroMemory(tz, sizeof(*tz));
	tz->device = device;
	tz->handle = device->handle;
	tz->poll_ms = ACPI_THERMAL_DEFAULT_POLL_MS;
	tz->logged_temp = -1;
	acpi_thermal_get_name(tz->handle, tz->name);

	sprintf(acpi_device_name(device), "%s", ACPI_THERMAL_DEVICE_NAME);
	sprintf(acpi_device_class(device), "%s", ACPI_THERMAL_CLASS);

	/* Reference trip points - reported only, never acted upon. */
	if (acpi_thermal_get_temp(tz->handle, "_CRT", &value))
		tz->critical = value;
	if (acpi_thermal_get_temp(tz->handle, "_HOT", &value))
		tz->hot = value;
	if (acpi_thermal_get_temp(tz->handle, "_PSV", &value))
		tz->passive = value;

	/* _TZP is a polling period in deciseconds; 0 means "event driven". */
	status = acpi_evaluate_integer(tz->handle, "_TZP", NULL, &value);
	if (ACPI_SUCCESS(status) && value) {
		ULONG ms = (ULONG)((value > 0xFFFF) ? 0xFFFF : value) * 100;

		if (ms < ACPI_THERMAL_MIN_POLL_MS)
			ms = ACPI_THERMAL_MIN_POLL_MS;
		else if (ms > ACPI_THERMAL_MAX_POLL_MS)
			ms = ACPI_THERMAL_MAX_POLL_MS;

		tz->poll_ms = ms;
	}

	acpi_thermal_get_trip_points(tz, TRUE);

	if (!tz->active_count) {
		/*
		 * A zone with no usable _ACx/_ALx pair gives us nothing to
		 * drive.  Not an error and not worth a release-build line -
		 * the boot summary already reports it as a zone we do not
		 * count trip points for.
		 */
		DPRINT("ACPI thermal: [%s] no usable active trip points, ignored\n",
			tz->name);
		ExFreePoolWithTag(tz, ACPI_THERMAL_TAG);
		return_VALUE(-19);
	}

	status = AcpiInstallNotifyHandler(tz->handle, ACPI_DEVICE_NOTIFY,
		acpi_thermal_notify, tz);
	if (ACPI_SUCCESS(status))
		tz->notify_installed = TRUE;
	else
		DPRINT1("ACPI thermal: [%s] notify handler failed: %s\n",
			tz->name, AcpiFormatException(status));

	acpi_driver_data(device) = tz;

	tz->next = acpi_thermal_zones;
	acpi_thermal_zones = tz;
	acpi_thermal_trip_count += tz->active_count;

	if (tz->poll_ms < acpi_thermal_poll_ms)
		acpi_thermal_poll_ms = tz->poll_ms;

	DPRINT1("ACPI thermal: [%s] %u active trip(s), poll %lums, "
		"_PSV %ldC _HOT %ldC _CRT %ldC\n",
		tz->name, tz->active_count, tz->poll_ms,
		tz->passive ? acpi_thermal_celsius(tz->passive) : -1,
		tz->hot ? acpi_thermal_celsius(tz->hot) : -1,
		tz->critical ? acpi_thermal_celsius(tz->critical) : -1);

	return_VALUE(0);
}

static int
acpi_thermal_remove (
	struct acpi_device	*device,
	int			type)
{
	UNREFERENCED_PARAMETER(type);

	/*
	 * Thermal zones are not removable and the poll thread holds raw
	 * pointers to our zone list, so just drop the binding and keep
	 * cooling the machine.  Teardown happens in acpi_thermal_exit().
	 */
	if (device)
		acpi_driver_data(device) = NULL;

	return_VALUE(0);
}

/* --------------------------------------------------------------------------
                             Initialization/Cleanup
   -------------------------------------------------------------------------- */

int
acpi_thermal_init (void)
{
	NTSTATUS		nt_status;
	HANDLE			handle;
	OBJECT_ATTRIBUTES	attributes;

	ACPI_FUNCTION_TRACE("acpi_thermal_init");

	KeInitializeEvent(&acpi_thermal_wake, SynchronizationEvent, FALSE);
	KeInitializeEvent(&acpi_thermal_stopped, NotificationEvent, FALSE);

	acpi_bus_register_driver(&acpi_thermal_driver);

	/*
	 * The one unconditional line this driver prints.  On a machine with
	 * no thermal zones (QEMU i440fx/q35) this is all that ever appears
	 * and nothing further is started.
	 */
	DPRINT1("ACPI thermal: %u zones, %u active trip points\n",
		acpi_thermal_zone_count, acpi_thermal_trip_count);

	if (!acpi_thermal_trip_count)
		return_VALUE(0);

	InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

	nt_status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &attributes,
		NULL, NULL, acpi_thermal_poll_thread, NULL);
	if (!NT_SUCCESS(nt_status)) {
		DPRINT1("ACPI thermal: unable to start poll thread: 0x%08lx\n",
			nt_status);
		return_VALUE(0);
	}

	ZwClose(handle);

	acpi_thermal_running = TRUE;

	return_VALUE(0);
}

void
acpi_thermal_exit (void)
{
	struct acpi_thermal_zone *tz, *tz_next;
	struct acpi_thermal_fan	*fan, *fan_next;

	ACPI_FUNCTION_TRACE("acpi_thermal_exit");

	if (acpi_thermal_running) {
		InterlockedExchange(&acpi_thermal_stop, 1);
		KeSetEvent(&acpi_thermal_wake, IO_NO_INCREMENT, FALSE);
		KeWaitForSingleObject(&acpi_thermal_stopped, Executive,
			KernelMode, FALSE, NULL);
		acpi_thermal_running = FALSE;
	}

	acpi_bus_unregister_driver(&acpi_thermal_driver);

	for (tz = acpi_thermal_zones; tz; tz = tz_next) {
		tz_next = tz->next;
		if (tz->notify_installed)
			AcpiRemoveNotifyHandler(tz->handle, ACPI_DEVICE_NOTIFY,
				acpi_thermal_notify);
		ExFreePoolWithTag(tz, ACPI_THERMAL_TAG);
	}
	acpi_thermal_zones = NULL;

	/*
	 * Leave the fans in whatever state they are in: switching them off
	 * on shutdown would be actively harmful on a hot machine.
	 */
	for (fan = acpi_thermal_fans; fan; fan = fan_next) {
		fan_next = fan->next;
		ExFreePoolWithTag(fan, ACPI_THERMAL_TAG);
	}
	acpi_thermal_fans = NULL;

	acpi_thermal_zone_count = 0;
	acpi_thermal_trip_count = 0;
	acpi_thermal_poll_ms = ACPI_THERMAL_DEFAULT_POLL_MS;

	return_VOID;
}
