---
name: never-touch-the-live-display
description: "Never run X clients, tests or pkill against the user's live displays (:0 and the nested :N); the user's desktop runs the tree's own bin/ binaries and a pkill by name killed their compositor twice"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 6b46cdfc-9648-450c-9abc-99a2882a5545
  modified: 2026-09-08T17:07:02.652Z
---

Do not run anything against the user's live X displays (:0, and the nested display the compositor session creates, :1/:3/...). Never `pkill`/`killall` by process name; only kill PIDs of processes this session started. Test only in private Xvfb servers started by me (e.g. :7 host, :8 nested) and kill them by PID.

**Why:** On 2026-09-08 a compositor test on :0 plus `pkill -f "l2kscaler --nested"` / `pkill -x l2kscaler` killed the user's own session compositor (their desktop is shown through l2kscaler, so the screen went dead) twice; the user had to reboot and said "stop doing that". Their session runs from ~/Linux-explorer (run-in-place: bin/l2kwm, bin/l2kscaler), so `make` rewrites the binaries they are running (GNU ld unlinks first, but prefer building test copies in a worktree).

**How to apply:** private displays for every test, PIDs only, no `make install`/no `make` in the tree while their session runs unless asked, and no xwd/xprop/xrandr against :0 or the nested display. Related: [[linux-explorer-state]].
