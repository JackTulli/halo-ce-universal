/*
NX_NET.C

The socket helpers behind the network layer (port/linux/src/xnet.c, whose
posix.h names them: Winsock over BSD sockets, plus the system link's
addresses), over nxdk's lwIP stack. Compiled against nxdk's headers, as the
Linux build compiles its own against glibc's.

The pools of lwIP are sized when nxdk's network library is built: the
build needs it made with the options tools/xbox_build.py gives
(port/xbox/README.md).
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>
#include <lwip/api.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "../../linux/src/posix.h"
#include "nx.h"

/* nxdk's network interface (lib/nxdk/net.c) */
extern struct netif *g_pnetif;

/* Winsock error codes (winsockx.h) */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEHOSTUNREACH 10065

/* Winsock SOL_SOCKET option values (winsockx.h) */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_REUSEADDR 0x0004
#define WINSOCK_SO_KEEPALIVE 0x0008
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_LINGER 0x0080
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

static __thread int last_error;

static int fail(void)
{
	switch (errno)
	{
	case EINTR: last_error = WSAEINTR; break;
	case EBADF: last_error = WSAEBADF; break;
	case EACCES: last_error = WSAEACCES; break;
	case EFAULT: last_error = WSAEFAULT; break;
	case EMFILE: last_error = WSAEMFILE; break;
#ifdef ENFILE
	/* lwIP's socket table full */
	case ENFILE: last_error = WSAEMFILE; break;
#endif
	case EAGAIN: last_error = WSAEWOULDBLOCK; break;
#if EWOULDBLOCK != EAGAIN
	/* lwIP reports a non-blocking socket's empty read or full send as
	EWOULDBLOCK, which is its own number in nxdk's C library (140, EAGAIN is
	11); unmapped it became WSAEINVAL and the game closed the endpoint */
	case EWOULDBLOCK: last_error = WSAEWOULDBLOCK; break;
#endif
	case EIO: last_error = WSAENETDOWN; break;
	case EINPROGRESS: last_error = WSAEINPROGRESS; break;
	case EALREADY: last_error = WSAEALREADY; break;
	case ENOTSOCK: last_error = WSAENOTSOCK; break;
	case EDESTADDRREQ: last_error = WSAEDESTADDRREQ; break;
	case EMSGSIZE: last_error = WSAEMSGSIZE; break;
	case EPROTOTYPE: last_error = WSAEPROTOTYPE; break;
	case ENOPROTOOPT: last_error = WSAENOPROTOOPT; break;
	case EPROTONOSUPPORT: last_error = WSAEPROTONOSUPPORT; break;
	case EOPNOTSUPP: last_error = WSAEOPNOTSUPP; break;
	case EAFNOSUPPORT: last_error = WSAEAFNOSUPPORT; break;
	case EADDRINUSE: last_error = WSAEADDRINUSE; break;
	case EADDRNOTAVAIL: last_error = WSAEADDRNOTAVAIL; break;
	case ENETDOWN: last_error = WSAENETDOWN; break;
	case ENETUNREACH: last_error = WSAENETUNREACH; break;
	case ECONNABORTED: last_error = WSAECONNABORTED; break;
	case ECONNRESET: last_error = WSAECONNRESET; break;
	case ENOBUFS: case ENOMEM: last_error = WSAENOBUFS; break;
	case EISCONN: last_error = WSAEISCONN; break;
	case ENOTCONN: last_error = WSAENOTCONN; break;
	case ETIMEDOUT: last_error = WSAETIMEDOUT; break;
	case ECONNREFUSED: last_error = WSAECONNREFUSED; break;
	case EHOSTUNREACH: last_error = WSAEHOSTUNREACH; break;
	default: last_error = WSAEINVAL; break;
	}
	return -1;
}

static int succeed(int result)
{
	if (result < 0)
		return fail();
	last_error = 0;
	return result;
}

/* ---------- addresses

Winsock's sockaddr_in starts with a 16-bit family; lwIP's (BSD's) with a
length byte and an 8-bit family. The game's addresses come in Winsock's
layout and go back out in it. */

#define WINSOCK_AF_INET 2

struct winsock_sockaddr_in
{
	unsigned short family;
	unsigned short port;
	unsigned int address;
	char zero[8];
};

