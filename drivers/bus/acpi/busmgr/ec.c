/*
 *  ec.c - ACPI Embedded Controller Driver
 *
 *  Polled Embedded Controller transaction engine and SCI query dispatch
 *  for the ReactOS ACPI bus manager, modeled after the Linux ACPI EC
 *  driver (drivers/acpi/ec.c):
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
 */

#include <precomp.h>

#define NDEBUG
#include <debug.h>

#define _COMPONENT		ACPI_EC_COMPONENT
ACPI_MODULE_NAME		("acpi_ec")

/* EC status register bits */
#define ACPI_EC_FLAG_OBF	0x01	/* Output buffer full */
#define ACPI_EC_FLAG_IBF	0x02	/* Input buffer full */
#define ACPI_EC_FLAG_CMD	0x08	/* Input buffer contains a command */
#define ACPI_EC_FLAG_BURST	0x10	/* Burst mode */
#define ACPI_EC_FLAG_SCI	0x20	/* EC-SCI occurred */
#define ACPI_EC_FLAG_SMI	0x40	/* EC-SMI occurred */

/* EC commands */
#define ACPI_EC_COMMAND_READ	0x80	/* RD_EC */
#define ACPI_EC_COMMAND_WRITE	0x81	/* WR_EC */
#define ACPI_EC_BURST_ENABLE	0x82	/* BE_EC */
#define ACPI_EC_BURST_DISABLE	0x83	/* BD_EC */
#define ACPI_EC_COMMAND_QUERY	0x84	/* QR_EC */

/* Overall timeout for a single handshake step (matches Linux ACPI_EC_DELAY) */
#define ACPI_EC_DELAY_US	500000
/* Busy-poll period and how long to busy-poll before yielding the CPU */
#define ACPI_EC_POLL_PERIOD_US	50
#define ACPI_EC_POLL_BUSY_US	5000
/* Timeout (ms) for acquiring the ACPI global lock for a transaction */
#define ACPI_EC_GLK_TIMEOUT	1000
/* Guard against SCI query storms */
#define ACPI_EC_MAX_QUERIES	16

#define ACPI_EC_TAG		'cEpA'

struct acpi_ec {
	ACPI_HANDLE		handle;		/* EC namespace node, or
						 * ACPI_ROOT_OBJECT for an ECDT
						 * EC whose path is unresolved */
	ULONG			gpe;
	BOOLEAN			gpe_valid;
	ULONG			data_addr;	/* Data register (e.g. 0x62) */
	ULONG			command_addr;	/* Command/status register (e.g. 0x66) */
	BOOLEAN			global_lock;	/* _GLK: transactions need the global lock */
	BOOLEAN			from_ecdt;
	KMUTEX			transaction_lock;
	LONG			query_pending;
};

/*
 * The boot/first EC.  Every EmbeddedControl OpRegion in the tables is
 * serviced by this EC; machines with more than one EC are not supported
 * (neither are they by Windows without vendor filter drivers).
 */
static struct acpi_ec *first_ec;

static int acpi_ec_add (struct acpi_device *device);
static int acpi_ec_remove (struct acpi_device *device, int type);

static struct acpi_driver acpi_ec_driver = {
    {0,0},
    ACPI_EC_DRIVER_NAME,
    ACPI_EC_CLASS,
    0,
    0,
    "PNP0C09",
    {acpi_ec_add,acpi_ec_remove}
};

/* --------------------------------------------------------------------------
                             Transaction Management
   -------------------------------------------------------------------------- */

static UCHAR
acpi_ec_read_status (
	struct acpi_ec		*ec)
{
	return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)ec->command_addr);
}

static UCHAR
acpi_ec_read_data_reg (
	struct acpi_ec		*ec)
{
	return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)ec->data_addr);
}

static void
acpi_ec_write_command (
	struct acpi_ec		*ec,
	UCHAR			command)
{
	WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)ec->command_addr, command);
}

static void
acpi_ec_write_data_reg (
	struct acpi_ec		*ec,
	UCHAR			data)
{
	WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)ec->data_addr, data);
}

/*
 * Wait until the status register bits selected by 'mask' read as 'value'.
 * Busy-polls for the first ACPI_EC_POLL_BUSY_US, then sleeps in 1ms steps
 * (when the thread context allows it) up to ACPI_EC_DELAY_US total.
 */
