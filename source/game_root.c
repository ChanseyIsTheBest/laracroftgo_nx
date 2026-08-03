/* game_root.c -- work out which folder the game lives in, at runtime.
 *
 * The port used to hardcode sdmc:/switch/lcgo. That made the folder name
 * load-bearing in a way nothing on screen explained: name it lcgo_nx to
 * match the .nro and you got "missing libmain.so" while staring at libmain.so.
 *
 * Resolution order:
 *   1. The directory containing our own .nro (argv[0]). This is almost always
 *      right and needs no searching -- whatever folder you dropped everything
 *      into is the game folder, whatever it is called.
 *   2. A scan of sdmc:/switch/<dir> for one that looks like an install. Covers
 *      launches where argv[0] is missing or not a real path (some forwarders,
 *      title-override setups).
 *   3. sdmc:/switch/<GAME_FOLDER>, the historical default, so an install that
 *      already worked keeps working.
 *
 * "Looks like an install" means it holds libmain.so (either at the top or in
 * lib/arm64-v8a/, both layouts are accepted) or an already-built assets.nxpack.
 * A folder is only accepted if one of those is present, so unrelated homebrew
 * folders under /switch are never picked up.
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

char g_game_root[512] = "sdmc:/switch/" GAME_FOLDER;

/* Rotating buffers: game_path() is routinely used more than once in a single
 * expression (two arguments of one call, say), so a single static buffer would
 * silently alias. */
#define PATH_SLOTS 8
static char g_path_buf[PATH_SLOTS][768];
static unsigned g_path_slot;

const char *game_path(const char *relative) {
  char *out = g_path_buf[g_path_slot++ % PATH_SLOTS];
  if (!relative || !*relative)
    snprintf(out, sizeof g_path_buf[0], "%s", g_game_root);
  else if (relative[0] == '/')
    snprintf(out, sizeof g_path_buf[0], "%s%s", g_game_root, relative);
  else
    snprintf(out, sizeof g_path_buf[0], "%s/%s", g_game_root, relative);
  return out;
}

const char *game_root(void) { return g_game_root; }

static int is_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Does this directory hold a game install? */
static int looks_like_install(const char *dir) {
  char probe[768];
  snprintf(probe, sizeof probe, "%s/libmain.so", dir);
  if (is_file(probe)) return 1;
  snprintf(probe, sizeof probe, "%s/lib/arm64-v8a/libmain.so", dir);
  if (is_file(probe)) return 1;
  snprintf(probe, sizeof probe, "%s/assets.nxpack", dir);
  if (is_file(probe)) return 1;
  return 0;
}

/* Strip the file name from argv[0] and normalise to an sdmc: path.
 * hbloader hands us either "sdmc:/switch/foo/bar.nro" or "/switch/foo/bar.nro"
 * depending on how it was launched. */
static int root_from_argv(const char *arg0, char *out, size_t cap) {
  if (!arg0 || !*arg0) return 0;
  char work[512];
  snprintf(work, sizeof work, "%s", arg0);
  for (char *p = work; *p; p++) if (*p == '\\') *p = '/';

  char *slash = strrchr(work, '/');
  if (!slash) return 0;
  *slash = '\0';
  if (!work[0]) return 0;

  if (!strncmp(work, "sdmc:", 5))
    snprintf(out, cap, "%s", work);
  else if (work[0] == '/')
    snprintf(out, cap, "sdmc:%s", work);
  else
    return 0;
  return 1;
}

/* Scan sdmc:/switch for an install. Returns the number of matches found and
 * writes the first into `out`; the count lets the caller warn when the answer
 * was ambiguous. */
static int scan_switch_dir(char *out, size_t cap) {
  DIR *d = opendir("sdmc:/switch");
  if (!d) return 0;
  int matches = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char candidate[768];
    snprintf(candidate, sizeof candidate, "sdmc:/switch/%s", e->d_name);
    struct stat st;
    if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    if (!looks_like_install(candidate)) continue;
    if (matches == 0) snprintf(out, cap, "%s", candidate);
    matches++;
  }
  closedir(d);
  return matches;
}

void game_root_resolve(int argc, char *argv[]) {
  /* Resolve BEFORE logging anything: debugPrintf opens debug.log under the
   * game root, so a log line emitted here would land in the fallback folder. */
  char candidate[512];
  const char *how = NULL;
  int matches = 0;
  int argv_dir_empty = 0;

  if (argc > 0 && argv && root_from_argv(argv[0], candidate, sizeof candidate)) {
    if (looks_like_install(candidate)) {
      snprintf(g_game_root, sizeof g_game_root, "%s", candidate);
      how = "next to the .nro";
    } else {
      argv_dir_empty = 1;
    }
  }

  if (!how) {
    matches = scan_switch_dir(candidate, sizeof candidate);
    if (matches > 0) {
      snprintf(g_game_root, sizeof g_game_root, "%s", candidate);
      how = "found under sdmc:/switch";
    }
  }

  debugPrintf("[root] game root = %s (%s)\n", g_game_root,
              how ? how : "fallback default -- no install found");
  if (argv_dir_empty)
    debugPrintf("[root] note: the .nro's own folder has no game files in it\n");
  if (matches > 1)
    debugPrintf("[root] WARNING: %d candidate folders under sdmc:/switch; keep the "
                ".nro beside the game files to make this unambiguous\n", matches);
}
