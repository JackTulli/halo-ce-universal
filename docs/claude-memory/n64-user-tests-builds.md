---
name: n64-user-tests-builds
description: "For the N64 port the user tests every build themselves -- just build and hand over the ROM, no hidden ares captures unless asked"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 1f2ff73a-8924-4cf9-9abe-f3539b0eaa1c
  modified: 2026-10-11T16:52:10.294Z
---

On 2026-10-09 the user said twice "just give me a build and let me test it that will be way easier" and "let me do all the testing just make me a rom" (after I spent time on hidden-ares screenshot verification of HUD and explosion changes).

**Why:** the user plays the ROM in Windows ares at full speed and judges looks and feel faster than my hidden captures (ares runs ~10 VPS hidden in WSL, timing differs).

2026-10-11 the user repeated it more strongly ("do not do any testing yourself just build me roms let me test", "no testing let me test") and it applies to subagents too: no ares runs at all, not even to verify a crash fix; builds only to check that code compiles. They also said not to build the combined ROM until the optimisation work was done -- ask/follow their sequencing rather than shipping intermediate ROMs.

**How to apply:** after an N64 change: build (`~/n64/tools/build.sh`, port.json flags []), copy to `/root/n64/roms/halo-n64.z64` and `C:\Users\Jack\n64\roms\halo-n64.z64`, tell the user what changed and what to look at. Only run hidden captures (`/root/n64/flagshot.sh`, debug flags like debug_title_time, debug_explosions, debug_hud_inset) when the user asks or to diagnose a reported bug. Same spirit as the Xbox rule in CLAUDE.md. Related: [[n64-build-env-windows]], [[halo-n64-port]].