static ACPI_STATUS
acpi_ec_wait (
	struct acpi_ec		*ec,
	UCHAR			mask,
	UCHAR			value)
{
	ULONG			elapsed_us = 0;

	while ((acpi_ec_read_status(ec) & mask) != value) {
		if (elapsed_us >= ACPI_EC_DELAY_US)
			return_ACPI_STATUS(AE_TIME);

		if (elapsed_us < ACPI_EC_POLL_BUSY_US) {
			KeStallExecutionProcessor(ACPI_EC_POLL_PERIOD_US);
			elapsed_us += ACPI_EC_POLL_PERIOD_US;
		}
		else if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
			LARGE_INTEGER interval;
			interval.QuadPart = -10 * 1000;	/* 1 ms */
			KeDelayExecutionThread(KernelMode, FALSE, &interval);
			elapsed_us += 1000;
		}
		else {
			KeStallExecutionProcessor(1000);
			elapsed_us += 1000;
		}
	}

	return_ACPI_STATUS(AE_OK);
}

static ACPI_STATUS
acpi_ec_transaction_unlocked (
	struct acpi_ec		*ec,
	UCHAR			command,
	const UCHAR		*wdata,
	ULONG			wdata_len,
	UCHAR			*rdata,
	ULONG			rdata_len)
{
	ACPI_STATUS		status;
	ULONG			i;

	/* Wait for the input buffer to drain, then send the command */
	status = acpi_ec_wait(ec, ACPI_EC_FLAG_IBF, 0);
	if (ACPI_FAILURE(status))
		return_ACPI_STATUS(status);
	acpi_ec_write_command(ec, command);

	for (i = 0; i < wdata_len; i++) {
		status = acpi_ec_wait(ec, ACPI_EC_FLAG_IBF, 0);
		if (ACPI_FAILURE(status))
			return_ACPI_STATUS(status);
		acpi_ec_write_data_reg(ec, wdata[i]);
	}

	for (i = 0; i < rdata_len; i++) {
		status = acpi_ec_wait(ec, ACPI_EC_FLAG_OBF, ACPI_EC_FLAG_OBF);
		if (ACPI_FAILURE(status))
			return_ACPI_STATUS(status);
		rdata[i] = acpi_ec_read_data_reg(ec);
	}

	return_ACPI_STATUS(AE_OK);
}

static ACPI_STATUS
acpi_ec_transaction (
	struct acpi_ec		*ec,
	UCHAR			command,
	const UCHAR		*wdata,
	ULONG			wdata_len,
	UCHAR			*rdata,
	ULONG			rdata_len)
{
	ACPI_STATUS		status;
	UINT32			glk_handle = 0;

	/*
	 * The handshake needs to poll and sleep with a bounded but long
	 * timeout; EmbeddedControl OpRegion accesses and SCI queries all
	 * come from AML executed at PASSIVE_LEVEL worker/PnP threads.
	 */
	if (KeGetCurrentIrql() > APC_LEVEL) {
		DPRINT1("EC transaction rejected at IRQL %u\n", KeGetCurrentIrql());
		return_ACPI_STATUS(AE_ERROR);
	}

	KeWaitForSingleObject(&ec->transaction_lock, Executive, KernelMode,
		FALSE, NULL);

	if (ec->global_lock) {
		status = AcpiAcquireGlobalLock(ACPI_EC_GLK_TIMEOUT, &glk_handle);
		if (ACPI_FAILURE(status)) {
			KeReleaseMutex(&ec->transaction_lock, FALSE);
			return_ACPI_STATUS(status);
		}
	}

	status = acpi_ec_transaction_unlocked(ec, command, wdata, wdata_len,
		rdata, rdata_len);

	if (ec->global_lock)
		AcpiReleaseGlobalLock(glk_handle);

	KeReleaseMutex(&ec->transaction_lock, FALSE);

	return_ACPI_STATUS(status);
}

static ACPI_STATUS
acpi_ec_read (
	struct acpi_ec		*ec,
	UCHAR			address,
	UCHAR			*data)
{
	return acpi_ec_transaction(ec, ACPI_EC_COMMAND_READ,
		&address, 1, data, 1);
}

static ACPI_STATUS
acpi_ec_write (
	struct acpi_ec		*ec,
	UCHAR			address,
	UCHAR			data)
{
	UCHAR			wdata[2];

	wdata[0] = address;
	wdata[1] = data;

	return acpi_ec_transaction(ec, ACPI_EC_COMMAND_WRITE,
		wdata, 2, NULL, 0);
}

