# Notes for Claude Code

This is the user's copy of
[cybersecurity/halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal)
with the port back to the original Xbox (`port/xbox`), internet play
through a master server (`port/master`) and the fixes made with Claude Code
in September 2026. Branch `xbox-port` has the work. Branch `main` is the
upstream project's.

The memory Claude Code kept while working here is in `docs/claude-memory/`
and loads with this file. It is a running log, so its early entries are out
of date. `port/xbox/README.md` describes the current state.

@docs/claude-memory/halo-ce-universal-study.md
@docs/claude-memory/never-touch-the-live-display.md
@docs/claude-memory/isolate-wine-test-prefixes.md

## The user's rules

- Do not test in a private emulator. After each build, pack both discs and
  ask the user to test them:

  ```sh
  python3 tools/xbox_disc.py --out /home/jack/xemufiles/halo-xbox.iso
  python3 tools/xbox_disc.py --out /home/jack/xemufiles/halo-xbox-performance.iso --performance --setting display.unlocked_framerate=true
  ```

- Build the Linux and Xbox copies from the same tree. Builds whose protocol
  version or session layout differ do not see each other's games.
- Deploy the master server only with its key, never with a password:
  `port/master/deploy.sh root@172.104.30.93 -i ~/.ssh/halo_master`.
- Never run anything against the user's live display (`:0`), and never kill
  processes by name. Kill only the processes you started, by their PID.
- GitHub Actions is off for this repository. The upstream workflow builds
  every push and publishes a release for each build of `main`. Ask the
  user before turning it on or making a release.
- When the memory in `~/.claude` changes, update its copy in
  `docs/claude-memory/` too. Leave out passwords, keys and other people's
  addresses.
- In the user's shell, `rm` and `cp` are aliases and `cd` also lists the
  folder. Use `/bin/rm`, `/bin/cp` and `builtin cd`.

## Build

nxdk's libraries must be built first, with `-O2` and the network options
(`port/xbox/README.md`, "Requirements"). Then:

```sh
export NXDK_DIR=/home/jack/nxdk PATH=/usr/lib/llvm-19/bin:/home/jack/nxdk/bin:$PATH
python3 configure.py
ninja xbox    # build/xbox/default.xbe
ninja linux   # build/linux/halo
```
