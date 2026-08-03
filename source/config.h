/* config.h -- Plants vs Zombies Fusion 3.6.1 Switch wrapper configuration
 * (forked from the Zookeeper DX / CR3 wrapper config.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ============================ MEMORY LAYOUT ==============================
 * These are engine-fitting parameters, not game content -- identical to the
 * Zookeeper DX port because PvZ Fusion is the SAME Unity minor version
 * (2022.3.62) and we apply the SAME 256MB->64MB region-granularity patch
 * (see nx_patch_lcgo.h). Do not change unless you know the allocator math.
 * ======================================================================== */

// The engine + libc++ + il2cpp heap need a generous newlib heap; the rest of
// system memory is handed to the .so loader (see __libnx_initheap).
#define MEMORY_MB 768

// Anonymous-mmap arena. Unity reserves big region-aligned pools by over-mmapping
// then munmapping the unaligned head/tail. We back anonymous mmaps from a
// dedicated, region-aligned arena with a per-page used-bitmap so sub-range
// munmap frees exactly the trimmed pages. Region granularity is 64MB to match
// the libunity patch.
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)    // 64MB region granularity (libunity patched 256MB->64MB, nx_patch_lcgo.h). NOTE: 16MB was tried and CORRUPTED Unity's Dynamic Heap allocator at init (overlapping regions from the over-map/trim pattern -> null-prev free-list crash). 64MB is the known-good floor.
#define MMAP_ARENA_RESERVE  ((size_t)192 * 1024 * 1024)   // heap-backed spill cap (3x64MB). INHERITED TUNING -- the prose history in the reference config referred to values (1024/896/512) that no longer match this constant; treat the number, not the story, as authoritative and re-check on hardware.

// Stack-region overcommit (OC) arena (see libc_shim.c): PROT_NONE reservations
// held in a stack-region window, committed pages backed from a small heap pool.
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)  // 32x64MB cheap PROT_NONE reservation. Was 1536; the window-finder clamps to the largest stack-region hole (min(this, hole)), so raising the cap lets a run use its full hole and spill fewer reservations into the (real-memory) arena.
// Commit-pool: real memory backing touched pages of the OC window. Unity is told it
// has 512 MB (libc_shim.c __sysconf PHYS_PAGES + dalvik.vm.heapsize), and it reserves
// its heaps as big PROT_NONE regions that route here, so this pool must be able to
// back the full 512 MB Unity believes it has -- at 256 MB the scene load exhausted it
// (~270 MB working set) and the next uncommitted page faulted -> hard OOM crash.
// malloc. UPDATE 2: trimming the pool 1280->1024 + arena 1024->512 fixed the malloc
// OOM (malloc 1409 -> game pushed past the null-buffer crash) but 512 starved the
// arena (see above). Final balance within the ~2945 MB (newlib+arena) budget:
// pool 896 + arena 896 + malloc 1153. Each sits ~1.6-1.8x above its known failure
// point (pool live 551 / arena fail 512 / malloc fail 641). If one OOMs again the
// log names which knob to grow.
/* OC commit-pool (touched pages only). NO LONGER INHERITED TUNING -- MEASURED.
 *
 * Was 896 MB, carried over as 1.6x PvZ Fusion's 551 MB working set, with the
 * note that Lara Croft GO "is a far smaller title, so this should be generous" and
 * that the value could only be settled on hardware. It has now been settled:
 * across three boots, including one that reached the title screen and menus,
 * the pool peaked at
 *
 *     218 MB, 218 MB, 240 MB   (of 896)
 *
 * so 896 was ~3.7x the real working set. 512 is 2.1x the observed peak -- still
 * generous for a title whose peak moved only 22 MB between a short boot and a
 * long one -- and returns 384 MB to the newlib heap.
 *
 * That matters because the pool is `memalign(0x1000, OC_POOL_BYTES)` out of the
 * SAME newlib heap the GPU arena reserves from, so this is the memory that
 * funds GPUA_BYTES at 1080p (imports_lcgo_extra.c). If a future title stalls or
 * the "[oc] committed" line ever approaches this cap, raise it -- the log
 * prints pool usage on every commit, so it is directly observable. */
#define OC_POOL_BYTES       ((size_t)512 * 1024 * 1024)

