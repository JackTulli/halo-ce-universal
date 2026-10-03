/*
WIN32_POSIX.C

The POSIX calls the platform layer shared with Linux makes, implemented with
Windows (the headers are in port/windows/include/posix): threads, mutexes and
condition variables, clocks and sleeping, sysconf, and the memory mapping the
Xbox memory window uses. Also the process start-up the Windows build needs,
and the description of what holds a range of the address space (posix.h).

Windows 98/ME (HALO_WIN9X) lack some of it, built from what they have:
condition variables from an event per waiting thread, a precise clock from
the system time read once plus the performance counter, positioned reads
and writes from SetFilePointer under a lock (9x files take no offsets in
an OVERLAPPED), GlobalMemoryStatus, Sleep(0) for SwitchToThread, and no
try-lock but on a mutex the thread already holds. Threads start with their
creator's floating point state (the x87 control word and MXCSR), which the
game sets and which systems playing together must share.
*/

#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>

#include "posix.h"
#include "pthread.h"
#include "sched.h"
#include "sys/mman.h"
#include "time.h"
#include "unistd.h"

/* ---------- start-up */

#ifdef HALO_WIN9X
/* pread and pwrite: a seek and a transfer at once */
static CRITICAL_SECTION positioned_lock;
#endif

__attribute__((constructor))
static void windows_startup(void)
{
#ifdef HALO_WIN9X
	InitializeCriticalSection(&positioned_lock);
#endif
	/* files are binary unless opened otherwise, as on the Xbox (the C
	runtime's default text mode would translate line ends) */
	_set_fmode(_O_BINARY);
	/* Sleep() waits in 1 ms steps rather than the default 15.6 ms: the
	game's frame pacing sleeps for short intervals */
	timeBeginPeriod(1);
}

static int errno_from_windows_error(DWORD error)
{
	switch (error)
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_COMMITMENT_LIMIT:
		return ENOMEM;
	case ERROR_INVALID_ADDRESS:
	case ERROR_INVALID_PARAMETER:
		return EINVAL;
	default:
		return EIO;
	}
}

/* ---------- threads */

struct thread_start
{
	void *(*start)(void *);
	void *argument;
#ifdef HALO_WIN9X
	/* the creator's floating point state */
	unsigned short control_word;
	unsigned long mxcsr;
#endif
};

static DWORD WINAPI thread_main(LPVOID parameter)
{
	struct thread_start start = *(struct thread_start *)parameter;
#ifdef HALO_WIN9X
	unsigned long mxcsr = start.mxcsr;
#endif

	free(parameter);
#ifdef HALO_WIN9X
	/* (a new thread on 9x starts with the system's: 64-bit precision, and
	whatever MXCSR the system gives it) */
	__asm__ volatile ("fldcw %0" : : "m" (start.control_word));
	/* (never DAZ, bit 6: a Pentium III without it faults on the load, and
	tools/win9x_check.py wants every load shown to clear it, here by the
	instruction before) */
	__asm__ volatile ("andl $-0x41, %0; ldmxcsr %0" : "+m" (mxcsr));
#endif
	start.start(start.argument);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	struct thread_start *parameter = malloc(sizeof(*parameter));
	DWORD identifier;
	HANDLE handle;

	if (!parameter)
		return EAGAIN;
	parameter->start = start;
	parameter->argument = argument;
#ifdef HALO_WIN9X
	__asm__ volatile ("fnstcw %0" : "=m" (parameter->control_word));
	__asm__ volatile ("stmxcsr %0" : "=m" (parameter->mxcsr));
	/* (98/ME know no reservation flag: the size is the stack's) */
	handle = CreateThread(NULL, attributes ? attributes->stack_size : 0, thread_main, parameter, 0, &identifier);
#else
	handle = CreateThread(NULL, attributes ? attributes->stack_size : 0, thread_main, parameter,
		STACK_SIZE_PARAM_IS_A_RESERVATION, &identifier);
#endif
	if (!handle)
	{
		free(parameter);
		return EAGAIN;
	}
	/* nothing joins threads: the handle is not needed */
	CloseHandle(handle);
	*thread = identifier;
	return 0;
}

int pthread_detach(pthread_t thread)
{
	(void)thread;
	return 0;
}