static ACPI_STATUS
acpi_ec_query (
	struct acpi_ec		*ec,
	UCHAR			*value)
{
	return acpi_ec_transaction(ec, ACPI_EC_COMMAND_QUERY,
		NULL, 0, value, 1);
}

/* --------------------------------------------------------------------------
                              Event (SCI) Handling
   -------------------------------------------------------------------------- */

/*
 * Runs at PASSIVE_LEVEL via AcpiOsExecute.  Drains the EC query queue:
 * as long as SCI_EVT is set, ask the EC which _Qxx event fired and
 * evaluate the corresponding handler below the EC device.
 */
static void
acpi_ec_query_worker (
	void			*context)
{
	struct acpi_ec		*ec = context;
	ULONG			i;

	for (i = 0; i < ACPI_EC_MAX_QUERIES; i++) {
		ACPI_STATUS	status;
		UCHAR		query = 0;
		char		method[8];

		if (!(acpi_ec_read_status(ec) & ACPI_EC_FLAG_SCI))
			break;

		status = acpi_ec_query(ec, &query);
		if (ACPI_FAILURE(status)) {
			DPRINT1("EC query transaction failed: %s\n",
				AcpiFormatException(status));
			break;
		}

		/* Query value 0 means "no outstanding event" */
		if (!query)
			break;

		sprintf(method, "_Q%02X", query);
		DPRINT("EC: dispatching query %s\n", method);

		if (ec->handle && ec->handle != ACPI_ROOT_OBJECT) {
			status = AcpiEvaluateObject(ec->handle, method, NULL, NULL);
			if (ACPI_FAILURE(status) && status != AE_NOT_FOUND)
				DPRINT1("EC: evaluation of %s failed: %s\n",
					method, AcpiFormatException(status));
		}
		else {
			DPRINT1("EC: no namespace node for query %s\n", method);
		}
	}

	InterlockedExchange(&ec->query_pending, 0);
}

/*
 * Runs at DISPATCH_LEVEL from the deferred SCI dispatcher.  Just checks
 * for a pending EC-SCI and defers the actual QR_EC transaction + _Qxx
 * evaluation to PASSIVE_LEVEL.
 */
static UINT32
acpi_ec_gpe_handler (
	ACPI_HANDLE		gpe_device,
	UINT32			gpe_number,
	void			*context)
{
	struct acpi_ec		*ec = context;

	if ((acpi_ec_read_status(ec) & ACPI_EC_FLAG_SCI) &&
	    InterlockedCompareExchange(&ec->query_pending, 1, 0) == 0) {
		if (ACPI_FAILURE(AcpiOsExecute(OSL_GPE_HANDLER,
				acpi_ec_query_worker, ec))) {
			InterlockedExchange(&ec->query_pending, 0);
		}
	}

	return ACPI_INTERRUPT_HANDLED | ACPI_REENABLE_GPE;
}

/* --------------------------------------------------------------------------
                             Address Space Management
   -------------------------------------------------------------------------- */

static ACPI_STATUS
acpi_ec_space_handler (
	UINT32			function,
	ACPI_PHYSICAL_ADDRESS	address,
	UINT32			bit_width,
	UINT64			*value,
	void			*handler_context,
	void			*region_context)
{
	struct acpi_ec		*ec = handler_context;
	ACPI_STATUS		status = AE_OK;
	UCHAR			*bytes = (UCHAR *)value;
	ULONG			count;
	ULONG			i;

	if (!ec || !value || (bit_width % 8) != 0 || bit_width == 0)
		return_ACPI_STATUS(AE_BAD_PARAMETER);

	count = bit_width / 8;
	if (address + count > 0x100)
		return_ACPI_STATUS(AE_BAD_PARAMETER);

	if (function == ACPI_READ)
		*value = 0;

	for (i = 0; i < count; i++) {
		if (function == ACPI_READ)
			status = acpi_ec_read(ec, (UCHAR)(address + i), &bytes[i]);
		else if (function == ACPI_WRITE)
			status = acpi_ec_write(ec, (UCHAR)(address + i), bytes[i]);
		else
			status = AE_BAD_PARAMETER;

		if (ACPI_FAILURE(status))
			break;
	}

	return_ACPI_STATUS(status);
}

/* --------------------------------------------------------------------------
                                   Setup
   -------------------------------------------------------------------------- */

static ACPI_STATUS
acpi_ec_install_handlers (
	struct acpi_ec		*ec)
{
	ACPI_STATUS		status;

