/* android_native_unity.c -- the 27 NDK symbols libunity.so imports, for the
 * ZOOKEEPER DX Switch port. Unity is NOT a NativeActivity, so unlike cr3_nx's
 * android_native.c there is no ANativeActivity glue / android_main / AInputQueue
 * here: the engine is driven by the JNI-registered natives (see main.c). We only
 * provide the raw NDK functions libunity calls directly:
 *
 *   ANativeWindow_acquire/_release/_fromSurface/_setBuffersGeometry/
 *                _getWidth/_getHeight/_getFormat      -> libnx NWindow
 *   ALooper_prepare/_acquire/_release/_pollOnce/_wake/_forThread
 *                                                     -> condvar wait/wake
 *   ASensorManager_ , ASensorEventQueue_ , ASensor_   -> "no sensors"
 *
 * IMPORTANT context-ownership note: the engine creates its OWN EGL context from
 * the ANativeWindow (cr3_nx's main.c creates none). The host must NOT create an
 * SDL_GL / EGL context. Use SDL for audio + HID only. Delete the
 * SDL_GL_SetAttribute/SDL_GL_CreateContext/SDL_GL_SwapWindow calls from the
 * earlier main_skeleton.c; the engine calls eglSwapBuffers itself.
 *
 * Needs devkitA64 + libnx (switch.h) + switch-mesa. Not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include <GLES3/gl3.h> /* docked cursor overlay */
#include "util.h"   /* debugPrintf */
#include "config.h" /* screen_width / screen_height, config.framerate */

/* Swap interval the window runs at, from config.txt.
 *   framerate 60 -> 1  (present every 60 Hz vsync)
 *   framerate 30 -> 2  (present every second vsync)
 * Kept in one place because mesa resets the interval behind our back on surface
 * changes, so it has to be re-applied from three call sites and they must all
 * agree -- if one hardcoded 1, `framerate 30` would be undone the next time the
 * surface was touched. See the eglSwapInterval interceptor in unity_imports.c,
 * which clamps the engine's own request to the same value. */
static inline u32 nx_swap_interval(void) {
  return (config.framerate == 30) ? 2u : 1u;
}

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

/* ==========================================================================
 * dock-aware screen state (also read by unity_jni.c's Display getters)
 * ========================================================================== */
static u32 g_w = 1280, g_h = 720;   /* PvZ Fusion: LANDSCAPE, matches Switch panel (was Zookeeper TATE 720x1280) */

/* Render height -> 16:9 landscape pair. Only 720 and 1080 are offered: they are
 * the two the Switch actually presents, and Lara Croft GO picks its UI atlas by
 * aspect ratio (DefinitionConfig::AspectRatio, PORTING §13c) rather than by
 * pixel count, so staying 16:9 keeps that decision correct at either size. */
static void res_to_wh(int res, u32 *w, u32 *h) {
  if (res == 1080) { *w = 1920; *h = 1080; }
  else             { *w = 1280; *h = 720;  }
}

void android_native_update_mode(void){
  /* LATCHED FOR THE SESSION. This runs every frame from the render loop, and
   * g_w/g_h feed the EGL surface -- changing them mid-run would leave the
   * surface and the engine's idea of the screen disagreeing, with nothing
   * recreating either. Decide once. */
  static int latched = 0;
  if (latched) return;
  latched = 1;

  const int docked = (appletGetOperationMode() == AppletOperationMode_Console);
  /* Per-mode setting, resolved against the dock state at launch. config.* is
   * zero until read_config() runs, so an out-of-range or unset value falls back
   * to that mode's native panel rather than being trusted. */
  int res = docked ? config.docked_res : config.handheld_res;
  if (res != 720 && res != 1080) res = docked ? 1080 : 720;

  /* NO HANDHELD CAP. An earlier build capped handheld to 720p, on the theory
   * that a 1920x1080 buffer could not be presented to a 720p panel. That was
   * wrong: the CloverPit port runs 1920x1080 in handheld on this same stack,
   * and its notes say plainly that "the handheld panel is 1280x720, so 1080p
   * was being downscaled by the compositor regardless". The compositor is fine
   * with it.
   *
   * The real cause was GFX_RESERVE_MB (config.h): switch-mesa allocates the
   * swapchain from the process pool we had taken all but 2 MB of, so the first
   * 1920x1080 present failed inside the driver. Fixed there, not here. */

  res_to_wh(res, &g_w, &g_h);
  /* Landscape throughout: keep the engine-reported surface size equal to the
   * real window so the render target matches what we present (no mismatch). */
  screen_width = (int)g_w; screen_height = (int)g_h;
  debugPrintf("[gfx] mode=%s res=%dp -> %ux%u (latched for this session)\n",
              docked ? "docked" : "handheld", res, g_w, g_h);
}