pthread_t pthread_self(void)
{
	return GetCurrentThreadId();
}

int pthread_equal(pthread_t thread1, pthread_t thread2)
{
	return thread1 == thread2;
}

int pthread_attr_init(pthread_attr_t *attributes)
{
	attributes->stack_size = 0;
	attributes->detached = 0;
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	attributes->detached = state;
	return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
	attributes->stack_size = size;
	return 0;
}

/* ---------- mutexes */

int pthread_mutexattr_init(pthread_mutexattr_t *attributes)
{
	attributes->type = PTHREAD_MUTEX_DEFAULT;
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type)
{
	attributes->type = type;
	return 0;
}

#ifdef HALO_WIN9X
/* 98/ME have no TryEnterCriticalSection, and their critical sections cannot
be looked into: the mutex keeps its owner and how deep it holds it. A
try-lock succeeds only for the thread holding the mutex already (nothing
the game runs tries a lock another thread may hold: RtlTryEnterCriticalSection,
xbox_kernel.c, has no callers) */
struct mutex_9x
{
	CRITICAL_SECTION section;
	volatile DWORD owner;
	LONG depth;
};
#define MUTEX_ALLOCATION struct mutex_9x
#define MUTEX_9X(mutex) ((struct mutex_9x *)(mutex)->section)
#else
#define MUTEX_ALLOCATION CRITICAL_SECTION
#endif

/* the critical section behind a mutex, created on first use so that
PTHREAD_MUTEX_INITIALIZER can be a constant */
static CRITICAL_SECTION *mutex_section(pthread_mutex_t *mutex)
{
	CRITICAL_SECTION *section = mutex->section;

	if (!section)
	{
		CRITICAL_SECTION *created = malloc(sizeof(MUTEX_ALLOCATION));

		if (!created)
			abort();
#ifdef HALO_WIN9X
		((struct mutex_9x *)created)->owner = 0;
		((struct mutex_9x *)created)->depth = 0;
#endif
		InitializeCriticalSection(created);
		section = InterlockedCompareExchangePointer(&mutex->section, created, NULL);
		if (section)
		{
			DeleteCriticalSection(created);
			free(created);
		}
		else
		{
			section = created;
		}
	}
	return section;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)attributes;
	mutex->section = NULL;
	mutex_section(mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	CRITICAL_SECTION *section = mutex->section;

	if (section)
	{
		DeleteCriticalSection(section);
		free(section);
		mutex->section = NULL;
	}
	return 0;
}

#ifdef HALO_WIN9X
int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	EnterCriticalSection(mutex_section(mutex));
	MUTEX_9X(mutex)->owner = GetCurrentThreadId();
	MUTEX_9X(mutex)->depth++;
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	mutex_section(mutex);
	if (MUTEX_9X(mutex)->owner != GetCurrentThreadId())
		return EBUSY;
	return pthread_mutex_lock(mutex);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	if (!--MUTEX_9X(mutex)->depth)
		MUTEX_9X(mutex)->owner = 0;
	LeaveCriticalSection(mutex_section(mutex));
	return 0;
}
#else
int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	EnterCriticalSection(mutex_section(mutex));
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	return TryEnterCriticalSection(mutex_section(mutex)) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	LeaveCriticalSection(mutex_section(mutex));
	return 0;
}
#endif

/* ---------- condition variables */

#ifdef HALO_WIN9X
/* 98/ME: each waiting thread queues itself with an auto-reset event of its
own and sleeps on it; a signal takes the first waiter from the queue and
sets its event, a broadcast every waiter's. A waiter that times out takes
itself off the queue, unless a signal took it first, in which case the
signal is its own (and its event, set by then, is reset). Waits release the
mutex once, as SleepConditionVariableCS does. */

struct condition_waiter
{
	HANDLE event;
	BOOL signalled;
	struct condition_waiter *next;
};

struct condition_9x
{
	CRITICAL_SECTION lock;
	struct condition_waiter *first, *last;
};

/* the thread's event, made on its first wait (and never closed: threads
are few and long-lived) */
static __thread HANDLE thread_event;

static struct condition_9x *condition_queue(pthread_cond_t *condition)
{
	struct condition_9x *queue = condition->variable;