/* Graphics-driver headroom, held back from the newlib heap.
 *
 * __libnx_initheap took everything except the stock 0x200000 (2 MB).
 * switch-mesa and the nouveau/nvidia layer allocate GPU memory from the SAME
 * process pool -- NOT from our GPU arena, which only ever sees what comes
 * through memalign. A single 1920x1080 RGBA swapchain buffer is ~8 MB, so a
 * 2 MB margin means the driver gets whatever happened to be left over.
 *
 * This is what killed 1080p. Our boot log reports
 *     [probe] mem total=3189 MB used=3185 MB free=3 MB
 * and the process then died INSIDE eglSwapBuffers on the first 1920x1080
 * present -- a driver allocation failing in the compositor path, which takes
 * the system down rather than returning an error we could see. The GPU arena
 * being 90% empty at the time was irrelevant: wrong pool.
 *
 *     1920x1080 = 7.9 MB/buffer  (23.7 MB triple-buffered)
 *     1280x720  = 3.5 MB/buffer  (10.5 MB triple-buffered)
 *
 * which is why 720p survived on scraps and 1080p did not.
 *
 * 192 MB covers a triple-buffered 1080p swapchain, the game's render textures,
 * GPU command buffers and texture staging, with margin. Taken straight from the
 * CloverPit port, which runs 1920x1080 in handheld on this exact value -- proof
 * that the compositor downscales a 1080p buffer to the 720p panel quite happily
 * once the driver can actually allocate it.
 *
 * If the game later OOMs on the MANAGED side instead, this is the knob to trade
 * back. Note we also freed 384 MB by right-sizing OC_POOL_BYTES above, so newlib
 * is still ahead of where it was. */
#define GFX_RESERVE_MB      192u

// Overcommit (alias-region) mode: reserve a big *virtual* window (PROT_NONE
// costs only address space) and commit physical pages on demand -- true
// overcommit, matching Android.
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)  // 6 GB virtual reservation window
#define OVERCOMMIT_HEAP_MB  608u                          // newlib malloc + .so load zone

/* ============================ GAME IDENTITY =============================== */

// PvZ Fusion ships the engine as the standard modern Unity trio; libmain.so
// dlopens libunity.so which dlopens libil2cpp.so. (No libcrx/MVGL here -- this
// is a normal IL2CPP game, so main.c loads libmain/libunity/libil2cpp directly
// and these SO_NAME macros are unused, kept only for parity with the base.)
#define SO_NAME      "libunity.so"
#define SO_CPP_NAME  "libil2cpp.so"

// The SD-card folder holding the .nro + the game files.
#define GAME_FOLDER  "lcgo"

/* The Android package this build reports to game code. Real value, from the
 * APK's dex: com.squareenixmontreal.lcgo. Firebase config matching, Google
 * Play Games, the billing client and the game's own Armory plugins all read
 * getPackageName(), so this must not be a placeholder. */
#define GAME_PACKAGE "com.squareenixmontreal.lcgo"

/* Unity's AndroidJavaObject reflection bridge (ReflectionHelper.getMethodID ->
 * java.lang.reflect.Method -> FromReflectedMethod -> jmethodID). Serving it lets
 * managed code actually reach the game's Java plugins; with it off those calls
 * silently no-op, which is how the port behaved before and is a safe fallback if
 * the reflection path misbehaves. */
#define LCGO_JNI_REFLECTION 1

#define CONFIG_NAME "config.txt"
#define LOG_NAME    game_path("debug.log")

// Returned for getenv("HOME")/getpwuid()->pw_dir. Point it at the (writable)
// game data root instead of letting the engine deref a NULL passwd.
/* ---- game root -----------------------------------------------------------
 * Resolved at runtime by game_root.c: the folder holding the .nro, else any
 * folder under sdmc:/switch that contains an install, else sdmc:/switch/
 * GAME_FOLDER. GAME_FOLDER is now only the fallback name, not a requirement.
 *
 * DATA_ROOT is a char array rather than a literal, so `"%s", DATA_ROOT` works
 * unchanged but `DATA_ROOT "/sub"` no longer compiles -- use game_path("sub"),
 * which returns a rotating static buffer (safe to use twice in one call). */
extern char g_game_root[512];
const char *game_path(const char *relative);
const char *game_root(void);
void game_root_resolve(int argc, char *argv[]);

#define GAME_HOME   ((const char *)g_game_root)
#define DATA_ROOT   GAME_HOME

// flip to 1 (and rebuild) to get file logging (debug.log) for on-hardware debugging
#define DEBUG_LOG 0   /* ON: this port has never booted on hardware yet, and
                       * debug.log is the only diagnostic you get. Every offset
                       * in nx_patch_lcgo.h is applied verify-first and logs a
                       * loud [patch]/[fmod]/[gc] line on mismatch -- that log
                       * IS the bring-up feedback loop. Turn it to 0 for normal
                       * play once the game runs; it costs SD writes. */

/* High-volume per-operation traces. These were invaluable for the black-screen /
 * boot-hang triage but are catastrophic for load speed once the game runs: every
 * every archive read/lseek and most mprot calls fflush two lines to the SD card, so
 * a synchronous scene load (~1700 bundle reads) takes minutes instead of seconds
 * and looks like a hang. Keep them OFF for normal play; flip to 1 to re-trace. */
#define TRACE_BUNDLE_IO 0   /* per-read/lseek trace of the main archive */
#define TRACE_MPROT     0   /* per-mprotect commit trace */

extern int screen_width;
extern int screen_height;

