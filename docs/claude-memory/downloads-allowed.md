---
name: downloads-allowed
description: The user allows downloading and installing build/test tools for the Halo port without asking each time
metadata:
  node_type: memory
  type: feedback
  originSessionId: 0ffd59b6-aab3-4875-8b47-5fbed5df157a
  modified: 2026-10-02T17:01:08.815Z
---

For the Halo port work, download or install whatever tool is needed (MSYS2 packages, validators, SDK pieces) without asking first; say afterwards what was installed.

**Why:** on 2026-10-02 I asked to install glslang to validate the OpenGL 2.0 shaders and the user answered "yes of crouse download anything you need".

**How to apply:** covers tools and packages on this PC. It does not cover changing security settings (firewall), releases, GitHub Actions, deploying the master server, or running things on their Windows 9x machines, which keep their own rules ([[halo-xbox-windows-build]]).