u32 android_native_width(void)  { return g_w; }
u32 android_native_height(void) { return g_h; }

/* ==========================================================================
 * ANativeWindow  ->  libnx NWindow
 * ========================================================================== */
/* fbstub45: pin the displayed region to exactly the dimensions Unity renders
 * into. nwindowSetDimensions may allocate a width-aligned (e.g. 720 -> 768)
 * swapchain buffer; without a matching crop the compositor can scan the extra
 * uninitialized columns, which shows up as the image being "cut off" / garbage
 * on the right edge. Cropping to (0,0,bw,bh) guarantees only the rendered
 * content is presented (stretched to the panel, no cutoff). */
/* PvZ Fusion is landscape, always: the game ships no portrait mode, and the
 * Switch panel is landscape too, so the render maps straight onto it. The
 * inherited TATE path -- a compositor rotation picked by config.portrait, from
 * the VLN reference port -- is gone along with the option that drove it.
 * Nothing rotates anywhere in this port now, so clear the buffer transform
 * explicitly rather than inheriting whatever a previous producer left set. */
/* Last geometry, so it can be re-asserted after mesa builds its swapchain. */
static u32 g_geom_bw = 1280, g_geom_bh = 720;

static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  g_geom_bw = bw; g_geom_bh = bh;
  Result rc = nwindowSetDimensions(w, bw, bh);
  nwindowSetCrop(w, 0, 0, bw, bh);
  nwindowSetTransform(w, 0);                 /* landscape: no rotation */

  /* EXPLICIT SWAP INTERVAL -- this is what was killing 1080p.
   *
   * Left unset, the compositor does not throttle presents at all, so the only
   * thing bounding our submission rate is the render loop. Flood the queue and
   * vi wedges: the process dies INSIDE eglSwapBuffers and never returns, which
   * is exactly the signature every 1080p log has shown (the "swap #1 ..." line
   * prints, the "eglSwapBuffers returned" line never does).
   *
   * Resolution is not the cause, only the trigger. At 1920x1080 the compositor
   * additionally has to downscale to the 720p handheld panel, so it falls
   * behind sooner; at 1280x720 it kept up and the bug stayed hidden. That is
   * why more memory changed nothing -- with the reserve finally working the
   * driver had 291 MB free and the arena was 90% empty, and it still wedged.
   *
   * Interval 1 makes the display server pace us at 60 Hz for free. Taken from
   * the CloverPit port, whose note calls unthrottled presents "exactly the
   * failure mode that has been wedging vi". Interval 2 halves that to 30. */
  nwindowSetSwapInterval(w, nx_swap_interval());

  u32 aw = 0, ah = 0;
  nwindowGetDimensions(w, &aw, &ah);
  debugPrintf("[gfx] window geom: requested %ux%u (rc=0x%x), nwindow reports %ux%u, crop 0,0,%u,%u, swap_interval=%u -> %d fps (landscape, no rotation)\n",
              bw, bh, rc, aw, ah, bw, bh,
              nx_swap_interval(), nx_swap_interval() == 2 ? 30 : 60);
}

