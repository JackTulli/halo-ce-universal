#!/usr/bin/env python3
"""The master server of Halo's internet play: lists the games being hosted,
passes the machines' signalling, and tells each its public address. One UDP
port; the protocol is in port/linux/src/p2p_master.c.

    python3 halo_master.py [--port 27920] [--bind 0.0.0.0] [--verbose]
                           [--status-file /var/www/html/halo/status.json]
                           [--state-file /var/lib/halo-master/stats.json]

It knows nothing of the game's packets: a machine that says it hosts is
listed for as long as it keeps saying so; searches go to every host, hosts'
answers go to whoever searched lately, relays go to the machine they name.

A machine's identifier belongs to the address and port it said HELLO from:
nothing else is taken from another endpoint in its name, and another
endpoint takes it over only once it has been silent for MACHINE_TIMEOUT (a
NAT's new mapping). Each address may hold a few identifiers and send so
many packets a second, each machine may search so often, and a host whose
game never answers the latency probes stops being probed.
With --status-file it writes what is online as JSON every few seconds, for
the status page (index.html), with each host's latency: the time from a
search relayed to the host until its game's answer comes back (which
includes the game's own processing, so it is a little more than the round
trip). While nobody searches, the server repeats the last search it saw as
a probe, every few seconds per host.

Hosts also report their lobby every two seconds (LOBBY: the game's name,
map, game type, whether it is on, and each player's name, team, score,
kills, deaths and round trip to the host; port/linux/game/network_lobby.c),
and answer the server's PING at once (PONG), which times the real round
trip to each host. With --state-file the server keeps lifetime statistics
there (players seen, games hosted and played, peaks, the maps and game
types played), which the status page shows.
"""

import argparse
import collections
import hashlib
import json
import logging
import os
import signal
import socket
import struct
import sys
import time

MAGIC = b"HM"
VERSION = 1
HEADER_SIZE = 4
IDENTIFIER_SIZE = 6
TOKEN_SIZE = 16
MAXIMUM_CANDIDATES = 4
MAXIMUM_MESSAGE_SIZE = 2048

# to the server
HELLO, SEARCH, ADVERTISEMENT, RELAY, LOBBY, PONG = 1, 2, 3, 4, 5, 6
# from it
WELCOME, SEARCH_RELAYED, GAME, RELAYED, PING = 16, 17, 18, 19, 20

# seconds
MACHINE_TIMEOUT = 10.0
SEARCHING_TIME = 6.0
SWEEP_INTERVAL = 1.0
STATUS_INTERVAL = 2.0
LATENCY_PROBE_INTERVAL = 5.0
PROBE_TIMEOUT = 3.0
LATENCY_SAMPLES = 8
# the clients search once a second
SEARCH_MINIMUM_INTERVAL = 0.5
# probes a host may leave unanswered before it is probed only this often
PROBE_MISSES = 3
PROBE_BACKOFF = 60.0

# per address: identifiers, and packets (a bucket of so many, refilled at a
# rate a second)
IDENTIFIERS_PER_ADDRESS = 16
PACKETS_PER_SECOND = 100.0
PACKET_BURST = 300.0
# the largest bodies: an identifier, two ports and a game datagram; a relay's
# two identifiers and a sealed signal
MAXIMUM_SEARCH_BODY = IDENTIFIER_SIZE + 4 + 1264
MAXIMUM_RELAY_BODY = 2 * IDENTIFIER_SIZE + 300
MAXIMUM_LOBBY_BODY = IDENTIFIER_SIZE + 1200
PING_SIZE = 8
# a host's round trip is timed this often; a lobby report older than this is
# stale
PING_INTERVAL = 3.0
LOBBY_TIMEOUT = 10.0
STATE_INTERVAL = 30.0

GAME_ENGINES = {1: "Capture the Flag", 2: "Slayer", 3: "Oddball", 4: "King of the Hill", 5: "Race"}
# the maps' names as the game shows them
MAP_TITLES = {
    "beavercreek": "Battle Creek", "bloodgulch": "Blood Gulch", "boardingaction": "Boarding Action",
    "carousel": "Derelict", "chillout": "Chill Out", "damnation": "Damnation", "dangercanyon": "Danger Canyon",
    "deathisland": "Death Island", "gephyrophobia": "Gephyrophobia", "hangemhigh": "Hang 'Em High",
    "icefields": "Ice Fields", "infinity": "Infinity", "longest": "Longest", "prisoner": "Prisoner",
    "putput": "Chiron TL-34", "ratrace": "Rat Race", "sidewinder": "Sidewinder", "timberland": "Timberland",
    "wizard": "Wizard",
}