	if (!queue)
	{
		struct condition_9x *created = calloc(1, sizeof(*created));

		if (!created)
			abort();
		InitializeCriticalSection(&created->lock);
		queue = InterlockedCompareExchangePointer(&condition->variable, created, NULL);
		if (queue)
		{
			DeleteCriticalSection(&created->lock);
			free(created);
		}
		else
		{
			queue = created;
		}
	}
	return queue;
}

/* FALSE on a timeout */
static BOOL condition_wait(pthread_cond_t *condition, pthread_mutex_t *mutex, DWORD milliseconds)
{
	struct condition_9x *queue = condition_queue(condition);
	struct condition_waiter waiter;
	struct mutex_9x *held = (struct mutex_9x *)mutex_section(mutex);
	LONG depth;
	BOOL signalled;

	if (!thread_event)
	{
		thread_event = CreateEventA(NULL, FALSE, FALSE, NULL);
		if (!thread_event)
			abort();
	}
	waiter.event = thread_event;
	waiter.signalled = FALSE;
	waiter.next = NULL;
	EnterCriticalSection(&queue->lock);
	if (queue->last)
		queue->last->next = &waiter;
	else
		queue->first = &waiter;
	queue->last = &waiter;
	LeaveCriticalSection(&queue->lock);

	depth = held->depth;
	held->depth = 0;
	held->owner = 0;
	LeaveCriticalSection(&held->section);
	WaitForSingleObject(waiter.event, milliseconds);
	EnterCriticalSection(&queue->lock);
	signalled = waiter.signalled;
	if (!signalled)
	{
		struct condition_waiter **link;

		for (link = &queue->first; *link; link = &(*link)->next)
		{
			if (*link == &waiter)
			{
				*link = waiter.next;
				break;
			}
		}
		queue->last = NULL;
		for (link = &queue->first; *link; link = &(*link)->next)
			queue->last = *link;
	}
	LeaveCriticalSection(&queue->lock);
	/* (a signal that came after the wait gave up left the event set) */
	if (signalled)
		WaitForSingleObject(waiter.event, 0);
	EnterCriticalSection(&held->section);
	held->owner = GetCurrentThreadId();
	held->depth = depth;
	return signalled;
}

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	condition->variable = NULL;
	condition_queue(condition);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	struct condition_9x *queue = condition->variable;

	if (queue && !queue->first)
	{
		DeleteCriticalSection(&queue->lock);
		free(queue);
		condition->variable = NULL;
	}
	return 0;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	condition_wait(condition, mutex, INFINITE);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct timespec now;
	long long milliseconds;

	clock_gettime(CLOCK_REALTIME, &now);
	milliseconds = ((long long)deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (milliseconds < 0)
		milliseconds = 0;
	return condition_wait(condition, mutex, milliseconds >= INFINITE ? INFINITE - 1 : (DWORD)milliseconds) ? 0 :
		ETIMEDOUT;
}

static void condition_wake(pthread_cond_t *condition, BOOL all)
{
	struct condition_9x *queue = condition_queue(condition);

	EnterCriticalSection(&queue->lock);
	while (queue->first)
	{
		struct condition_waiter *waiter = queue->first;

		queue->first = waiter->next;
		if (!queue->first)
			queue->last = NULL;
		waiter->signalled = TRUE;
		SetEvent(waiter->event);
		if (!all)
			break;
	}
	LeaveCriticalSection(&queue->lock);
}

int pthread_cond_signal(pthread_cond_t *condition)
{
	condition_wake(condition, FALSE);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
	condition_wake(condition, TRUE);
	return 0;
}
#else
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "pthread_cond_t holds a CONDITION_VARIABLE");

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	InitializeConditionVariable((PCONDITION_VARIABLE)&condition->variable);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	(void)condition;
	return 0;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	SleepConditionVariableCS((PCONDITION_VARIABLE)&condition->variable, mutex_section(mutex), INFINITE);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct timespec now;
	long long milliseconds;

	clock_gettime(CLOCK_REALTIME, &now);
	milliseconds = ((long long)deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (milliseconds < 0)
		milliseconds = 0;
	if (!SleepConditionVariableCS((PCONDITION_VARIABLE)&condition->variable, mutex_section(mutex),
		milliseconds >= INFINITE ? INFINITE - 1 : (DWORD)milliseconds))
	{
		return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
	}
	return 0;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
	WakeConditionVariable((PCONDITION_VARIABLE)&condition->variable);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
	WakeAllConditionVariable((PCONDITION_VARIABLE)&condition->variable);
	return 0;
}
#endif

