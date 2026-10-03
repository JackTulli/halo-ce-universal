/*
WIN32_LAUNCHER.C

The settings window shown at start (launcher.h), for the native Windows
build: the Windows 98/ME build's unit as it is
(port/win9x/platform/w9x_launcher.c), which keeps to what Windows 95's
USER32 and comctl32 have and so runs on every Windows since. It is shown
from port_config.c when the settings are first read, before SDL has made
the window.
*/

#include "../../win9x/platform/w9x_launcher.c"