/* fails (as Winsock does, with WSAEFAULT or WSAEAFNOSUPPORT) unless
address is an IPv4 address */
static int to_lwip_address(const void *address, int length, struct sockaddr_in *result)
{
	const struct winsock_sockaddr_in *winsock = address;

	if (!address || length < (int)sizeof(*winsock))
	{
		last_error = WSAEFAULT;
		return 0;
	}
	if (winsock->family != WINSOCK_AF_INET)
	{
		last_error = WSAEAFNOSUPPORT;
		return 0;
	}
	memset(result, 0, sizeof(*result));
	result->sin_len = sizeof(*result);
	result->sin_family = AF_INET;
	result->sin_port = winsock->port;
	result->sin_addr.s_addr = winsock->address;
	/* the console talking to itself (a local game's client and server)
	goes to its own address, which lwIP loops back */
	if (winsock->address == lwip_htonl(0x7F000001UL) && posix_local_ipv4_address())
		result->sin_addr.s_addr = posix_local_ipv4_address();
	return 1;
}

static void from_lwip_address(const struct sockaddr_in *address, void *result, int *length)
{
	struct winsock_sockaddr_in winsock;

	if (!result || !length)
		return;
	memset(&winsock, 0, sizeof(winsock));
	winsock.family = WINSOCK_AF_INET;
	winsock.port = address->sin_port;
	winsock.address = address->sin_addr.s_addr;
	/* and what came from its own address came from 127.0.0.1 */
	if (winsock.address && winsock.address == posix_local_ipv4_address())
		winsock.address = lwip_htonl(0x7F000001UL);
	memcpy(result, &winsock, *length < (int)sizeof(winsock) ? (size_t)*length : sizeof(winsock));
	*length = sizeof(winsock);
}

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	return succeed(lwip_socket(family, type, protocol));
}

