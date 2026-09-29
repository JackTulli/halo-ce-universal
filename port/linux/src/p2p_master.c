/*
P2P_MASTER.C

Internet play's master server (p2p.c): one UDP server, network.master_server
(port/master), that every copy of the game talks to through its tunnel
socket. It does three things, and knows nothing of the game's packets:

- It lists games. A hosting machine says so every few seconds (HELLO), with
  the token that lets others join it and the addresses it can be reached
  at. A machine whose game searches for system link games sends the server
  the game's search broadcasts (SEARCH); the server hands each one to every
  host, whose game answers as it would on a LAN, with an advertisement
  broadcast, which the host sends on to the server (ADVERTISEMENT); the
  server hands that to every machine that searched lately (GAME), with the
  host's token and addresses. The machine lists the host as a peer and gives
  the advertisement to its game as if the host, at its virtual address, had
  sent it: the game shows it under Multiplayer, System Link, and choosing it
  connects to the host, which starts the join (p2p.c, p2p_outgoing).
- It passes signalling (RELAY). A joiner sends the host JOIN: its
  identifier, a nonce and its addresses; the host answers ACCEPT: the nonce,
  a key for their tunnel and its own addresses. Both are sealed with a key
  derived from the token (p2p_crypto.c), so the server can read and forge
  neither. The joiner repeats JOIN until the tunnel reaches the host.
- It tells each machine its public address (WELCOME, answering HELLO): the
  address the tunnel's NAT mapping has, which the machine then offers.

Invite links (halo://join/...) go through the same signalling, so they work
between any two copies that use the same server, listed or not.

Messages start with "HM", a version and a type; identifiers are 6 bytes,
ports and addresses are in network byte order, and a candidate list is a
count and 6 bytes (address, port) each:

  HELLO          id, flags (1: hosting), players (the machines the host's tunnel
                 carries), token (16; zeros unless hosting), candidates
  SEARCH         id, source port, destination port, the game's datagram
  ADVERTISEMENT  id, source port, destination port, the game's datagram
  RELAY          from id, to id, a sealed signal
  WELCOME        address, port (the sender's, as the server sees them)
  SEARCH (17)    searcher id, source port, destination port, the datagram
  GAME           host id, token, candidates, source port, destination port, the datagram
  RELAYED        from id, the sealed signal

The game thread hands broadcasts in (p2p_master_broadcast_sent); the p2p
thread does the rest, under p2p_lock.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	MESSAGE_VERSION = 1,
	HEADER_SIZE = 4,
	/* the largest datagram the game sends (WSAStartup's iMaxUdpDg) */
	MAXIMUM_PAYLOAD_SIZE = 1264,
	CANDIDATES_SIZE = 1 + 6 * P2P_MAXIMUM_CANDIDATES,
	MAXIMUM_MESSAGE_SIZE = HEADER_SIZE + P2P_IDENTIFIER_SIZE + P2P_TOKEN_SIZE + CANDIDATES_SIZE + 4 +
		MAXIMUM_PAYLOAD_SIZE,
	MAXIMUM_SIGNAL_SIZE = 256,
	NONCE_SIZE = 8,
	/* the hosted lobby's report (port/linux/game/network_lobby.c) and the
	server's round-trip probes */
	MAXIMUM_LOBBY_SIZE = 1200,
	PING_SIZE = 8,
	/* the joiners a host remembers, so a repeated request gets the same
	key: as many as it takes */
	MAXIMUM_JOINERS = P2P_MAXIMUM_PEERS + 1,
	/* advertisements waiting for the p2p thread: the game answers one
	search at a time */
	MAXIMUM_QUEUED = 8,
	DEFAULT_PORT = 27920,

	/* milliseconds */
	HELLO_INTERVAL = 2000,
	HOSTING_HELLO_INTERVAL = 3000,
	SEARCH_INTERVAL = 1000,
	SEARCHING_TIME = 5000,
	JOIN_INTERVAL = 2000,
	ANSWER_INTERVAL = 1000,
	SILENCE_TIMEOUT = 10000,
	LOBBY_INTERVAL = 2000,
	LOOKUP_RETRY_INTERVAL = 30000,
	/* how long a join waits for the server to tell this machine's public
	address */
	READY_WAIT = 4000,
};

