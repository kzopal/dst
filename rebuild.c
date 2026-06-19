/* See LICENSE file for copyright and license details. */
/*
 * dst - dynamic suckless terminal
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * dst: `dst --rebuild`.
 *
 * Reads the `include <url>` patch directives from ~/.config/dst/config and
 * rebuilds st with those patches applied:
 *
 *   1. start from a pristine source tree (copy-clean-then-patch, so removing
 *      an include line drops that patch on the next rebuild);
 *   2. fetch each diff (cached locally) and apply it in order with patch -p1;
 *   3. build to a temporary location with the system toolchain;
 *   4. only on success move the binary into place (a failed rebuild never
 *      clobbers the working binary).
 *
 * This is orchestration only: curl/patch/make/cc do the real work. The
 * component is independent of the runtime parser; the one thing it shares is
 * config_path(), so it reads the same file.
 *
 * Locations (all overridable):
 *   pristine source   $DST_SRC              else $cache/src (auto-cloned from DST_REPO)
 *   cache root        $XDG_CACHE_HOME/dst   else ~/.cache/dst   (build/, patches/)
 *   install target    $DST_BIN              else ~/.local/bin/dst
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "rebuild.h"

extern int config_path(char *buf, size_t len);

#define MAXINC  128
#define MAXLINE 1024

static char cachedir[1024];
static char srcdir[1024];
static char builddir[1024];
static char patchdir[1024];
static char installpath[1024];
static int  srcexplicit;  /* DST_SRC was set by user */

