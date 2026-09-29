# The master server

`halo_master.py` is the master server of internet play (`port/linux/README.md`,
"Internet play"): one UDP port, no knowledge of the game's packets, and on
disk only its lifetime statistics (`--state-file`). It

- lists the games being hosted: a hosting machine says so every few seconds,
  with the token that lets others join it and the addresses it can be reached
  at;
- passes searches from the machines whose System Link screen is open to
  every host, and the hosts' answers (the game's own advertisement packets)
  back to whoever searched lately, with the host's token and addresses;
- passes signalling between a joiner and a host (sealed by the machines with
  a key from the token; the token of a listed game is public, see the
  Security section of `port/linux/README.md`);
- tells each machine its public address, as the server sees it;
- keeps each host's lobby as the host reports it every two seconds (LOBBY:
  the game's name, map, game type and variant, whether it is on and for how
  long, and each player's name, team, score, kills, deaths and round trip to
  the host through the tunnel; `port/linux/game/network_lobby.c` builds it);
- times the real round trip to each host every three seconds (PING, which
  the host's internet play answers at once with PONG; hosts on older builds
  keep the old estimate, a relayed search answered by the host's game);
- counts, across restarts, the players seen (by a hash of their names, not
  the names), players who joined games, games hosted and matches played,
  hours played, the most players and lobbies at once, and the maps and game
  types played.

A machine's identifier belongs to the endpoint it first said hello from;
each address has a packet budget and an identifier cap, searches a minimum
interval, and message sizes are capped.

The protocol is described in `port/linux/src/p2p_master.c`.

## Run

    python3 halo_master.py [--port 27920] [--bind 0.0.0.0] [--verbose]
                           [--status-file /var/www/html/halo/status.json]
                           [--state-file /var/lib/halo-master/stats.json]

Python 3 alone; open UDP 27920 on the firewall. `--verbose` logs every
machine and relay. `--status-file` writes what is online as JSON every two
seconds: the lobbies with their details (name, map, game type, time in game,
players with their teams, scores and pings, the host's round trip), the
machines browsing the list, and the lifetime statistics. `--state-file`
keeps the lifetime statistics (written every 30 seconds and when the server
stops).

## The status page

`index.html` is a static page for the web server that serves the status
file's folder, in the original Xbox dashboard's green: it reads
`status.json` next to it every three seconds and shows how many lobbies,
players and browsing machines there are; each lobby as a card with its map,
game type, whether it is on (and its clock), the host's round trip, and a
table of its players (team, score, kills, deaths, and signal bars for each
one's round trip to the host); and the lifetime statistics with the most
played maps and game types. It says when the master server has stopped
reporting (judged by its status changing, not by the viewer's clock). Every
name from the file goes into the page as text. With Apache's default root it
is `http://<server>/halo/`.

## Deploy

On a Linux host with systemd and Apache (Ubuntu or Debian), as root:

    port/master/deploy.sh root@172.104.30.93 [-i ~/.ssh/key]

copies `halo_master.py` to `/opt/halo-master` and `index.html` to
`/var/www/html/halo` (owned by www-data, which the service runs as and
writes `status.json` into), installs `halo-master.service` (sandboxed,
restarted however often it stops, a 256 MB memory ceiling, its statistics
in `/var/lib/halo-master`), allows UDP 27920 in ufw when ufw is active, and
(re)starts it. `journalctl -u halo-master -f` follows the log.

## Testing on one machine

Two copies of the game on one machine, or two emulated consoles, can use a
server on that machine. Machines the server sees at 127.0.0.1 (an emulator's
NAT on the server's host) are offered to others at the server's own address,
so that two emulated consoles reach each other through their NATs' mappings.
