/* lcgo_entitlements.c -- restore DLC the player already owns.
 *
 * Lara Croft GO's paid items are Google Play IAPs. There is no Play billing on
 * this console, so GoogleShop can never initialise here and the player's Play
 * receipts cannot be presented to anything. The entitlement check is not being
 * defeated -- it is UNREACHABLE on this platform.
 *
 * This reads <data_root>/lcgo_purchases.txt and, for each item the player has
 * uncommented, makes the game see that item as owned. An item left commented
 * out does nothing at all: the file is the player's statement of what they own,
 * and the default state is owning nothing.
 *
 * ---------------------------------------------------------------------------
 * WHY A GETTER HOOK AND NOT A SAVE EDIT
 * ---------------------------------------------------------------------------
 * We intercept PlayerPrefsEx.GetBool and answer the outfit-lock query, rather
 * than writing entitlement flags into the save. Consequences:
 *
 *   - NOTHING IS PERSISTED. Delete lcgo_purchases.txt and the game is exactly
 *     as it was, with no residue.
 *   - This game has CLOUD SAVES (GameStructure.m_IsCloudSaveEnabled, and an
 *     ICloudComponent m_Persistence that syncs to Square Enix). A hook that
 *     wrote entitlement state into the save could push that state upstream to
 *     the player's real account. A read-side hook cannot: the save on disk is
 *     never modified, so there is nothing to sync.
 *
 * That second point is the reason this file exists in this shape rather than as
 * a save patcher, which would have been simpler to write.
 *
 * ---------------------------------------------------------------------------
 * DERIVATION (all from THIS build's global-metadata.dat and libil2cpp.so)
 * ---------------------------------------------------------------------------
 *   key template   "{0}_IsOutfitLocked"        <- verbatim in metadata
 *                  "CurrentOutfitIndex"
 *   accessors      PlayerPrefsEx.GetBool(string)      RVA 0xF8376C
 *                  PlayerPrefsEx.SetBool(string,bool) RVA 0xF83760
 *   GetBool body   str x30,[sp,#-0x10]!  (0xf81f0ffe)
 *                  mov x1,xzr
 *                  bl  PlayerPrefs.GetInt (0x1F00984)
 *                  cmp w0,#1 ; cset w0,eq ; ret
 *   content        OutfitDesc (dump.cs:157894) holds m_IsLocked, m_ModelName
 *                  and a direct Material reference. A serialized Material can
 *                  only reference an asset that was in the build, so the outfit
 *                  models are already in the local bundles -- nothing is
 *                  downloaded, they are merely flagged locked.
 *
 * The hint pack ("Complete Walkthrough",
 * com.squareenix.laracroftgo.completewalkthrough) is a SEPARATE product whose
 * ownership lives in the shop Inventory, not in PlayerPrefs -- see the note in
 * nx_patch_lcgo.h. LCGO_ENT_TRACE logs every key the game asks for so the hint
 * query can be identified on hardware instead of guessed at here.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "nx_patch_lcgo.h"

#if LCGO_HAVE_ENTITLEMENTS

extern so_module il2cpp_mod;   /* defined in main.c */

/* PlayerPrefsEx.GetBool(string key) -> bool. il2cpp static method: the trailing
 * MethodInfo* is present but unused by us. */
typedef uint8_t (*ppx_getbool_fn)(void *key, void *method);

static ppx_getbool_fn g_ppx_getbool_orig;   /* -> the BODY, i.e. entry + 4    */

static int g_own_outfits;      /* uncommented in lcgo_purchases.txt           */
static int g_own_hints;
static int g_ent_ready;

#define LCGO_ENT_FILE "lcgo_purchases.txt"

/* ---- Il2CppString -> C, ASCII-only (keys are ASCII) ---------------------- */
static void ent_str(void *s, char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (!s) return;
  const int32_t n = *(const int32_t *)((const char *)s + LCGO_STR_LEN_OFF);
  const uint16_t *c = (const uint16_t *)((const char *)s + LCGO_STR_CHARS_OFF);
  if (n < 0 || n > 4096) return;
  size_t o = 0;
  for (int32_t i = 0; i < n && o + 1 < cap; i++)
    out[o++] = (c[i] >= 0x20 && c[i] < 0x7F) ? (char)c[i] : '?';
  out[o] = 0;
}

static int ends_with(const char *s, const char *suffix) {
  const size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && !strcmp(s + ls - lx, suffix);
}

