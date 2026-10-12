# Claude Code's memory for this project

These files are a copy of the memory Claude Code kept while it worked on
this repository in September 2026, in the format of its memory folder
(`~/.claude/projects/<project>/memory/`). `CLAUDE.md` at the root loads
them. The project memory is a running log, so its early entries are out of
date; `port/xbox/README.md` describes the current state. Memories about
the user's other projects are not included.

- [Halo CE universal port study](halo-ce-universal-study.md) — the Xbox port with nxdk, the test loop, the master server, the audits and the lessons learned
- [Never touch the live display](never-touch-the-live-display.md) — no tests or process kills against the user's own displays (:0 and nested ones)
- [Isolate Wine test prefixes](isolate-wine-test-prefixes.md) — fresh Wine prefixes link Documents to the real home folder; unlink them before running a game
- [Halo CE N64 port](halo-n64-port.md) — multiplayer split-screen port to the N64 (branch n64-port): libdragon + tiny3d, map converter, state and gotchas
- [N64 build env on Windows](n64-build-env-windows.md) — the n64-port builds in WSL Ubuntu (/home/jack/halo-ce-universal, /root/n64); ares + launcher in C:\Users\Jack\n64
- [N64: the user tests builds](n64-user-tests-builds.md) — just build and hand over the ROM; no hidden ares captures unless asked
