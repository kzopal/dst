/* See LICENSE file for copyright and license details. */

/*
 * dst: `--rebuild` entry point. Reads the `include` patch directives from the
 * dst config, rebuilds st from a pristine source tree with those patches
 * applied, and atomically installs the result. Returns a process exit code.
 * Defined in rebuild.c; dispatched from main().
 */
int rebuild(void);
