/*
WIN32_NET.C

The socket half of port/linux/src/posix.h for Windows (the Linux version is
posix_net.c). The Xbox's Winsock and Windows' share option numbers and error
codes, so this passes nearly everything straight through.

Windows 98/ME (HALO_WIN9X) has Winsock 2 but none of what came after it:
sockets come from socket() (WSASocketW's flag against inheritance is
Windows 7's, and refused), names are looked up with gethostbyname, this
machine's address is the one a UDP socket gets when it is connected
towards the internet (no sending is involved; IP Helper's adapter list is
XP's), and random bytes come from CryptoAPI, loaded at run time, or failing
that from a hash of what varies between machines and moments.
*/

/* select() takes as many sockets as the Linux version (FD_SETSIZE) */
#define FD_SETSIZE 1024

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef HALO_WIN9X
#include <wincrypt.h>
#include <stdio.h>
#else
#include <iphlpapi.h>
#include <bcrypt.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "posix.h"

/* mstcpip.h has it only in some Windows SDKs */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static __thread int last_error;

static int fail(void)
{
	last_error = WSAGetLastError();
	return -1;
}

static int succeed(int result)
{
	if (result == SOCKET_ERROR)
		return fail();
	last_error = 0;
	return result;
}

/* sockets are small integers on Windows too; the Xbox's are 32-bit */
static int from_socket(SOCKET socket)
{
	return socket == INVALID_SOCKET ? -1 : (int)socket;
}

static void start_winsock(void)
{
	static LONG started;

	if (!InterlockedCompareExchange(&started, 1, 0))
	{
		WSADATA data;

		WSAStartup(MAKEWORD(2, 2), &data);
	}
}

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	SOCKET result;

	start_winsock();
#ifdef HALO_WIN9X
	/* (98/ME report no ICMP port unreachable on a UDP socket: Windows 2000
	began that, and SIO_UDP_CONNRESET with it) */
	result = socket(family, type, protocol);
	if (result == INVALID_SOCKET)
		return fail();
#else
	result = WSASocketW(family, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (result == INVALID_SOCKET)
		return fail();
	if (type == SOCK_DGRAM)
	{
		/* Windows reports an ICMP port unreachable (a datagram to an address
		nothing listens on, such as a network.broadcast machine not running)
		as a WSAECONNRESET from the socket's next recvfrom; neither the Xbox
		nor Linux does */
		BOOL report = FALSE;
		DWORD returned = 0;

		WSAIoctl(result, SIO_UDP_CONNRESET, &report, sizeof(report), NULL, 0, &returned, NULL, NULL);
	}
#endif
	last_error = 0;
	return from_socket(result);
}

