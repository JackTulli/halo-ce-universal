/*
HALO_XBOX_LIMITS.H

The Xbox build's session limits and memory capacity: the limits every build
shares (port/include/halo_session_limits.h: 16 players on up to 16
machines, the retail game's pools, which fit the console's 64 MB), and the
console's own game state, at 0x80061000 below the tag cache
(physical_memory_map.c). The game sources use these names under #ifdef
HALO_LINUX, which the Xbox build defines too (it is the clang build).
*/

#ifndef __HALO_XBOX_LIMITS_H
#define __HALO_XBOX_LIMITS_H

/* the memory window: the console's own, at 0x80000000 (the names the shared
game sources use for it, tag cache and saved games) */
#include "../../include/halo_memory_window.h"
typedef char halo_xbox_window_is_the_consoles[HALO_WINDOW_BASE == HALO_XBOX_WINDOW_BASE ? 1 : -1];

/* a host polls its listening socket and one socket per machine */
#define HALO_PORT_FD_SETSIZE 64

/* ---------- game state (the Xbox's, physical_memory_map.c) */

#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x80061000
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x305000
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x40000
#define HALO_PORT_GAME_STATE_SIZE (HALO_PORT_GAME_STATE_CPU_SIZE+HALO_PORT_GAME_STATE_GPU_SIZE)

/* ---------- session limits, pools, the layout of struct network_game and
the protocol, shared with every build */

#include "../../include/halo_session_limits.h"

#endif /* __HALO_XBOX_LIMITS_H */