/* to the server */
enum
{
	_message_hello = 1,
	_message_search,
	_message_advertisement,
	_message_relay,
	/* the hosted lobby: its name, map, game type and players, for the
	server's status page */
	_message_lobby,
	/* the answer to a ping, at once, for the server's round trip */
	_message_pong,
};

/* from it */
enum
{
	_message_welcome = 16,
	_message_search_relayed,
	_message_game,
	_message_relayed,
	_message_ping,
};

/* signalling, inside relays, sealed */
enum
{
	_signal_join = 'J',
	_signal_accept = 'A',
	SIGNAL_VERSION = 1,
};

struct joiner
{
	unsigned char identifier[P2P_IDENTIFIER_SIZE];
	unsigned char nonce[NONCE_SIZE];
	unsigned char key[P2P_SHA256_SIZE];
	unsigned long answered_time;
	int used;
};

struct queued
{
	unsigned char data[MAXIMUM_PAYLOAD_SIZE];
	int size;
	unsigned short source_port;
	unsigned short destination_port;
};

static struct
{
	int started;
	char host_name[128];
	unsigned short port;
	/* 0 until looked up */
	unsigned long address;
	unsigned long looked_up_time;
	int reported_lookup;
	unsigned long hello_sent_time;
	unsigned long heard_time;
	/* the first time a hello was wanted; for p2p_master_ready */
	unsigned long wanted_time;
	int reported_silence;
	int has_mapped;
	struct p2p_candidate mapped;

	/* hosting */
	int hosting;
	unsigned char host_token[P2P_TOKEN_SIZE];
	unsigned char host_key[P2P_SHA256_SIZE];
	struct joiner joiners[MAXIMUM_JOINERS];
	int next_joiner;
	/* the socket relayed searches reach the hosting game's server from */
	int injector_socket;
	unsigned short injector_port;
	struct queued advertisements[MAXIMUM_QUEUED];
	int advertisement_count;
	/* the lobby's latest report, and when it last went */
	unsigned char lobby[MAXIMUM_LOBBY_SIZE];
	int lobby_size;
	unsigned long lobby_sent_time;

	/* searching: the newest search broadcast, until it is sent on */
	struct queued search;
	int search_pending;
	unsigned long search_time;
	unsigned long search_sent_time;

	/* joining */
	int joining;
	unsigned char join_host[P2P_IDENTIFIER_SIZE];
	unsigned char join_key[P2P_SHA256_SIZE];
	unsigned char join_nonce[NONCE_SIZE];
	unsigned long join_sent_time;
} master = { .injector_socket = -1 };

/* ---------- helpers */

static int elapsed(unsigned long since, unsigned long time)
{
	return (long)(p2p_now() - since) >= (long)time;
}

static unsigned short network_short(unsigned short value)
{
	return (unsigned short)(value << 8 | value >> 8);
}

static unsigned long loopback(void)
{
	unsigned long value = 0x7F000001UL;
	unsigned char bytes[4] = { 127, 0, 0, 1 };

	memcpy(&value, bytes, 4);
	return value;
}

static const char *address_text(unsigned long address, unsigned short port, char *text)
{
	unsigned char bytes[4];

	memcpy(bytes, &address, 4);
	sprintf(text, "%u.%u.%u.%u:%u", bytes[0], bytes[1], bytes[2], bytes[3], network_short(port));
	return text;
}

static void derive(const unsigned char *token, const char *label, unsigned char *digest)
{
	p2p_hmac_sha256(token, P2P_TOKEN_SIZE, label, (int)strlen(label), digest);
}

static int put_candidates(unsigned char *message)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	int count = p2p_local_candidates(candidates, P2P_MAXIMUM_CANDIDATES);
	int index;

	message[0] = (unsigned char)count;
	for (index = 0; index < count; index++)
	{
		memcpy(message + 1 + index * 6, &candidates[index].address, 4);
		memcpy(message + 1 + index * 6 + 4, &candidates[index].port, 2);
	}
	return 1 + count * 6;
}

/* returns the count, or -1 for a malformed list; *used takes its size */
static int get_candidates(const unsigned char *message, int size, struct p2p_candidate *candidates, int *used)
{
	int count;
	int index;

	if (size < 1)
		return -1;
	count = message[0];
	if (count > P2P_MAXIMUM_CANDIDATES || size < 1 + count * 6)
		return -1;
	for (index = 0; index < count; index++)
	{
		memcpy(&candidates[index].address, message + 1 + index * 6, 4);
		memcpy(&candidates[index].port, message + 1 + index * 6 + 4, 2);
	}
	*used = 1 + count * 6;
	return count;
}

