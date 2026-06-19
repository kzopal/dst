/* See LICENSE file for copyright and license details. */

/*
 * dst: runtime configuration parser.
 *
 * Reads a line-oriented rc-style file (see config.sample for the syntax) and
 * overwrites the compile-time defaults that config.h installs into x.c's
 * globals. Single pass, no dependencies. Anything malformed is warned about
 * (with a line number) and skipped; a missing file falls back to the defaults
 * silently.
 *
 * The only existing-source change this needs is the load_config() call near
 * the top of main(); everything else lives here.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * st globals we populate. These are *defined* (with their defaults) in
 * config.h, which is #included only into x.c; here we reference them by
 * external linkage. config.c must NOT include config.h, or those definitions
 * would be duplicated.
 */
extern char *font;
extern int borderpx;
extern char *shell;
extern char *termname;
extern unsigned int tabspaces;
extern int allowaltscreen;
extern int allowwindowops;
extern unsigned int blinktimeout;
extern unsigned int cursorthickness;
extern int bellvolume;
extern unsigned int cursorshape;
extern unsigned int cols, rows;
extern char *colorname[];

/*
 * Special slots in colorname[] (mirrors the default{cs,rcs,fg,bg} indices in
 * config.h). Kept as literals so the directive table can be a constant.
 */
#define COL_CURSOR  256
#define COL_RCURSOR 257
#define COL_FG      258
#define COL_BG      259

#define MAXVARS 64
#define MAXLINE 1024

enum { K_STR, K_INT, K_UINT, K_COLOR };

struct directive {
	const char *name;
	int kind;
	void *target;   /* K_STR: char** | K_INT: int* | K_UINT: unsigned* */
	int index;      /* K_COLOR: colorname[] slot */
};

static const struct directive directives[] = {
	{ "font",            K_STR,  &font,            0 },
	{ "shell",           K_STR,  &shell,           0 },
	{ "termname",        K_STR,  &termname,        0 },
	{ "borderpx",        K_INT,  &borderpx,        0 },
	{ "bellvolume",      K_INT,  &bellvolume,      0 },
	{ "allowaltscreen",  K_INT,  &allowaltscreen,  0 },
	{ "allowwindowops",  K_INT,  &allowwindowops,  0 },
	{ "tabspaces",       K_UINT, &tabspaces,       0 },
	{ "blinktimeout",    K_UINT, &blinktimeout,    0 },
	{ "cursorthickness", K_UINT, &cursorthickness, 0 },
	{ "cursorshape",     K_UINT, &cursorshape,     0 },
	{ "cols",            K_UINT, &cols,            0 },
	{ "rows",            K_UINT, &rows,            0 },
	{ "color0",          K_COLOR, NULL,  0 },
	{ "color1",          K_COLOR, NULL,  1 },
	{ "color2",          K_COLOR, NULL,  2 },
	{ "color3",          K_COLOR, NULL,  3 },
	{ "color4",          K_COLOR, NULL,  4 },
	{ "color5",          K_COLOR, NULL,  5 },
	{ "color6",          K_COLOR, NULL,  6 },
	{ "color7",          K_COLOR, NULL,  7 },
	{ "color8",          K_COLOR, NULL,  8 },
	{ "color9",          K_COLOR, NULL,  9 },
	{ "color10",         K_COLOR, NULL, 10 },
	{ "color11",         K_COLOR, NULL, 11 },
	{ "color12",         K_COLOR, NULL, 12 },
	{ "color13",         K_COLOR, NULL, 13 },
	{ "color14",         K_COLOR, NULL, 14 },
	{ "color15",         K_COLOR, NULL, 15 },
	{ "cursor",          K_COLOR, NULL, COL_CURSOR  },
	{ "reverse_cursor",  K_COLOR, NULL, COL_RCURSOR },
	{ "foreground",      K_COLOR, NULL, COL_FG      },
	{ "background",      K_COLOR, NULL, COL_BG      },
};

struct var { char *name; char *val; };   /* name keeps its leading '$' */
static struct var vars[MAXVARS];
static int nvars;

static const char *cfgpath;
static int cfglineno;

static void
warn(const char *msg, const char *arg)
{
	fprintf(stderr, "dst: %s:%d: %s%s%s\n", cfgpath, cfglineno,
	        msg, arg ? " " : "", arg ? arg : "");
}

static char *
dupstr(const char *s)
{
	char *p = strdup(s);
	if (!p) {
		fprintf(stderr, "dst: out of memory\n");
		exit(1);
	}
	return p;
}

