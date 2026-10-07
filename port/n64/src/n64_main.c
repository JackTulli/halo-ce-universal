/*
N64_MAIN.C

The N64 build's entry point and system services (port/n64/src/n64.h):
libdragon's start-up, the debug log, time, and the jump into the game's own
main (source/shell/shell_xbox.c, compiled as halo_shell_main).
*/

#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "n64.h"

int halo_shell_main(void);
void n64_profile_start(void);
void n64_exceptions_install(void);

void n64_log(const char *text)
{
	size_t length = strlen(text);

	while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
		length--;
	debugf("%.*s\n", (int)length, text);
}

void n64_fatal(const char *text)
{
	debugf("FATAL: %s\n", text);
	assertf(0, "%s", text);
	abort();
}

void n64_backtrace(void)
{
	debug_backtrace();
}

unsigned long n64_ticks(void)
{
	return TICKS_READ();
}

unsigned long long n64_ticks64(void)
{
	return get_ticks();
}

unsigned long long n64_microseconds(void)
{
	return get_ticks_us();
}

void n64_wait_microseconds(unsigned long microseconds)
{
	wait_ticks(TICKS_FROM_US(microseconds));
}

unsigned long n64_heap_free(void)
{
	struct mallinfo info = mallinfo();
	return (unsigned long)(get_memory_size() - info.uordblks);
}

int main(void)
{
	debug_init_isviewer();
	debug_init_usblog();
	n64_exceptions_install();
	dfs_init(DFS_DEFAULT_LOCATION);
	joypad_init();
	debugf("halo-n64: %d KB of RAM\n", (int)(get_memory_size() / 1024));
	{
		/* an "autotest" file on the cartridge: scripted controllers
		(xdk_input.c) for unattended test runs */
		extern int n64_test_input;
		FILE *flag = fopen("rom:/autotest", "rb");

		if (flag)
		{
			n64_test_input = 1;
			fclose(flag);
			debugf("halo-n64: test mode, scripted controllers\n");
		}
	}
	n64_profile_start();
	halo_shell_main();
	debugf("halo-n64: the game returned\n");
	for (;;)
		;
}
