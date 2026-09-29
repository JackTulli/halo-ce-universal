/*
NX_CRASH.C

The exception report. The game's own __try/__except around its main loop is
compiled out on the Xbox (halo_xbox_prefix.h), so a fault would go to the
kernel, which stops the console without a word. Instead, every thread the
game runs on carries a handler frame of the kernel's structured exception
handling (the chain at fs:[0], which the Xbox kernel walks for its own
kernel-mode code): the first fault prints the exception, the registers, the
last Win32 call the platform layer traced and a walk of the frame pointers,
then holds the thread so the watchdog and the log keep running. The
addresses are the XBE's (tools/xbox_symbolize.py names them).

The entry point is here as well: the game's main (source/shell/shell_xbox.c)
is compiled as halo_shell_main, and main installs the frame around it.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx.h"

int halo_shell_main(void);

/* the kernel's EXCEPTION_DISPOSITION */
#define DISPOSITION_CONTINUE_SEARCH 1

struct exception_frame
{
	struct exception_frame *previous;
	void *handler;
};

#define MAXIMUM_STACK_DEPTH 24

static const char *exception_name(unsigned long code)
{
	switch (code)
	{
	case 0xC0000005: return "access violation";
	case 0xC000008C: return "array bounds exceeded";
	case 0xC0000094: return "integer divide by zero";
	case 0xC0000095: return "integer overflow";
	case 0xC000008E: return "floating-point divide by zero";
	case 0xC0000090: return "floating-point invalid operation";
	case 0xC0000091: return "floating-point overflow";
	case 0xC0000092: return "floating-point stack check";
	case 0xC0000093: return "floating-point underflow";
	case 0xC000008D: return "floating-point denormal operand";
	case 0xC000008F: return "floating-point inexact result";
	case 0xC000001D: return "illegal instruction";
	case 0xC0000096: return "privileged instruction";
	case 0xC00000FD: return "stack overflow";
	case 0x80000003: return "breakpoint";
	case 0x80000004: return "single step";
	case 0x80000002: return "datatype misalignment";
	case 0xC0000025: return "noncontinuable exception";
	default: return "exception";
	}
}

/* the return addresses up the chain of frame pointers, within the thread's
stack (the game keeps frame pointers; nxdk's own code may not, which ends
the walk early) */
static void print_stack(unsigned long frame)
{
	PKTHREAD thread = KeGetCurrentThread();
	unsigned long low = (unsigned long)thread->StackLimit;
	unsigned long high = (unsigned long)thread->StackBase;
	char text[256];
	int depth, count = 0;

	strcpy(text, "halo-xbox: stack:");
	for (depth = 0; depth < MAXIMUM_STACK_DEPTH; depth++)
	{
		const unsigned long *record = (const unsigned long *)frame;
		unsigned long return_address;

		if (frame < low || frame + 8 > high || (frame & 3))
			break;
		return_address = record[1];
		snprintf(text + strlen(text), sizeof(text) - strlen(text), " %08lx", return_address);
		if (++count == 8)
		{
			strcat(text, "\n");
			nx_print(text);
			strcpy(text, "halo-xbox:       ");
			count = 0;
		}
		if (record[0] <= frame)
			break;
		frame = record[0];
	}
	if (count || depth == 0)
	{
		strcat(text, depth == 0 ? " (no frame within the stack)\n" : "\n");
		nx_print(text);
	}
}

static int __cdecl exception_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
	void *dispatcher_context)
{
	static volatile long reporting;
	char text[256];

	(void)frame;
	(void)dispatcher_context;
	if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
		return DISPOSITION_CONTINUE_SEARCH;

	/* one report at a time; a second faulting thread waits its turn */
	while (__atomic_exchange_n(&reporting, 1, __ATOMIC_ACQUIRE))
		Sleep(100);
	nx_gpu_show_console();

	snprintf(text, sizeof(text), "halo-xbox: EXCEPTION %08lx (%s) at %08lx on thread %08lx\n",
		(unsigned long)record->ExceptionCode, exception_name((unsigned long)record->ExceptionCode),
		(unsigned long)record->ExceptionAddress, (unsigned long)KeGetCurrentThread());
	nx_print(text);
	if ((unsigned long)record->ExceptionCode == 0xC0000005 && record->NumberParameters >= 2)
	{
		snprintf(text, sizeof(text), "halo-xbox: %s of address %08lx\n",
			record->ExceptionInformation[0] ? "write" : "read", (unsigned long)record->ExceptionInformation[1]);
		nx_print(text);
	}
	snprintf(text, sizeof(text),
		"halo-xbox: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx eip %08lx\n",
		(unsigned long)context->Eax, (unsigned long)context->Ebx, (unsigned long)context->Ecx,
		(unsigned long)context->Edx, (unsigned long)context->Esi, (unsigned long)context->Edi,
		(unsigned long)context->Ebp, (unsigned long)context->Esp, (unsigned long)context->Eip);
	nx_print(text);
	snprintf(text, sizeof(text), "halo-xbox: last Win32 call %s\n", platform_last_call());
	nx_print(text);
	print_stack(context->Ebp);
	nx_report_memory("at the exception");
	nx_print("halo-xbox: holding this thread; reset the console\n");
	__atomic_store_n(&reporting, 0, __ATOMIC_RELEASE);

	for (;;)
		Sleep(1000);
}

