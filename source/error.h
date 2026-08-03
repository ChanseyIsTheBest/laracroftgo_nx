/* error.h -- error handler
 *
 * Copyright (C) 2021 fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

/* Tell fatal_error() the game owns the display, so it logs and exits instead
 * of calling consoleInit() (which aborts inside vi once EGL holds it). */
void fatal_error_display_busy(int busy);

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

/* First-boot progress console. The asset-pack build reads and rewrites ~290 MB
 * of SD card, which takes long enough that a silent black screen looks like a
 * hang. These put a line of text on screen while that happens. Cheap no-ops
 * once startup_status_end() has been called. */
void startup_status_begin(const char *message);
void startup_status_update(const char *message);
void startup_status_end(void);

#endif