/*
 * Pull the next whitespace-separated token out of *p, advancing *p past it.
 * A token may be wrapped in "double quotes" to embed spaces. Tokens are
 * null-terminated in place. Returns NULL when the string is exhausted.
 */
static char *
token(char **p)
{
	char *s = *p, *start;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s) {
		*p = s;
		return NULL;
	}
	if (*s == '"') {
		start = ++s;
		while (*s && *s != '"')
			s++;
		if (*s == '"')
			*s++ = '\0';
	} else {
		start = s;
		while (*s && !isspace((unsigned char)*s))
			s++;
		if (*s)
			*s++ = '\0';
	}
	*p = s;
	return start;
}

/* set $name value -> remember $name for later substitution. */
static void
def_var(char *args)
{
	char *name = token(&args);
	char *val = token(&args);

	if (!name || name[0] != '$' || !val) {
		warn("usage: set $name value", NULL);
		return;
	}
	if (nvars >= MAXVARS) {
		warn("too many variables, ignoring", name);
		return;
	}
	vars[nvars].name = dupstr(name);
	vars[nvars].val = dupstr(val);
	nvars++;
}

/*
 * i3-style raw substitution: copy `in` to `out`, replacing every $name that
 * matches a defined variable with its value. Done before tokenizing, so a
 * variable can expand into several tokens.
 */
static void
subst(const char *in, char *out, size_t outsz)
{
	size_t o = 0;
	const char *p = in;

	while (*p && o + 1 < outsz) {
		if (*p == '$') {
			const char *q = p + 1;
			size_t len, i;
			int found = 0;

			while (*q && (isalnum((unsigned char)*q) || *q == '_'))
				q++;
			len = q - p;   /* length including the '$' */
			for (i = 0; i < (size_t)nvars; i++) {
				if (strlen(vars[i].name) == len &&
				    strncmp(vars[i].name, p, len) == 0) {
					const char *v = vars[i].val;
					while (*v && o + 1 < outsz)
						out[o++] = *v++;
					p = q;
					found = 1;
					break;
				}
			}
			if (found)
				continue;
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
}

static void
apply(char *dir, char *args)
{
	size_t i;

	if (strcmp(dir, "set") == 0) {
		def_var(args);
		return;
	}
	/*
	 * `include <url>` is a patch directive consumed only by `dst --rebuild`
	 * (see rebuild.c). On the normal launch path it is a deliberate no-op,
	 * not an error.
	 */
	if (strcmp(dir, "include") == 0)
		return;
	for (i = 0; i < sizeof(directives) / sizeof(directives[0]); i++) {
		const struct directive *d = &directives[i];
		char *val, *end;
		long n;

		if (strcmp(dir, d->name) != 0)
			continue;
		if (!(val = token(&args))) {
			warn("missing value for", dir);
			return;
		}
		switch (d->kind) {
		case K_STR:
			*(char **)d->target = dupstr(val);
			break;
		case K_COLOR:
			colorname[d->index] = dupstr(val);
			break;
		case K_INT:
		case K_UINT:
			n = strtol(val, &end, 0);
			if (*end != '\0') {
				warn("expected a number for", dir);
				return;
			}
			if (d->kind == K_UINT)
				*(unsigned int *)d->target = (unsigned int)n;
			else
				*(int *)d->target = (int)n;
			break;
		}
		return;
	}
	warn("unknown directive", dir);
}

/*
 * Resolve the config file path into buf: $XDG_CONFIG_HOME/dst/config, else
 * ~/.config/dst/config. Returns 1 on success, 0 if neither var is set.
 * Shared with rebuild.c so both code paths read the same file.
 */
int
config_path(char *buf, size_t len)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	if (xdg && *xdg)
		snprintf(buf, len, "%s/dst/config", xdg);
	else if (home && *home)
		snprintf(buf, len, "%s/.config/dst/config", home);
	else
		return 0;
	return 1;
}

void
load_config(void)
{
	char path[1024], raw[MAXLINE], line[MAXLINE];
	FILE *f;

	if (!config_path(path, sizeof path))
		return;   /* nowhere to look; keep compiled defaults */

	if (!(f = fopen(path, "r")))
		return;   /* no config file; keep compiled defaults */

	cfgpath = path;
	cfglineno = 0;
	while (fgets(raw, sizeof raw, f)) {
		char *s = raw, *p, *dir;

		cfglineno++;
		while (*s && isspace((unsigned char)*s))
			s++;
		if (*s == '\0' || *s == '#')   /* blank or full-line comment */
			continue;
		subst(s, line, sizeof line);
		p = line;
		if (!(dir = token(&p)))
			continue;
		apply(dir, p);
	}
	fclose(f);
}