def map_title(name):
    if not name:
        return ""
    return MAP_TITLES.get(name.lower(), name.replace("_", " ").title())


def clean_text(raw):
    """A player's or game's name as UTF-8 from the wire: printable, short."""
    text = raw.decode("utf-8", errors="replace")
    return "".join(ch for ch in text if ch.isprintable())[:32]


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, count):
        if self.offset + count > len(self.data):
            raise ValueError("short")
        chunk = self.data[self.offset:self.offset + count]
        self.offset += count
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def s16(self):
        return struct.unpack("<h", self.take(2))[0]

    def text(self):
        return clean_text(self.take(self.u8()))


def parse_lobby(data):
    """A host's lobby report (port/linux/game/network_lobby.c), or None."""
    try:
        reader = Reader(data)
        if reader.u8() != 1:
            return None
        flags = reader.u8()
        count = reader.u8()
        maximum = reader.u8()
        engine = reader.u8()
        reader.u8()
        seconds = reader.u16()
        name, map_name, variant = reader.text(), reader.text(), reader.text()
        players = []
        for _ in range(count):
            player_name = reader.text()
            team = reader.u8()
            player_flags = reader.u8()
            ping = reader.u16()
            score, kills, deaths = reader.s16(), reader.s16(), reader.s16()
            players.append({
                "name": player_name,
                "team": team if team != 255 else None,
                "host": bool(player_flags & 1),
                "ping": ping if player_flags & 2 else None,
                "score": score, "kills": kills, "deaths": deaths,
            })
    except (ValueError, struct.error):
        return None
    return {
        "in_game": bool(flags & 1), "teams": bool(flags & 2), "max_players": maximum,
        "engine": engine, "mode": GAME_ENGINES.get(engine, ""), "game_seconds": seconds,
        "name": name, "map": map_name, "map_title": map_title(map_name), "variant": variant,
        "players": players,
    }


class Lifetime:
    """What the server has seen since it first ran, kept in a file."""

    def __init__(self, path):
        self.path = path
        self.first_started = time.time()
        self.player_hashes = set()
        self.machines_seen = 0
        self.games_hosted = 0
        self.matches_started = 0
        self.player_joins = 0
        self.player_seconds = 0.0
        self.peak_players = 0
        self.peak_players_time = 0.0
        self.peak_lobbies = 0
        self.peak_lobbies_time = 0.0
        self.maps = collections.Counter()
        self.modes = collections.Counter()
        self.dirty = False
        self.saved = 0.0
        self.load()

    def load(self):
        if not self.path:
            return
        try:
            with open(self.path) as file:
                state = json.load(file)
        except (OSError, ValueError):
            return
        self.first_started = float(state.get("first_started", self.first_started))
        self.player_hashes = set(state.get("player_hashes", []))
        for key in ("machines_seen", "games_hosted", "matches_started", "player_joins", "peak_players", "peak_lobbies"):
            setattr(self, key, int(state.get(key, 0)))
        for key in ("player_seconds", "peak_players_time", "peak_lobbies_time"):
            setattr(self, key, float(state.get(key, 0.0)))
        self.maps = collections.Counter(state.get("maps", {}))
        self.modes = collections.Counter(state.get("modes", {}))

    def save(self, force=False):
        if not self.path or not self.dirty or (not force and time.monotonic() - self.saved < STATE_INTERVAL):
            return
        state = {
            "first_started": self.first_started, "player_hashes": sorted(self.player_hashes),
            "machines_seen": self.machines_seen, "games_hosted": self.games_hosted,
            "matches_started": self.matches_started, "player_joins": self.player_joins,
            "player_seconds": self.player_seconds, "peak_players": self.peak_players,
            "peak_players_time": self.peak_players_time, "peak_lobbies": self.peak_lobbies,
            "peak_lobbies_time": self.peak_lobbies_time, "maps": dict(self.maps), "modes": dict(self.modes),
        }
        temporary = self.path + ".new"
        try:
            with open(temporary, "w") as file:
                json.dump(state, file)
            os.replace(temporary, self.path)
            self.dirty = False
            self.saved = time.monotonic()
        except OSError as error:
            logging.warning("cannot write %s: %s", self.path, error)
            self.saved = time.monotonic()

    @staticmethod
    def player_hash(name):
        # players are counted, not listed: a name's hash only
        return hashlib.sha256(name.strip().lower().encode("utf-8")).hexdigest()[:16]

    def summary(self):
        def top(counter):
            return [[name, count] for name, count in counter.most_common(5)]
        return {
            "since": self.first_started, "unique_players": len(self.player_hashes),
            "machines_seen": self.machines_seen, "games_hosted": self.games_hosted,
            "matches_started": self.matches_started, "player_joins": self.player_joins,
            "hours_played": round(self.player_seconds / 3600.0, 1),
            "peak_players": self.peak_players, "peak_players_time": self.peak_players_time,
            "peak_lobbies": self.peak_lobbies, "peak_lobbies_time": self.peak_lobbies_time,
            "top_maps": [[map_title(name), count] for name, count in self.maps.most_common(5)],
            "top_modes": top(self.modes),
        }