/* ---------- time */

int clock_gettime(clockid_t clock, struct timespec *time)
{
	if (clock == CLOCK_MONOTONIC)
	{
		static LARGE_INTEGER frequency;
		LARGE_INTEGER counter;

		if (!frequency.QuadPart)
			QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		time->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
		time->tv_nsec = (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
	}
	else
	{
		/* 100 ns intervals since 1601 */
		FILETIME now;
		unsigned long long intervals;

#ifdef HALO_WIN9X
		/* 98/ME's system time moves in steps of 55 ms or so: read once,
		then carried on by the performance counter */
		static LARGE_INTEGER frequency, counter_start;
		static unsigned long long intervals_start;
		static volatile LONG started;
		LARGE_INTEGER counter;

		if (started != 2)
		{
			if (!InterlockedCompareExchange(&started, 1, 0))
			{
				QueryPerformanceFrequency(&frequency);
				QueryPerformanceCounter(&counter_start);
				GetSystemTimeAsFileTime(&now);
				intervals_start = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
				InterlockedExchange(&started, 2);
			}
			while (started != 2)
				Sleep(0);
		}
		QueryPerformanceCounter(&counter);
		intervals = intervals_start + (unsigned long long)((counter.QuadPart - counter_start.QuadPart) /
			frequency.QuadPart) * 10000000ULL + (unsigned long long)(((counter.QuadPart - counter_start.QuadPart) %
			frequency.QuadPart) * 10000000LL / frequency.QuadPart);
#else
		GetSystemTimePreciseAsFileTime(&now);
		intervals = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
#endif
		intervals -= 116444736000000000ULL;
		time->tv_sec = (time_t)(intervals / 10000000ULL);
		time->tv_nsec = (long)(intervals % 10000000ULL) * 100;
	}
	return 0;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
	long long milliseconds = (long long)duration->tv_sec * 1000 + (duration->tv_nsec + 999999) / 1000000;

	Sleep(milliseconds > 0x7fffffff ? 0x7fffffff : (DWORD)milliseconds);
	if (remaining)
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *time, struct timespec *remaining)
{
	struct timespec now;
	long long nanoseconds;

	if (!(flags & TIMER_ABSTIME))
		return nanosleep(time, remaining);
#ifdef HALO_WIN9X
	/* one processor: a yield until the deadline takes the time from the
	game, so the whole interval is slept, to the millisecond above it */
	for (;;)
	{
		clock_gettime(clock, &now);
		nanoseconds = ((long long)time->tv_sec - now.tv_sec) * 1000000000LL + (time->tv_nsec - now.tv_nsec);
		if (nanoseconds <= 0)
			return 0;
		Sleep((DWORD)((nanoseconds + 999999) / 1000000));
	}
#endif
	/* sleep until about a millisecond before the deadline (Sleep rounds
	up to the timer period), then yield until it passes */
	for (;;)
	{
		clock_gettime(clock, &now);
		nanoseconds = ((long long)time->tv_sec - now.tv_sec) * 1000000000LL + (time->tv_nsec - now.tv_nsec);
		if (nanoseconds <= 0)
			return 0;
		if (nanoseconds > 2000000)
			Sleep((DWORD)(nanoseconds / 1000000 - 1));
		else
			SwitchToThread();
	}
}

int sched_yield(void)
{
#ifdef HALO_WIN9X
	/* (98/ME have no SwitchToThread; Sleep(0) yields to threads of the same
	priority) */
	Sleep(0);
#else
	SwitchToThread();
#endif
	return 0;
}

long sysconf(int name)
{
	SYSTEM_INFO system;
#ifdef HALO_WIN9X
	MEMORYSTATUS memory;
#else
	MEMORYSTATUSEX memory;
#endif

	GetSystemInfo(&system);
	switch (name)
	{
	case _SC_PAGESIZE:
		return (long)system.dwPageSize;
	case _SC_NPROCESSORS_ONLN:
		return (long)system.dwNumberOfProcessors;
	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES:
		memory.dwLength = sizeof(memory);
#ifdef HALO_WIN9X
		GlobalMemoryStatus(&memory);
		return (long)((name == _SC_PHYS_PAGES ? memory.dwTotalPhys : memory.dwAvailPhys) / system.dwPageSize);
#else
		if (!GlobalMemoryStatusEx(&memory))
			return -1;
		return (long)((name == _SC_PHYS_PAGES ? memory.ullTotalPhys : memory.ullAvailPhys) / system.dwPageSize);
#endif
	default:
		errno = EINVAL;
		return -1;
	}
}

/* ---------- files */

#ifdef HALO_WIN9X
/* 98/ME: the file's position moved to offset and back, under a lock (the
descriptor's own reads and writes go on from where they were) */
static ssize_t positioned_transfer(int descriptor, void *buffer, size_t count, off_t offset, BOOL write)
{
	HANDLE handle = (HANDLE)_get_osfhandle(descriptor);
	LONG offset_high = (LONG)((unsigned long long)offset >> 32);
	LONG saved_high = 0;
	DWORD saved, transferred = 0, error = 0;
	BOOL result;

	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}
	EnterCriticalSection(&positioned_lock);
	saved = SetFilePointer(handle, 0, &saved_high, FILE_CURRENT);
	if (SetFilePointer(handle, (LONG)(DWORD)offset, &offset_high, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
		GetLastError() != NO_ERROR)
	{
		error = GetLastError();
		LeaveCriticalSection(&positioned_lock);
		errno = errno_from_windows_error(error);
		return -1;
	}
	result = write ?
		WriteFile(handle, buffer, (DWORD)count, &transferred, NULL) :
		ReadFile(handle, buffer, (DWORD)count, &transferred, NULL);
	if (!result)
		error = GetLastError();
	SetFilePointer(handle, (LONG)saved, &saved_high, FILE_BEGIN);
	LeaveCriticalSection(&positioned_lock);
	if (!result)
	{
		if (!write && error == ERROR_HANDLE_EOF)
			return 0;
		errno = errno_from_windows_error(error);
		return -1;
	}
	return (ssize_t)transferred;
}
#else
static ssize_t positioned_transfer(int descriptor, void *buffer, size_t count, off_t offset, BOOL write)
{
	HANDLE handle = (HANDLE)_get_osfhandle(descriptor);
	OVERLAPPED overlapped;
	DWORD transferred = 0;
	BOOL result;

	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}
	memset(&overlapped, 0, sizeof(overlapped));
	overlapped.Offset = (DWORD)offset;
	overlapped.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	result = write ?
		WriteFile(handle, buffer, (DWORD)count, &transferred, &overlapped) :
		ReadFile(handle, buffer, (DWORD)count, &transferred, &overlapped);
	if (!result)
	{
		if (!write && GetLastError() == ERROR_HANDLE_EOF)
			return 0;
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return (ssize_t)transferred;
}
#endif

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, buffer, count, offset, FALSE);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, (void *)buffer, count, offset, TRUE);
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
	DWORD length;
	DWORD index;

	if (strcmp(path, "/proc/self/exe") != 0)
	{
		errno = EINVAL;
		return -1;
	}
	length = GetModuleFileNameA(NULL, buffer, (DWORD)size);
	if (!length || length >= size)
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	for (index = 0; index < length; index++)
	{
		if (buffer[index] == '\\')
			buffer[index] = '/';
	}
	return (ssize_t)length;
}