/* Keep the presented buffer at the RENDER size so the compositor supersamples.
 *
 * How this is meant to work on Switch: Unity renders into a 1920x1080 buffer,
 * the crop rect selects the whole of it, and the compositor scales that rect
 * onto the layer -- which in handheld is the 720p panel. Rendering above panel
 * resolution and letting the display server downscale IS the supersampling; it
 * costs GPU time and gains sharpness, and CloverPit ships exactly this.
 *
 * What breaks it: mesa rebuilds its swapchain inside eglCreateWindowSurface and
 * resets the window to the panel's native size. Unity creates the surface twice,
 * and the boot log caught the second pass doing it:
 *
 *   nwindow after eglCreateWindowSurface: ... 1920x1080   <- first, correct
 *   nwindow after eglCreateWindowSurface: ... 1280x720    <- second, reset
 *   glViewport 0,0 1920x1080                              <- render size unchanged
 *
 * A 1920x1080 render into a 1280x720 buffer is not scaled, it is CROPPED -- only
 * the top-left corner reaches the panel, which is the "cut off" symptom. So the
 * dimensions have to go back, not just the crop.
 *
 * Cheap on the common path: one struct read, and nothing else unless the window
 * has actually drifted. Safe to call per frame. */
/* Apply the geometry while the producer is DISCONNECTED.
 *
 * nwindowSetDimensions returns LibnxError_AlreadyInitialized (0xf59) once a
 * producer has connected, so it can only be set between surfaces -- which is
 * why re-asserting after eglCreateWindowSurface never worked, no matter how
 * often it was retried:
 *
 *   [gfx] reassert: window was 1280x720, want 1920x1080 -> now 1280x720 (rc=0xf59)
 *
 * Unity creates the surface twice. The first one is built at the size we set at
 * boot; the second is built after mesa has reset the window to the panel's
 * native size, and by then the size is frozen. So force it in the window BEFORE
 * mesa connects, from the eglCreateWindowSurface wrapper. */
void nx_window_force_geom(void) {
  NWindow *w = nwindowGetDefault();
  u32 aw = 0, ah = 0;
  nwindowGetDimensions(w, &aw, &ah);
  const Result rc = nwindowSetDimensions(w, g_geom_bw, g_geom_bh);
  nwindowSetCrop(w, 0, 0, g_geom_bw, g_geom_bh);
  nwindowSetTransform(w, 0u);
  nwindowSetSwapInterval(w, nx_swap_interval());
  u32 nw = 0, nh = 0;
  nwindowGetDimensions(w, &nw, &nh);
  static int nlog = 0;
  if (nlog < 8) {
    nlog++;
    debugPrintf("[gfx] force_geom (pre-surface): was %ux%u -> %ux%u want %ux%u (rc=0x%x)%s\n",
                aw, ah, nw, nh, g_geom_bw, g_geom_bh, rc,
                (nw == g_geom_bw && nh == g_geom_bh) ? "" : "  <-- STILL WRONG");
  }
}

void nx_window_reassert(void) {
  NWindow *w = nwindowGetDefault();
  u32 aw = 0, ah = 0;
  nwindowGetDimensions(w, &aw, &ah);
  if (aw == g_geom_bw && ah == g_geom_bh) return;   /* already right */

  const Result rc = nwindowSetDimensions(w, g_geom_bw, g_geom_bh);
  u32 nw = 0, nh = 0;
  nwindowGetDimensions(w, &nw, &nh);
  /* Crop selects the region the compositor scales onto the layer: the whole
   * buffer, so the full render is presented rather than a corner of it. */
  nwindowSetCrop(w, 0, 0, g_geom_bw, g_geom_bh);
  nwindowSetTransform(w, 0u);
  nwindowSetSwapInterval(w, nx_swap_interval());    /* mesa resets this too */

  static int nlog = 0;
  if (nlog < 8) {
    nlog++;
    debugPrintf("[gfx] reassert: window was %ux%u, want %ux%u -> now %ux%u (rc=0x%x)%s\n",
                aw, ah, g_geom_bw, g_geom_bh, nw, nh, rc,
                (nw == g_geom_bw && nh == g_geom_bh)
                  ? "  supersampling to the panel"
                  : "  <-- SIZE DID NOT STICK: render will be cropped, not scaled");
  }
}

