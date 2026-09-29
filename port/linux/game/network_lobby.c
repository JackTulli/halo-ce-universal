/*
NETWORK_LOBBY.C

The hosted game's lobby, told to the master server for its status page
(port/master: the page at /halo/ shows every listed game's name, map, game
type and players): every two seconds while this machine hosts a game
others may join, its name, map and game variant, whether the game is on
and for how long, and each player's name, team, score, kills and deaths,
and the round trip to the player's machine through internet play's tunnel
(p2p.c measures it every second; players on the host's own machine have
none, and players on the LAN are reached without the tunnel).

A report (little-endian, strings UTF-8 with a length byte):
	u8 version (1), u8 flags (1: in game, 2: teams), u8 player count,
	u8 maximum players, u8 game engine (game_engine.h), u8 reserved,
	u16 seconds in game, str game name, str map, str variant;
	per player: str name, u8 team, u8 flags (1: on the host's machine,
	2: ping known), u16 ping in milliseconds, s16 score, s16 kills,
	s16 deaths.
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "networking/network_connection.h"
#include "bungie_net/network/transport.h"

#include <string.h>

/* ---------- constants */

enum
{
	LOBBY_REPORT_VERSION = 1,
	LOBBY_REPORT_INTERVAL = 2000,
	LOBBY_STRING_BYTES = 32,
	LOBBY_REPORT_BYTES = 1200,
	LOBBY_FLAG_IN_GAME = 1,
	LOBBY_FLAG_TEAMS = 2,
	LOBBY_PLAYER_ON_HOST = 1,
	LOBBY_PLAYER_PING_KNOWN = 2,
};

/* ---------- the game's settings record, as the host keeps it (the same
layout the networking units declare, checked against the session limits) */

struct lobby_network_machine
{
	wchar_t name[32];
	char machine_index;
	byte padding41[3];
};

struct lobby_network_game
{
	wchar_t name[16];
	struct
	{
		long version;
		char name[0x80];
	} map;
	struct game_variant variant;
	byte opaque10C;
	char minimum_players;
	byte maximum_players;
	byte maximum_teams;
	short difficulty;
	short machine_count;
	struct lobby_network_machine machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short player_count;
	struct network_player players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
};

typedef char lobby_network_game_machines_offset_assert[
	offsetof(struct lobby_network_game, machines) == HALO_PORT_NETWORK_GAME_MACHINES_OFFSET ? 1 : -1];
typedef char lobby_network_game_players_offset_assert[
	offsetof(struct lobby_network_game, players) == HALO_PORT_NETWORK_GAME_PLAYERS_OFFSET ? 1 : -1];

/* the host's game and a remote machine's connection
(network_server_manager.c) */
struct network_game *network_game_server_get_game(struct network_game_server *server);
struct network_connection *network_game_server_machine_connection(struct network_game_server *server,
	long machine_index);

/* ---------- the report */

struct lobby_writer
{
	byte *data;
	long size;
	boolean full;
};

static void put_byte(struct lobby_writer *writer, long value)
{
	if (writer->size + 1 > LOBBY_REPORT_BYTES)
	{
		writer->full = TRUE;
		return;
	}
	writer->data[writer->size++] = (byte)value;
}

static void put_short(struct lobby_writer *writer, long value)
{
	put_byte(writer, value & 0xFF);
	put_byte(writer, (value >> 8) & 0xFF);
}

/* UTF-16 to UTF-8, printable characters only, at most LOBBY_STRING_BYTES */
static void put_wide_string(struct lobby_writer *writer, wchar_t const *text, long maximum_count)
{
	byte encoded[LOBBY_STRING_BYTES];
	long length = 0;
	long index;

	for (index = 0; index < maximum_count && text[index]; index++)
	{
		unsigned long character = (unsigned short)text[index];
		byte bytes[3];
		long count;

		if (character < 0x20 || (character >= 0x7F && character < 0xA0) || (character >= 0xD800 && character < 0xE000))
			continue;
		if (character < 0x80)
		{
			bytes[0] = (byte)character;
			count = 1;
		}
		else if (character < 0x800)
		{
			bytes[0] = (byte)(0xC0 | (character >> 6));
			bytes[1] = (byte)(0x80 | (character & 0x3F));
			count = 2;
		}
		else
		{
			bytes[0] = (byte)(0xE0 | (character >> 12));
			bytes[1] = (byte)(0x80 | ((character >> 6) & 0x3F));
			bytes[2] = (byte)(0x80 | (character & 0x3F));
			count = 3;
		}
		if (length + count > LOBBY_STRING_BYTES)
			break;
		memcpy(encoded + length, bytes, (size_t)count);
		length += count;
	}
	put_byte(writer, length);
	for (index = 0; index < length; index++)
		put_byte(writer, encoded[index]);
}

