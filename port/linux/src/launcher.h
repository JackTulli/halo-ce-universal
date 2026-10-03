/*
LAUNCHER.H

The settings window the Windows builds show at start (the launcher): a
Win32 dialog over the settings table of port_config.c, in a Windows-facing
unit (port/win9x/platform/w9x_launcher.c, which the native Windows build
compiles as port/windows/src/win32_launcher.c). Only plain C types cross
here, as through posix.h: port_config.c (config_launcher) hands over the
settings this build reads, each with its value as text, and takes the
values back when the player chooses Play.
*/

#ifndef __HALO_LAUNCHER_H
#define __HALO_LAUNCHER_H

/* the setting's type (port_config.c's config_type, in its order) */
enum launcher_setting_type
{
	_launcher_boolean,
	_launcher_integer,
	_launcher_real,
	_launcher_string,
};

struct launcher_setting
{
	/* "section.key", as in the file */
	const char *name;
	enum launcher_setting_type type;
	/* the default as text: "true" or "false", the number, a string's text
	without its quotes */
	const char *default_text;
	/* the comment the file carries, lines separated by '\n' */
	const char *comment;
	/* the value as text, in the same spellings; the dialog writes the
	player's choice back into it (value_size bytes) */
	char *value;
	int value_size;
};

/* shows the dialog and waits; returns 1 for Play (the values are updated),
0 for Quit. Nothing of the game's exists yet: no window, no sound */
int launcher_show(struct launcher_setting *settings, int count);

/* port_config.c's log line (xbox_kernel.c) */
void platform_log(const char *format, ...);

#endif