/* What mesa actually built. libnx's NWindow exposes all of this and none of it
 * was being looked at:
 *   slots_configured        allocated buffer slots -> swapchain depth
 *   swap_interval           proves nwindowSetSwapInterval took effect
 *   consumer_running_behind the compositor saying it cannot keep up -- the
 *                           submission-flooding condition itself, which has
 *                           been suspected for several rounds with no evidence
 * Cheap struct reads, no syscalls. */
void nx_window_report(const char *when) {
  NWindow *w = nwindowGetDefault();
  unsigned n = 0;
  for (u64 m = w->slots_configured; m; m >>= 1) n += (unsigned)(m & 1);
  debugPrintf("[gfx] nwindow %s: buffers=%u (cfg=0x%llx req=0x%llx) cur_slot=%d "
              "swap_interval=%u behind=%d %ux%u\n",
              when, n,
              (unsigned long long)w->slots_configured,
              (unsigned long long)w->slots_requested,
              (int)w->cur_slot, (unsigned)w->swap_interval,
              (int)w->consumer_running_behind, w->width, w->height);
}

ANativeWindow *android_native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return android_native_window();               /* one surface == our window */
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_getFormat(ANativeWindow *w){ (void)w; return AWINDOW_FORMAT_RGBA_8888; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  /* The NX window is a FIXED-SIZE display. Resizing the real window to a
   * non-native size (e.g. the game's saved 640x1137 low-res, applied EARLY at
   * startup when Unity knows it from PlayerPrefs) makes mesa build a 640x1137
   * swapchain whose buffers the NX display path never consumes -> the first
   * eglSwapBuffers blocks forever (the boot-2 hang). Android devices without
   * hardware resolution scaling behave exactly like this fix: the resize
   * "succeeds" (returns 0) but readback (getWidth/getHeight, eglQuerySurface)
   * still shows the native size, which is precisely how Unity detects
   * "Hardware resolution scaling not supported" and falls back to its software
   * blit -- the same path that already works when SetResolution happens late
   * at frame 2. So: accept only the native geometry; report success for the
   * rest so Unity's own fallback engages. */
  if (width > 0 && height > 0) {
    if ((u32)width != g_w || (u32)height != g_h) {
      debugPrintf("[gfx] setBuffersGeometry %dx%d REJECTED (fixed-size window stays %ux%u; engine will blit-scale)\n",
                  width, height, g_w, g_h);
      return 0;
    }
    nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  }
  return 0;
}

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, port cr3_nx's fake-fd PollItem layer in here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 0); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. (CR3 imported no ASensorManager; Unity does, so these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* cr3 dead-handler stub: no orientation sensor -> report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID -> Unity input, via nx_pointer.
 *
 * nx_pointer owns every pointing device on the console -- touchscreen, USB
 * mouse, left stick, gyro -- plus the on-screen cursor and its settings file.
 * It hands back device-independent NxpEvents (id, x, y, phase); everything
 * below is the translation from those into the fake Android MotionEvents that
 * nativeInjectEvent expects (unity_input.c), and the B -> Back key mapping,
 * which is a game binding rather than a pointer concern.
 *
 * This replaces the port's original stick-cursor/dot-overlay implementation.
 * ========================================================================== */
#include <stdio.h>
#include "unity_input.h"
#include "nx_pointer.h"

/* Locked stdio from libc_shim.c. nx_pointer writes pointer.cfg from the render
 * thread while the engine's workers are opening bundle files on their own
 * threads; both sides go through these so they never touch newlib's FILE table
 * at the same time. See the note above fopen_fn in nx_pointer.h. */
FILE *nx_fopen_locked(const char *path, const char *mode);
int   nx_fclose_locked(FILE *f);

