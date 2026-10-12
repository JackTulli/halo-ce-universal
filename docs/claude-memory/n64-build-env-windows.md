---
name: n64-build-env-windows
description: "Where the Halo N64 port's build environment lives on the user's Windows 11 PC (WSL Ubuntu, set up 2026-10-09)"
metadata:
  node_type: memory
  type: project
  originSessionId: 1f2ff73a-8924-4cf9-9abe-f3539b0eaa1c
  modified: 2026-10-11T15:58:57.376Z
---

The N64 port (branch `n64-port`) was developed on a Linux box (user `jack`, ~/n64, Flatpak ares). On 2026-10-09 it was set up on the user's Windows 11 PC (32 GB RAM, 4 logical CPUs) inside WSL2 Ubuntu 24.04, where the user is `root`:

- Clone: `/home/jack/halo-ce-universal` (branch n64-port) -- that path because port/n64/devtools/*.sh hard-code it. Git there uses Windows' gh.exe as its credential helper (repo-local config) and user JackTulli / noreply email.
- `/root/n64`: `toolchain` (libdragon .deb, GCC 16.2), `libdragon` (preview 39d0d6096 + port/n64/devtools/libdragon-rspq-buffer.patch, installed into the toolchain), `tiny3d`, `invader` (shallow), `roms`, and `tools` -> symlink to the clone's port/n64/devtools.
- Xbox maps (13 MP + ui + a30) copied into the clone's build/xbox/disc/maps from `/root/halo-ce-universal` -- a SEPARATE WSL clone of the Windows checkout on `xbox-port` with uncommitted edits; leave it alone.
- `systemd-run --user --scope` works in WSL as root, so build.sh's 9 GB memory cap works ([[memory-cap-heavy-builds]]). WSL has ~15 GB (no .wslconfig).
- First clean build here (dd17c9bd, 2026-10-09): `~/n64/tools/build.sh` OK in 638 s (4 CPUs), ROM 63,242,240 bytes; copied to ~/n64/roms/halo-n64.z64 and C:\Users\Jack\n64\roms.
- Playing: ares v148 for Windows in `C:\Users\Jack\n64\ares-v148`; `C:\Users\Jack\n64\play-halo-n64.bat [rom]` copies `~/n64/roms/<rom>` out of WSL and starts ares with the Expansion Pak.
- Hidden testing (2026-10-09, user asked for a Beaver Creek screenshot "without me seeing"): WSL has Xvfb + x11-utils + Flatpak ares 148 (`--user`). WSLg sets DISPLAY=:0, WAYLAND_DISPLAY=wayland-0 and PULSE_SERVER to the WINDOWS desktop/speakers -- unset them, use :97, and pass `--nosocket=wayland --nosocket=pulseaudio`. The Flatpak sandbox's /root is a stand-in, so ROMs under /root/n64 show "Unloaded" unless `--filesystem=/root/n64`; devtools/ares-*.sh lack both fixes. Use `/root/n64/hidden-shots.sh rom start interval count outprefix` (kills only its own PIDs, crops 750x576 at 118,81, `-draw_mouse 0`). Beaver Creek test ROM = flags `autotest debug_map=beavercreek` (DFS-only rebuild, 4 s; restore flags to [] after). ares runs ~13-18 VPS here (in-game counter ~28-30 fps); the autotest run is deterministic. Screenshots in C:\Users\Jack\n64\screenshots.
- The Windows checkout `C:\Users\Jack\Documents\GitHub\halo-ce-universal` is on `xbox-port` with uncommitted edits (win32_net.c, nx_kernel.c); it only has origin/n64-port as a remote-tracking ref.

- Parallel agents (2026-10-11: perf, sidewinder, menu480): each works in a full copy `/home/jack/n64-agents/<name>` (cp -a of the main tree incl. uncommitted work; `base` = the untouched starting snapshot for 3-way merges with `git merge-file`), builds with `REPO=<tree> /root/n64/agent-tools/build.sh` (flock /root/n64/build.lock + 9 GB cap: one heavy build at a time), debug ROMs with `agent-tools/flags-rom.sh`, ares runs with `agent-tools/run-rom.sh` (flock /root/n64/ares.lock). Their ROMs land in /root/n64/roms/<name>.z64; the coordinator merges into the main tree and builds the combined ROM.

**Why:** the Linux-era docs and memory assume ~/halo-ce-universal and Flatpak; on this machine everything is in WSL.
**How to apply:** run N64 builds via `wsl.exe -d Ubuntu -e bash -c '~/n64/tools/build.sh'` (call wsl with -e, and MSYS_NO_PATHCONV=1 from Git Bash); never launch ares on the user's live desktop without asking. Related: [[halo-n64-port]].