class Machine:
    def __init__(self, identifier, address):
        self.identifier = identifier
        self.address = address
        self.seen = time.monotonic()
        self.hosting = False
        self.hosting_since = 0.0
        self.players = 0
        self.token = bytes(TOKEN_SIZE)
        self.candidates = []
        self.searched = 0.0
        # host latency: when the last relayed search went out unanswered, and
        # the recent answer times (milliseconds)
        self.probe_sent = None
        self.latency = collections.deque(maxlen=LATENCY_SAMPLES)
        self.latency_time = 0.0
        self.probe_misses = 0
        self.probe_backoff_until = 0.0
        # the lobby the host reports, and the real round trip (PING/PONG)
        self.lobby = None
        self.lobby_time = 0.0
        self.session_players = set()
        self.was_in_game = False
        self.ping_token = None
        self.ping_sent = 0.0
        self.round_trips = collections.deque(maxlen=LATENCY_SAMPLES)

    @property
    def name(self):
        return self.identifier.hex()

    def candidates_bytes(self):
        """The host's own candidates, with the address the server sees it at."""
        candidates = list(self.candidates)
        observed = (socket.inet_aton(self.address[0]), self.address[1])
        if observed not in candidates:
            candidates.insert(0, observed)
        candidates = candidates[:MAXIMUM_CANDIDATES]
        return bytes([len(candidates)]) + b"".join(ip + struct.pack("!H", port) for ip, port in candidates)