	/*
	 * Install the EmbeddedControl address space handler first so any
	 * _REG/_Qxx AML triggered by the GPE below can already reach the EC.
	 */
	status = AcpiInstallAddressSpaceHandler(ec->handle,
		ACPI_ADR_SPACE_EC, acpi_ec_space_handler, NULL, ec);
	if (ACPI_FAILURE(status)) {
		DPRINT1("EC: failed to install address space handler: %s\n",
			AcpiFormatException(status));
		return_ACPI_STATUS(status);
	}

	if (ec->gpe_valid) {
		status = AcpiInstallGpeHandler(NULL, ec->gpe,
			ACPI_GPE_EDGE_TRIGGERED, acpi_ec_gpe_handler, ec);
		if (ACPI_SUCCESS(status)) {
			status = AcpiEnableGpe(NULL, ec->gpe);
			if (ACPI_FAILURE(status))
				DPRINT1("EC: failed to enable GPE 0x%lx: %s\n",
					ec->gpe, AcpiFormatException(status));
		}
		else {
			DPRINT1("EC: failed to install GPE 0x%lx handler: %s\n",
				ec->gpe, AcpiFormatException(status));
			/* Not fatal: the EC still works for OpRegion access */
			ec->gpe_valid = FALSE;
		}
	}

	return_ACPI_STATUS(AE_OK);
}

static void
acpi_ec_check_global_lock (
	struct acpi_ec		*ec)
{
	ACPI_STATUS		status;
	unsigned long long	glk = 0;

	if (!ec->handle || ec->handle == ACPI_ROOT_OBJECT)
		return;

	status = acpi_evaluate_integer(ec->handle, "_GLK", NULL, &glk);
	if (ACPI_SUCCESS(status) && glk)
		ec->global_lock = TRUE;
}

static struct acpi_ec *
acpi_ec_alloc (void)
{
	struct acpi_ec		*ec;

	ec = ExAllocatePoolWithTag(NonPagedPool, sizeof(struct acpi_ec),
		ACPI_EC_TAG);
	if (!ec)
		return NULL;
	memset(ec, 0, sizeof(struct acpi_ec));

	KeInitializeMutex(&ec->transaction_lock, 0);

	return ec;
}

/*
 * Boot-time probe: ACPI 2.0+ platforms describe the EC in the ECDT so the
 * OS can service EmbeddedControl OpRegions before (and while) the namespace
 * device enumeration runs.  Called after the namespace is loaded but before
 * AcpiInitializeObjects, mirroring the Linux boot flow.
 */
int
acpi_ec_ecdt_probe (void)
{
	ACPI_TABLE_ECDT		*ecdt;
	ACPI_STATUS		status;
	struct acpi_ec		*ec;
	ACPI_HANDLE		handle;

	status = AcpiGetTable(ACPI_SIG_ECDT, 1, (ACPI_TABLE_HEADER **)&ecdt);
	if (ACPI_FAILURE(status)) {
		/* Not having an ECDT is not fatal */
		return 0;
	}

	if (!ecdt->Control.Address || !ecdt->Data.Address) {
		DPRINT1("EC: invalid ECDT register addresses\n");
		return 0;
	}

	ec = acpi_ec_alloc();
	if (!ec)
		return -12;

	ec->command_addr = (ULONG)ecdt->Control.Address;
	ec->data_addr = (ULONG)ecdt->Data.Address;
	ec->gpe = ecdt->Gpe;
	ec->gpe_valid = TRUE;
	ec->from_ecdt = TRUE;
	ec->handle = ACPI_ROOT_OBJECT;

	/*
	 * Resolve the namespace path from the ECDT so EC queries can invoke
	 * the _Qxx methods scoped below the EC device.  Install the address
	 * space handler at the root either way: the boot EC services every
	 * EmbeddedControl OpRegion in the tables.
	 */
	if (ecdt->Id[0] != '\0' &&
	    ACPI_SUCCESS(AcpiGetHandle(NULL, (char *)ecdt->Id, &handle))) {
		ec->handle = handle;
		acpi_ec_check_global_lock(ec);
	}

	status = acpi_ec_install_handlers(ec);
	if (ACPI_FAILURE(status)) {
		ExFreePoolWithTag(ec, ACPI_EC_TAG);
		return 0;
	}

	first_ec = ec;

	DPRINT1("ACPI: EC from ECDT: data 0x%lx, cmd/status 0x%lx, GPE 0x%lx, path %s\n",
		ec->data_addr, ec->command_addr, ec->gpe, (char *)ecdt->Id);

	return 0;
}