int pause(void)
{
	Sleep(INFINITE);
	return -1;
}

/* ---------- memory mapping */

static DWORD windows_protection(int protection)
{
	switch (protection & (PROT_READ | PROT_WRITE | PROT_EXEC))
	{
	case PROT_NONE: return PAGE_NOACCESS;
	case PROT_READ: return PAGE_READONLY;
	case PROT_EXEC: return PAGE_EXECUTE;
	case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
	case PROT_READ | PROT_WRITE | PROT_EXEC:
	case PROT_WRITE | PROT_EXEC: return PAGE_EXECUTE_READWRITE;
	default: return PAGE_READWRITE;
	}
}

static BOOL address_reserved(void *address)
{
	MEMORY_BASIC_INFORMATION information;

	return address && VirtualQuery(address, &information, sizeof(information)) &&
		information.State != MEM_FREE;
}

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, long offset)
{
	void *result;

	(void)descriptor;
	(void)offset;
	if (!(flags & MAP_ANONYMOUS))
	{
		errno = ENODEV;
		return MAP_FAILED;
	}
	if (protection == PROT_NONE && (flags & MAP_NORESERVE))
	{
		if ((flags & MAP_FIXED) && address_reserved(address))
		{
			/* give pages inside a reservation back */
			if (!VirtualFree(address, length, MEM_DECOMMIT))
			{
				errno = errno_from_windows_error(GetLastError());
				return MAP_FAILED;
			}
			return address;
		}
		result = VirtualAlloc(address, length, MEM_RESERVE, PAGE_NOACCESS);
	}
	else if ((flags & MAP_FIXED) && address_reserved(address))
	{
		/* fresh zeroed pages inside a reservation: committing pages that
		are already committed would keep their contents */
		VirtualFree(address, length, MEM_DECOMMIT);
		result = VirtualAlloc(address, length, MEM_COMMIT, windows_protection(protection));
	}
	else
	{
		result = VirtualAlloc(address, length, MEM_RESERVE | MEM_COMMIT, windows_protection(protection));
	}
	if (!result)
	{
		errno = errno_from_windows_error(GetLastError());
		return MAP_FAILED;
	}
	return result;
}