static int put_header(unsigned char *message, unsigned char type)
{
	message[0] = 'H';
	message[1] = 'M';
	message[2] = MESSAGE_VERSION;
	message[3] = type;
	return HEADER_SIZE;
}

static void send_to_server(const unsigned char *message, int size)
{
	if (master.address)
		p2p_tunnel_send(message, size, master.address, master.port);
}

/* ---------- the server */

void p2p_master_start(void)
{
	const char *text;
	const char *colon;
	size_t length;

	if (master.started)
		return;
	master.started = 1;
	master.port = network_short(DEFAULT_PORT);
	text = config_string("network.master_server");
	while (text && *text == ' ')
		text++;
	length = text ? strlen(text) : 0;
	while (length && text[length - 1] == ' ')
		length--;
	if (!length)
	{
		platform_log("Internet play: no master server (network.master_server): games on the internet are not "
			"listed, and invites cannot be answered");
		return;
	}
	if (length >= sizeof(master.host_name))
		length = sizeof(master.host_name) - 1;
	memcpy(master.host_name, text, length);
	master.host_name[length] = 0;
	colon = strchr(master.host_name, ':');
	if (colon)
	{
		master.port = network_short((unsigned short)atoi(colon + 1));
		master.host_name[colon - master.host_name] = 0;
	}
}

static int configured(void)
{
	return master.host_name[0] != 0;
}

/* on the p2p thread, which lets go of the lock while the name is looked up */
static void look_up(void)
{
	char text[32];

	if (master.address || !configured())
		return;
	if (master.looked_up_time && !elapsed(master.looked_up_time, LOOKUP_RETRY_INTERVAL))
		return;
	master.looked_up_time = p2p_now();
	master.address = p2p_resolve(master.host_name);
	if (!master.address)
	{
		if (!master.reported_lookup++)
			platform_log("Internet play: cannot look up the master server %s", master.host_name);
		return;
	}
	platform_log("Internet play: the master server is %s (%s)", master.host_name,
		address_text(master.address, master.port, text));
}