/* --------------------------------------------------------------------------
                                Driver Interface
   -------------------------------------------------------------------------- */

static ACPI_STATUS
acpi_ec_io_resource (
	ACPI_RESOURCE		*resource,
	void			*context)
{
	struct acpi_ec		*ec = context;
	ULONG			address;

	switch (resource->Type) {
	case ACPI_RESOURCE_TYPE_IO:
		address = resource->Data.Io.Minimum;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		address = resource->Data.FixedIo.Address;
		break;
	default:
		return AE_OK;
	}

	/* First I/O port is the data register, second is command/status */
	if (!ec->data_addr)
		ec->data_addr = address;
	else if (!ec->command_addr)
		ec->command_addr = address;

	return AE_OK;
}

static int
acpi_ec_add (
	struct acpi_device	*device)
{
	ACPI_STATUS		status;
	struct acpi_ec		*ec;
	unsigned long long	val = 0;

	if (!device)
		return_VALUE(-1);

	sprintf(acpi_device_name(device), "%s", ACPI_EC_DEVICE_NAME);
	sprintf(acpi_device_class(device), "%s", ACPI_EC_CLASS);

	/*
	 * If the boot EC from the ECDT is already up, don't set up a second
	 * transaction engine; just bind the namespace node so SCI queries
	 * can evaluate the _Qxx methods scoped below the EC device.
	 */
	if (first_ec) {
		if (first_ec->from_ecdt &&
		    first_ec->handle == ACPI_ROOT_OBJECT) {
			first_ec->handle = device->handle;
			acpi_ec_check_global_lock(first_ec);
			DPRINT1("ACPI: EC: ECDT EC bound to namespace node [%s]\n",
				acpi_device_bid(device));
		}
		acpi_driver_data(device) = first_ec;
		return_VALUE(0);
	}

	ec = acpi_ec_alloc();
	if (!ec)
		return_VALUE(-12);

	ec->handle = device->handle;

	/* _CRS: two I/O ports, data register first, then command/status */
	status = AcpiWalkResources(device->handle, METHOD_NAME__CRS,
		acpi_ec_io_resource, ec);
	if (ACPI_FAILURE(status) || !ec->data_addr || !ec->command_addr) {
		DPRINT1("EC: failed to locate I/O ports via _CRS: %s\n",
			AcpiFormatException(status));
		ExFreePoolWithTag(ec, ACPI_EC_TAG);
		return_VALUE(-15);
	}

	/* _GPE: an integer GPE number (GPE block devices not supported) */
	status = acpi_evaluate_integer(device->handle, "_GPE", NULL, &val);
	if (ACPI_SUCCESS(status)) {
		ec->gpe = (ULONG)val;
		ec->gpe_valid = TRUE;
	}
	else {
		DPRINT1("EC: unable to evaluate _GPE: %s\n",
			AcpiFormatException(status));
	}

	acpi_ec_check_global_lock(ec);

	status = acpi_ec_install_handlers(ec);
	if (ACPI_FAILURE(status)) {
		ExFreePoolWithTag(ec, ACPI_EC_TAG);
		return_VALUE(-15);
	}

	first_ec = ec;
	acpi_driver_data(device) = ec;

	DPRINT1("ACPI: EC: data 0x%lx, cmd/status 0x%lx, GPE 0x%lx [%s]\n",
		ec->data_addr, ec->command_addr, ec->gpe,
		acpi_device_bid(device));

	return_VALUE(0);
}

static int
acpi_ec_remove (
	struct acpi_device	*device,
	int			type)
{
	if (!device)
		return_VALUE(-1);

	/*
	 * The EC is not a removable device and the boot EC keeps servicing
	 * EmbeddedControl OpRegions until shutdown; just drop the binding.
	 */
	acpi_driver_data(device) = NULL;

	return_VALUE(0);
}

int
acpi_ec_init (void)
{
	int			result;

	ACPI_FUNCTION_TRACE("acpi_ec_init");

	result = acpi_bus_register_driver(&acpi_ec_driver);
	if (result < 0)
		return_VALUE(-15);

	return_VALUE(0);
}

void
acpi_ec_exit (void)
{
	ACPI_FUNCTION_TRACE("acpi_ec_exit");

	acpi_bus_unregister_driver(&acpi_ec_driver);

	return_VOID;
}