/* the map's name: the last part of its path (levels\test\bloodgulch\bloodgulch) */
static void put_map_name(struct lobby_writer *writer, char const *path)
{
	char const *name = path;
	char const *cursor;
	long length = 0;
	long index;

	for (cursor = path; *cursor && cursor - path < 0x80; cursor++)
	{
		if (*cursor == '\\' || *cursor == '/')
			name = cursor + 1;
	}
	while (name[length] && length < LOBBY_STRING_BYTES && name + length - path < 0x80)
		length++;
	put_byte(writer, length);
	for (index = 0; index < length; index++)
		put_byte(writer, (name[index] >= 0x20 && name[index] < 0x7F) ? name[index] : '?');
}

/* the round trip to a remote machine through the tunnel, or NONE */
static long machine_round_trip(struct network_game_server *server, long machine_index)
{
	struct network_connection *connection = network_game_server_machine_connection(server, machine_index);
	struct transport_address address;
	unsigned long host_order;

	if (!connection)
		return NONE;
	memset(&address, 0, sizeof(address));
	network_connection_get_address(connection, &address, NULL);
	host_order = address.address.long_words[0];
	if (!host_order)
		return NONE;
	/* the game keeps the address in host order; the tunnel's peers are
	known by it in network order */
	return p2p_peer_round_trip(((host_order & 0xFF) << 24) | ((host_order & 0xFF00) << 8) |
		((host_order >> 8) & 0xFF00) | (host_order >> 24));
}

static void put_player(struct lobby_writer *writer, struct network_game_server *server, short host_machine,
	struct network_player const *network_player, long team, long score, long kills, long deaths)
{
	long flags = 0;
	long ping = 0;

	put_wide_string(writer, network_player->name, NUMBEROF(network_player->name));
	put_byte(writer, team < 0 || team > 255 ? 255 : team);
	if (network_player->machine_index == host_machine)
	{
		flags |= LOBBY_PLAYER_ON_HOST;
	}
	else
	{
		long round_trip = machine_round_trip(server, network_player->machine_index);

		if (round_trip != NONE)
		{
			flags |= LOBBY_PLAYER_PING_KNOWN;
			ping = round_trip > 0xFFFE ? 0xFFFE : round_trip;
		}
	}
	put_byte(writer, flags);
	put_short(writer, ping);
	put_short(writer, PIN(score, -32768, 32767));
	put_short(writer, PIN(kills, -32768, 32767));
	put_short(writer, PIN(deaths, -32768, 32767));
}

static long build_report(byte *data)
{
	struct network_game_server *server = global_network_game_server_get();
	struct lobby_network_game const *game;
	struct lobby_writer writer = { data, 0, FALSE };
	short host_machine = network_game_client_get_local_machine_index();
	boolean in_game = game_in_progress() && game_engine != NULL;
	long engine = 0;
	long player_count_offset;
	long count = 0;

	if (!server)
		return 0;
	game = (struct lobby_network_game const *)network_game_server_get_game(server);
	if (!game)
		return 0;
	engine = game->variant.game_engine_index;
	put_byte(&writer, LOBBY_REPORT_VERSION);
	put_byte(&writer, (in_game ? LOBBY_FLAG_IN_GAME : 0) |
		(game->variant.universal_variant.teams ? LOBBY_FLAG_TEAMS : 0));
	player_count_offset = writer.size;
	put_byte(&writer, 0);
	put_byte(&writer, game->maximum_players);
	put_byte(&writer, engine < 0 || engine > 255 ? 0 : engine);
	put_byte(&writer, 0);
	put_short(&writer, in_game ? PIN(game_time_get() / TICKS_PER_SECOND, 0, 0xFFFF) : 0);
	put_wide_string(&writer, game->name, NUMBEROF(game->name));
	put_map_name(&writer, game->map.name);
	put_wide_string(&writer, game->variant.human_readable_game_description,
		NUMBEROF(game->variant.human_readable_game_description));
	if (in_game)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL && count < 255)
		{
			long score = game_engine->get_player_score ?
				game_engine->get_player_score(iterator.datum_index, FALSE) : 0;

			put_player(&writer, server, host_machine, &player->network_player_data, player->team_index, score,
				player->statistics.kills[0], player->statistics.deaths);
			count++;
		}
	}
	else
	{
		long index;

		for (index = 0; index < game->player_count && index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS && count < 255; index++)
		{
			put_player(&writer, server, host_machine, &game->players[index], game->players[index].team_index, 0, 0, 0);
			count++;
		}
	}
	if (writer.full)
		return 0;
	data[player_count_offset] = (byte)count;
	return writer.size;
}

/* ---------- public code */

/* once a frame (main.c): a report every two seconds while hosting */
void network_lobby_update(void)
{
	static unsigned long reported_time;
	byte report[LOBBY_REPORT_BYTES];
	unsigned long now = system_milliseconds();
	long size;

	if (!global_network_game_server_get() || !network_game_should_accept_remote_connections())
		return;
	if (reported_time && now - reported_time < LOBBY_REPORT_INTERVAL)
		return;
	reported_time = now;
	size = build_report(report);
	if (size > 0)
		p2p_lobby_report(report, size);
}
