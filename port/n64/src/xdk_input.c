/*
XDK_INPUT.C

The XInput gamepads the game reads (input/input_xbox.c), from the N64's
four controller ports (n64_input.c).

The game is played with the Xbox's two sticks; the N64 controller has one,
so the default layout is the one N64 shooters used (Turok, Perfect Dark's
1.2): the stick turns and looks, the C buttons walk and strafe.

    stick       look (Xbox right stick)
    C buttons   move and strafe (Xbox left stick)
    Z           fire (right trigger)
    R           throw a grenade (left trigger)
    A           jump (A)
    B           reload / action (X)
    L           melee (B)
    D-pad up    switch weapon (Y)
    D-pad down  crouch (left stick click)
    D-pad left  zoom (right stick click)
    D-pad right switch grenades (white)
    Start       start

Test mode (a file "autotest" on the cartridge): the first two ports are
driven by a script instead, so an unattended run exercises the game
(n64_test_input).
*/

#include "platform.h"

#include <string.h>

#define PORT_COUNT 4

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static struct controller controllers[PORT_COUNT];
static DWORD reported_gamepads;
static unsigned long last_poll_tick;

int n64_test_input = 0;

static void poll(void)
{
	/* once per game frame is plenty: the game reads every port in turn */
	unsigned long now = n64_ticks();

	if (now - last_poll_tick > N64_TICKS_PER_SECOND / 240)
	{
		n64_input_poll();
		last_poll_tick = now;
	}
}

static DWORD connected_gamepads(void)
{
	DWORD mask = 0;
	int port;

	if (n64_test_input)
		return 0x3;
	poll();
	for (port = 0; port < PORT_COUNT; port++)
	{
		struct n64_pad pad;

		n64_input_get(port, &pad);
		if (pad.connected)
			mask |= 1 << port;
	}
	return mask;
}

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		DWORD connected = connected_gamepads();

		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller >= controllers && controller < controllers + PORT_COUNT)
		controller->open = FALSE;
}

static int controller_port(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller < controllers || controller >= controllers + PORT_COUNT || !controller->open)
		return -1;
	return (int)(controller - controllers);
}

/* the N64 stick reads about +-85 at full tilt */
static SHORT stick(int value)
{
	long scaled = (long)value * 32767 / 80;

	if (scaled > 32767)
		scaled = 32767;
	if (scaled < -32768)
		scaled = -32768;
	return (SHORT)scaled;
}

static void pad_to_gamepad(const struct n64_pad *pad, XINPUT_GAMEPAD *gamepad)
{
	unsigned short b = pad->buttons;

	gamepad->sThumbRX = stick(pad->stick_x);
	gamepad->sThumbRY = stick(pad->stick_y);
	gamepad->sThumbLY = (b & N64_BUTTON_C_UP) ? 32767 : (b & N64_BUTTON_C_DOWN) ? -32768 : 0;
	gamepad->sThumbLX = (b & N64_BUTTON_C_RIGHT) ? 32767 : (b & N64_BUTTON_C_LEFT) ? -32768 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = (b & N64_BUTTON_Z) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = (b & N64_BUTTON_R) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_A] = (b & N64_BUTTON_A) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_X] = (b & N64_BUTTON_B) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_B] = (b & N64_BUTTON_L) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_Y] = (b & N64_BUTTON_D_UP) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_WHITE] = (b & N64_BUTTON_D_RIGHT) ? 255 : 0;
	if (b & N64_BUTTON_D_DOWN)
		gamepad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (b & N64_BUTTON_D_LEFT)
		gamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
	if (b & N64_BUTTON_START)
		gamepad->wButtons |= XINPUT_GAMEPAD_START;
}

/* the test script: walk forward, turn now and then, fire in bursts, jump
and throw a grenade occasionally; each port differs */
static void test_gamepad(int port, XINPUT_GAMEPAD *gamepad)
{
	unsigned long seconds_x8 = (unsigned long)(n64_microseconds() / 125000);
	unsigned long phase = seconds_x8 + port * 37;

	gamepad->sThumbLY = 26000;
	gamepad->sThumbLX = ((phase / 24) & 1) ? 12000 : -12000;
	gamepad->sThumbRX = ((phase / 16) % 3 == 0) ? 20000 : ((phase / 16) % 3 == 1) ? -8000 : 0;
	gamepad->sThumbRY = ((phase / 40) & 1) ? 3000 : -3000;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = ((phase % 12) < 5) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_A] = ((phase % 50) == 7) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = ((phase % 120) == 30) ? 255 : 0;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_X] = ((phase % 64) == 9) ? 255 : 0;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);

	memset(state, 0, sizeof(*state));
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	if (n64_test_input)
	{
		if (port < 2)
			test_gamepad(port, &state->Gamepad);
	}
	else
	{
		struct n64_pad pad;

		poll();
		n64_input_get(port, &pad);
		if (!pad.connected)
			return ERROR_DEVICE_NOT_CONNECTED;
		pad_to_gamepad(&pad, &state->Gamepad);
	}
	if (memcmp(&state->Gamepad, &controllers[port].previous, sizeof(state->Gamepad)))
	{
		controllers[port].packet_number++;
		controllers[port].previous = state->Gamepad;
	}
	state->dwPacketNumber = controllers[port].packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	(void)device;
	if (feedback)
		feedback->Header.dwStatus = ERROR_SUCCESS;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	(void)keystroke;
	return ERROR_HANDLE_EOF;
}
