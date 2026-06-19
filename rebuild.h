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
 * dst: `--rebuild` rebuilds st from a pristine source tree with the declared
 * patches applied and atomically installs the result. `--install` ensures a
 * config file exists and installs the binary under /usr/local.
 * Both return a process exit code. Defined in rebuild.c.
 */
int rebuild(void);
int install_dst(void);
int edit_config(void);
