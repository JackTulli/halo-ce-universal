---
name: memory-cap-heavy-builds
description: Heavy builds froze the user's 15 GB machine three times (2026-10-08/09) -- always run them memory-capped with limited parallelism
metadata:
  type: feedback
---

On 2026-10-08 and 2026-10-09 my N64 builds froze the user's whole computer three times ("you have entirely crashed my computer 3 times now froze it completely"): ninja ran 13 map conversions at once (~800 MB each) next to a 16-job LTO link on a machine with 15 GB RAM and 1 GB swap; the kernel thrashed instead of OOM-killing, and the journal just stops.

**Why:** memory exhaustion on a desktop with tiny swap freezes the machine for the user (they had to hard-reset), not just my build.

**How to apply:** run any heavy build or batch job through `systemd-run --user --scope --quiet -p MemoryMax=9G -p MemorySwapMax=0 ...` so it is killed instead of freezing the machine; limit parallelism (ninja -j 8; halo-ce-universal's N64 build has a `convert` pool of 3 and -flto=4, ~/n64/tools/build.sh does the capping). Never call `ninja` on the N64 build directly -- use build.sh. Measure a new tool's peak memory alone first (scratchpad peak.py) before running many in parallel. Related: [[halo-n64-port]], [[never-touch-the-live-display]].