int posix_socket_close(int socket)
{
	return succeed(lwip_close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_in local;

	if (!to_lwip_address(address, address_length, &local))
		return -1;
	return succeed(lwip_bind(socket, (struct sockaddr *)&local, sizeof(local)));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	/* a non-blocking connect under way is WSAEWOULDBLOCK to the game
	(port/linux/src/posix_net.c says why) */
	struct sockaddr_in peer;
	int result;

	if (!to_lwip_address(address, address_length, &peer))
		return -1;
	result = lwip_connect(socket, (struct sockaddr *)&peer, sizeof(peer));
	if (result < 0 && errno == EINPROGRESS)
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed(lwip_listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_in peer;
	socklen_t length = sizeof(peer);
	int result = lwip_accept(socket, (struct sockaddr *)&peer, &length);

	if (result >= 0)
		from_lwip_address(&peer, address, address_length);
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed((int)lwip_send(socket, buffer, (size_t)length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_in destination;

	if (!address)
		return succeed((int)lwip_send(socket, buffer, (size_t)length, flags));
	if (!to_lwip_address(address, address_length, &destination))
		return -1;
	return succeed((int)lwip_sendto(socket, buffer, (size_t)length, flags, (struct sockaddr *)&destination,
		sizeof(destination)));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed((int)lwip_recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct sockaddr_in source;
	socklen_t source_length = sizeof(source);
	int result = (int)lwip_recvfrom(socket, buffer, (size_t)length, flags, (struct sockaddr *)&source,
		&source_length);

	if (result >= 0)
		from_lwip_address(&source, address, address_length);
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(lwip_shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags = lwip_fcntl(socket, F_GETFL, 0);

	if (flags < 0)
		return fail();
	flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return succeed(lwip_fcntl(socket, F_SETFL, flags));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = lwip_ioctl(socket, FIONREAD, &available);

	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed(result);
}

/* -1: an option lwIP does not have, which the game may set freely (buffer
sizes); -2: an Xbox-only option; 0: translated */
static int translate_option(int level, int name, int *host_level, int *host_name)
{
	if (level != WINSOCK_SOL_SOCKET)
	{
		/* IPPROTO_IP / IPPROTO_TCP option numbers are shared */
		*host_level = level;
		*host_name = name;
		return 0;
	}
	*host_level = SOL_SOCKET;
	switch (name)
	{
	case WINSOCK_SO_REUSEADDR: *host_name = SO_REUSEADDR; return 0;
	case WINSOCK_SO_KEEPALIVE: *host_name = SO_KEEPALIVE; return 0;
	case WINSOCK_SO_BROADCAST: *host_name = SO_BROADCAST; return 0;
	case WINSOCK_SO_LINGER: *host_name = SO_LINGER; return 0;
	case WINSOCK_SO_ERROR: *host_name = SO_ERROR; return 0;
	case WINSOCK_SO_TYPE: *host_name = SO_TYPE; return 0;
	/* lwIP's buffers have their sizes */
	case WINSOCK_SO_SNDBUF: case WINSOCK_SO_RCVBUF: return -1;
	default: return -2;
	}
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int host_level, host_name;

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		/* buffer sizes, and Xbox-only options such as SO_ENCRYPT: nothing to do */
		last_error = 0;
		return 0;
	}
	return succeed(lwip_setsockopt(socket, host_level, host_name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int host_level, host_name;
	socklen_t socket_length = (socklen_t)*length;
	int result;

	if (translate_option(level, name, &host_level, &host_name) == -1 && *length >= (int)sizeof(int))
	{
		/* the buffer sizes lwIP has no option for: a plausible size */
		*(int *)value = 65536;
		*length = sizeof(int);
		last_error = 0;
		return 0;
	}
	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = lwip_getsockopt(socket, host_level, host_name, value, &socket_length);
	*length = (int)socket_length;
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_in local;
	socklen_t length = sizeof(local);
	int result = lwip_getsockname(socket, (struct sockaddr *)&local, &length);

	if (result >= 0)
		from_lwip_address(&local, address, address_length);
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_in peer;
	socklen_t length = sizeof(peer);
	int result = lwip_getpeername(socket, (struct sockaddr *)&peer, &length);

	if (result >= 0)
		from_lwip_address(&peer, address, address_length);
	return succeed(result);
}

static int fill_set(fd_set *set, const int *descriptors, int count, int maximum)
{
	int index;

	FD_ZERO(set);
	for (index = 0; index < count; index++)
	{
		if (descriptors[index] >= LWIP_SOCKET_OFFSET && descriptors[index] < LWIP_SOCKET_OFFSET + FD_SETSIZE)
		{
			FD_SET(descriptors[index], set);
			if (descriptors[index] > maximum)
				maximum = descriptors[index];
		}
	}
	return maximum;
}

static void keep_ready(fd_set *set, int *descriptors, int *count)
{
	int index, kept = 0;

	for (index = 0; index < *count; index++)
	{
		if (descriptors[index] >= LWIP_SOCKET_OFFSET && descriptors[index] < LWIP_SOCKET_OFFSET + FD_SETSIZE &&
			FD_ISSET(descriptors[index], set))
			descriptors[kept++] = descriptors[index];
	}
	*count = kept;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	fd_set read_set, write_set, error_set;
	struct timeval timeout;
	int maximum = -1;
	int result;

	maximum = fill_set(&read_set, read, read ? *read_count : 0, maximum);
	maximum = fill_set(&write_set, write, write ? *write_count : 0, maximum);
	maximum = fill_set(&error_set, error, error ? *error_count : 0, maximum);
	timeout.tv_sec = timeout_seconds;
	timeout.tv_usec = timeout_microseconds;
	result = lwip_select(maximum + 1, read ? &read_set : NULL, write ? &write_set : NULL,
		error ? &error_set : NULL, infinite ? NULL : &timeout);
	if (result < 0)
		return fail();
	if (write)
	{
		/* Winsock reports a socket writeable once its connect has succeeded;
		one whose connect failed is not (port/linux/src/posix_net.c) */
		int index;

		for (index = 0; index < *write_count; index++)
		{
			int descriptor = write[index];
			int pending = 0;
			socklen_t length = sizeof(pending);

			if (descriptor >= LWIP_SOCKET_OFFSET && descriptor < LWIP_SOCKET_OFFSET + FD_SETSIZE &&
				FD_ISSET(descriptor, &write_set) &&
				lwip_getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
			{
				FD_CLR(descriptor, &write_set);
				result--;
				errno = pending;
				fail();
			}
		}
	}
	if (read)
		keep_ready(&read_set, read, read_count);
	if (write)
		keep_ready(&write_set, write, write_count);
	if (error)
		keep_ready(&error_set, error, error_count);
	if (result > 0)
		last_error = 0;
	return result;
}

/* the console's address (network byte order), once DHCP has given one */
/* lwIP's default interface, which nxdk's start-up makes the console's */
static struct netif *the_netif(void)
{
	return netif_default ? netif_default : g_pnetif;
}

posix_ulong posix_local_ipv4_address(void)
{
	struct netif *netif = the_netif();

	if (!netif)
		return 0;
	return ip4_addr_get_u32(netif_ip4_addr(netif));
}

/* the interface being up: the emulator's adapter never reports the
cable, so the link flag itself is not consulted */
int posix_network_link_up(void)
{
	struct netif *netif = the_netif();

	return netif && netif_is_up(netif);
}

/* for the start-up log */
void nx_net_describe(char *text, unsigned long size)
{
	struct netif *netif = the_netif();

	snprintf(text, size, "netif %p (g_pnetif %p) flags %02x", (void *)netif, (void *)g_pnetif, netif ? netif->flags : 0);
}

/* ---------- random bytes

Internet play's identifiers, tokens, tunnel keys and nonces come from here
(p2p.c, p2p_master.c, p2p_crypto.c). The game's generator (nx_random, a
linear congruential one whose byte stream repeats every 2^24 bytes and is
pinned down by one public identifier) is not for them: this is an
HMAC-SHA-256 generator in the manner of NIST's HMAC_DRBG, keyed once from
what the console can tell about itself and its moment (the cycle counter's
jitter across kernel calls, the time, the per-console hard disk key, the
game generator's state) and rekeyed after every request, with the cycle
counter stirred in each time. */

void p2p_hmac_sha256(const unsigned char *key, int key_size, const void *data, int size, unsigned char *digest);

static unsigned char drbg_key[32], drbg_value[32];
static int drbg_seeded;
static volatile long drbg_lock;

static void drbg_update(const void *data, int size)
{
	unsigned char block[32 + 1 + 64];
	int length = 0;

	memcpy(block, drbg_value, 32);
	block[32] = 0;
	if (size > 64)
		size = 64;
	if (size > 0)
		memcpy(block + 33, data, (size_t)size);
	length = 33 + (size > 0 ? size : 0);
	p2p_hmac_sha256(drbg_key, 32, block, length, drbg_key);
	p2p_hmac_sha256(drbg_key, 32, drbg_value, 32, drbg_value);
	block[32] = 1;
	memcpy(block, drbg_value, 32);
	p2p_hmac_sha256(drbg_key, 32, block, length, drbg_key);
	p2p_hmac_sha256(drbg_key, 32, drbg_value, 32, drbg_value);
}

static void drbg_seed(void)
{
	unsigned long long samples[64];
	LARGE_INTEGER time;
	unsigned long state = nx_random();
	int index;

	memset(drbg_key, 0, sizeof(drbg_key));
	memset(drbg_value, 1, sizeof(drbg_value));
	/* the cycle counter across kernel calls whose length varies: their
	timing's jitter (interrupts, the memory, the emulator's host) */
	for (index = 0; index < 64; index++)
	{
		samples[index] = __builtin_ia32_rdtsc();
		if (index & 1)
			SwitchToThread();
		else
			samples[index] ^= (unsigned long long)KeQueryPerformanceCounter() << 7;
	}
	drbg_update(samples, sizeof(samples));
	KeQuerySystemTime(&time);
	drbg_update(&time, sizeof(time));
	drbg_update(XboxHDKey, sizeof(XboxHDKey));
	drbg_update(&state, sizeof(state));
	drbg_seeded = 1;
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	unsigned char *cursor = buffer;
	unsigned long long stir;

	while (__atomic_exchange_n(&drbg_lock, 1, __ATOMIC_ACQUIRE))
		SwitchToThread();
	if (!drbg_seeded)
		drbg_seed();
	while (size > 0)
	{
		posix_ulong count = size < 32 ? size : 32;

		p2p_hmac_sha256(drbg_key, 32, drbg_value, 32, drbg_value);
		memcpy(cursor, drbg_value, count);
		cursor += count;
		size -= count;
	}
	stir = __builtin_ia32_rdtsc();
	drbg_update(&stir, sizeof(stir));
	__atomic_store_n(&drbg_lock, 0, __ATOMIC_RELEASE);
}

/* a dotted quad, or a name through lwIP's DNS (the servers DHCP gave);
blocks, so the caller must not hold locks the game's threads want */
posix_ulong posix_resolve_ipv4(const char *host)
{
	posix_ulong address = ipaddr_addr(host);
	ip_addr_t looked_up;

	if (address != IPADDR_NONE)
		return address;
	if (netconn_gethostbyname(host, &looked_up) != ERR_OK || !IP_IS_V4(&looked_up))
		return 0;
	return ip4_addr_get_u32(ip_2_ip4(&looked_up));
}