/* ---- the hook ------------------------------------------------------------ */
static uint8_t ent_getbool(void *key, void *method) {
  char k[128];
  ent_str(key, k, sizeof k);

#if LCGO_ENT_TRACE
  /* Log every distinct key once. This is how the hint query gets identified:
   * open the shop/hints screen and read debug.log. Capped so a per-frame query
   * cannot flood the card. */
  {
    static char seen[24][128];   /* same width as k, so no truncation */
    static int  nseen = 0;
    int known = 0;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], k)) { known = 1; break; }
    if (!known && nseen < 24) {
      snprintf(seen[nseen], sizeof seen[0], "%s", k);
      nseen++;
      debugPrintf("[ent] PlayerPrefsEx.GetBool(\"%s\")\n", k);
    }
  }
#endif

  /* Outfit lock: the game asks "is this outfit locked?". If the player owns the
   * pack, the truthful answer on this platform is no. */
  if (g_own_outfits && ends_with(k, LCGO_OUTFIT_LOCK_SUFFIX)) {
    static int logged = 0;
    if (logged < 8) {
      logged++;
      debugPrintf("[ent] \"%s\" -> unlocked (Outfit Pack owned)\n", k);
    }
    return 0;                       /* not locked */
  }

  if (!g_ppx_getbool_orig) return 0;   /* cannot happen once installed        */
  return g_ppx_getbool_orig(key, method);
}

/* ---- config file --------------------------------------------------------- */
static void ent_write_default(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("[ent] could not create %s\n", path); return; }
  fprintf(f,
    "# Lara Croft GO -- entitlements you already own.\n"
    "#\n"
    "# There is no Google Play billing on this console, so the game cannot ask\n"
    "# Google what you have bought, and your Play receipts cannot be shown to\n"
    "# anything here. This file is your own statement of what you own.\n"
    "#\n"
    "# Uncomment a line (remove the leading #) to restore that item. Anything\n"
    "# left commented out stays locked -- the default is owning nothing.\n"
    "#\n"
    "# Nothing here is written to your save. These lines only change what the\n"
    "# game is told when it asks; delete this file and the game is exactly as\n"
    "# it was. That matters because this game has cloud saves.\n"
    "\n"
    "# Square Enix Outfit Pack\n"
    "#outfits\n"
    "\n"
    "# Complete Walkthrough (hints)\n"
    "# NOTE: not yet working. Hint ownership is stored in the shop inventory\n"
    "# rather than in the same place as the outfits, so this line currently\n"
    "# only enables logging that will identify the right hook. If you own it,\n"
    "# uncomment it and send debug.log -- the answer will be in the [ent] lines.\n"
    "#hints\n");
  fclose(f);
  debugPrintf("[ent] wrote default %s\n", path);
}

static void ent_read(void) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, LCGO_ENT_FILE);

  FILE *f = fopen(path, "r");
  if (!f) { ent_write_default(path); return; }

  char line[256];
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if      (!strcmp(p, "outfits")) g_own_outfits = 1;
    else if (!strcmp(p, "hints"))   g_own_hints   = 1;
    else debugPrintf("[ent] ignoring unknown entry \"%s\"\n", p);
  }
  fclose(f);

  debugPrintf("[ent] %s: outfits=%s hints=%s\n", LCGO_ENT_FILE,
              g_own_outfits ? "OWNED" : "not owned",
              g_own_hints   ? "OWNED" : "not owned");
  if (g_own_hints)
    debugPrintf("[ent] NOTE: hints are not restorable yet -- see the note in "
                "%s. The [ent] key trace below is what will identify the "
                "correct hook.\n", LCGO_ENT_FILE);
}

