/*
XBOX_POSIX.C

The POSIX threads the platform layer's shared units use (internet play,
port/linux/src/p2p.c: a thread, and mutexes with static initializers),
over the console's threads (nx_crash.c, under its exception handler) and
the kernel's critical sections (include/crt/pthread.h).
*/

#include "platform.h"

#include <pthread.h>
#include <stdlib.h>

enum
{
	/* internet play's thread, and room for a few more */
	MAXIMUM_THREADS = 4,
	THREAD_STACK_SIZE = 64 * 1024,
};

struct thread_start
{
	void *(*start)(void *);
	void *argument;
	int used;
};

static struct thread_start starts[MAXIMUM_THREADS];

static DWORD WINAPI thread_main(LPVOID parameter)
{
	struct thread_start *start = parameter;
	void *(*routine)(void *) = start->start;
	void *argument = start->argument;

	start->used = 0;
	routine(argument);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	int index;

	(void)attributes;
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (__sync_bool_compare_and_swap(&starts[index].used, 0, 1))
			break;
	}
	if (index == MAXIMUM_THREADS)
		return -1;
	starts[index].start = start;
	starts[index].argument = argument;
	{
		HANDLE handle = CreateThread(NULL, THREAD_STACK_SIZE, thread_main, &starts[index], 0, thread);

		if (!handle)
		{
			starts[index].used = 0;
			return -1;
		}
		/* the one POSIX thread is internet play's, which drains the tunnel
		socket: above the game, whose waits spin at times, as the retail
		network stack ran in the interrupt's deferred calls (a socket left
		holding the network card's 64 receive buffers stops the card) */
		SetThreadPriority(handle, THREAD_PRIORITY_ABOVE_NORMAL);
	}
	return 0;
}

int pthread_detach(pthread_t thread)
{
	(void)thread;
	return 0;
}

static PRTL_CRITICAL_SECTION section_of(pthread_mutex_t *mutex)
{
	if (mutex->state != 2)
	{
		if (__sync_bool_compare_and_swap(&mutex->state, 0, 1))
		{
			PRTL_CRITICAL_SECTION section = malloc(sizeof(*section));

			RtlInitializeCriticalSection(section);
			mutex->section = section;
			__sync_synchronize();
			mutex->state = 2;
		}
		else
		{
			/* (Sleep(1): Sleep(0) gives way only to threads of the same
			priority, and the one making the section may be lower: the
			internet play thread runs above the game) */
			while (mutex->state != 2)
				Sleep(1);
		}
	}
	return mutex->section;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)attributes;
	mutex->section = NULL;
	mutex->state = 0;
	section_of(mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	/* a critical section needs no undoing; its memory is kept */
	(void)mutex;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	RtlEnterCriticalSection(section_of(mutex));
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	RtlLeaveCriticalSection(section_of(mutex));
	return 0;
}