/* Our own pad, used ONLY for B -> Back. nx_pointer keeps a separate PadState of
 * its own; that is fine, because padUpdate() snapshots HID shared memory into
 * whichever struct you hand it, so each PadState tracks its own press/release
 * edges independently. */
static PadState g_pad;

static void nxp_log_line(const char *msg){ debugPrintf("%s", msg); }

void android_native_input_init(void){
  NxpConfig c;
  memset(&c, 0, sizeof c);

  c.screen_w        = (int)g_w;      /* render space; nxp_set_screen keeps it  */
  c.screen_h        = (int)g_h;      /* current across dock transitions        */
  c.panel_w         = 1280;          /* the touch panel reports in its own     */
  c.panel_h         = 720;           /* 1280x720 space at every resolution     */
  c.data_dir        = GAME_HOME;     /* sdmc:/switch/lcgo                */

  /* Touch fingers take ids 0..7, so the cursor gets 8. This must be set
   * explicitly: nxp_init only substitutes the default for a NEGATIVE cursor_id,
   * and a memset-zeroed config would give the cursor id 0 -- colliding with the
   * first finger. Both stay inside UI_MAX_POINTERS (10). */
  c.cursor_id       = 8;
  c.max_touch_slots = 8;

  c.stick_speed     = 0.0f;          /* 0 => library default, then pointer.cfg */
  c.mouse_sens      = 0.0f;
  c.log             = nxp_log_line;
  c.fopen_fn        = nx_fopen_locked;
  c.fclose_fn       = nx_fclose_locked;

  nxp_init(&c);

  /* Pad for the Back key. nxp_init has already called padConfigureInput. */
  padInitializeDefault(&g_pad);

  /* input_log_fn intentionally left unset: the MotionEvent-getter trace is a
   * debug aid (set it to debugPrintf to re-enable). Leaving it off keeps the
   * touch path log-silent so taps don't stutter on slow SD writes. */
}

/* Flush a pending sensitivity change on the way out. Normally nx_pointer saves
 * by itself 3s after the last adjustment; this catches the case where the
 * player quits inside that window. */
void android_native_input_shutdown(void){
  nxp_save_settings();
}

/* inject signature == recovered nativeInjectEvent: (env,thiz,InputEvent,int)->Z */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

/* ---- live pointer set ----------------------------------------------------
 * Android hands the engine the FULL set of pointers that are currently down on
 * every event; the action word says what happened, and for the multi-pointer
 * variants its high byte says which INDEX in that array it happened to. So we
 * have to keep the set ourselves rather than forwarding events one at a time.
 * Fingers and the cursor coexist here, which is what makes "touch the screen
 * while the cursor is up" behave like real multitouch instead of a fight. */
#define NXG_MAX UI_MAX_POINTERS

static int   g_live_id[NXG_MAX];
static float g_live_x [NXG_MAX];
static float g_live_y [NXG_MAX];
static int   g_live_n = 0;

static int live_find(int id){
  for (int i = 0; i < g_live_n; i++)
    if (g_live_id[i] == id) return i;
  return -1;
}

