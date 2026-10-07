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

/* Halo's deepest calls (collision, physics under the game tick) need more
than the 64 KB libdragon keeps for main's stack at the top of RAM, and the
heap grows up to it: the game runs on a stack of its own, filled with a
pattern so that its deepest use can be reported (n64_stack_used) */
#define GAME_STACK_SIZE (256 * 1024)
#define GAME_STACK_FILL 0xA5A5A5A5u

static uint32_t *game_stack;

__asm__(
	".text\n"
	".align 3\n"
	".globl n64_call_on_stack\n"
	".type n64_call_on_stack, @function\n"
	".set push\n"
	".set noreorder\n"
	"n64_call_on_stack:\n"
	"	addiu $sp, $sp, -16\n"
	"	sd $ra, 8($sp)\n"
	"	sd $s0, 0($sp)\n"
	"	move $s0, $sp\n"
	"	jalr $a0\n"
	"	move $sp, $a1\n"
	"	move $sp, $s0\n"
	"	ld $s0, 0($sp)\n"
	"	ld $ra, 8($sp)\n"
	"	jr $ra\n"
	"	addiu $sp, $sp, 16\n"
	".set pop\n"
	".size n64_call_on_stack, .-n64_call_on_stack\n");

/* calls function with the stack pointer at top */
void n64_call_on_stack(void (*function)(void), void *top);

unsigned long n64_stack_used(void)
{
	unsigned long words = GAME_STACK_SIZE / 4, i;

	if (!game_stack)
		return 0;
	for (i = 0; i < words && game_stack[i] == GAME_STACK_FILL; i++)
		;
	return (words - i) * 4;
}

static void game_main(void)
{
	halo_shell_main();
	debugf("halo-n64: the game returned\n");
	for (;;)
		;
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
	game_stack = memalign(16, GAME_STACK_SIZE);
	if (!game_stack)
	{
		game_main();
		return 0;
	}
	for (unsigned long i = 0; i < GAME_STACK_SIZE / 4; i++)
		game_stack[i] = GAME_STACK_FILL;
	/* the top, less the argument area the ABI lets a callee use */
	n64_call_on_stack(game_main, (uint8_t *)game_stack + GAME_STACK_SIZE - 64);
	return 0;
}