/* ---- install ------------------------------------------------------------- */
void lcgo_entitlements_init(void) {
  ent_read();

  /* Nothing owned and no trace requested: do not touch the game at all. */
  if (!g_own_outfits && !g_own_hints && !LCGO_ENT_TRACE) {
    debugPrintf("[ent] nothing owned -- hook not installed\n");
    return;
  }

  const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  const uint32_t w = *(volatile uint32_t *)(ib + LCGO_PPX_GETBOOL_RVA);
  if (w != LCGO_WORD_PPX_GETBOOL) {
    debugPrintf("[ent] SKIP: PlayerPrefsEx.GetBool @il2cpp+0x%x = 0x%08x, "
                "expected 0x%08x -- offset wrong for this libil2cpp; "
                "entitlements NOT applied\n",
                (unsigned)LCGO_PPX_GETBOOL_RVA, w, (unsigned)LCGO_WORD_PPX_GETBOOL);
    return;
  }

  /* SPLICE WITH A RELOCATED PROLOGUE.
   *
   * so_util has no trampoline helper -- only so_patch_code -- so the pieces are
   * explicit. The subtlety that matters: an absolute branch needs 16 bytes
   * (ldr x16,#8 / br x16 / .quad target), which covers entry..entry+15. The
   * whole 6-instruction function is only 24 bytes, so patching the entry
   * DESTROYS the body. Calling "entry + 4" would land inside our own stub and
   * execute the target pointer as code.
   *
   * So we relocate instead of chaining: copy the four instructions that follow
   * the overwritten prologue into a local thunk, append a return, and call
   * THAT. None of them is PC-relative except the `bl PlayerPrefs.GetInt`, which
   * we re-encode for the thunk's own address.
   *
   * Original (24 bytes):
   *   +0x00  str  x30,[sp,#-0x10]!     <- overwritten by the branch
   *   +0x04  mov  x1, xzr
   *   +0x08  bl   PlayerPrefs.GetInt   <- PC-relative, must be re-encoded
   *   +0x0c  cmp  w0, #1
   *   +0x10  cset w0, eq
   *   +0x14  ldr  x30,[sp],#0x10 ; ret
   */
  {
    static uint32_t thunk[12] __attribute__((aligned(16)));
    const uintptr_t entry = ib + LCGO_PPX_GETBOOL_RVA;
    const uint64_t  getint = (uint64_t)(ib + 0x1f00984); /* PlayerPrefs.GetInt */

    /* Rebuild the body at our own address.
     *
     * !! THE `bl` RE-ENCODE DOES NOT WORK AND HAS BEEN REPLACED. !!
     * The first hardware run failed with
     *     [ent] SKIP: PlayerPrefs.GetInt out of bl range (-88651745156)
     * because libil2cpp maps at 0x5d0bd11000 while this .nro's static data is
     * ~82.6 GB away. ARM64 `bl` reaches +/-128 MB, so it is 661x short -- a
     * static thunk in our own image can NEVER bl into the game's mapping.
     *
     * So call the target ABSOLUTELY instead: materialise the 64-bit address
     * into x16 with `ldr x16,[pc,#N]` and `blr x16`. No range limit, and x16 is
     * the architectural intra-procedure scratch register, so clobbering it
     * across a call is exactly what it is for. The literal is stored after the
     * `ret` so it is never executed.
     */
    thunk[0] = 0xf81f0ffeu;                        /* str  x30,[sp,#-0x10]!    */
    thunk[1] = 0xaa1f03e1u;                        /* mov  x1, xzr             */
    thunk[2] = 0x580000d0u;                        /* ldr  x16, #24 -> thunk[8]*/
    thunk[3] = 0xd63f0200u;                        /* blr  x16                 */
    thunk[4] = 0x7100041fu;                        /* cmp  w0, #1              */
    thunk[5] = 0x1a9f17e0u;                        /* cset w0, eq              */
    thunk[6] = 0xf84107feu;                        /* ldr  x30,[sp],#0x10      */
    thunk[7] = 0xd65f03c0u;                        /* ret                      */
    memcpy(&thunk[8], &getint, sizeof getint);     /* .quad PlayerPrefs.GetInt */

    g_ppx_getbool_orig = (ppx_getbool_fn)(void *)thunk;

    /* Now it is safe to overwrite the original entry. */
    uint8_t stub[16];
    const uint32_t br_abs[2] = { 0x58000050u /* ldr x16,#8 */,
                                 0xd61f0200u /* br  x16    */ };
    const uint64_t target = (uint64_t)(uintptr_t)&ent_getbool;
    memcpy(stub, br_abs, sizeof br_abs);
    memcpy(stub + 8, &target, sizeof target);
    if (so_patch_code((void *)entry, stub, sizeof stub) != 0) {
      debugPrintf("[ent] SKIP: so_patch_code failed on PlayerPrefsEx.GetBool\n");
      g_ppx_getbool_orig = NULL;
      return;
    }
  }

  g_ent_ready = 1;
  debugPrintf("[ent] hook armed on PlayerPrefsEx.GetBool @il2cpp+0x%x "
              "(outfits=%s, trace=%d)\n",
              (unsigned)LCGO_PPX_GETBOOL_RVA,
              g_own_outfits ? "on" : "off", (int)LCGO_ENT_TRACE);
}

#else  /* !LCGO_HAVE_ENTITLEMENTS */
void lcgo_entitlements_init(void) { }
#endif
