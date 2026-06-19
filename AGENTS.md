# AGENTS.md — dst (dynamic suckless terminal)

Instructions for AI coding agents (opencode etc.) continuing this project.
Read this fully before changing anything.

## What dst is

A fork of suckless **st** that replaces st's compile-time `config.h` model with
a **runtime config file** (`~/.config/dst/config`) and adds a **declarative
patch manager** (`dst --rebuild`). Ethos: small diff vs upstream st, no new
build/runtime dependencies, elegance over features. Source filenames stay as
upstream (`st.c`, `x.c`, `st.h`) so upstream st patches keep applying — only the
binary/man/docs are rebranded to `dst`.

Upstream remote is `upstream` (https://git.suckless.org/st). dst work lives on
`master`. Base pin: st 0.9.3 + 2 commits (`688f70a`). dst version: 0.1.

## Architecture — three SEPARATE components (keep them separate)

1. **Runtime config parser** — `config.c` (prototypes in `config.def.h`).
   Reads `~/.config/dst/config` (respects `$XDG_CONFIG_HOME`) at startup. i3-style
   line format, hand-rolled (no INI lib): strip ws, skip blank + full-line `#`
   comments, raw-string `$var` substitution BEFORE tokenizing, then a directive
   dispatch table (`{name, kind, target, index}`, linear scan). Quoted `"..."`
   tokens allowed. Populates st's existing globals; precedence is
   **defaults < config file < CLI flags** (`load_config()` runs before `ARGBEGIN`).
   Unknown/malformed → warn to stderr w/ line number, skip, never abort. Missing
   file → silent defaults. `strdup` persistent strings once (no free; process-lifetime).
   `include` lines are a deliberate no-op here.

2. **Patch directives** — `include <url>` lines in the SAME config file. Ignored
   by component 1; consumed only by component 3.

3. **`dst --rebuild`** — `rebuild.c` (prototype `rebuild.h`), dispatched from
   `main()` when `argv[1] == "--rebuild"`. fork+execvp (NOT shell strings) to the
   system toolchain. Reads `include` URLs (its own minimal line reader; shares only
   `config_path()` with component 1), then: wipe build dir, copy pristine source,
   `make clean`, fetch each diff (cached under `$cache/patches`), `patch -p1` in
   listed order (stop on first reject), build, and only on success `mv` the binary
   into place. Paths overridable by env: `DST_SRC` (else `$cache/src`, auto-cloned
   from `DST_REPO` at tag `DST_TAG` when missing),
   `$XDG_CACHE_HOME/dst` (else `~/.cache/dst`; holds `build/`, `patches/`),
   install `DST_BIN` (else `~/.local/bin/dst`). Needs `cc`+`make` always,
   `curl`+`patch` only when there are includes.

## Edits to existing st source (keep minimal & surgical)

- `config.def.h`: dropped `static`/`const` on the configurable globals so the
  parser can write them (incl. writable `colorname[]`); added `load_config()` and
  `config_path()` prototypes. `config.h` is generated from this by the Makefile
  and is gitignored.
- `x.c`: `#include "rebuild.h"`; `--rebuild` dispatch as first line of `main()`;
  `load_config()` before `ARGBEGIN`; default window title `"st"`→`"dst"`; `--rebuild`
  added to `usage()`.
- `Makefile`: `SRC += config.c rebuild.c`; target renamed `st`→`dst`; man page
  `st.1`→`dst.1`; tarball `dst-$(VERSION)`. terminfo `st.info` kept (TERM stays
  `st-256color`).
- `config.mk`: `VERSION = 0.1`.

## Build & test

```
make                       # builds ./dst
make clean
```
Quick checks without an X display (the parser/rebuild code is pure libc):
- `cc -std=c99 -D_XOPEN_SOURCE=600 -Wall -Wextra -fsyntax-only config.c rebuild.c`
- Parser: link `config.o` against a stub `main` that defines the extern globals
  (font, borderpx, colorname[260], …), point `$XDG_CONFIG_HOME` at a temp config,
  call `load_config()`, print results.
- Rebuild end-to-end: set `DST_SRC` (or let it auto-clone), isolated
  `XDG_CACHE_HOME`/`XDG_CONFIG_HOME`/`DST_BIN`, put an `include` line in the
  config, run `./dst --rebuild`. (Verified working with
  `st-scrollback-0.9.2.diff` against the 0.9.3 base.)

Safety: never `rm -rf` a variable path without a guard; prefer `mktemp -d` and
copy with `tar`/`cp` excludes rather than delete-then-copy.

## Status — done

- All three components implemented; `make` links clean under `-Wall -Wextra`.
- Runtime parser tested (directives, quotes, `$var` subst, special color slots,
  include-skip, line-numbered warnings, malformed handling).
- `--rebuild` tested END-TO-END with a real upstream patch: fetch → `patch -p1`
  (applied with offsets) → fresh compile fusing the patch with dst's own code →
  atomic install. Verified both features present in the built binary.
- Full rebrand to `dst` (binary, README, `dst.1`, version) done.

## Remaining work (TODO)

1. **Publish to GitHub.** Repo not created yet; `gh` CLI not installed in the
   working env. Once available:
   `gh repo create dst --source=. --public --remote=origin --push`
   GitHub description = the README tagline.
2. **[DONE] Productionize the pin.** `--rebuild` now auto-clones from `DST_REPO`
   at `DST_TAG` when no source tree is found and `DST_SRC` is unset. Patches still
   apply with FUZZ — consider pinning to an exact st tag for byte-exact application,
   or add the optional per-patch checksum the original design mentioned.
3. **Exercise more patches.** Only `st-scrollback` has been run live. Try patches
   that touch the same lines as dst's edits (config.def.h de-static lines) to find
   real conflicts; document any that need ordering or manual resolution.
4. **Runtime keybindings** — explicitly out of scope for v1 (C function pointers
   need a string→function table). Deferred; leave shortcuts in config.def.h.
5. **Nice-to-haves**: more directives if requested; a `-C <path>` flag to point at
   an alternate config; man page `SEE ALSO` already lists tabbed/utmp/stty/scroll.

## Conventions

- Match suckless/st C style (tabs, K&R braces, terse). No new dependencies.
- Keep dst's own logic in new files (`config.c`, `rebuild.c`); touch existing
  source only where unavoidable, so upstream patches/rebases stay clean.
- Don't track generated `config.h` or build artifacts (see `.gitignore`).

