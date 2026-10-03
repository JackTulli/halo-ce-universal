/*
NX_INPUT.C

The controllers, through nxdk's USB host stack and its Xbox input device
(XID) driver: the four ports' gamepads, their reports and their rumble
motors, behind plain C (nx.h) for the XAPI's XInput functions
(xinput_usb.c).

The stack does its housekeeping when polled (nx_input_poll, which the
game's device change check makes every frame); a gamepad's reports arrive
from the controller's interrupt transfers, whose completion copies them.
*/

#include <usbh_lib.h>
#include <xid_driver.h>
#include <hub.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "nx.h"

struct pad
{
	xid_dev_t *device;
	volatile int connected;
	volatile unsigned long packet;
	xid_gamepad_in report;
	int rumble_known;
	unsigned short rumble_left, rumble_right;
	/* a change the game asked for, sent by the next poll */
	volatile int rumble_pending;
	volatile unsigned short want_left, want_right;
	unsigned long rumble_sent_time;
	/* a report's transfer failed (a CRC error, a timeout: the controller
	halts the endpoint): the next poll clears the halt and reads again */
	volatile int read_failed;
	EP_INFO_T *failed_endpoint;
};

/* A rumble report is a control transfer the USB stack waits out on the
polling thread (1-3 ms on a console's OHCI), and the game changes the
motors every tick while a rumble fades: a change goes at most every
RUMBLE_INTERVAL, and one smaller than a sixteenth of the range not at all,
but stopping always does, at once. */
#define RUMBLE_INTERVAL 60
#define RUMBLE_LEVEL(value) ((unsigned)(value) >> 12)

static int rumble_differs(const struct pad *pad)
{
	if (!pad->rumble_known)
		return 1;
	if ((pad->want_left == 0) != (pad->rumble_left == 0) || (pad->want_right == 0) != (pad->rumble_right == 0))
		return 1;
	return RUMBLE_LEVEL(pad->want_left) != RUMBLE_LEVEL(pad->rumble_left) ||
		RUMBLE_LEVEL(pad->want_right) != RUMBLE_LEVEL(pad->rumble_right);
}

/* the USB stack's control request (libusbohci's usb.h) */
extern int usbh_ctrl_xfer(UDEV_T *udev, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	uint16_t wLength, uint8_t *buff, uint32_t *xfer_len, uint32_t timeout);

static struct pad pads[NX_INPUT_PORTS];
static void rumble_send(struct pad *pad, unsigned short left, unsigned short right);
static int started;

/* the console's port of a device: SDL's reading of the hub topology
(the port numbers behind the internal hub, when there is one) */
static int port_of(xid_dev_t *xid)
{
	UDEV_T *udev = xid->iface->udev;
	int internal_hub = (XboxHardwareInfo.Flags & XBOX_HW_FLAG_INTERNAL_USB_HUB) != 0;

	while (udev)
	{
		UDEV_T *parent = udev->parent ? udev->parent->iface->udev : NULL;

		if ((internal_hub && parent && !parent->parent) || (!internal_hub && !udev->parent))
		{
			switch (udev->port_num)
			{
			case 3: return 0;
			case 4: return 1;
			case 1: return 2;
			case 2: return 3;
			default: return -1;
			}
		}
		udev = parent;
	}
	return -1;
}

static void read_callback(UTR_T *transfer)
{
	xid_dev_t *xid = (xid_dev_t *)transfer->context;
	struct pad *pad = xid ? (struct pad *)xid->user_data : NULL;

	if (!pad)
		return;
	if (transfer->status < 0)
	{
		/* (a disconnection's abort is the disconnected callback's) */
		if (transfer->status != USBH_ERR_ABORT && transfer->status != USBH_ERR_DISCONNECTED)
		{
			/* nothing held down while the reads stop: before, one failed
			transfer left the pad's last report, a stick held over, until
			it was unplugged */
			memset(&pad->report, 0, sizeof(pad->report));
			pad->failed_endpoint = transfer->ep;
			pad->read_failed = 1;
		}
		return;
	}
	if (transfer->xfer_len >= sizeof(pad->report))
	{
		memcpy(&pad->report, transfer->buff, sizeof(pad->report));
		pad->packet++;
	}
	/* the next report */
	transfer->xfer_len = 0;
	transfer->bIsTransferDone = 0;
	usbh_int_xfer(transfer);
}

static void connected(xid_dev_t *xid, int status)
{
	int port = port_of(xid);
	struct pad *pad;
	char text[80];

	(void)status;
	if ((usbh_xid_get_type(xid) >> 8) != XID_TYPE_GAMECONTROLLER)
		return;
	if (port < 0)
	{
		/* an unknown place in the topology: the first free port */
		for (port = 0; port < NX_INPUT_PORTS && pads[port].device; port++)
			;
		if (port == NX_INPUT_PORTS)
			return;
	}
	pad = &pads[port];
	memset(&pad->report, 0, sizeof(pad->report));
	pad->device = xid;
	xid->user_data = pad;
	pad->read_failed = 0;
	pad->connected = 1;
	usbh_xid_read(xid, 0, read_callback);
	snprintf(text, sizeof(text), "halo-xbox: gamepad on port %d\n", port + 1);
	nx_print(text);
}