int munmap(void *address, size_t length)
{
	MEMORY_BASIC_INFORMATION information;

	if (!VirtualQuery(address, &information, sizeof(information)))
	{
		errno = EINVAL;
		return -1;
	}
	if (information.AllocationBase == address &&
		!VirtualFree(address, 0, MEM_RELEASE))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	if (information.AllocationBase != address &&
		!VirtualFree(address, length, MEM_DECOMMIT))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return 0;
}

int mprotect(void *address, size_t length, int protection)
{
	DWORD previous;

	if (!VirtualProtect(address, length, windows_protection(protection), &previous))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return 0;
}

/* Each allocation in the range once: where it overlaps the range, where it
begins, and the module it is part of or what kind of memory it is. */
int posix_describe_address_range(posix_ulong address, posix_ulong size,
	char *buffer, posix_ulong buffer_size)
{
	unsigned long long position = address;
	unsigned long long end = (unsigned long long)address + size;
	posix_ulong used = 0;
	int found = 0;

	if (!buffer_size)
		return 0;
	buffer[0] = 0;
	while (position < end)
	{
		MEMORY_BASIC_INFORMATION information;
		unsigned long long next;
		int written = 0;

		if (!VirtualQuery((void *)(ULONG_PTR)position, &information, sizeof(information)))
		{
			/* above 2 GB without /LARGEADDRESSAWARE, or 9x's shared arena */
			snprintf(buffer + used, buffer_size - used, "%s0x%08lx and up: outside this process's address space",
				found ? "; " : "", (unsigned long)position);
			found = 1;
			break;
		}
		next = (unsigned long long)(ULONG_PTR)information.BaseAddress + information.RegionSize;
		if (information.State != MEM_FREE)
		{
			MEMORY_BASIC_INFORMATION following;
			char module[MAX_PATH];
			const char *what;

			/* the allocation's other regions in the range */
			while (next < end && VirtualQuery((void *)(ULONG_PTR)next, &following, sizeof(following)) &&
				following.State != MEM_FREE && following.AllocationBase == information.AllocationBase)
			{
				next = (unsigned long long)(ULONG_PTR)following.BaseAddress + following.RegionSize;
			}
			if (information.Type == MEM_IMAGE &&
				GetModuleFileNameA((HMODULE)information.AllocationBase, module, sizeof(module)))
			{
				what = module;
			}
			else
			{
				what = information.Type == MEM_IMAGE ? "an image" :
					information.Type == MEM_MAPPED ? "a mapped view" : "private memory";
			}
			written = snprintf(buffer + used, buffer_size - used, "%s0x%08lx-0x%08lx (allocated at %p): %s",
				found ? "; " : "", (unsigned long)position, (unsigned long)((next < end ? next : end) - 1),
				information.AllocationBase, what);
			found = 1;
			if (written < 0 || (posix_ulong)written >= buffer_size - used)
				break;
			used += (posix_ulong)written;
		}
		position = next;
	}
	return found;
}