static void emit(inject_fn inject, void *env, void *thiz, int action){
  if (g_live_n <= 0) return;
  inject(env, thiz,
         unity_motionevent(action, g_live_n, g_live_id, g_live_x, g_live_y), 0);
}

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  /* android_native_update_mode() has already run for this frame, so g_w/g_h are
   * current. Docking swaps the surface between 1280x720 and 1920x1080 and both
   * event coordinates and the overlay live in that space, so tell nx_pointer
   * before it builds this frame's events. Cheap no-op when nothing changed. */
  nxp_set_screen((int)g_w, (int)g_h);

  nxp_update();

  NxpEvent ev[24];
  const int n = nxp_poll(ev, (int)(sizeof ev / sizeof ev[0]));

  /* MOVEs are batched: several pointers can move in one frame, and Android
   * expresses that as ONE action with every pointer's new position, not one
   * event each. A DOWN or UP closes the batch. */
  int move_pending = 0;

  for (int i = 0; i < n; i++){
    const int   id = ev[i].id;
    const float x  = ev[i].x, y = ev[i].y;
    int idx = live_find(id);

    if (ev[i].phase == NXP_MOVE){
      if (idx < 0) continue;                    /* never saw its DOWN */
      g_live_x[idx] = x; g_live_y[idx] = y;
      move_pending = 1;
      continue;
    }

    if (move_pending){
      emit(inject, env, thiz, AMOTION_ACTION_MOVE);
      move_pending = 0;
    }

    if (ev[i].phase == NXP_DOWN){
      if (idx >= 0){                            /* already down -- treat as move */
        g_live_x[idx] = x; g_live_y[idx] = y;
        move_pending = 1;
        continue;
      }
      if (g_live_n >= NXG_MAX) continue;        /* out of slots */
      idx = g_live_n++;
      g_live_id[idx] = id; g_live_x[idx] = x; g_live_y[idx] = y;
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_DOWN
             : (AMOTION_ACTION_POINTER_DOWN | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    }
    else if (ev[i].phase == NXP_UP){
      if (idx < 0) continue;
      g_live_x[idx] = x; g_live_y[idx] = y;
      /* The lifting pointer is still IN the array for its own UP -- that is how
       * getActionIndex() identifies which one left. Remove it afterwards. */
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_UP
             : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
      for (int k = idx + 1; k < g_live_n; k++){
        g_live_id[k-1] = g_live_id[k];
        g_live_x [k-1] = g_live_x [k];
        g_live_y [k-1] = g_live_y [k];
      }
      g_live_n--;
    }
  }

  if (move_pending) emit(inject, env, thiz, AMOTION_ACTION_MOVE);

  /* ---- B -> Android Back, edge-triggered ---- */
  padUpdate(&g_pad);
  const u64 bdown = padGetButtonsDown(&g_pad);
  const u64 bup   = padGetButtonsUp(&g_pad);
  if (bdown & HidNpadButton_B)
    inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (bup & HidNpadButton_B)
    inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
}

/* ==========================================================================
 * Cursor overlay. nx_pointer draws it (built-in arrow, or cursor.png if one is
 * on the SD card) and saves/restores the GL state it touches; the wrapper here
 * supplies the two things it cannot know about from inside the library.
 * Called by the swap wrapper in imports.c, right before eglSwapBuffers.
 * ========================================================================== */
void android_native_draw_cursor(void){
  if (!nxp_cursor_visible()) return;

  /* 1. VAO. Unity leaves one of its own vertex-array objects bound, and under
   *    GLES3 a non-zero VAO forbids client-side vertex arrays -- which is
   *    exactly what the cursor draws with, so glVertexAttribPointer would raise
   *    INVALID_OPERATION and nothing would appear. Binding VAO 0 for the
   *    duration also means every attribute change lands in a scratch VAO
   *    instead of Unity's, so restoring the binding restores it exactly.
   * 2. Viewport. The cursor shader maps render-space pixels straight to NDC, so
   *    it needs the viewport to cover the whole window; the engine may well
   *    have left it set to some intermediate render target. */
  /* 3. Framebuffer. The overlay must land on the backbuffer, but the engine may
   *    still have an offscreen target bound at this point -- in which case the
   *    cursor would be drawn into a texture nobody presents, and, worse, the
   *    binding would leak into the next frame. Save, force 0, restore. */
  GLint prev_vao = 0, prev_fbo = 0, vp[4];
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &prev_fbo);
  glGetIntegerv(GL_VIEWPORT, vp);

  glBindVertexArray(0);
  if (prev_fbo != 0) glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, 0);
  glViewport(0, 0, (GLsizei)g_w, (GLsizei)g_h);

  nxp_draw();

  glViewport(vp[0], vp[1], vp[2], vp[3]);
  if (prev_fbo != 0) glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, (GLuint)prev_fbo);
  glBindVertexArray((GLuint)prev_vao);
}