/* ----------------------------- Language ----------------------------------
 * Lara Croft GO shipped a broad language set. The game reads locale through
 * AndroidJavaClass -> our jni_fake getLanguage()/java.util.Locale, so these
 * indices map to the 2-letter token lang_code() returns. 0 follows the Switch
 * system language. If this build expects different tokens, edit lang_code()
 * in jni_fake.c -- see PORTING.md section 5.                                */
#define LANG_AUTO 0
#define LANG_EN   1
#define LANG_FR   2
#define LANG_DE   3
#define LANG_ES   4
#define LANG_IT   5
#define LANG_PT   6
#define LANG_RU   7
#define LANG_JA   8
#define LANG_KO   9
#define LANG_ZH   10

/* No orientation field: the port renders landscape only, matching Lara Croft GO's
 * Android build. CONFIRM this against your APK's AndroidManifest.xml
 * (android:screenOrientation on the activity) before you build -- a portrait
 * title rendered landscape gives a black screen that looks like a render bug,
 * not a layout bug. The retired `portrait` knob is dropped from any existing
 * config.txt on next launch. */
typedef struct {
  int handheld_res; /* render height in handheld: 720 or 1080 */
  int docked_res;   /* render height when docked:  720 or 1080 */
  int framerate;    /* 30 or 60 */
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);


/* ---- app identity reported to game code -----------------------------------
 * Surfaced as PackageInfo.versionName / versionCode and hence
 * UnityEngine.Application.version.
 *
 * WHY THIS IS NOT "1.0.0": Lara Croft GO checks its own version against a remote
 * minimum. Deca.RemoteVersioning.RemoteVersioningConfig::ValidateAppVersion
 * (il2cpp 0x1632010) does, in effect:
 *
 *     Version.TryParse(Application.version, out app);
 *     if (app < RecommendedVersion) shouldUpdate = true;
 *     if (app < MinVersion)         mustUpdate  = true;
 *
 * and TitleScreenViewController raises the "LARA CROFT GO UPDATED! Your game
 * version is out of date." popup on the result. With the old placeholder
 * "1.0.0" that comparison always failed, so the popup reappeared every time
 * CONTINUE returned to the title screen.
 *
 * APP_VERSION_NAME below is the REAL versionName, dumped from the base APK.
 * Reporting the truth is preferable to an override: the game runs fine on a
 * phone at this version, and anything else that reads app identity (analytics,
 * crash keys) then sees something coherent.
 *
 * If you ever need to re-derive it:
 *     aapt dump badging base.apk | grep -E "versionName|versionCode"
 * It is NOT recoverable from classes*.dex -- every BuildConfig.VERSION_NAME in
 * there belongs to a third-party library -- the dex string pool for THIS game
 * yields only SDK versions (Firebase, play-services, and similar). Do not pick
 * a plausible-looking triple out of that pool: the pool is sorted
 * lexicographically, so unrelated versions sit next to each other by sort order,
 * not by association.
 *
 * RESIDUAL RISK, worth knowing if the popup ever comes back. The min and
 * recommended versions come from Firebase Remote Config, which is stubbed in
 * this port -- so the game compares against whatever defaults are baked into
 * the shipped RemoteVersioningConfig asset, NOT against what a real phone gets
 * from the live service. Those are normally <= the shipping version, but if the
 * baked default happens to exceed it the popup will persist even with a
 * truthful version. In that case set APP_VERSION_NAME to "99.0.0" as a
 * deliberate override -- this port can never update itself, so "already
 * current" is a defensible answer. Debug.LogWarning is not routed to debug.log,
 * so the thresholds cannot be read from a boot log to check in advance.
 *
 * !! UNVERIFIED FOR LARA CROFT GO !!
 * The Deus Ex GO port carried "2.5.6", dumped from ITS base APK. That value is
 * meaningless here and has been replaced with a placeholder. The real version
 * lives in the APK's AndroidManifest.xml, which was not available when this
 * fork was made -- it cannot be recovered from libunity/libil2cpp/classes*.dex
 * (the only version-like strings in those are bundled SDK versions: Firebase,
 * play-services, etc).
 *
 * To set it correctly, from your own APK:
 *     aapt dump badging base.apk | head -1
 * and paste versionName / versionCode below.
 *
 * WHY IT MATTERS: the game's Armory layer compares this against a server-side
 * "minimum supported version". A version string that parses but reads as very
 * old can trigger a forced-update wall; one that is absurdly new is usually the
 * safer failure. Both jni_fake.c paths that serve it (PackageInfo.versionName
 * and the *VersionName* getter) return this constant verbatim.
 * -------------------------------------------------------------------------- */
#define APP_VERSION_NAME "9.9.9"   /* PLACEHOLDER -- set from your APK (see above) */
#define APP_VERSION_CODE 999999    /* PLACEHOLDER -- nothing observed reads it     */

#endif
