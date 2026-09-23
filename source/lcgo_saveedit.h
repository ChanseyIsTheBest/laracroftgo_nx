/* lcgo_saveedit.h -- edit the SaveData*.runtime files at boot from save.txt
 * (see lcgo_saveedit.c). MIT. */
#ifndef LCGO_SAVEEDIT_H
#define LCGO_SAVEEDIT_H

#include <stddef.h>

/* Portable core, tested on a PC. dir holds save.txt and the five
 * SaveData<Type>.runtime files; the report goes to dir/save.log.
 * Returns 1 = a save file was rewritten, 0 = nothing to do, -1 = something was
 * refused or failed (see save.log). */
int lcgo_saveedit_run_dir(const char *dir);

/* Switch entry point: GAME_HOME/save.txt -> GAME_HOME/SaveData*.runtime.
 * Compiled to nothing when LCGO_SAVE_EDIT is 0. */
void lcgo_saveedit_run(void);

/* The container on its own, for tools and tests. decode: file text -> JSON
 * (malloc'd, NUL-terminated) and the 5 LZMA property bytes; returns 0 or -1
 * with a reason in err. encode: JSON -> file text (malloc'd, NUL-terminated),
 * reusing the property bytes a decode returned. */
int   lcgo_save_decode(const char *text, size_t n, char **json, size_t *jlen,
                       unsigned char props[5], char *err, size_t errsz);
char *lcgo_save_encode(const char *json, size_t jlen, const unsigned char props[5],
                       size_t *out_len);

/* PC builds only: also print the report to stdout. */
extern int lcgo_saveedit_verbose;

#endif /* LCGO_SAVEEDIT_H */
