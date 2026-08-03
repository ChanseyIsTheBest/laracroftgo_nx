/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

static int status_active;

void startup_status_update(const char *message) {
  if (!status_active) return;
  printf("\x1b[2J\x1b[H\n\n  Lara Croft GO\n\n  %s\n\n  Please wait...", message);
  consoleUpdate(NULL);
}

void startup_status_begin(const char *message) {
  if (!status_active) {
    consoleInit(NULL);
    status_active = 1;
  }
  startup_status_update(message);
}

void startup_status_end(void) {
  if (!status_active) return;
  consoleExit(NULL);
  status_active = 0;
}

/* Set once the game's graphics stack owns the display. After that, consoleInit()
 * is not safe: it re-enters vi/nwindow for a display the EGL surface already
 * holds, and aborts inside libnx rather than showing anything. */
static int display_busy;
void fatal_error_display_busy(int busy) { display_busy = busy; }

void fatal_error(const char *fmt, ...) {
  /* LOG BEFORE TOUCHING THE CONSOLE.
   *
   * This used to go straight to consoleInit(), so a fatal error mid-run produced
   * NO evidence at all: nothing in debug.log, and then a User Break inside
   * libnx's console/vi code because the game already owned the display. The
   * Atmosphere report then pointed at console.c, which says where the error
   * HANDLER died and nothing about what went wrong. A 1080p crash was
   * undiagnosable for exactly this reason. debugPrintf flushes every line, so
   * the message survives even if everything below fails. */
  char msg[1024];
  {
    va_list la;
    va_start(la, fmt);
    vsnprintf(msg, sizeof msg, fmt, la);
    va_end(la);
  }
  debugPrintf("[FATAL] %s\n", msg);

  if (display_busy) {
    /* Nothing can be shown on screen at this point, and trying is what turned a
     * reportable error into an unexplained break. The log has the message. */
    debugPrintf("[FATAL] display is owned by the game -- skipping the on-screen "
                "message and exiting. See the line above for the cause.\n");
    exit(1);
  }

  startup_status_end();
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  printf("%s", msg);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