/* Run argv as a child process; return its exit status (0 == success). */
static int
run(char *argv[])
{
	pid_t pid;
	int status;

	fflush(NULL);
	if ((pid = fork()) < 0) {
		perror("dst: fork");
		return -1;
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		fprintf(stderr, "dst: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* True if `tool` is found on PATH. */
static int
have(const char *tool)
{
	char cmd[256];
	char *argv[] = { "sh", "-c", cmd, NULL };

	snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
	return run(argv) == 0;
}

static const char *
urlbasename(const char *url)
{
	const char *b = strrchr(url, '/');
	return b ? b + 1 : url;
}

static void
setpaths(void)
{
	const char *home = getenv("HOME");
	const char *xdgc = getenv("XDG_CACHE_HOME");
	const char *src = getenv("DST_SRC");
	const char *bin = getenv("DST_BIN");

	if (!home)
		home = ".";
	if (xdgc && *xdgc)
		snprintf(cachedir, sizeof cachedir, "%s/dst", xdgc);
	else
		snprintf(cachedir, sizeof cachedir, "%s/.cache/dst", home);
	snprintf(builddir, sizeof builddir, "%s/build", cachedir);
	snprintf(patchdir, sizeof patchdir, "%s/patches", cachedir);
	srcexplicit = (src && *src);
	if (srcexplicit)
		snprintf(srcdir, sizeof srcdir, "%s", src);
	else
		snprintf(srcdir, sizeof srcdir, "%s/src", cachedir);
	if (bin && *bin)
		snprintf(installpath, sizeof installpath, "%s", bin);
	else
		snprintf(installpath, sizeof installpath, "%s/.local/bin/dst", home);
}

/* Base URL for dst-patches; used to resolve short include names. */
#define PATCHES_BASE "https://raw.githubusercontent.com/kzopal/dst-patches/master"

/*
 * Return the INDEX‑derived full URL for a short include name, or NULL if the
 * name is unknown.  The INDEX is fetched (if not already cached) into
 * patchdir so it persists across rebuilds.
 */
static const char *
resolve_short_name(const char *shortname)
{
	static char url[2048];
	char idxpath[2048], line[1024], name[256], file[256];
	FILE *f;

	if (!patchdir[0])
		setpaths();

	snprintf(idxpath, sizeof idxpath, "%s/INDEX", patchdir);

	if (access(idxpath, R_OK) != 0) {
		char *a[] = { "curl", "-fsSL", "-o", idxpath, PATCHES_BASE "/INDEX", NULL };
		printf("dst: fetching patch index\n");
		if (run(a)) {
			fprintf(stderr, "dst: failed to fetch patch index from %s\n"
			    "     use a full URL to bypass short-name lookup\n",
			    PATCHES_BASE "/INDEX");
			return NULL;
		}
	}

	f = fopen(idxpath, "r");
	if (!f)
		return NULL;

	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		if (sscanf(p, "%255s %255s", name, file) < 2)
			continue;
		if (strcmp(name, shortname) == 0) {
			snprintf(url, sizeof url, "%s/%s", PATCHES_BASE, file);
			fclose(f);
			return url;
		}
	}
	fclose(f);
	return NULL;
}

/* Collect include URLs from the config file. Returns count, or -1 on error. */
static int
read_includes(char inc[][MAXLINE], int max)
{
	char path[1024], raw[MAXLINE];
	FILE *f;
	int n = 0, lineno = 0;

	if (!config_path(path, sizeof path)) {
		fprintf(stderr, "dst: cannot locate config (set HOME or XDG_CONFIG_HOME)\n");
		return -1;
	}
	if (!(f = fopen(path, "r"))) {
		fprintf(stderr, "dst: no config at %s; nothing to rebuild\n", path);
		return -1;
	}
	while (fgets(raw, sizeof raw, f)) {
		char *s = raw, *url, *e;

		lineno++;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#' || *s == '\n' || *s == '\r' || *s == '\0')
			continue;
		/* only "include" lines concern us; everything else is parser turf */
		if (strncmp(s, "include", 7) != 0 || (s[7] != ' ' && s[7] != '\t'))
			continue;
		s += 7;
		while (*s == ' ' || *s == '\t')
			s++;
		url = s;
		for (e = url + strlen(url); e > url && (e[-1] == '\n' ||
		     e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); )
			*--e = '\0';
		if (*url == '\0') {
			fprintf(stderr, "dst: %s:%d: include with no URL\n", path, lineno);
			continue;
		}
		if (n >= max) {
			fprintf(stderr, "dst: too many include lines (max %d)\n", max);
			break;
		}
		/* resolve short names that have no scheme vs full URLs */
		if (strstr(url, "://")) {
			snprintf(inc[n++], MAXLINE, "%s", url);
		} else {
			const char *resolved = resolve_short_name(url);
			if (!resolved) {
				fprintf(stderr, "dst: %s:%d: unknown patch \"%s\"\n",
				    path, lineno, url);
				continue;
			}
			snprintf(inc[n++], MAXLINE, "%s", resolved);
		}
	}
	fclose(f);
	return n;
}

int
rebuild(void)
{
	static char inc[MAXINC][MAXLINE];
	char cache[2048], built[2048], instdir[1024], *slash;
	int n, i;

	setpaths();

	if ((n = read_includes(inc, MAXINC)) < 0)
		return 1;

	/* cc/make are always needed; curl/patch only when there are patches */
	{
		const char *always[] = { "cc", "make" };
		const char *forpatch[] = { "patch", "curl" };
		for (i = 0; i < (int)(sizeof always / sizeof always[0]); i++)
			if (!have(always[i])) {
				fprintf(stderr, "dst: required tool not found: %s\n", always[i]);
				return 1;
			}
		for (i = 0; n > 0 && i < (int)(sizeof forpatch / sizeof forpatch[0]); i++)
			if (!have(forpatch[i])) {
				fprintf(stderr, "dst: required tool not found: %s\n", forpatch[i]);
				return 1;
			}
	}

	if (access(srcdir, R_OK) != 0) {
		if (!srcexplicit) {
			/* auto-bootstrap: clone the repo at the pinned tag */
			if (!have("git")) {
				fprintf(stderr, "dst: git required to clone source from %s\n", DST_REPO);
				return 1;
			}
			printf("dst: cloning source from %s (tag %s)\n", DST_REPO, DST_TAG);
			{ char *a[] = { "git", "clone", "--branch", DST_TAG, "--depth", "1",
			                DST_REPO, srcdir, NULL };
			  if (run(a)) {
				fprintf(stderr,
				    "dst: failed to clone source from %s\n"
				    "     set DST_SRC to point at a local checkout, or check connectivity\n",
				    DST_REPO);
				return 1;
			  } }
		} else {
			fprintf(stderr,
			    "dst: pristine source not found at %s\n"
			    "     check that DST_SRC points at a valid dst source tree\n",
			    srcdir);
			return 1;
		}
	}

	/* ensure cache dirs exist */
	{ char *a[] = { "mkdir", "-p", patchdir, NULL }; if (run(a)) return 1; }

	/* copy-clean-then-patch: wipe build dir, recopy pristine source */
	{ char *a[] = { "rm", "-rf", builddir, NULL }; if (run(a)) return 1; }
	{ char *a[] = { "cp", "-R", srcdir, builddir, NULL };
	  if (run(a)) { fprintf(stderr, "dst: failed to copy source tree\n"); return 1; } }
	/* drop VCS metadata so it can't interfere with the build */
	{ char g[1100]; snprintf(g, sizeof g, "%s/.git", builddir);
	  char *a[] = { "rm", "-rf", g, NULL }; run(a); }

	/* fetch + apply each patch, in listed order */
	for (i = 0; i < n; i++) {
		snprintf(cache, sizeof cache, "%s/%s", patchdir, urlbasename(inc[i]));
		if (access(cache, R_OK) != 0) {
			char *a[] = { "curl", "-fsSL", "-o", cache, inc[i], NULL };
			printf("dst: fetching %s\n", inc[i]);
			if (run(a)) {
				fprintf(stderr, "dst: failed to fetch %s\n", inc[i]);
				return 1;
			}
		}
		printf("dst: applying %s\n", urlbasename(inc[i]));
		{ char *a[] = { "patch", "-p1", "-d", builddir, "-i", cache, NULL };
		  if (run(a)) {
			fprintf(stderr, "dst: patch failed: %s\n"
			    "     fix or remove the include line; rejects are under %s\n",
			    urlbasename(inc[i]), builddir);
			return 1;
		  } }
	}

	/* build into the temporary build dir; clean first so that a dirty
	 * source tree (stale .o/binary) can't masquerade as up-to-date */
	{ char *a[] = { "make", "-C", builddir, "clean", NULL }; run(a); }
	printf("dst: building\n");
	{ char *a[] = { "make", "-C", builddir, NULL };
	  if (run(a)) { fprintf(stderr, "dst: build failed; working binary left untouched\n"); return 1; } }

	/* atomic-ish install: only now move the fresh binary into place */
	snprintf(built, sizeof built, "%s/dst", builddir);
	if (access(built, X_OK) != 0) {
		fprintf(stderr, "dst: build produced no binary at %s\n", built);
		return 1;
	}
	snprintf(instdir, sizeof instdir, "%s", installpath);
	if ((slash = strrchr(instdir, '/'))) {
		*slash = '\0';
		char *a[] = { "mkdir", "-p", instdir, NULL };
		run(a);
	}
	{ char *a[] = { "mv", "-f", built, installpath, NULL };
	  if (run(a)) { fprintf(stderr, "dst: install to %s failed\n", installpath); return 1; } }

	printf("dst: installed %s (%d patch%s applied)\n",
	       installpath, n, n == 1 ? "" : "es");
	return 0;
}

static void
makedirs(const char *path)
{
	char buf[1024], *p;
	snprintf(buf, sizeof buf, "%s", path);
	for (p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(buf, 0755);
			*p = '/';
		}
	/* final component */
	mkdir(buf, 0755);
}

static int
ensure_config(void)
{
	char path[1024], dir[1024], *slash;
	FILE *f;

	if (!config_path(path, sizeof path))
		return -1;
	if (access(path, F_OK) == 0)
		return 0;   /* already exists */

	snprintf(dir, sizeof dir, "%s", path);
	if ((slash = strrchr(dir, '/')))
		*slash = '\0';
	makedirs(dir);

	if (!(f = fopen(path, "w"))) {
		fprintf(stderr, "dst: cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(f, "# dst configuration\n"
	           "# See dst(1) and config.sample for all directives.\n"
	           "\n"
	           "font  \"monospace:pixelsize=12:antialias=true:autohint=true\"\n"
	           "shell %s\n"
	           "termname st-256color\n"
	           "borderpx 2\n"
	           "cursorshape 2\n"
	           "cursorthickness 2\n"
	           "bellvolume 0\n"
	           "allowaltscreen 1\n"
	           "\n"
		           "# Add include lines here for dst --rebuild, e.g.:\n"
		           "# include scrollback\n"
		           "# include vertcenter\n"
		           "# include https://raw.githubusercontent.com/…/patch.diff\n",
	           getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
	fclose(f);
	printf("dst: created %s\n", path);
	return 0;
}

static int
install_binary(const char *self, const char *target)
{
	char dir[1024], *slash, *a[] = { "install", "-m", "755",
	                                 (char *)self, (char *)target, NULL };

	if (strcmp(self, target) == 0)
		return 0;  /* already there */
	snprintf(dir, sizeof dir, "%s", target);
	if ((slash = strrchr(dir, '/')))
		*slash = '\0';
	makedirs(dir);
	if (access(dir, W_OK) != 0) {
		fprintf(stderr, "dst: write permission denied for %s\n", dir);
		fprintf(stderr, "     try: sudo dst --install\n");
	} else if (run(a) != 0) {
		fprintf(stderr, "dst: install to %s failed\n", target);
		return -1;
	}
	if (access(target, X_OK) == 0) {
		printf("dst: installed %s\n", target);
		return 0;
	}
	return -1;
}

int
install_dst(void)
{
	char self[1024];
	ssize_t n;
	char *first_try = "/usr/local/bin/dst";
	char *fallback;
	const char *home = getenv("HOME");

	n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n < 0) {
		perror("dst: readlink /proc/self/exe");
		return 1;
	}
	self[n] = '\0';

	ensure_config();

	if (install_binary(self, first_try) == 0) {
		printf("dst: install man page via: sudo make install (from src tree)\n");
		return 0;
	}

	/* fallback to ~/.local/bin/dst */
	if (home) {
		static char fb[1024];
		snprintf(fb, sizeof fb, "%s/.local/bin/dst", home);
		fallback = fb;
	} else {
		fallback = NULL;
	}
	if (fallback && install_binary(self, fallback) == 0) {
		printf("dst: install man page via: sudo make install (from src tree)\n");
		return 0;
	}

	fprintf(stderr, "dst: could not install to any location\n");
	return 1;
}

int
edit_config(void)
{
	char path[1024], *editor;
	pid_t pid;
	int st;

	if (config_path(path, sizeof path) == 0)
		return 1;

	if (access(path, F_OK) != 0)
		ensure_config();

	if (!(editor = getenv("EDITOR")))
		editor = "nano";

	switch ((pid = fork())) {
	case -1:
		fprintf(stderr, "dst: fork: %s\n", strerror(errno));
		return 1;
	case 0:
		execlp(editor, editor, path, (char *)NULL);
		fprintf(stderr, "dst: exec %s: %s\n", editor, strerror(errno));
		_exit(1);
	default:
		waitpid(pid, &st, 0);
		return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
	}
}
