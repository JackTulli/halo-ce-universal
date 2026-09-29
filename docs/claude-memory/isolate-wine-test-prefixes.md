---
name: isolate-wine-test-prefixes
description: New Wine prefixes symlink Documents/Desktop/etc. to the real $HOME; unlink them before running games in a test prefix
metadata:
  node_type: memory
  type: feedback
  originSessionId: 75ddbf94-1404-40b5-b97e-54ceb9939584
  modified: 2026-09-29T14:36:16.757Z
---

A fresh Wine prefix (wineboot) makes drive_c/users/<user>/{Documents,Desktop,Downloads,Music,Pictures,Videos} symlinks into the real home directory. On 2026-09-29 a Chimera test run of Halo CE in a scratch prefix wrote into the user's real ~/Documents/My Games/Halo CE (savegame.bin, profiles, playlists re-saved by Halo; empty chimera/ dirs created, later removed).

**Why:** test prefixes are supposed to be throwaway; game saves/profiles in the real Documents are the user's data.

**How to apply:** right after wineboot in any test prefix, replace those symlinks with real directories (or set `winetricks sandbox`) before launching anything. Pairs with [[never-touch-the-live-display]] (always xvfb-run for tests).