class Master:
    def __init__(self, sock, status_file=None, state_file=None):
        self.sock = sock
        self.lifetime = Lifetime(state_file)
        self.machines = {}
        self.swept = time.monotonic()
        self.status_file = status_file
        self.status_written = 0.0
        self.started = time.time()
        # the newest search seen, repeated as a latency probe
        self.last_search = None
        # per address: [tokens, when refilled]
        self.buckets = {}

    def header(self, message_type):
        return MAGIC + bytes([VERSION, message_type])

    def send(self, machine, message):
        try:
            self.sock.sendto(message, machine.address)
        except OSError as error:
            logging.debug("cannot send to %s at %s: %s", machine.name, machine.address, error)

    def admit(self, address):
        """The address's packet budget: False once it is spent."""
        now = time.monotonic()
        bucket = self.buckets.get(address[0])
        if bucket is None:
            bucket = self.buckets[address[0]] = [PACKET_BURST, now]
        bucket[0] = min(PACKET_BURST, bucket[0] + (now - bucket[1]) * PACKETS_PER_SECOND)
        bucket[1] = now
        if bucket[0] < 1.0:
            return False
        bucket[0] -= 1.0
        return True

    def machine(self, identifier, address, hello=False):
        """The machine of an identifier, from the endpoint that owns it; a
        HELLO makes one, or moves one that has gone silent. None otherwise."""
        now = time.monotonic()
        machine = self.machines.get(identifier)
        if machine is None:
            if not hello:
                return None
            if sum(1 for m in self.machines.values() if m.address[0] == address[0]) >= IDENTIFIERS_PER_ADDRESS:
                logging.debug("%s:%d holds too many identifiers", *address)
                return None
            machine = Machine(identifier, address)
            self.machines[identifier] = machine
            self.lifetime.machines_seen += 1
            self.lifetime.dirty = True
            logging.debug("machine %s at %s:%d", machine.name, *address)
        elif machine.address != address:
            if not hello or now - machine.seen <= MACHINE_TIMEOUT:
                logging.debug("machine %s is at %s:%d, not %s:%d", machine.name, *machine.address, *address)
                return None
            logging.debug("machine %s moved from %s:%d to %s:%d", machine.name, *machine.address, *address)
            machine.address = address
        machine.seen = now
        return machine

    def hosts(self):
        return [m for m in self.machines.values() if m.hosting]

    def searchers(self):
        now = time.monotonic()
        return [m for m in self.machines.values() if now - m.searched < SEARCHING_TIME]

    def handle(self, data, address):
        if len(data) < HEADER_SIZE or data[:2] != MAGIC or data[2] != VERSION:
            return
        if not self.admit(address):
            return
        message_type = data[3]
        body = data[HEADER_SIZE:]
        if len(body) < IDENTIFIER_SIZE:
            return
        identifier = body[:IDENTIFIER_SIZE]
        if message_type == HELLO:
            self.hello(identifier, body, address)
        elif message_type == SEARCH:
            if len(body) > MAXIMUM_SEARCH_BODY:
                return
            machine = self.machine(identifier, address)
            if machine is None:
                return
            now = time.monotonic()
            if now - machine.searched < SEARCH_MINIMUM_INTERVAL:
                return
            machine.searched = now
            self.last_search = body
            for host in self.hosts():
                if host is not machine:
                    self.probe(host, body)
        elif message_type == ADVERTISEMENT:
            if len(body) > MAXIMUM_SEARCH_BODY:
                return
            machine = self.machine(identifier, address)
            if machine is None or not machine.hosting:
                return
            self.answered(machine)
            game = (self.header(GAME) + identifier + machine.token + machine.candidates_bytes() +
                    body[IDENTIFIER_SIZE:])
            for searcher in self.searchers():
                if searcher is not machine:
                    self.send(searcher, game)
        elif message_type == RELAY:
            if len(body) < 2 * IDENTIFIER_SIZE or len(body) > MAXIMUM_RELAY_BODY:
                return
            if self.machine(identifier, address) is None:
                return
            to = self.machines.get(body[IDENTIFIER_SIZE:2 * IDENTIFIER_SIZE])
            if to is None:
                logging.debug("relay from %s to an unknown machine", identifier.hex())
                return
            self.send(to, self.header(RELAYED) + identifier + body[2 * IDENTIFIER_SIZE:])
        elif message_type == LOBBY:
            if len(body) > MAXIMUM_LOBBY_BODY:
                return
            machine = self.machine(identifier, address)
            if machine is None or not machine.hosting:
                return
            lobby = parse_lobby(body[IDENTIFIER_SIZE:])
            if lobby is not None:
                self.lobby_reported(machine, lobby)
        elif message_type == PONG:
            if len(body) != IDENTIFIER_SIZE + PING_SIZE:
                return
            machine = self.machine(identifier, address)
            if machine is None or machine.ping_token is None or body[IDENTIFIER_SIZE:] != machine.ping_token:
                return
            machine.round_trips.append((time.monotonic() - machine.ping_sent) * 1000.0)
            machine.ping_token = None

    def hello(self, identifier, body, address):
        fixed = IDENTIFIER_SIZE + 2 + TOKEN_SIZE + 1
        if len(body) < fixed:
            return
        flags = body[IDENTIFIER_SIZE]
        players = body[IDENTIFIER_SIZE + 1]
        token = body[IDENTIFIER_SIZE + 2:IDENTIFIER_SIZE + 2 + TOKEN_SIZE]
        count = body[fixed - 1]
        candidates = []
        if count <= MAXIMUM_CANDIDATES and len(body) >= fixed + 6 * count:
            for index in range(count):
                offset = fixed + 6 * index
                candidates.append((body[offset:offset + 4], struct.unpack("!H", body[offset + 4:offset + 6])[0]))
        machine = self.machine(identifier, address, hello=True)
        if machine is None:
            return
        hosting = bool(flags & 1)
        if hosting != machine.hosting:
            logging.info("%s at %s:%d %s", machine.name, *address, "hosts a game" if hosting else "stopped hosting")
            machine.hosting_since = time.time() if hosting else 0.0
            machine.lobby = None
            machine.session_players = set()
            machine.was_in_game = False
            if hosting:
                self.lifetime.games_hosted += 1
                self.lifetime.dirty = True
        machine.hosting = hosting
        machine.players = players if hosting else 0
        machine.token = token if hosting else bytes(TOKEN_SIZE)
        machine.candidates = candidates
        self.send(machine, self.header(WELCOME) + socket.inet_aton(address[0]) + struct.pack("!H", address[1]))

    def lobby_reported(self, machine, lobby):
        """A host's lobby: the players who joined and the games started count
        in the lifetime statistics."""
        machine.lobby = lobby
        machine.lobby_time = time.monotonic()
        for player in lobby["players"]:
            if not player["name"]:
                continue
            key = Lifetime.player_hash(player["name"])
            if key not in machine.session_players:
                machine.session_players.add(key)
                self.lifetime.player_joins += 1
                self.lifetime.player_hashes.add(key)
                self.lifetime.dirty = True
        if lobby["in_game"] and not machine.was_in_game:
            self.lifetime.matches_started += 1
            if lobby["map"]:
                self.lifetime.maps[lobby["map"].lower()] += 1
            if lobby["mode"]:
                self.lifetime.modes[lobby["mode"]] += 1
            self.lifetime.dirty = True
        machine.was_in_game = lobby["in_game"]

    def ping_hosts(self, now):
        """The real round trip: a PING the host's internet play answers at
        once (older builds do not, and keep the probe's latency)."""
        for host in self.hosts():
            if now - host.ping_sent >= PING_INTERVAL:
                host.ping_token = os.urandom(PING_SIZE)
                host.ping_sent = now
                self.send(host, self.header(PING) + host.ping_token)

    def probe(self, host, search):
        """Relays a search to the host, timing it unless one is in flight."""
        now = time.monotonic()
        self.send(host, self.header(SEARCH_RELAYED) + search)
        if host.probe_sent is not None and now - host.probe_sent > PROBE_TIMEOUT:
            # the last one went unanswered
            host.probe_misses += 1
            if host.probe_misses >= PROBE_MISSES:
                host.probe_backoff_until = now + PROBE_BACKOFF
        if host.probe_sent is None or now - host.probe_sent > PROBE_TIMEOUT:
            host.probe_sent = now

    def answered(self, host):
        """The host's game answered a search: a latency sample."""
        now = time.monotonic()
        if host.probe_sent is not None and now - host.probe_sent <= PROBE_TIMEOUT:
            host.latency.append((now - host.probe_sent) * 1000.0)
            host.latency_time = now
        host.probe_sent = None
        host.probe_misses = 0
        host.probe_backoff_until = 0.0

    def sweep(self):
        now = time.monotonic()
        if now - self.swept < SWEEP_INTERVAL:
            return
        self.swept = now
        if self.last_search:
            for host in self.hosts():
                if (host.probe_sent is None or now - host.probe_sent > PROBE_TIMEOUT) and \
                        now - host.latency_time >= LATENCY_PROBE_INTERVAL and now >= host.probe_backoff_until:
                    self.probe(host, self.last_search)
        for identifier, machine in list(self.machines.items()):
            if now - machine.seen > MACHINE_TIMEOUT:
                if machine.hosting:
                    logging.info("%s went away (was hosting)", machine.name)
                del self.machines[identifier]
        for address, bucket in list(self.buckets.items()):
            if now - bucket[1] > MACHINE_TIMEOUT:
                del self.buckets[address]
        self.ping_hosts(now)
        self.count_players(now)
        self.write_status()
        self.lifetime.save()

    def lobby_of(self, machine):
        if machine.lobby is not None and time.monotonic() - machine.lobby_time <= LOBBY_TIMEOUT:
            return machine.lobby
        return None

    def players_in(self, machine):
        lobby = self.lobby_of(machine)
        return len(lobby["players"]) if lobby else 1 + machine.players

    def count_players(self, now):
        """Peaks, and the time played (players in games that are on)."""
        hosts = self.hosts()
        players = sum(self.players_in(host) for host in hosts)
        stamp = time.time()
        if players > self.lifetime.peak_players:
            self.lifetime.peak_players, self.lifetime.peak_players_time = players, stamp
            self.lifetime.dirty = True
        if len(hosts) > self.lifetime.peak_lobbies:
            self.lifetime.peak_lobbies, self.lifetime.peak_lobbies_time = len(hosts), stamp
            self.lifetime.dirty = True
        elapsed = now - getattr(self, "counted", now)
        self.counted = now
        if 0 < elapsed < 10:
            playing = sum(len(lobby["players"]) for lobby in (self.lobby_of(h) for h in hosts)
                          if lobby and lobby["in_game"])
            if playing:
                self.lifetime.player_seconds += playing * elapsed
                self.lifetime.dirty = True

    def status(self):
        """What is online: the lobbies (hosting machines) with their players,
        and the machines browsing the list."""
        now = time.monotonic()
        hosts = self.hosts()
        lobbies = []
        for machine in sorted(hosts, key=lambda m: m.hosting_since):
            lobby = self.lobby_of(machine)
            entry = {
                "host": machine.name[:6],
                "players": self.players_in(machine),
                "since": machine.hosting_since,
                # the real round trip (PING/PONG), the best of the recent ones;
                # else the probe's: a search answered by the host's game
                "ping": round(min(machine.round_trips)) if machine.round_trips else None,
                "latency": round(min(machine.latency)) if machine.latency else None,
                "detailed": lobby is not None,
            }
            if lobby:
                entry.update({
                    "name": lobby["name"], "map": lobby["map"], "map_title": lobby["map_title"],
                    "mode": lobby["mode"], "variant": lobby["variant"], "teams": lobby["teams"],
                    "in_game": lobby["in_game"], "game_seconds": lobby["game_seconds"],
                    "max_players": lobby["max_players"], "player_list": lobby["players"],
                })
            lobbies.append(entry)
        browsing = sum(1 for m in self.machines.values() if not m.hosting and now - m.searched < SEARCHING_TIME)
        return {
            "updated": time.time(),
            "started": self.started,
            "lobbies": len(lobbies),
            "players": sum(lobby["players"] for lobby in lobbies),
            "browsing": browsing,
            "games": lobbies,
            "lifetime": self.lifetime.summary(),
        }

    def write_status(self):
        if not self.status_file or time.monotonic() - self.status_written < STATUS_INTERVAL:
            return
        self.status_written = time.monotonic()
        temporary = self.status_file + ".new"
        try:
            with open(temporary, "w") as file:
                json.dump(self.status(), file)
            os.replace(temporary, self.status_file)
        except OSError as error:
            logging.warning("cannot write %s: %s", self.status_file, error)
            self.status_written = time.monotonic() + 30


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=27920, help="the UDP port (default 27920)")
    parser.add_argument("--bind", default="0.0.0.0", help="the address to listen on (default all)")
    parser.add_argument("--verbose", action="store_true", help="log every machine and relay")
    parser.add_argument("--status-file", metavar="PATH",
                        help="write what is online here as JSON every few seconds (for index.html)")
    parser.add_argument("--state-file", metavar="PATH",
                        help="keep the lifetime statistics here (JSON), across restarts")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    sock.settimeout(SWEEP_INTERVAL)
    logging.info("listening on %s:%d (UDP)", args.bind, args.port)
    master = Master(sock, args.status_file, args.state_file)

    def stop(*_):
        # the lifetime statistics are kept when the service stops
        master.lifetime.save(force=True)
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while True:
        try:
            data, address = sock.recvfrom(MAXIMUM_MESSAGE_SIZE)
        except socket.timeout:
            pass
        except OSError as error:
            logging.debug("receive failed: %s", error)
        else:
            try:
                master.handle(data, address)
            except Exception:
                logging.exception("a message from %s:%d failed", *address)
        master.sweep()


if __name__ == "__main__":
    main()