static void disconnected(xid_dev_t *xid, int status)
{
	struct pad *pad = (struct pad *)xid->user_data;

	(void)status;
	if (pad)
	{
		pad->connected = 0;
		pad->device = NULL;
		pad->rumble_known = 0;
		xid->user_data = NULL;
	}
}

int nx_input_start(void)
{
	if (started)
		return 1;
	usbh_core_init();
	usbh_xid_init();
	usbh_install_xid_conn_callback(connected, disconnected);
	started = 1;
	return 1;
}

void nx_input_poll(void)
{
	unsigned long port;

	if (!started)
		return;
	usbh_pooling_hubs();
	for (port = 0; port < NX_INPUT_PORTS; port++)
	{
		struct pad *pad = &pads[port];

		if (pad->read_failed && pad->device)
		{
			/* the endpoint descriptor's halt bit (OHCI: its third word,
			bit 0; bit 1, the toggle carry, stays), then a read again */
			if (pad->failed_endpoint && pad->failed_endpoint->hw_pipe)
				((volatile uint32_t *)pad->failed_endpoint->hw_pipe)[2] &= ~1UL;
			pad->read_failed = 0;
			/* at the completion DPC's level, which also submits reads (the
			submission does not wait) */
			{
				KIRQL irql = KeRaiseIrqlToDpcLevel();

				usbh_xid_read(pad->device, 0, read_callback);
				KfLowerIrql(irql);
			}
		}

		if (pad->rumble_pending)
		{
			int stopping = !pad->want_left && !pad->want_right;

			/* too soon after the last report: kept for a later poll */
			if (!stopping && pad->rumble_known && GetTickCount() - pad->rumble_sent_time < RUMBLE_INTERVAL)
				continue;
			pad->rumble_pending = 0;
			if (pad->connected && pad->device && rumble_differs(pad))
			{
				rumble_send(pad, pad->want_left, pad->want_right);
				pad->rumble_sent_time = GetTickCount();
			}
		}
	}
}

int nx_input_gamepad(unsigned long port, struct nx_gamepad_state *state, unsigned long *packet)
{
	struct pad *pad;
	xid_gamepad_in report;

	if (port >= NX_INPUT_PORTS || !pads[port].connected)
		return 0;
	pad = &pads[port];
	memcpy(&report, &pad->report, sizeof(report));
	state->buttons = report.dButtons;
	state->analog_buttons[0] = report.a;
	state->analog_buttons[1] = report.b;
	state->analog_buttons[2] = report.x;
	state->analog_buttons[3] = report.y;
	state->analog_buttons[4] = report.black;
	state->analog_buttons[5] = report.white;
	state->analog_buttons[6] = report.l;
	state->analog_buttons[7] = report.r;
	state->thumb_left_x = report.leftStickX;
	state->thumb_left_y = report.leftStickY;
	state->thumb_right_x = report.rightStickX;
	state->thumb_right_y = report.rightStickY;
	if (packet)
		*packet = pad->packet;
	return 1;
}

/* the rumble report, sent: a control transfer the USB stack waits out */
static void rumble_send(struct pad *pad, unsigned short left, unsigned short right)
{
	unsigned char report[6];
	uint32_t done = 0;

	/* the output report as a HID SET_REPORT class request to the interface:
	the same 6 bytes on the interrupt OUT endpoint (usbh_xid_rumble) make the
	emulator's USB core assert in its packet copy and take the machine down */
	report[0] = 0;
	report[1] = sizeof(report);
	report[2] = (unsigned char)left;
	report[3] = (unsigned char)(left >> 8);
	report[4] = (unsigned char)right;
	report[5] = (unsigned char)(right >> 8);
	usbh_ctrl_xfer(pad->device->iface->udev, 0x21, 0x09, 0x0200, pad->device->iface->if_num, sizeof(report), report,
		&done, 100);
	pad->rumble_known = 1;
	pad->rumble_left = left;
	pad->rumble_right = right;
}

/* The game sets the motors from a thread of its own at a high priority;
the transfer is only recorded here and made by the next poll, on the game's
thread: the USB stack has no lock of its own, and its wait for the transfer
spun at that priority, ahead of everything else */
void nx_input_rumble(unsigned long port, unsigned short left, unsigned short right)
{
	struct pad *pad;

	if (port >= NX_INPUT_PORTS || !pads[port].connected || !pads[port].device)
		return;
	pad = &pads[port];
	if (!pad->rumble_pending && pad->rumble_known && pad->rumble_left == left && pad->rumble_right == right)
		return;
	pad->want_left = left;
	pad->want_right = right;
	pad->rumble_pending = 1;
}
