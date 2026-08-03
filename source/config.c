/* config.c -- simple configuration parser (PvZ Fusion 3.6.1 port)
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "util.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(handheld_res); \
  CONFIG_VAR_INT(docked_res); \
  CONFIG_VAR_INT(framerate);

Config config;
static int config_needs_rewrite = 0;

// actual screen size in use right now. PvZ Fusion is LANDSCAPE -> 1280x720.
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  // retired options -> drop them and rewrite the file without them
  // "portrait" joined this list when the TATE path was removed: PvZ Fusion is
  // landscape only, so an existing config.txt carrying it gets rewritten without.
  if (!strcmp(name, "touchscreen") || !strcmp(name, "controller_cursor") ||
      !strcmp(name, "show_fps") || !strcmp(name, "widescreen") ||
      !strcmp(name, "portrait") ||
      /* screen_width/screen_height were internal render geometry exposed by
       * accident; handheld_res/docked_res replace them.
       * "language" is gone: the in-game language now always follows the Switch
       * system language, which is what LANG_AUTO (the default) already did.
       * "resolution" was a brief single-value form, now split per mode;
       * "target_fps" was renamed to "framerate".
       *
       * NOTE "framerate" is a LIVE OPTION and must not be listed here. It
       * reaches FOUR places that must agree -- the nwindow swap interval, the
       * eglSwapInterval clamp, the Choreographer tick period, and
       * Application.targetFrameRate -- so if it is ever changed, change all
       * four. The one that actually governs the rate on this Android player is
       * targetFrameRate; see nx_force_target_framerate() in main.c.
       *
       * NOTE handheld_res and docked_res are LIVE OPTIONS and must not appear
       * here -- they were briefly retired when the single "resolution" key
       * replaced them, and leaving them listed would silently drop the very
       * settings this file exists to carry. */
      !strcmp(name, "screen_width") || !strcmp(name, "screen_height") ||
      !strcmp(name, "language") ||
      !strcmp(name, "target_fps") || !strcmp(name, "resolution")) {
    config_needs_rewrite = 1;
    return;
  }

  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config_needs_rewrite = 0;
  /* Each mode's native panel: the handheld screen is 720p, the dock outputs
   * 1080p. Both accept either value. */
  config.handheld_res = 720;
  config.docked_res   = 1080;
  config.framerate    = 60;

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  return config_needs_rewrite ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f,
    "# lcgo_nx configuration -- lines are \"name value\"; # starts a comment\n"
    "#\n"
    "# handheld_res -- render height in handheld mode:  720 [default] or 1080\n"
    "# docked_res   -- render height when docked:       720 or 1080 [default]\n"
    "#\n"
    "#                 Defaults match each mode\'s native panel. Both accept\n"
    "#                 either value: 1080 in handheld is supersampled down by\n"
    "#                 the compositor (sharper, costs GPU), 720 docked is\n"
    "#                 upscaled (faster, softer).\n"
    "#\n"
    "#                 The mode is read ONCE at launch and held for the session,\n"
    "#                 because the render size feeds the graphics surface and\n"
    "#                 cannot change under it. Dock BEFORE launching to get the\n"
    "#                 docked setting.\n"
    "#\n"
    "# framerate    -- frames per second the game targets:\n"
    "#                 60 [default] or 30\n"
    "#\n"
    "#                 Unity's Android default is 30 and this game never sets\n"
    "#                 the property itself, so the port sets it actively. On\n"
    "#                 the Android player QualitySettings.vSyncCount is ignored\n"
    "#                 -- Application.targetFrameRate is what governs the rate.\n"
    "#\n"
    "# Anything out of range falls back to the default shown above.\n"
    "# In-game language always follows the Switch system language.\n"
    "\n");

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s %g\n", #var, config.var)
  #define CONFIG_VAR_STR(var) if (config.var[0]) fprintf(f, "%s %s\n", #var, config.var)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR

  fclose(f);

  return 0;
}