static void send_hello(void)
{
	unsigned char message[HEADER_SIZE + P2P_IDENTIFIER_SIZE + 2 + P2P_TOKEN_SIZE + CANDIDATES_SIZE];
	int size = put_header(message, _message_hello);
	int players = master.hosting ? p2p_connected_player_count() : 0;

	memcpy(message + size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
	size += P2P_IDENTIFIER_SIZE;
	message[size++] = master.hosting ? 1 : 0;
	message[size++] = (unsigned char)(players > 255 ? 255 : players);
	if (master.hosting)
		memcpy(message + size, master.host_token, P2P_TOKEN_SIZE);
	else
		memset(message + size, 0, P2P_TOKEN_SIZE);
	size += P2P_TOKEN_SIZE;
	size += put_candidates(message + size);
	send_to_server(message, size);
	master.hello_sent_time = p2p_now();
	if (!master.wanted_time)
		master.wanted_time = master.hello_sent_time;
}

static void send_datagram(unsigned char type, const struct queued *datagram)
{
	unsigned char message[MAXIMUM_MESSAGE_SIZE];
	int size = put_header(message, type);

	memcpy(message + size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
	size += P2P_IDENTIFIER_SIZE;
	memcpy(message + size, &datagram->source_port, 2);
	size += 2;
	memcpy(message + size, &datagram->destination_port, 2);
	size += 2;
	memcpy(message + size, datagram->data, (size_t)datagram->size);
	size += datagram->size;
	send_to_server(message, size);
}

static void send_relay(const unsigned char *to, const unsigned char *sealed, int sealed_size)
{
	unsigned char message[HEADER_SIZE + 2 * P2P_IDENTIFIER_SIZE + MAXIMUM_SIGNAL_SIZE + P2P_SEAL_OVERHEAD];
	int size = put_header(message, _message_relay);

	memcpy(message + size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
	size += P2P_IDENTIFIER_SIZE;
	memcpy(message + size, to, P2P_IDENTIFIER_SIZE);
	size += P2P_IDENTIFIER_SIZE;
	memcpy(message + size, sealed, (size_t)sealed_size);
	size += sealed_size;
	send_to_server(message, size);
}

/* ---------- signalling */

static void send_join(void)
{
	unsigned char message[MAXIMUM_SIGNAL_SIZE];
	unsigned char sealed[MAXIMUM_SIGNAL_SIZE + P2P_SEAL_OVERHEAD];
	int size = 0;

	message[size++] = _signal_join;
	message[size++] = SIGNAL_VERSION;
	memcpy(message + size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
	size += P2P_IDENTIFIER_SIZE;
	memcpy(message + size, master.join_nonce, NONCE_SIZE);
	size += NONCE_SIZE;
	size += put_candidates(message + size);
	size = p2p_seal(master.join_key, message, size, sealed);
	send_relay(master.join_host, sealed, size);
	master.join_sent_time = p2p_now();
}

/* the host: a joiner asked */
static void join_received(const unsigned char *message, int size)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	unsigned char answer[MAXIMUM_SIGNAL_SIZE];
	unsigned char sealed[MAXIMUM_SIGNAL_SIZE + P2P_SEAL_OVERHEAD];
	const unsigned char *identifier = message + 2;
	const unsigned char *nonce = identifier + P2P_IDENTIFIER_SIZE;
	struct joiner *joiner = NULL;
	int count;
	int used;
	int index;
	int answer_size = 0;

	if (size < 2 + P2P_IDENTIFIER_SIZE + NONCE_SIZE + 1)
		return;
	count = get_candidates(nonce + NONCE_SIZE, size - (2 + P2P_IDENTIFIER_SIZE + NONCE_SIZE), candidates, &used);
	if (count < 0)
		return;
	for (index = 0; index < MAXIMUM_JOINERS; index++)
	{
		if (master.joiners[index].used &&
			!memcmp(master.joiners[index].identifier, identifier, P2P_IDENTIFIER_SIZE))
		{
			joiner = &master.joiners[index];
			break;
		}
	}
	if (joiner && !memcmp(joiner->nonce, nonce, NONCE_SIZE))
	{
		/* the same request again, repeated until the tunnel connects */
		if (!elapsed(joiner->answered_time, ANSWER_INTERVAL))
			return;
	}
	else
	{
		if (!joiner)
		{
			joiner = &master.joiners[master.next_joiner++ % MAXIMUM_JOINERS];
			memcpy(joiner->identifier, identifier, P2P_IDENTIFIER_SIZE);
			joiner->used = 1;
		}
		memcpy(joiner->nonce, nonce, NONCE_SIZE);
		posix_random_bytes(joiner->key, sizeof(joiner->key));
	}
	joiner->answered_time = p2p_now();
	p2p_peer_offered(identifier, joiner->key, candidates, count, 0);

	answer[answer_size++] = _signal_accept;
	answer[answer_size++] = SIGNAL_VERSION;
	memcpy(answer + answer_size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
	answer_size += P2P_IDENTIFIER_SIZE;
	memcpy(answer + answer_size, identifier, P2P_IDENTIFIER_SIZE);
	answer_size += P2P_IDENTIFIER_SIZE;
	memcpy(answer + answer_size, nonce, NONCE_SIZE);
	answer_size += NONCE_SIZE;
	memcpy(answer + answer_size, joiner->key, P2P_SHA256_SIZE);
	answer_size += P2P_SHA256_SIZE;
	answer_size += put_candidates(answer + answer_size);
	answer_size = p2p_seal(master.host_key, answer, answer_size, sealed);
	send_relay(identifier, sealed, answer_size);
}

/* the joiner: the host answered */
static void accept_received(const unsigned char *message, int size)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	const unsigned char *host = message + 2;
	const unsigned char *joiner = host + P2P_IDENTIFIER_SIZE;
	const unsigned char *nonce = joiner + P2P_IDENTIFIER_SIZE;
	const unsigned char *key = nonce + NONCE_SIZE;
	int fixed = 2 + 2 * P2P_IDENTIFIER_SIZE + NONCE_SIZE + P2P_SHA256_SIZE;
	int count;
	int used;

	if (size < fixed + 1 || memcmp(host, master.join_host, P2P_IDENTIFIER_SIZE) ||
		memcmp(joiner, p2p_identifier(), P2P_IDENTIFIER_SIZE) || memcmp(nonce, master.join_nonce, NONCE_SIZE))
		return;
	count = get_candidates(message + fixed, size - fixed, candidates, &used);
	if (count >= 0)
		p2p_peer_offered(host, key, candidates, count, 1);
}

static void signal_received(const unsigned char *from, const unsigned char *sealed, int size)
{
	unsigned char message[MAXIMUM_SIGNAL_SIZE];
	int message_size;

	if (size > MAXIMUM_SIGNAL_SIZE + P2P_SEAL_OVERHEAD)
		return;
	if (master.hosting)
	{
		message_size = p2p_open(master.host_key, sealed, size, message);
		if (message_size >= 2 && message[0] == _signal_join && message[1] == SIGNAL_VERSION &&
			!memcmp(message + 2, from, P2P_IDENTIFIER_SIZE))
		{
			join_received(message, message_size);
			return;
		}
	}
	if (master.joining && !memcmp(from, master.join_host, P2P_IDENTIFIER_SIZE))
	{
		message_size = p2p_open(master.join_key, sealed, size, message);
		if (message_size >= 2 && message[0] == _signal_accept && message[1] == SIGNAL_VERSION)
			accept_received(message, message_size);
	}
}

/* ---------- what the server sends */

static void welcome_received(const unsigned char *body, int size)
{
	struct p2p_candidate mapped;
	char text[32];

	if (size < 6)
		return;
	memcpy(&mapped.address, body, 4);
	memcpy(&mapped.port, body + 4, 2);
	/* a machine on the server's own host (the emulator's NAT on the
	machine running the server) is reached through the server's address */
	if (mapped.address == loopback())
		mapped.address = master.address;
	if (!master.has_mapped || mapped.address != master.mapped.address || mapped.port != master.mapped.port)
	{
		master.mapped = mapped;
		master.has_mapped = 1;
		platform_log("Internet play: this machine's address on the internet is %s (from the master server)",
			address_text(mapped.address, mapped.port, text));
	}
}

/* the hosting game's server gets a search as if the searcher were on the LAN */
static void search_relayed(const unsigned char *body, int size)
{
	struct sockaddr_in to;
	unsigned short destination_port;
	int fixed = P2P_IDENTIFIER_SIZE + 4;

	if (!master.hosting || master.injector_socket < 0 || size <= fixed || size - fixed > MAXIMUM_PAYLOAD_SIZE)
		return;
	memcpy(&destination_port, body + P2P_IDENTIFIER_SIZE + 2, 2);
	/* a search is for the game's server, and nothing else here */
	if (!p2p_game_port(destination_port, P2P_GAME_SERVER_PORT))
		return;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = p2p_local_address();
	to.sin_port = destination_port;
	posix_socket_sendto(master.injector_socket, body + fixed, size - fixed, 0, &to, sizeof(to));
}

/* a host's answer: the host is listed, and the game gets the answer from it */
static void game_received(const unsigned char *body, int size)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	const unsigned char *host = body;
	const unsigned char *token = host + P2P_IDENTIFIER_SIZE;
	int offset = P2P_IDENTIFIER_SIZE + P2P_TOKEN_SIZE;
	unsigned short source_port, destination_port;
	int count;
	int used;

	if (size < offset + 1)
		return;
	count = get_candidates(body + offset, size - offset, candidates, &used);
	if (count < 0)
		return;
	offset += used;
	if (size - offset <= 4 || size - offset - 4 > MAXIMUM_PAYLOAD_SIZE)
		return;
	memcpy(&source_port, body + offset, 2);
	memcpy(&destination_port, body + offset + 2, 2);
	offset += 4;
	/* an answer comes from a host's server to a client's port */
	if (!p2p_game_port(source_port, P2P_GAME_SERVER_PORT) || !p2p_game_port(destination_port, P2P_GAME_CLIENT_PORT))
		return;
	if (!memcmp(host, p2p_identifier(), P2P_IDENTIFIER_SIZE))
		return;
	if (!p2p_peer_listed(host, token, candidates, count))
		return;
	p2p_deliver_datagram(host, source_port, destination_port, body + offset, size - offset);
}

int p2p_master_received(const unsigned char *packet, int size, unsigned long address, unsigned short port)
{
	const unsigned char *body = packet + HEADER_SIZE;
	int body_size = size - HEADER_SIZE;

	if (size < HEADER_SIZE || packet[0] != 'H' || packet[1] != 'M')
		return 0;
	/* the server's alone */
	if (packet[2] != MESSAGE_VERSION || !master.address || address != master.address || port != master.port)
		return 1;
	master.heard_time = p2p_now();
	master.reported_silence = 0;
	switch (packet[3])
	{
	case _message_welcome:
		welcome_received(body, body_size);
		break;
	case _message_search_relayed:
		search_relayed(body, body_size);
		break;
	case _message_game:
		game_received(body, body_size);
		break;
	case _message_relayed:
		if (body_size > P2P_IDENTIFIER_SIZE)
			signal_received(body, body + P2P_IDENTIFIER_SIZE, body_size - P2P_IDENTIFIER_SIZE);
		break;
	case _message_ping:
		if (body_size == PING_SIZE)
		{
			/* answered at once: the server times the round trip */
			unsigned char message[HEADER_SIZE + P2P_IDENTIFIER_SIZE + PING_SIZE];
			int message_size = put_header(message, _message_pong);

			memcpy(message + message_size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
			message_size += P2P_IDENTIFIER_SIZE;
			memcpy(message + message_size, body, PING_SIZE);
			message_size += PING_SIZE;
			send_to_server(message, message_size);
		}
		break;
	}
	return 1;
}

/* the hosted lobby's report (p2p_lobby_report, the lock held) */
void p2p_master_lobby(const void *data, int size)
{
	if (size <= 0 || size > MAXIMUM_LOBBY_SIZE)
		return;
	memcpy(master.lobby, data, (size_t)size);
	master.lobby_size = size;
}

static void send_lobby(void)
{
	unsigned char message[HEADER_SIZE + P2P_IDENTIFIER_SIZE + MAXIMUM_LOBBY_SIZE];
	int size = put_header(message, _message_lobby);

	memcpy(message + size, p2p_identifier(), P2P_IDENTIFIER_SIZE);
	size += P2P_IDENTIFIER_SIZE;
	memcpy(message + size, master.lobby, (size_t)master.lobby_size);
	size += master.lobby_size;
	send_to_server(message, size);
	master.lobby_sent_time = p2p_now();
}

/* ---------- the game's broadcasts (the game's thread) */

void p2p_master_broadcast_sent(int socket, unsigned short port, const void *data, int size)
{
	struct sockaddr_in bound;
	int bound_length = sizeof(bound);
	unsigned short source_port = 0;
	struct queued *entry;

	if (size <= 0 || size > MAXIMUM_PAYLOAD_SIZE)
		return;
	if (posix_socket_getsockname(socket, &bound, &bound_length) == 0)
		source_port = bound.sin_port;
	/* the game's searches and answers only */
	if (!p2p_game_port(source_port, 0) || !p2p_game_port(port, 0))
		return;
	p2p_lock_acquire();
	if (master.hosting)
	{
		/* the game answered a search: its advertisement, for whoever
		searched (the newest replaces the oldest when the queue is full) */
		if (master.advertisement_count == MAXIMUM_QUEUED)
		{
			memmove(master.advertisements, master.advertisements + 1,
				sizeof(master.advertisements[0]) * (MAXIMUM_QUEUED - 1));
			master.advertisement_count--;
		}
		entry = &master.advertisements[master.advertisement_count++];
	}
	else
	{
		/* the game searches: the newest search goes to the server */
		entry = &master.search;
		master.search_pending = 1;
		master.search_time = p2p_now();
	}
	memcpy(entry->data, data, (size_t)size);
	entry->size = size;
	entry->source_port = source_port;
	entry->destination_port = port;
	p2p_lock_release();
}

/* ---------- the p2p thread */

void p2p_master_select_sets(int *read, int *read_count, int *write, int *write_count, int maximum_count)
{
	(void)write;
	(void)write_count;
	if (master.injector_socket >= 0 && *read_count < maximum_count)
		read[(*read_count)++] = master.injector_socket;
}

static int searching(void)
{
	return master.search_time && !elapsed(master.search_time, SEARCHING_TIME);
}

void p2p_master_update(const int *read, int read_count, const int *write, int write_count)
{
	int index;

	(void)write;
	(void)write_count;
	/* anything the hosting game sends back to the injector is noise */
	for (index = 0; index < read_count; index++)
	{
		if (read[index] == master.injector_socket)
		{
			unsigned char buffer[64];

			while (posix_socket_recv(master.injector_socket, buffer, sizeof(buffer), 0) > 0)
				;
		}
	}
	if (!master.started)
	{
		if (!master.search_pending && !master.advertisement_count)
			return;
		p2p_master_start();
	}
	look_up();
	if (!master.address)
	{
		master.search_pending = 0;
		master.advertisement_count = 0;
		return;
	}
	if (master.hosting || searching() || master.joining ||
		(master.wanted_time && !elapsed(master.wanted_time, READY_WAIT)))
	{
		unsigned long interval = master.hosting ? HOSTING_HELLO_INTERVAL : HELLO_INTERVAL;

		if (!master.hello_sent_time || elapsed(master.hello_sent_time, interval))
			send_hello();
		if (master.hosting && master.lobby_size &&
			(!master.lobby_sent_time || elapsed(master.lobby_sent_time, LOBBY_INTERVAL)))
			send_lobby();
		/* (hellos go every few seconds, so the silence is counted from the
		last answer, or from the first hello) */
		if (!master.reported_silence && master.hello_sent_time &&
			elapsed(master.heard_time ? master.heard_time : master.wanted_time, SILENCE_TIMEOUT))
		{
			platform_log("Internet play: no answer from the master server %s", master.host_name);
			master.reported_silence = 1;
		}
	}
	if (master.search_pending && (!master.search_sent_time || elapsed(master.search_sent_time, SEARCH_INTERVAL)))
	{
		send_datagram(_message_search, &master.search);
		master.search_sent_time = p2p_now();
		master.search_pending = 0;
	}
	for (index = 0; index < master.advertisement_count; index++)
		send_datagram(_message_advertisement, &master.advertisements[index]);
	master.advertisement_count = 0;
	if (master.joining && (!master.join_sent_time || elapsed(master.join_sent_time, JOIN_INTERVAL)))
		send_join();
}

/* ---------- what p2p.c asks */

int p2p_master_mapped_address(struct p2p_candidate *candidate)
{
	if (!master.has_mapped)
		return 0;
	*candidate = master.mapped;
	return 1;
}

int p2p_master_connected(void)
{
	return master.address && master.heard_time && !elapsed(master.heard_time, SILENCE_TIMEOUT);
}

int p2p_master_ready(void)
{
	if (master.has_mapped || !configured())
		return 1;
	if (master.looked_up_time && !master.address)
		return 1;
	if (!master.wanted_time)
	{
		/* asks on the next pass */
		master.wanted_time = p2p_now();
		master.hello_sent_time = 0;
		return 0;
	}
	return elapsed(master.wanted_time, READY_WAIT);
}

void p2p_master_host(const unsigned char *token)
{
	memcpy(master.host_token, token, P2P_TOKEN_SIZE);
	derive(token, "seal", master.host_key);
	memset(master.joiners, 0, sizeof(master.joiners));
	master.hosting = 1;
	if (master.injector_socket < 0)
		master.injector_socket = p2p_open_local_udp_socket(&master.injector_port);
	/* at once */
	master.hello_sent_time = 0;
}

void p2p_master_stop_hosting(void)
{
	master.hosting = 0;
	memset(master.joiners, 0, sizeof(master.joiners));
	master.advertisement_count = 0;
	master.lobby_size = 0;
	if (master.injector_socket >= 0)
	{
		posix_socket_close(master.injector_socket);
		master.injector_socket = -1;
	}
	/* unlisted at once, not when the listing expires */
	if (master.address)
		send_hello();
}

void p2p_master_join(const unsigned char *host_identifier, const unsigned char *token)
{
	memcpy(master.join_host, host_identifier, P2P_IDENTIFIER_SIZE);
	derive(token, "seal", master.join_key);
	posix_random_bytes(master.join_nonce, NONCE_SIZE);
	master.joining = 1;
	/* a hello first, so the server knows where to relay the answer */
	master.hello_sent_time = 0;
	master.join_sent_time = 0;
}

void p2p_master_stop_joining(void)
{
	master.joining = 0;
}
