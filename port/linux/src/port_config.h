/*
PORT_CONFIG.H

The native ports' settings, read from config.toml (port_config.c): next to
the executable on the desktop, in the data folder (the one holding maps/)
on Android. A missing file is written with the defaults. Each setting can
also be set for one run with its HALO_* environment variable, which wins
over the file (the tools and the Android app pass settings that way).

Settings are named "section.key", as in the file: "display.vsync".
*/

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
/* never NULL; "" when unset */
const char *config_string(const char *name);

/* ---------- the table itself (the launcher, launcher.h; not the console)

Every setting in the table's order, those of other builds too: read_here
says whether this build reads it. Values go in and out as text in the
spellings the file and the environment use: "true" or "false", the number,
a string's text without its quotes. */

struct config_setting_info
{
	/* "section.key" */
	const char *name;
	/* 0 boolean, 1 integer, 2 real, 3 string (launcher.h's
	launcher_setting_type) */
	int type;
	/* the comment the file carries, lines separated by '\n' */
	const char *comment;
	int read_here;
	char default_text[128];
};

int config_setting_count(void);
/* FALSE for an index out of the table */
int config_setting_info(int index, struct config_setting_info *info);
/* the current value as text (reads the file first if it has not been) */
int config_setting_text(int index, char *text, int size);
/* sets the current value from text, as the environment variable would */
int config_setting_set_text(int index, const char *text);
/* writes config.toml with the current values of this build's settings,
keeping the file's comments, its order and the keys this build does not
read; a missing or unreadable file is written anew. FALSE when it could
not be written */
int config_save(void);

#endif