/* nxdk's assert (its C library's, which lwIP and the network driver use
too) prints to the screen and halts the processor; this one reports over the
log and holds the thread instead, so the watchdog keeps reporting */
void _xbox_assert(const char *expression, const char *file, const char *function, unsigned long line)
{
	char text[512];

	if (KeGetCurrentIrql() == PASSIVE_LEVEL)
		nx_gpu_show_console();
	snprintf(text, sizeof(text), "halo-xbox: ASSERTION %s in %s (%s, line %lu) on thread %08lx\n",
		expression, function, file, line, (unsigned long)KeGetCurrentThread());
	nx_print(text);
	snprintf(text, sizeof(text), "halo-xbox: last Win32 call %s\n", platform_last_call());
	nx_print(text);
	print_stack((unsigned long)__builtin_frame_address(0));
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
	{
		/* no waiting up here: stop the processor as nxdk does */
		nx_print("halo-xbox: asserted at a raised IRQL; halting\n");
		__asm__ __volatile__("cli\n1:\thlt\n\tjmp 1b");
	}
	nx_report_memory("at the assertion");
	nx_print("halo-xbox: holding this thread; reset the console\n");
	for (;;)
		Sleep(1000);
}

/* ---------- the way out

A fault the kernel takes at dispatch level (a bad buffer in a driver), or
its own bugcheck, ends in HalReturnToFirmware without a word; so does an
exit. The kernel calls its shutdown notifications on the way, from the
faulting thread: this one shows the console, names the last SDK call and
every address of the executable found on the thread's stack (the trap
frame's is among them), and holds for a while. */

static HAL_SHUTDOWN_REGISTRATION shutdown_registration;

static void NTAPI shutdown_notification(PHAL_SHUTDOWN_REGISTRATION registration)
{
	PKTHREAD thread = KeGetCurrentThread();
	const unsigned long *cursor = (const unsigned long *)__builtin_frame_address(0);
	const unsigned long *top = (const unsigned long *)thread->StackBase;
	char text[256];
	int found = 0;

	(void)registration;
	nx_gpu_show_console();
	snprintf(text, sizeof(text), "halo-xbox: the kernel is returning to the firmware (IRQL %d, thread %08lx)\n",
		(int)KeGetCurrentIrql(), (unsigned long)thread);
	nx_print(text);
	snprintf(text, sizeof(text), "halo-xbox: last Win32 call %s\n", platform_last_call());
	nx_print(text);
	strcpy(text, "halo-xbox: executable addresses on the stack:");
	for (; cursor < top && found < 24; cursor++)
	{
		if (*cursor >= 0x00010000UL && *cursor < 0x00600000UL)
		{
			snprintf(text + strlen(text), sizeof(text) - strlen(text), " %08lx", *cursor);
			if (++found % 8 == 0)
			{
				strcat(text, "\n");
				nx_print(text);
				strcpy(text, "halo-xbox:       ");
			}
		}
	}
	if (found % 8 || !found)
	{
		strcat(text, found ? "\n" : " none\n");
		nx_print(text);
	}
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		KfLowerIrql(PASSIVE_LEVEL);
	nx_print("halo-xbox: holding 20 seconds before the console restarts\n");
	Sleep(20000);
}

/* the frame must live on the thread's stack for as long as it is linked
in, so the callers place it in their own frames */
#define PUSH_EXCEPTION_FRAME(frame) \
	do \
	{ \
		(frame).handler = (void *)exception_handler; \
		__asm__ __volatile__("movl %%fs:0, %0" : "=r"((frame).previous)); \
		__asm__ __volatile__("movl %0, %%fs:0" : : "r"(&(frame)) : "memory"); \
	} while (0)
#define POP_EXCEPTION_FRAME(frame) \
	__asm__ __volatile__("movl %0, %%fs:0" : : "r"((frame).previous) : "memory")

/* ---------- the entry point */

int main(int argc, char **argv)
{
	struct exception_frame frame;
	int result;

	(void)argc;
	(void)argv;
	shutdown_registration.NotificationRoutine = shutdown_notification;
	shutdown_registration.Priority = 100;
	HalRegisterShutdownNotification(&shutdown_registration, TRUE);
	PUSH_EXCEPTION_FRAME(frame);
	result = halo_shell_main();
	POP_EXCEPTION_FRAME(frame);
	return result;
}

/* ---------- threads */

struct thread_start
{
	unsigned long (__stdcall *routine)(void *);
	void *parameter;
};

static DWORD WINAPI thread_trampoline(LPVOID parameter)
{
	struct thread_start start = *(struct thread_start *)parameter;
	struct exception_frame frame;
	unsigned long result;

	free(parameter);
	PUSH_EXCEPTION_FRAME(frame);
	result = start.routine(start.parameter);
	POP_EXCEPTION_FRAME(frame);
	return result;
}

void *nx_create_thread(unsigned long stack_size, unsigned long (__stdcall *routine)(void *), void *parameter,
	unsigned long flags, unsigned long *thread_id)
{
	struct thread_start *start = malloc(sizeof(*start));
	HANDLE thread;

	if (!start)
		return NULL;
	start->routine = routine;
	start->parameter = parameter;
	thread = CreateThread(NULL, stack_size, thread_trampoline, start, flags, (LPDWORD)thread_id);
	if (!thread)
		free(start);
	return thread;
}