int posix_socket_close(int socket)
{
	return succeed(closesocket((SOCKET)socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	return succeed(bind((SOCKET)socket, address, address_length));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	return succeed(connect((SOCKET)socket, address, address_length));
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed(listen((SOCKET)socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	SOCKET result = accept((SOCKET)socket, address, address_length);

	if (result == INVALID_SOCKET)
		return fail();
	last_error = 0;
	return from_socket(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed(send((SOCKET)socket, buffer, length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	return succeed(sendto((SOCKET)socket, buffer, length, flags, address, address_length));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed(recv((SOCKET)socket, buffer, length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	return succeed(recvfrom((SOCKET)socket, buffer, length, flags, address, address_length));
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(shutdown((SOCKET)socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	u_long value = nonblocking ? 1 : 0;

	return succeed(ioctlsocket((SOCKET)socket, FIONBIO, &value));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	u_long available = 0;
	int result = ioctlsocket((SOCKET)socket, FIONREAD, &available);

	if (result != SOCKET_ERROR)
		*count = (posix_ulong)available;
	return succeed(result);
}

/* SOL_SOCKET options Windows knows; the Xbox's own (SO_ENCRYPT and the
like) have nothing to do here */
static int known_option(int level, int name)
{
	if (level != SOL_SOCKET)
		return 1;
	switch (name)
	{
	case SO_REUSEADDR:
	case SO_KEEPALIVE:
	case SO_BROADCAST:
	case SO_LINGER:
	case SO_SNDBUF:
	case SO_RCVBUF:
	case SO_ERROR:
	case SO_TYPE:
		return 1;
	default:
		return 0;
	}
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	if (!known_option(level, name))
	{
		last_error = 0;
		return 0;
	}
	return succeed(setsockopt((SOCKET)socket, level, name, value, length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	if (!known_option(level, name))
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	return succeed(getsockopt((SOCKET)socket, level, name, value, length));
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	return succeed(getsockname((SOCKET)socket, address, address_length));
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	return succeed(getpeername((SOCKET)socket, address, address_length));
}

static int fill_set(fd_set *set, const int *descriptors, int count)
{
	int index, filled = 0;

	FD_ZERO(set);
	for (index = 0; index < count; index++)
	{
		if (descriptors[index] >= 0 && set->fd_count < FD_SETSIZE)
		{
			FD_SET((SOCKET)descriptors[index], set);
			filled++;
		}
	}
	return filled;
}

static void keep_ready(fd_set *set, int *descriptors, int *count)
{
	int index, kept = 0;

	for (index = 0; index < *count; index++)
	{
		if (descriptors[index] >= 0 && FD_ISSET((SOCKET)descriptors[index], set))
			descriptors[kept++] = descriptors[index];
	}
	*count = kept;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	fd_set read_set, write_set, error_set;
	struct timeval timeout;
	int sockets = 0;
	int result;

	sockets += fill_set(&read_set, read, read ? *read_count : 0);
	sockets += fill_set(&write_set, write, write ? *write_count : 0);
	sockets += fill_set(&error_set, error, error ? *error_count : 0);
	if (!sockets)
	{
		/* Windows rejects a select() without sockets; POSIX waits */
		if (!infinite)
			Sleep((DWORD)(timeout_seconds * 1000 + timeout_microseconds / 1000));
		if (read)
			*read_count = 0;
		if (write)
			*write_count = 0;
		if (error)
			*error_count = 0;
		last_error = 0;
		return 0;
	}
	timeout.tv_sec = timeout_seconds;
	timeout.tv_usec = timeout_microseconds;
	result = select(0, read ? &read_set : NULL, write ? &write_set : NULL,
		error ? &error_set : NULL, infinite ? NULL : &timeout);
	if (result == SOCKET_ERROR)
		return fail();
	if (read)
		keep_ready(&read_set, read, read_count);
	if (write)
		keep_ready(&write_set, write, write_count);
	if (error)
		keep_ready(&error_set, error, error_count);
	last_error = 0;
	return result;
}

#ifdef HALO_WIN9X
/* the address of the interface the default route leaves by (connecting a
UDP socket sends nothing), else the first the host name resolves to (a
network without a gateway) */
posix_ulong posix_local_ipv4_address(void)
{
	struct sockaddr_in remote, local;
	int length = sizeof(local);
	posix_ulong result = 0;
	SOCKET probe;
	char name[256];

	start_winsock();
	probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (probe != INVALID_SOCKET)
	{
		memset(&remote, 0, sizeof(remote));
		remote.sin_family = AF_INET;
		/* (an address of the internet's, never sent to: 192.0.2.1 is
		RFC 5737's documentation network) */
		remote.sin_addr.s_addr = htonl(0xC0000201UL);
		remote.sin_port = htons(9);
		memset(&local, 0, sizeof(local));
		if (connect(probe, (struct sockaddr *)&remote, sizeof(remote)) == 0 &&
			getsockname(probe, (struct sockaddr *)&local, &length) == 0 &&
			local.sin_addr.s_addr != htonl(INADDR_ANY) && local.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			result = local.sin_addr.s_addr;
		}
		closesocket(probe);
	}
	if (!result && gethostname(name, sizeof(name)) == 0)
	{
		struct hostent *host = gethostbyname(name);
		int index;

		for (index = 0; host && host->h_addrtype == AF_INET && host->h_addr_list[index]; index++)
		{
			unsigned long address;

			memcpy(&address, host->h_addr_list[index], sizeof(address));
			if (address != htonl(INADDR_LOOPBACK) && address != htonl(INADDR_ANY))
			{
				result = address;
				break;
			}
		}
	}
	return result;
}
#else
posix_ulong posix_local_ipv4_address(void)
{
	ULONG size = 16 * 1024;
	IP_ADAPTER_ADDRESSES *adapters = NULL, *adapter;
	posix_ulong result = 0;
	ULONG status;

	do
	{
		free(adapters);
		adapters = malloc(size);
		if (!adapters)
			return 0;
		status = GetAdaptersAddresses(AF_INET,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, NULL,
			adapters, &size);
	}
	while (status == ERROR_BUFFER_OVERFLOW);
	if (status == NO_ERROR)
	{
		/* an adapter with a gateway first (the one the network and the
		internet are behind): a virtual machine's host-only adapter (VirtualBox,
		192.168.56.1) comes up first in the list and has none */
		int pass;

		for (pass = 0; pass < 2 && !result; pass++)
		for (adapter = adapters; adapter && !result; adapter = adapter->Next)
		{
			IP_ADAPTER_UNICAST_ADDRESS *address;

			if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			if (pass == 0 && !adapter->FirstGatewayAddress)
				continue;
			for (address = adapter->FirstUnicastAddress; address; address = address->Next)
			{
				struct sockaddr_in *ipv4 = (struct sockaddr_in *)address->Address.lpSockaddr;

				if (ipv4->sin_family == AF_INET && ipv4->sin_addr.s_addr != htonl(INADDR_LOOPBACK))
				{
					result = ipv4->sin_addr.s_addr;
					break;
				}
			}
		}
	}
	free(adapters);
	return result;
}
#endif

/* as on Linux: the link is an adapter with an address */
int posix_network_link_up(void)
{
	return posix_local_ipv4_address() != 0;
}

#ifdef HALO_WIN9X
/* p2p_crypto.c */
void p2p_sha256(const void *data, int size, unsigned char *digest);

/* CryptoAPI's generator (95 OSR2 with Internet Explorer 3.02, 98, ME); a
hash of the clocks, the cursor, the memory and the process otherwise, chained
from one call to the next */
void posix_random_bytes(void *buffer, posix_ulong size)
{
	typedef BOOL (WINAPI *acquire_function)(HCRYPTPROV *provider, LPCSTR container, LPCSTR name, DWORD type,
		DWORD flags);
	typedef BOOL (WINAPI *generate_function)(HCRYPTPROV provider, DWORD size, BYTE *buffer);
	static LONG looked;
	static HCRYPTPROV provider;
	static generate_function volatile generate;
	static unsigned char chain[16];
	static LONG counter;
	unsigned char *bytes = buffer;
	generate_function ready;

	if (!InterlockedExchange(&looked, 1))
	{
		HMODULE advapi = LoadLibraryA("advapi32.dll");
		acquire_function acquire = advapi ? (acquire_function)GetProcAddress(advapi, "CryptAcquireContextA") : NULL;
		generate_function found = advapi ? (generate_function)GetProcAddress(advapi, "CryptGenRandom") : NULL;

		/* (published only with the provider acquired: a thread here in the
		meantime takes the hash below) */
		if (acquire && found && acquire(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
			InterlockedExchangePointer((PVOID volatile *)&generate, (PVOID)found);
	}
	ready = generate;
	if (ready && ready(provider, (DWORD)size, bytes))
		return;
	while (size)
	{
		struct
		{
			unsigned char chain[16];
			LONG counter;
			unsigned long long cycles;
			LARGE_INTEGER performance;
			FILETIME time;
			DWORD ticks, process, thread;
			POINT cursor;
			MEMORYSTATUS memory;
			const void *stack;
		} entropy;
		unsigned char digest[32];
		posix_ulong count = size < 16 ? size : 16;

		memset(&entropy, 0, sizeof(entropy));
		memcpy(entropy.chain, chain, sizeof(chain));
		entropy.counter = InterlockedIncrement(&counter);
		entropy.cycles = __builtin_ia32_rdtsc();
		QueryPerformanceCounter(&entropy.performance);
		GetSystemTimeAsFileTime(&entropy.time);
		entropy.ticks = GetTickCount();
		entropy.process = GetCurrentProcessId();
		entropy.thread = GetCurrentThreadId();
		GetCursorPos(&entropy.cursor);
		entropy.memory.dwLength = sizeof(entropy.memory);
		GlobalMemoryStatus(&entropy.memory);
		entropy.stack = &entropy;
		p2p_sha256(&entropy, (int)sizeof(entropy), digest);
		/* half the digest out, the other half, never seen, on to the next */
		memcpy(bytes, digest, count);
		memcpy(chain, digest + 16, 16);
		bytes += count;
		size -= count;
	}
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	unsigned long address;
	struct hostent *entry;

	start_winsock();
	address = inet_addr(host);
	if (address != INADDR_NONE || !strcmp(host, "255.255.255.255"))
		return address;
	entry = gethostbyname(host);
	if (!entry || entry->h_addrtype != AF_INET || !entry->h_addr_list[0])
		return 0;
	memcpy(&address, entry->h_addr_list[0], sizeof(address));
	return address;
}
#else
void posix_random_bytes(void *buffer, posix_ulong size)
{
	BCryptGenRandom(NULL, buffer, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct addrinfo hints, *results;
	posix_ulong address = 0;

	start_winsock();
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &results) != 0)
		return 0;
	if (results && results->ai_addr && results->ai_addr->sa_family == AF_INET)
		address = ((struct sockaddr_in *)results->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(results);
	return address;
}
#endif
