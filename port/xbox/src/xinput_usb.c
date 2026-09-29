/*
XINPUT_USB.C

The XAPI's input devices for the Xbox build: the gamepads on the four
ports, through nxdk's USB stack (nx_input.c). The game asks for device
changes every frame, opens a handle per inserted port, reads the state of
each and sends the rumble motors their speeds. Memory units and the debug
keyboard are never reported.
*/

#include "platform.h"

#include <string.h>

/* the device type tables the game names (xdk_xbox.h, xdk_xkbd.h) */
XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

/* the ports the game has been told about */
static DWORD reported_ports;

static DWORD connected_ports(void)
{
	DWORD mask = 0;
	unsigned long port;
	struct nx_gamepad_state state;

	for (port = 0; port < NX_INPUT_PORTS; port++)
	{
		if (nx_input_gamepad(port, &state, NULL))
			mask |= 1UL << port;
	}
	return mask;
}

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	PLATFORM_TRACE();
	(void)preallocation_type_count;
	(void)preallocation_types;
	if (!nx_input_start())
		platform_log("the USB stack did not start: no controllers");
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	DWORD now;

	PLATFORM_TRACE();
	*insertions = 0;
	*removals = 0;
	if (device_type != &XDEVICE_TYPE_GAMEPAD_TABLE)
		return FALSE;
	nx_input_poll();
	now = connected_ports();
	*insertions = now & ~reported_ports;
	*removals = reported_ports & ~now;
	reported_ports = now;
	return (*insertions | *removals) != 0;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	struct nx_gamepad_state state;

	PLATFORM_TRACE();
	(void)slot;
	(void)polling_parameters;
	if (device_type == &XDEVICE_TYPE_GAMEPAD_TABLE && port < NX_INPUT_PORTS && nx_input_gamepad(port, &state, NULL))
		return (HANDLE)(port + 1);
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	PLATFORM_TRACE();
	(void)device;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	unsigned long port = (unsigned long)device - 1;
	struct nx_gamepad_state pad;
	unsigned long packet = 0;

	PLATFORM_TRACE();
	memset(state, 0, sizeof(*state));
	if (!device || port >= NX_INPUT_PORTS || !nx_input_gamepad(port, &pad, &packet))
		return ERROR_DEVICE_NOT_CONNECTED;
	state->dwPacketNumber = packet;
	state->Gamepad.wButtons = pad.buttons;
	memcpy(state->Gamepad.bAnalogButtons, pad.analog_buttons, sizeof(state->Gamepad.bAnalogButtons));
	state->Gamepad.sThumbLX = pad.thumb_left_x;
	state->Gamepad.sThumbLY = pad.thumb_left_y;
	state->Gamepad.sThumbRX = pad.thumb_right_x;
	state->Gamepad.sThumbRY = pad.thumb_right_y;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	unsigned long port = (unsigned long)device - 1;

	PLATFORM_TRACE();
	if (!device || port >= NX_INPUT_PORTS)
	{
		if (feedback)
			feedback->Header.dwStatus = ERROR_DEVICE_NOT_CONNECTED;
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	if (feedback)
	{
		nx_input_rumble(port, feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
		/* done at once, as far as the game is concerned */
		feedback->Header.dwStatus = ERROR_SUCCESS;
	}
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	PLATFORM_TRACE();
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	PLATFORM_TRACE();
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}

/* the scripted test input of the native builds (port/linux/game/network_test.c) */
void test_input_hold_action(int hold)
{
	(void)hold;
}
