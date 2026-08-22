/* main.c -- ZOOKEEPER DX Switch wrapper entry point.
 *
 * Unity 2022.3 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives the
 * lifecycle the Java UnityPlayer normally runs (initJni -> recreate GFX state ->
 * render loop), calling the native entry points recovered from libunity.so's
 * JNI_OnLoad (see unity_entrypoints.h). The engine owns its own EGL/GLES3 context
 * created from android_native_window(); SDL is audio/HID only.
 *
 * Heap + syscall scaffolding adapted from cr3_nx's main.c (MIT).
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "asset_pack.h"
#include "nx_patch_lcgo.h"      /* Lara Croft GO: derived libunity patch + phase flags */
#include "unity_sendmessage.h"  /* Java->Unity callback channel */
#include "nx_nre_trace.h"       /* managed stack trace on NullReferenceException */
#include "nx_fonts.h"           /* console shared fonts -> /system/fonts */
#include <dirent.h>
#include <strings.h>  /* strncasecmp */
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "diag.h"

#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"
#define LIB_FB_APP          "libFirebaseCppApp-11_9_0.so"
#define LIB_FB_ANALYTICS    "libFirebaseCppAnalytics.so"
#define LIB_FB_MESSAGING    "libFirebaseCppMessaging.so"
#define LIB_FB_REMOTECONFIG "libFirebaseCppRemoteConfig.so"

void unity_environment_init(const char *data_root);   /* unity_glue.c */
int  nx_join_asset_splits(void);                      /* asset_splits.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena. In overcommit mode (g_overcommit) this is a big *virtual* window in
 * the alias region: Unity's PROT_NONE pool reservations cost only address space
 * and physical pages are committed on demand via svcMapPhysicalMemory. In the
 * fallback path it's a fully heap-backed 256MB-aligned slab (the Switch has no
 * native overcommit). Consumed by mmap_fake/munmap_fake (libc_shim.c). */
extern void *__real_memalign(size_t align, size_t size); /* bypass the GPU-arena wrapper */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;          /* 1 = alias-region on-demand commit */
u64    g_alias_base = 0, g_alias_size = 0;
/* captured in __libnx_initheap for logging from main() (log file isn't open yet) */
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
/* graphics-reserve outcome, captured in __libnx_initheap, logged from main() */
unsigned g_gfx_free_mb = 0, g_gfx_heap_mb = 0;
int g_gfx_override = 0, g_gfx_base_moved = 0;
/* granular setup diagnostics so a failed gate tells us WHICH step bailed */
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
/* stack-region overcommit arena armer (libc_shim.c) */
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;   /* system resource size (0 => svcMapPhysicalMemory unusable) */

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Replacement icall for UnityEngine.Application::get_internetReachability.
 * Returns NetworkReachability.NotReachable (0). See the frame-0 install site
 * for why (unblocks FirebaseManager.IsGetMessage / the boot coroutine). The
 * il2cpp icall ABI for this static getter is "int32_t func(MethodInfo*)"; we
 * ignore the hidden arg and just report no network. */
/* Screen/Display size for the managed accessors -- see LCGO_SCREEN_HOOK_LIST. */
static int32_t nx_screen_width(void)  { return (int32_t)(screen_width  > 0 ? screen_width  : 1280); }
static int32_t nx_screen_height(void) { return (int32_t)(screen_height > 0 ? screen_height : 720); }

static int32_t nx_internet_reachability(void) { return 0; }  /* NetworkReachability.NotReachable -- installed via LCGO_TIME_HOOK_LIST */

/* Replacement for Common.FirebaseManager.IsGetMessage (instance method, returns
 * bool). The FirebaseLoading boot state spins on this; force it complete and log
 * once so the run tells us whether state 5 is even reached. */
static int32_t nx_is_get_message(void) {
  static int once = 0;
  if (!once) { once = 1; debugPrintf("[hook] FirebaseManager.IsGetMessage reached -> forced 1\n"); }
  return 1;
}

/* Unity's native time base is frozen in our environment (it never advances the
 * engine clock -- likely it expects Android Choreographer frame timestamps we
 * don't deliver). Every managed Time.* accessor therefore reads a frozen value:
 * deltaTime==0, time/realtimeSinceStartup constant. That freezes all time-based
 * game logic -- DOTween (the boot logo fade), WaitForSeconds, etc. -- which is
 * what holds the black screen (the fade never completes, so boot never starts).
 * Work around it by driving our own monotonic frame clock and redirecting the
 * managed Time accessors to it. nx_time_tick() runs once per render-loop frame. */
static volatile float  g_unity_dt   = 1.0f / 60.0f;
static volatile double g_unity_time = 0.0;
static volatile uint32_t g_frame_count = 0;   /* Time.frameCount source */
static uint64_t g_time_prev_ns  = 0;
static uint64_t g_time_start_ns = 0;
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void nx_time_tick(void) {
  uint64_t now = nx_now_ns();
  g_frame_count++;                    /* advance Time.frameCount once per frame */
  if (!g_time_start_ns) g_time_start_ns = now;
  if (g_time_prev_ns) {
    double dt = (double)(now - g_time_prev_ns) / 1e9;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;            /* clamp, mirrors Unity maximumDeltaTime */
    g_unity_dt = (float)dt;
    g_unity_time += dt;

    /* Report the render loop's REAL cadence.
     *
     * deltaTime here is a MEASUREMENT, not a setting: it is the wall time
     * between iterations of the nativeRender loop, so it reports whatever rate
     * the loop is achieving rather than imposing one. That makes it the single
     * most useful number for a "why is this 30 fps" question -- it separates
     * "the loop is running at 30" from "the loop runs at 60 but the game's
     * logic advances at 30".
     *
     * Printed once per 120 frames, alongside the min/max seen in that window so
     * a steady 33 ms cap is distinguishable from a 16 ms average with stalls. */
    static uint64_t win_start = 0; static unsigned win_n = 0;
    static double win_min = 1e9, win_max = 0.0;
    if (!win_start) win_start = now;
    if (dt < win_min) win_min = dt;
    if (dt > win_max) win_max = dt;
    if (++win_n >= 120) {
      const double secs = (double)(now - win_start) / 1e9;
      if (secs > 0.0)
        debugPrintf("[fps] render loop: %u frames in %d ms = %d fps "
                    "(dt avg %d.%02d ms, min %d.%02d, max %d.%02d) target %d\n",
                    win_n, (int)(secs * 1000.0), (int)((double)win_n / secs),
                    (int)(secs * 1000.0 / win_n), (int)(secs * 100000.0 / win_n) % 100,
                    (int)(win_min * 1000.0), (int)(win_min * 100000.0) % 100,
                    (int)(win_max * 1000.0), (int)(win_max * 100000.0) % 100,
                    config.framerate);
      win_start = now; win_n = 0; win_min = 1e9; win_max = 0.0;
    }
  }
  g_time_prev_ns = now;
}
static float nx_delta_time(void) { return g_unity_dt; }
static float nx_time_f(void)     { return (float)g_unity_time; }
static int   nx_frame_count(void){ return (int)g_frame_count; }
uint32_t     port_frame_count(void){ return g_frame_count; } /* audio pump warmup gate */
static float nx_realtime_since_startup(void) {
  uint64_t now = nx_now_ns();
  if (!g_time_start_ns) g_time_start_ns = now;
  return (float)((double)(now - g_time_start_ns) / 1e9);
}

/* fbstub42: TimeManager::Update entry hook. The Switch port's player loop drives
 * Update with a frozen vsync timestamp as newTime, so deltaTime collapses to the
 * 1e-5 floor and every native time reader (the PreloadManager included) starves,
 * which is what wedges async scene loading (the resident-scene black screen). We
 * redirect Update's entry (libunity 0x4a171c) here, replay its tiny prologue
 * (frameCount++, aux counter++, pause check), then re-enter its body (0x4a1740 --
 * frameless, re-reads everything from x0) with a wall-clock newTime, so Update
 * derives all deltaTime variants and m_Time correctly. All offsets are DERIVED
 * FOR THIS BUILD -- see nx_patch_lcgo.h for the signature evidence. */
static void   (*g_unity_update_body)(void *, double) = NULL; /* LCGO_OFF_TIMEMGR_UPDATE_BODY */
static void     *g_tm = NULL;                       /* captured TimeManager instance */
static void   *(*g_get_time_manager)(void) = NULL;  /* LCGO_OFF_GET_TIME_MANAGER (subsystem 7) */
static uint64_t  g_clk_base_ns = 0;
static volatile uint64_t g_last_main_tick_ns = 0;
static Mutex     g_clock_lock;                       /* main-hook vs clock-thread */
static Thread    g_clock_thr;
#define CLOCK_STALL_NS 100000000ULL                  /* 100ms main silence => stalled */
/* Re-run Update's body with a wall-clock newTime so deltaTime/m_Time advance even
 * while UnityMain is parked in a synchronous scene-load (the frame-0 async hang). */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall    = (double)(now - g_clk_base_ns) / 1e9;
  double sref    = *(volatile double *)((char *)tm + 0xe8);   /* m_StartupRef */
  double newTime = sref + wall;
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;
  { static int once = 0; if (!once) { once = 1; debugPrintf("[time] Update hook first fire (tm=%p)\n", tm); } }
  g_last_main_tick_ns = nx_now_ns();                          /* main thread is live */
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;            /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;            /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return; /* paused -> early return  */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);                              /* ~8ms keep-alive */
    /* Fetch the TimeManager singleton directly -- the entry hook may never fire
     * during a boot-coroutine load (the player loop isn't ticking Update yet), so
     * g_tm can stay NULL. GetTimeManager() returns it once the subsystem exists;
     * until then it's NULL and we skip. */
    void *tm = g_get_time_manager ? g_get_time_manager() : g_tm;
    { static void *seen = NULL; if (tm && tm != seen) { seen = tm; debugPrintf("[time] clock thread got TimeManager=%p (driving native clock)\n", tm); } }
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {                       /* only while main is silent */
      nx_clock_tick(tm);
      { static unsigned _n = 0; if ((_n++ & 0x3f) == 0) debugPrintf("[time] clock thread driving Update (main stalled)\n"); }
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  /* Refuse to splice anything while the 40f1 offsets are undetermined: these
   * three writes are NOT verify-first, so a stale offset corrupts rather than
   * degrades. Fill LCGO_OFF_TIMEMGR_* in nx_patch_lcgo.h to arm this. */
  if (!LCGO_OFF_TIMEMGR_UPDATE_ENTRY || !LCGO_OFF_TIMEMGR_UPDATE_BODY ||
      !LCGO_OFF_GET_TIME_MANAGER) {
    debugPrintf("[boot] time-fix SKIPPED: TimeManager offsets not derived for this "
                "libunity (see nx_patch_lcgo.h)\n");
    return;
  }
  /* Verify-first. The entry splice is a raw 16-byte overwrite, so a stale offset
   * would corrupt rather than degrade; check both landmarks before touching
   * anything. Update's prologue opens `ldr x8,[x0,#0xc8]`, GetTimeManager opens
   * `mov w0,#7` (subsystem index). */
  {
    uint32_t w_upd = *(volatile uint32_t *)(ub + LCGO_OFF_TIMEMGR_UPDATE_ENTRY);
    uint32_t w_gtm = *(volatile uint32_t *)(ub + LCGO_OFF_GET_TIME_MANAGER);
    if (w_upd != LCGO_WORD_TIMEMGR_STOCK || w_gtm != LCGO_WORD_GETTIMEMGR_STOCK) {
      debugPrintf("[time] SKIP time-fix: Update@+0x%x=0x%08x (want 0x%08x), "
                  "GetTimeManager@+0x%x=0x%08x (want 0x%08x) -- libunity differs "
                  "from expected 2022.3.40f1\n",
                  LCGO_OFF_TIMEMGR_UPDATE_ENTRY, w_upd, LCGO_WORD_TIMEMGR_STOCK,
                  LCGO_OFF_GET_TIME_MANAGER, w_gtm, LCGO_WORD_GETTIMEMGR_STOCK);
      return;
    }
  }
  g_unity_update_body = (void (*)(void *, double))(ub + LCGO_OFF_TIMEMGR_UPDATE_BODY);
  g_get_time_manager  = (void *(*)(void))(ub + LCGO_OFF_GET_TIME_MANAGER);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  so_patch_code((void *)(ub + LCGO_OFF_TIMEMGR_UPDATE_ENTRY), stub, sizeof stub);
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+0x%x "
              "+ clock thread (newTime <- startupRef + wallclock)\n",
              LCGO_OFF_TIMEMGR_UPDATE_ENTRY);
}

/* (The reference port carried an il2cpp "finish-flag" probe here that walked a
 * PvZ-specific boot state machine from a hardcoded il2cpp offset. That chain has
 * no counterpart in this game, so it has been removed rather than left as a
 * stale offset behind a disabled flag.) */


/* Measured for THIS build: libmain 0.05M + libunity 18.2M + libil2cpp 50.1M
 * = 71M mapped (1MB-aligned). 160M leaves headroom for relocated segments. */
#define SO_REGION_BYTES (160u * 1024 * 1024)

/* Reserve the virtual arena window at the TOP of the alias region (deep in the
 * 64GB region, where libnx never allocates) after verifying it is fully unmapped.
 * No physical backing yet -- pages are committed on demand. */
static void *overcommit_reserve_window(size_t size) {
  size = (size + MMAP_ARENA_ALIGN - 1) & ~(MMAP_ARENA_ALIGN - 1);
  if (!g_alias_base || g_alias_size < size + MMAP_ARENA_ALIGN) return NULL;
  u64 top = g_alias_base + g_alias_size;
  u64 win = (top - size) & ~(MMAP_ARENA_ALIGN - 1);
  u64 a = win;
  while (a < win + size) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return NULL;
    if (mi.type != MemType_Unmapped) return NULL;   /* collision -> bail */
    a = mi.addr + mi.size;
  }
  return (void *)win;
}

/* virtmemFindStack refuses large windows even when the stack region has room, so
 * scan the region directly via svcQueryMemory for the largest 256MB-aligned
 * unmapped hole (and log the whole map for diagnosis). svcMapMemory only aliases
 * into the stack region, so the OC window must live here. */
static void  *g_oc_win2    = NULL;   /* second-largest stack hole (OC window 2) */
static size_t g_oc_win2_sz = 0;
extern int oc_arena_add_window(void *window, size_t window_bytes);
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0, sec_a = 0, sec_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;                              /* no-progress guard */
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { sec_l = best_l; sec_a = best_a; best_l = he - hs; best_a = hs; }
        else if (he - hs > sec_l) { sec_l = he - hs; sec_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  if (sec_a) {   /* stash the runner-up for OC window 2 */
    u64 a2 = (sec_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
    if (a2 < sec_a + sec_l) {
      u64 v2 = ((sec_a + sec_l) - a2) & ~(MMAP_ARENA_ALIGN - 1);
      if (v2 > want) v2 = want;
      if (v2) { g_oc_win2 = (void *)a2; g_oc_win2_sz = v2; }
    }
  }
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

/* Try to set up alias-region overcommit, recording each step's outcome into the
 * g_oc_* globals (logged from main). Alias-region overcommit turned out to be
 * impossible on this process: svcMapPhysicalMemory requires a non-zero kernel
 * "system resource" pool (for page-table/block bookkeeping) and our title-override
 * process has none -> it returns InvalidState (0xfa01). The unsafe pool
 * (svcMapPhysicalMemoryUnsafe) is ~44MB and already consumed. So we just record the
 * diagnostics and stay on the fully heap-backed arena. (Confirmed via Atmosphere
 * kern_svc_physical_memory.cpp: `R_UNLESS(GetTotalSystemResourceSize() > 0,
 * ResultInvalidState())`.) */
static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from cr3_nx.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  const size_t gfx_reserve = (size_t)GFX_RESERVE_MB * 1024 * 1024;

  if (envHasHeapOverride()) {
    /* TITLE OVERRIDE PATH -- and the one that actually matters, because that is
     * how this port is launched. hbloader has already called svcSetHeapSize and
     * handed us the result, so the reserve in the `else` branch below NEVER RUNS.
     * That is why raising GFX_RESERVE_MB appeared to do nothing: the boot log
     * still read `free=3 MB` at 192 MB and at 600 MB alike.
     *
     * Leaving the tail unused is not enough either. The memory is already
     * COMMITTED to this process; switch-mesa needs it back in the system pool
     * before nvservices can map it. So shrink the process heap and return it.
     *
     * The arithmetic must be relative to the HEAP BASE, not to the override
     * address: hbloader loads the NRO itself out of the low end of the heap, so
     * the override region starts above the base and svcSetHeapSize sizes the
     * whole thing. Shrinking by the wrong origin would unmap the running NRO. */
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();

    u64 heap_base = 0;
    if (R_SUCCEEDED(svcGetInfo(&heap_base, InfoType_HeapRegionAddress,
                               CUR_PROCESS_HANDLE, 0)) &&
        heap_base && (uintptr_t)addr >= (uintptr_t)heap_base) {
      const size_t head      = (uintptr_t)addr - (uintptr_t)heap_base; /* NRO etc */
      const size_t cur_total = head + size;
      const size_t floor     = head + (size_t)1536 * 1024 * 1024;      /* keep this much */
      if (cur_total > gfx_reserve && cur_total - gfx_reserve > floor) {
        const size_t want_total = (cur_total - gfx_reserve) & ~0x1FFFFF;
        void *na = NULL;
        if (R_SUCCEEDED(svcSetHeapSize(&na, want_total))) {
          if ((uintptr_t)na == (uintptr_t)heap_base) {
            size = ((uintptr_t)heap_base + want_total) - (uintptr_t)addr;
          } else {
            /* The base moved, so `addr` now points into memory we no longer
             * own. Rejecting the result is NOT enough -- the heap has already
             * shrunk. Put it back before anything touches it. */
            void *rb = NULL;
            svcSetHeapSize(&rb, cur_total);
            g_gfx_base_moved = 1;   /* reported from main(); see above */
          }
        }
        /* On failure addr/size stay exactly as hbloader gave them, i.e. the old
         * behaviour: a smaller heap is only adopted when the shrink succeeded
         * AND the base held. */
      }
    }
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    /* Non-override path (forwarder / applet). Same reserve, simpler: nothing
     * has claimed the heap yet, so we just ask for less. */
    if (mem_available > mem_used + gfx_reserve)
      size = (mem_available - mem_used - gfx_reserve) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  /* Capture what the reserve achieved; main() logs it. debugPrintf CANNOT be
   * used here -- __libnx_initheap runs before the filesystem is up and before
   * the log file exists, which is exactly why g_oc_* above use the same
   * capture-then-log-later pattern. */
  {
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    g_gfx_free_mb  = (unsigned)((tot - used) >> 20);
    g_gfx_heap_mb  = (unsigned)(size >> 20);
    g_gfx_override = envHasHeapOverride() ? 1 : 0;
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Preferred path: alias-region overcommit. Secures the virtual window and
   * test-commits a page FIRST, then shrinks the heap to [newlib + so_zone] so the
   * freed physical (~2.5GB) is available for on-demand commits. Everything is
   * secured before the shrink so a failure can't strand us with a shrunk heap. */
  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed 256MB-aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);   /* clamp to RAM */
    fake_heap_size = size - so_zone - arena_sz - big_align;       /* newlib gets the rest */
  } else {
    /* heap too small for a dedicated arena (e.g. applet mode): skip it; the mmap
     * allocator falls back to a memalign-backed bitmap arena. */
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}


/* ===========================================================================
 * First-boot asset optimisation.
 *
 * assets/bin/Data holds ~3640 loose files, most of them GUID-named content
 * blobs of a few KB. Reading those from an SD card through FAT is dominated by
 * per-file directory lookups rather than by the bytes themselves, so scene
 * loads crawl. We fold the whole tree into one flat pack with an index
 * (asset_pack.c) and serve every read out of it through the libc shim; after
 * that the engine does one file open instead of thousands.
 *
 * This runs once. Afterwards the loose tree is gone and the pack is what boots.
 * ======================================================================== */

static void remove_tree(const char *path) {
  DIR *dir = opendir(path);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      char child[768];
      snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
      struct stat st;
      if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
      else unlink(child);
    }
    closedir(dir);
  }
  rmdir(path);
  unlink(path);
}

/* Users install by extracting APKs, which drags in a lot that a Switch build
 * has no use for: the Java side, resources, signatures, the 32-bit libraries.
 * None of it is read by this port. Deleting it keeps the pack build from
 * sweeping it up and reclaims a few hundred MB.
 *
 * Only the engine's own assets/bin/Data is preserved -- everything named here
 * is Android packaging, not game content. */
static void cleanup_android_files(void) {
  static const char *dirs[] = {
    "META-INF", "res", "kotlin", "org", "okhttp3", "com", "androidx",
    "google", "firebase-common", "DebugProbesKt.bin",
    "lib/armeabi-v7a", "lib/x86", "lib/x86_64",
  };
  static const char *files[] = {
    "base.apk", "AndroidManifest.xml", "resources.arsc", "classes.dex",
    "classes2.dex", "classes3.dex", "classes4.dex", "stamp-cert-sha256",
    "assets/google-services-desktop.json",
    "assets/UnityServicesProjectConfiguration.json",
  };
  /* Only act if this actually looks like an extracted APK -- never start
   * deleting in a directory that is already clean. */
  if (access(game_path("AndroidManifest.xml"), F_OK) != 0 &&
      access(game_path("classes.dex"), F_OK) != 0 &&
      access(game_path("resources.arsc"), F_OK) != 0 &&
      access(game_path("META-INF"), F_OK) != 0)
    return;

  debugPrintf("[pack] removing unused Android files\n");
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, dirs[i]);
    remove_tree(path);
  }
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++) {
    char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    unlink(path);
  }
  /* Any remaining *.dex / *.properties / *.version at the root is packaging. */
  DIR *root = opendir(DATA_ROOT);
  if (root) {
    struct dirent *e;
    while ((e = readdir(root)) != NULL) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot) continue;
      if (strcmp(dot, ".dex") && strcmp(dot, ".properties") && strcmp(dot, ".version"))
        continue;
      char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, e->d_name);
      unlink(path);
    }
    closedir(root);
  }
}

/* Unity and IL2CPP write into assets/bin/Data at runtime (extracted metadata,
 * unity.ver). The pack is read-only, so leave the directories behind for those
 * writes to land in. */
static void create_asset_skeleton(void) {
  /* Relative here, resolved at call time: game_path() is a function now, so
   * these cannot be a static initialiser. */
  static const char *dirs[] = {
    "assets", "assets/bin", "assets/bin/Data",
    "assets/bin/Data/Managed", "assets/bin/Data/Managed/Metadata",
    "assets/bin/Data/Resources",
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
    mkdir(game_path(dirs[i]), 0777);
}

/* Accept either module layout. Extracting the APK leaves the libraries in
 * lib/arm64-v8a/; most install guides say to move them to the root of the game
 * folder. Both work -- there is no reason to make a wrong guess fatal. */
static int resolve_module_path(const char *name, char *out, size_t cap) {
  struct stat st;
  snprintf(out, cap, "%s/%s", DATA_ROOT, name);
  if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 1;
  snprintf(out, cap, "%s/lib/arm64-v8a/%s", DATA_ROOT, name);
  if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 1;
  return 0;
}

/* Short directory listing for error messages. "Missing libmain.so" when the
 * user can see libmain.so on the card is a useless message; showing the exact
 * path searched and what is actually there answers it immediately. */
static void describe_dir(const char *dir, char *out, size_t cap) {
  DIR *d = opendir(dir);
  if (!d) { snprintf(out, cap, "  (cannot open this folder)"); return; }
  size_t used = 0; int n = 0;
  struct dirent *e;
  out[0] = 0;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (n >= 12) { if (cap - used > 8) snprintf(out + used, cap - used, "  ...\n"); break; }
    int w = snprintf(out + used, cap - used, "  %s\n", e->d_name);
    if (w < 0 || (size_t)w >= cap - used) break;
    used += (size_t)w; n++;
  }
  closedir(d);
  if (!n) snprintf(out, cap, "  (this folder is empty)");
}

/* This title has no data.unity3d -- it ships an App Bundle whose assets/bin/Data
 * is a loose tree (globalgamemanagers, level0-2, sharedassets0-2 and ~3600
 * content files), split across the base APK and an install-time asset pack
 * that the user merges by hand. Check the files the engine cannot boot without,
 * so a half-merged copy fails here by name instead of as a black screen later.
 * Run AFTER nx_join_asset_splits(), since two of these are joined at boot. */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/boot.config",
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/globalgamemanagers.assets",       /* joined from 7 parts */
    "assets/bin/Data/level0",
    "assets/bin/Data/unity default resources",
    "assets/bin/Data/Resources/unity_builtin_extra",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/ScriptingAssemblies.json",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    /* The three modules may sit at the root or in lib/arm64-v8a/. */
    if (i < 3) {
      if (resolve_module_path(files[i], path, sizeof path)) continue;
      char listing[512];
      describe_dir(DATA_ROOT, listing, sizeof listing);
      fatal_error("Could not find %s.\n\n"
                  "Looked in:\n  %s/\n  %s/lib/arm64-v8a/\n\n"
                  "%s/ currently contains:\n%s\n"
                  "Put the .nro in the same folder as the\n"
                  "game files -- any folder name works.",
                  files[i], DATA_ROOT, DATA_ROOT, DATA_ROOT, listing);
    }
    /* Assets may already be packed, in which case the index is the authority
     * and the loose tree is gone. */
    if (asset_pack_active() && !strncmp(files[i], "assets/", 7) &&
        asset_pack_stat_relative(files[i] + 7, NULL, NULL))
      continue;
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0) {
      char listing[512];
      describe_dir(game_path("assets/bin/Data"), listing, sizeof listing);
      fatal_error("Missing game data:\n  %s\n\n"
                  "assets/bin/Data currently contains:\n%s\n"
                  "The base APK's assets folder and the extracted\n"
                  "UnityDataAssetPack must be merged together.", path, listing);
    }
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  if (!resolve_module_path(name, path, sizeof path)) {
    debugPrintf("[mod] %s not found at %s/ or %s/lib/arm64-v8a/\n",
                name, DATA_ROOT, DATA_ROOT);
    return -1;
  }
  debugPrintf("[mod] loading %s\n", path);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  debugPrintf("[mod] %-14s virtbase=%p size=0x%zx  (resolve: addr - virtbase = vaddr)\n",
              name, mod->load_virtbase, mod->load_size);
  crx_resolve_imports(mod);   /* so_resolve(mod, dynlib_functions, ...) */
  /* NOTE: so_patch_stack_canaries() intentionally NOT called. Per-thread bionic
   * TLS (install_bionic_tls) makes the engine's stack-protector guard consistent,
   * so the canary checks pass on their own. NOPing 2000+ b.ne sites risked a
   * false-positive in non-canary code (e.g. allocator list logic) -> corruption. */
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* ---------------------------------------------------------------------------
 * In-memory libunity patch (ported from VLN's nx_patch_unity_regions): the SD
 * card now ships the STOCK libunity.so and the boot patches it after load,
 * instead of distributing a pre-modified binary. 23 instruction words, from a
 * signature-matched derivation against THIS build's libunity (Unity 2022.3.40f1,
 * see nx_patch_lcgo.h): 21 sites relax the allocator's memory-region granularity
 * 256MB->64MB so the engine fits the so_loader address space on a 4GB Switch.
 * The reference's two extra "branch force" words were crash-era workarounds for
 * an upstream mesa bug that is now fixed, and are not carried over. Verify-first
 * like VLN: every original word must match before anything
 * is written; a fully pre-patched .so is detected and accepted; any other
 * mismatch leaves the binary untouched (different Unity build) with a loud log.
 * ------------------------------------------------------------------------- */
static int nx_patch_libunity(uintptr_t ub) {
  /* Lara Croft GO (Unity 2022.3.40f1): tables live in nx_patch_lcgo.h.
   * VERIFY-FIRST like the original -- every {from} word must match before ANY
   * write; a fully pre-patched .so is accepted; any mismatch patches nothing. */
  const NxPatchWord *P = LCGO_PATCH_WORDS;
  const int N = LCGO_PATCH_WORDS_N;
  int stock = 0, patched = 0;
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur == P[i].from) stock++;
    else if (cur == P[i].to) patched++;
    else {
      debugPrintf("[patch] libunity word mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all (offset table may be off for this build)\n",
                  (unsigned)P[i].off, cur, P[i].from);
      return 0;
    }
  }
  if (patched == N) {
    debugPrintf("[patch] libunity already pre-patched (%d sites) -- ok\n", N);
  } else if (stock != N) {
    debugPrintf("[patch] libunity PARTIALLY patched (%d/%d) -> SKIP (won't mix builds)\n", patched, N);
    return 0;
  } else {
    for (int i = 0; i < N; i++)
      so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to);
    debugPrintf("[patch] libunity region granularity 256MB->64MB patched (%d sites)\n", N);
  }
  /* Optional branch forces (only when you have located them; see nx_patch_lcgo.h). */
  if (LCGO_BRANCH_FORCES_N > 0) {
    const NxPatchWord *B = LCGO_BRANCH_FORCES;
    for (int i = 0; i < LCGO_BRANCH_FORCES_N; i++) {
      uint32_t cur = *(volatile uint32_t *)(ub + B[i].off);
      if (cur == B[i].from)
        so_patch_code((void *)(ub + B[i].off), &B[i].to, sizeof B[i].to);
      else if (cur != B[i].to)
        debugPrintf("[patch] branch-force @+0x%x mismatch (have 0x%08x) -> skip\n",
                    (unsigned)B[i].off, cur);
    }
    debugPrintf("[patch] libunity branch forces applied (%d sites)\n", LCGO_BRANCH_FORCES_N);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * PvZ 62f1c1 first-boot il2cpp hacks. With the GC stop-the-world bridge fixed for
 * PvZ (libc_shim.c pthread_kill_gc now reads the correct suspend/restart/ack
 * globals), the Boehm GC works, so -- exactly like the VLN port -- we do NOT try to
 * disable it. An earlier attempt to il2cpp_gc_disable() mid-il2cpp_init deadlocked
 * on a GC lock held by a not-yet-running helper thread (verified on hardware: hung
 * right after set_mode, UnityMain parked in a futex lock). The GC now runs normally
 * and the bridge keeps its POSIX-signal stop-the-world from hanging. All this does
 * is redirect Time.get_* to our frame clock. Called once at boot, before the loop.
 * ------------------------------------------------------------------------- */
/* Shared by the Resources trace and the scene census. Kept outside both
 * feature guards so either can be disabled independently. */
/* Copy an Il2CppString's chars into a C buffer. Resource paths are ASCII; any
 * non-ASCII unit becomes '?' rather than being mangled into invalid UTF-8. */
static void nx_il2cpp_str(void *s, char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (!s) { snprintf(out, cap, "<null>"); return; }
  const int32_t n = *(const int32_t *)((const char *)s + LCGO_STR_LEN_OFF);
  const uint16_t *c = (const uint16_t *)((const char *)s + LCGO_STR_CHARS_OFF);
  if (n < 0) { snprintf(out, cap, "<bad len %d>", (int)n); return; }
  size_t o = 0;
  for (int32_t i = 0; i < n && o + 1 < cap; i++)
    out[o++] = (c[i] >= 0x20 && c[i] < 0x7F) ? (char)c[i] : '?';
  out[o] = 0;
}

/* ---------------------------------------------------------------------------
 * Scene census -- see LCGO_SCENE_CENSUS_LIST in nx_patch_lcgo.h for the
 * derivation, and for why this measures scene contents rather than the camera.
 * ------------------------------------------------------------------------- */
#if LCGO_HAVE_SCENE_CENSUS
typedef void *(*nx_findobjs_fn)(void *reflection_type, int include_inactive);
typedef void *(*nx_dom_get_fn)(void);
typedef void **(*nx_dom_asms_fn)(void *domain, size_t *count);
typedef void *(*nx_asm_image_fn)(void *assembly);
typedef void *(*nx_cls_from_name_fn)(void *image, const char *ns, const char *name);
typedef void *(*nx_cls_get_type_fn)(void *klass);
typedef void *(*nx_type_get_obj_fn)(void *type);
typedef void *(*nx_getname_fn)(void *obj);

/* Find a managed class by (namespace, name) across every loaded assembly.
 * Iterating beats naming the assembly: Canvas is in UnityEngine.UIModule,
 * Renderer in UnityEngine.CoreModule, and both could move on a version bump. */
static void *nx_find_class(const char *ns, const char *name) {
  static nx_dom_get_fn       dom_get;
  static nx_dom_asms_fn      dom_asms;
  static nx_asm_image_fn     asm_image;
  static nx_cls_from_name_fn cls_from_name;
  static int resolved;
  if (!resolved) {
    resolved = 1;
    dom_get       = (nx_dom_get_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get");
    dom_asms      = (nx_dom_asms_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get_assemblies");
    asm_image     = (nx_asm_image_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_assembly_get_image");
    cls_from_name = (nx_cls_from_name_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_class_from_name");
  }
  if (!dom_get || !dom_asms || !asm_image || !cls_from_name) return NULL;
  void *domain = dom_get();
  if (!domain) return NULL;
  size_t n = 0;
  void **asms = dom_asms(domain, &n);
  if (!asms) return NULL;
  for (size_t i = 0; i < n; i++) {
    void *img = asm_image(asms[i]);
    if (!img) continue;
    void *k = cls_from_name(img, ns, name);
    if (k) return k;
  }
  return NULL;
}

static void nx_scene_census(int frame) {
  static const int when[] = LCGO_CENSUS_FRAMES;
  static int done[sizeof(when) / sizeof(when[0])];
  int slot = -1;
  for (unsigned i = 0; i < sizeof(when) / sizeof(when[0]); i++)
    if (frame == when[i] && !done[i]) { slot = (int)i; break; }
  if (slot < 0) return;
  done[slot] = 1;

  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  const uint32_t w = *(volatile uint32_t *)(b + LCGO_FINDOBJECTS_RVA);
  if (w != LCGO_WORD_FINDOBJ_STOCK) {
    debugPrintf("[census] SKIP: FindObjectsOfType @il2cpp+0x%x = 0x%08x, expected "
                "0x%08x -- offset wrong for this libil2cpp\n",
                (unsigned)LCGO_FINDOBJECTS_RVA, w, (unsigned)LCGO_WORD_FINDOBJ_STOCK);
    return;
  }
  static nx_cls_get_type_fn cls_get_type;
  static nx_type_get_obj_fn type_get_obj;
  if (!cls_get_type) {
    cls_get_type = (nx_cls_get_type_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_class_get_type");
    type_get_obj = (nx_type_get_obj_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_type_get_object");
  }
  if (!cls_get_type || !type_get_obj) {
    debugPrintf("[census] SKIP: il2cpp_class_get_type/type_get_object not exported\n");
    return;
  }
  const nx_findobjs_fn find = (nx_findobjs_fn)(b + LCGO_FINDOBJECTS_RVA);
  /* Object::get_name is optional garnish -- if its prologue does not verify we
   * still report the counts, which are the load-bearing part. */
  nx_getname_fn getname = NULL;
  if (*(volatile uint32_t *)(b + LCGO_OBJ_GET_NAME_RVA) == LCGO_WORD_GETNAME_STOCK)
    getname = (nx_getname_fn)(b + LCGO_OBJ_GET_NAME_RVA);

  char line[224];
  int o = snprintf(line, sizeof line, "[census] f%d", frame);
#define LCGO_CENSUS_ROW(ns, cls, label)                                            \
  do {                                                                             \
    void *k = nx_find_class(ns, cls);                                              \
    if (!k) { o += snprintf(line + o, sizeof line - o, " %s=?", label); break; }    \
    void *t = cls_get_type(k);                                                      \
    void *rt = t ? type_get_obj(t) : NULL;                                          \
    if (!rt) { o += snprintf(line + o, sizeof line - o, " %s=?", label); break; }   \
    /* includeInactive=1: an inactive canvas is the interesting case -- it means   \
     * the UI was built and then switched off, which is a different fault from     \
     * never having been built. */                                                 \
    void *arr = find(rt, 1);                                                        \
    int n = arr ? *(const int *)((const char *)arr + LCGO_IL2CPP_ARRAY_LEN) : -1;   \
    o += snprintf(line + o, sizeof line - o, " %s=%d", label, n);                   \
    /* Name the first instance. Which single Canvas exists matters more than the  \
     * fact that one does. */                                                      \
    if (getname && arr && n > 0) {                                                  \
      void *first = *(void *const *)((const char *)arr + LCGO_IL2CPP_ARRAY_DATA);   \
      if (first) {                                                                  \
        void *nm = getname(first);                                                  \
        char nb[64];                                                                \
        nx_il2cpp_str(nm, nb, sizeof nb);                                            \
        o += snprintf(line + o, sizeof line - o, "(\"%s\")", nb);                    \
      }                                                                             \
    }                                                                               \
  } while (0);
  LCGO_SCENE_CENSUS_LIST(LCGO_CENSUS_ROW)
#undef LCGO_CENSUS_ROW
  debugPrintf("%s  (incl. inactive)\n", line);
}
#else
static void nx_scene_census(int frame) { (void)frame; }
#endif

/* ---------------------------------------------------------------------------
 * Managed-state probe. Asks the il2cpp runtime, once a second, whether a camera
 * and a scene actually exist -- see LCGO_MANAGED_PROBE_LIST in nx_patch_lcgo.h
 * for the derivation and for how to read the output.
 *
 * This writes nothing. It reads each entry's prologue word to confirm the RVA
 * really is the lazy-icall thunk we think it is, and if so calls it. A wrong
 * offset logs and disables that entry rather than calling into whatever else
 * lives there.
 *
 * Two ordering constraints, both load-bearing:
 *   - Called only from the render loop, after nativeRender returns, so it runs
 *     on the Unity main thread. These icalls touch runtime state that is not
 *     safe to reach from the loader's other threads.
 *   - Not before frame 2. The thunks resolve themselves on first call
 *     (bl resolve_icall); calling one before the icall table is populated would
 *     fault. By the time a frame has been presented, it is populated.
 * ------------------------------------------------------------------------- */
#if LCGO_HAVE_MANAGED_PROBE
static void nx_managed_probe(int frame) {
  static struct { uint32_t off; const char *nm; char kind; void *fn; int bad; } pr[] = {
#define LCGO_PR_ROW(rva, nm, kind) { (uint32_t)(rva), nm, kind, NULL, 0 },
    LCGO_MANAGED_PROBE_LIST(LCGO_PR_ROW)
#undef LCGO_PR_ROW
  };
  static int64_t last[sizeof(pr) / sizeof(pr[0])];
  static int     have_last = 0, quiet = 0;

  if (frame < 2 || (frame % LCGO_PROBE_PERIOD_FRAMES) != 0) return;

  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  int64_t now[sizeof(pr) / sizeof(pr[0])];
  int changed = 0, live = 0;

  for (unsigned i = 0; i < sizeof(pr) / sizeof(pr[0]); i++) {
    now[i] = -1;
    if (pr[i].bad) continue;
    if (!pr[i].fn) {                       /* verify-first, once */
      volatile uint32_t *pw = (volatile uint32_t *)(b + pr[i].off);
      if (*pw != LCGO_PROBE_VERIFY_WORD) {
        debugPrintf("[probe] SKIP %s @il2cpp+0x%x: 0x%08x, not the icall thunk "
                    "prologue -- offset wrong for this libil2cpp\n",
                    pr[i].nm, pr[i].off, *pw);
        pr[i].bad = 1;
        continue;
      }
      pr[i].fn = (void *)(b + pr[i].off);
    }
    switch (pr[i].kind) {
      case 'i': now[i] = (int64_t)((int (*)(void))pr[i].fn)(); break;
      case 'p': now[i] = ((void *(*)(void))pr[i].fn)() ? 1 : 0; break;
      /* timeScale returns in s0; scale by 1000 so it prints as an integer
       * without dragging float formatting into debugPrintf. */
      case 'f': now[i] = (int64_t)(((float (*)(void))pr[i].fn)() * 1000.0f); break;
      default:  break;
    }
    live++;
    if (!have_last || now[i] != last[i]) changed = 1;
  }
  if (!live) return;

  quiet += LCGO_PROBE_PERIOD_FRAMES;
  if (!changed && have_last && quiet < LCGO_PROBE_MAX_QUIET) return;
  quiet = 0;

  char line[192];   /* content is 5 short fields; leaves headroom for the suffix */
  int o = snprintf(line, sizeof line, "[probe] f%d", frame);
  for (unsigned i = 0; i < sizeof(pr) / sizeof(pr[0]) && o > 0 && o < (int)sizeof line; i++) {
    if (pr[i].bad) continue;
    if (pr[i].kind == 'p')
      o += snprintf(line + o, sizeof line - o, " %s=%s", pr[i].nm, now[i] ? "obj" : "NULL");
    else if (pr[i].kind == 'f')
      o += snprintf(line + o, sizeof line - o, " %s=%d.%03d", pr[i].nm,
                    (int)(now[i] / 1000), (int)(now[i] % 1000 < 0 ? -now[i] % 1000 : now[i] % 1000));
    else
      o += snprintf(line + o, sizeof line - o, " %s=%d", pr[i].nm, (int)now[i]);
  }
  debugPrintf("%s%s\n", line, changed && have_last ? "   <-- changed" : "");

  memcpy(last, now, sizeof now);
  have_last = 1;
}
#else
static void nx_managed_probe(int frame) { (void)frame; }
#endif

/* Shared by the frame-rate override and the Resources trace: libil2cpp's
 * exported by-name icall resolver, used to reach the ORIGINAL implementation
 * after a cache swap. */
typedef void *(*nx_resolve_icall_fn)(const char *sig);

/* ---------------------------------------------------------------------------
 * Frame-rate override -- see LCGO_SET_TFR_* in nx_patch_lcgo.h.
 * ------------------------------------------------------------------------- */
#if LCGO_HAVE_FPS_OVERRIDE
typedef void (*nx_set_tfr_fn)(int fps);
/* Resolved once at install: out-of-range values fall back to the documented
 * default rather than being pushed into the engine unchecked. */
static int                g_tfr_want = 60;
static nx_set_tfr_fn      g_tfr_orig;
static nx_resolve_icall_fn g_tfr_resolve;
static void              **g_tfr_slot;

static void nx_set_target_framerate(int fps) {
  if (!g_tfr_orig) {
    g_tfr_orig = (nx_set_tfr_fn)g_tfr_resolve(LCGO_SET_TFR_SIG);
    if (!g_tfr_orig) {
      /* Self-disarm, same contract as the Resources trace: put the slot back so
       * the thunk resolves itself next call. Losing one set_targetFrameRate is
       * harmless; permanently swallowing every one would silently pin the game
       * to whatever rate happened to be active. */
      if (g_tfr_slot) *g_tfr_slot = NULL;
      debugPrintf("[fps] resolve FAILED -- override disarmed, game keeps its own cap\n");
      return;
    }
  }
  const int want = g_tfr_want;
  if (want != fps)
    debugPrintf("[fps] game asked for %d, forcing %d\n", fps, want);
  else
    debugPrintf("[fps] game asked for %d (unchanged)\n", fps);
  g_tfr_orig(want);
}

#if LCGO_HAVE_VSYNC_OVERRIDE
static void nx_install_vsync_override(void);   /* fwd: called from the TFR installer */

/* QualitySettings::set_vSyncCount override.
 *
 * WHY THIS EXISTS. `framerate 60` in config.txt did nothing on its own. Unity
 * ignores Application.targetFrameRate entirely whenever vSyncCount is non-zero
 * -- presentation is then vsync/vSyncCount. This game sets a quality profile
 * that assigns vSyncCount (both setters survived IL2CPP stripping, which only
 * happens when managed code calls them), so whatever the game chose won, and
 * forcing targetFrameRate could never raise the cap.
 *
 * The window is created with nwindowSetSwapInterval(w, 1), i.e. one present per
 * 60 Hz vsync, so the mapping the engine sees is:
 *      vSyncCount 1 -> 60 fps        vSyncCount 2 -> 30 fps
 *
 * We still force vSyncCount to 1 for consistency, but note this is very likely
 * INERT: the Android player ignores vSyncCount, which is why the game asks for
 * 0. The setting that actually matters is targetFrameRate -- see
 * nx_force_target_framerate().
 *
 * Same cache-swap technique and the same tail-call contract as the TFR hook:
 * entered with the caller's x30 live, signature void(int). */
static int                 g_vsync_want = 1;
static nx_set_tfr_fn       g_vsync_orig;      /* void(int), same shape */
static void              **g_vsync_slot;

static void nx_set_vsync_count(int count) {
  if (!g_vsync_orig) {
    g_vsync_orig = (nx_set_tfr_fn)g_tfr_resolve(LCGO_SET_VSYNC_SIG);
    if (!g_vsync_orig) {
      if (g_vsync_slot) *g_vsync_slot = NULL;   /* self-disarm, as TFR does */
      debugPrintf("[fps] vsync resolve FAILED -- override disarmed\n");
      return;
    }
  }
  const int want = g_vsync_want;
  if (want != count)
    debugPrintf("[fps] game asked for vSyncCount=%d, forcing %d (=%d fps)\n",
                count, want, want == 2 ? 30 : 60);
  else
    debugPrintf("[fps] game asked for vSyncCount=%d (unchanged)\n", count);
  g_vsync_orig(want);
}

static void nx_install_vsync_override(void) {
  /* 60 -> present every vsync; 30 -> every second vsync. */
  g_vsync_want = (g_tfr_want == 30) ? 2 : 1;
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  const uint32_t w = *(volatile uint32_t *)(b + LCGO_SET_VSYNC_THUNK);
  if (w != LCGO_WORD_VSYNC_THUNK_STOCK) {
    debugPrintf("[fps] SKIP vsync: set_vSyncCount @il2cpp+0x%x = 0x%08x, expected "
                "0x%08x -- offset wrong for this libil2cpp\n",
                (unsigned)LCGO_SET_VSYNC_THUNK, w, (unsigned)LCGO_WORD_VSYNC_THUNK_STOCK);
    return;
  }
  /* Resolve independently rather than relying on the TFR installer having got
   * that far -- the two hooks should not be able to disable each other. */
  if (!g_tfr_resolve)
    g_tfr_resolve =
        (nx_resolve_icall_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_resolve_icall");
  if (!g_tfr_resolve) {
    debugPrintf("[fps] SKIP vsync: il2cpp_resolve_icall not exported\n");
    return;
  }
  g_vsync_slot = (void **)(b + LCGO_SET_VSYNC_CACHE);
  if (*g_vsync_slot && *g_vsync_slot != (void *)nx_set_vsync_count)
    g_vsync_orig = (nx_set_tfr_fn)*g_vsync_slot;
  *g_vsync_slot = (void *)nx_set_vsync_count;
  debugPrintf("[fps] vsync override armed: forcing vSyncCount=%d for %d fps "
              "(cache il2cpp+0x%x)\n",
              g_vsync_want, g_tfr_want, (unsigned)LCGO_SET_VSYNC_CACHE);
}
#endif /* LCGO_HAVE_VSYNC_OVERRIDE */

static void nx_install_fps_override(void) {
  g_tfr_want = (config.framerate == 30 || config.framerate == 60)
                 ? config.framerate : 60;
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  const uint32_t w = *(volatile uint32_t *)(b + LCGO_SET_TFR_THUNK);
  if (w != LCGO_WORD_TFR_THUNK_STOCK) {
    debugPrintf("[fps] SKIP: set_targetFrameRate @il2cpp+0x%x = 0x%08x, expected "
                "0x%08x -- offset wrong for this libil2cpp\n",
                (unsigned)LCGO_SET_TFR_THUNK, w, (unsigned)LCGO_WORD_TFR_THUNK_STOCK);
#if LCGO_HAVE_VSYNC_OVERRIDE
    nx_install_vsync_override();   /* vsync is the one that actually gates fps */
#endif
    return;
  }
  g_tfr_resolve =
      (nx_resolve_icall_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_resolve_icall");
  if (!g_tfr_resolve) {
    debugPrintf("[fps] SKIP: il2cpp_resolve_icall not exported -- cannot call "
                "through to the original, so not arming\n");
    return;   /* vsync needs the same resolver, so nothing to attempt */
  }
  g_tfr_slot = (void **)(b + LCGO_SET_TFR_CACHE);
  if (*g_tfr_slot && *g_tfr_slot != (void *)nx_set_target_framerate)
    g_tfr_orig = (nx_set_tfr_fn)*g_tfr_slot;
  *g_tfr_slot = (void *)nx_set_target_framerate;
  debugPrintf("[fps] override armed: forcing %d fps (cache il2cpp+0x%x)\n",
              g_tfr_want, (unsigned)LCGO_SET_TFR_CACHE);

  /* Resolve the real icall NOW so nx_force_target_framerate() can call it
   * directly, without waiting for the game to touch the property. */
  if (!g_tfr_orig) g_tfr_orig = (nx_set_tfr_fn)g_tfr_resolve(LCGO_SET_TFR_SIG);
  if (!g_tfr_orig)
    debugPrintf("[fps] WARNING: could not resolve set_targetFrameRate -- "
                "cannot set it actively, only intercept\n");

#if LCGO_HAVE_VSYNC_OVERRIDE
  nx_install_vsync_override();
#endif
}

/* Actively SET Application.targetFrameRate, rather than waiting to intercept it.
 *
 * WHY THIS EXISTS -- this is the actual 30 fps cause, and the reason every
 * earlier frame-rate fix did nothing.
 *
 * nx_set_target_framerate() above is a REPLACEMENT for the icall: it only runs
 * if managed code assigns Application.targetFrameRate. This game never does --
 * the boot log shows the hook armed and then fired ZERO times, while the
 * vSyncCount hook fired once with the game asking for 0.
 *
 * vSyncCount=0 is the correct Android value: Unity's Android player IGNORES
 * QualitySettings.vSyncCount and paces purely off Application.targetFrameRate.
 * So forcing vSyncCount to 1 was a no-op (confirmed: it was forced, and the
 * game stayed at 30), and targetFrameRate was left at Unity's MOBILE DEFAULT --
 * which is 30, not -1. Nothing on our side ever wrote 60 anywhere the engine
 * would read it.
 *
 * Hence: call the resolved icall directly with 60. Re-asserted periodically
 * because a quality-level change or a scene load can reapply the player
 * settings and put the 30 back. */
static void nx_force_target_framerate(void) {
#if LCGO_HAVE_NATIVE_TFR
  /* PRIMARY PATH: write libunity's own target-frame-rate global directly.
   *
   * GetActualTargetFrameRate() reads this int when vSyncCount <= 0, and
   * substitutes a hardcoded 30.0f if it is not > 0. It ships as -1 and this
   * game never sets it, which IS the 30 fps cap. Storing 60 here is the whole
   * fix, and unlike the managed hook it needs no icall resolution and no
   * cooperation from the game. */
  {
    static int native_ok = -1;            /* -1 unchecked, 0 bad offset, 1 good */
    const uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    if (native_ok < 0) {
      const uint32_t w0 = *(volatile uint32_t *)(ub + LCGO_SET_TFR_NATIVE_FN);
      const uint32_t w1 = *(volatile uint32_t *)(ub + LCGO_SET_TFR_NATIVE_FN + 4);
      native_ok = (w0 == LCGO_WORD_SETTFR_ADRP && w1 == LCGO_WORD_SETTFR_STR);
      if (!native_ok)
        debugPrintf("[fps] SKIP native TFR: SetTargetFrameRate @libunity+0x%x = "
                    "0x%08x/0x%08x, expected 0x%08x/0x%08x\n",
                    (unsigned)LCGO_SET_TFR_NATIVE_FN, w0, w1,
                    (unsigned)LCGO_WORD_SETTFR_ADRP, (unsigned)LCGO_WORD_SETTFR_STR);
    }
    if (native_ok) {
      volatile int32_t *g = (volatile int32_t *)(ub + LCGO_NATIVE_TFR_GLOBAL);
      const int32_t was = *g;
      if (was != g_tfr_want) {
        *g = g_tfr_want;
        debugPrintf("[fps] native targetFrameRate: %d -> %d "
                    "(libunity+0x%x; %d meant 'unset' and fell back to the "
                    "hardcoded 30)\n",
                    (int)was, g_tfr_want, (unsigned)LCGO_NATIVE_TFR_GLOBAL, (int)was);
      }
    }
  }
#endif
#if LCGO_HAVE_FPS_OVERRIDE
  if (!g_tfr_orig) return;               /* managed path optional; native did it */
  static int announced = 0;
  if (!announced) {
    announced = 1;
    debugPrintf("[fps] actively setting Application.targetFrameRate=%d "
                "(the game never assigns it; Unity's mobile default is 30)\n",
                g_tfr_want);
  }
  /* Call the ORIGINAL icall, not the hook: the hook would just call this. */
  g_tfr_orig(g_tfr_want);
#endif
}

#else
static void nx_install_fps_override(void) { }
static void nx_force_target_framerate(void) { }
#endif

/* ---------------------------------------------------------------------------
 * Resources.Load trace -- see LCGO_RES_LOAD_* in nx_patch_lcgo.h for the
 * derivation and for why this hooks by swapping an icall cache pointer rather
 * than splicing code.
 * ------------------------------------------------------------------------- */
#if LCGO_HAVE_RES_TRACE
typedef void *(*nx_res_load_fn)(void *path, void *type);
static nx_res_load_fn       g_res_load_orig;
static nx_resolve_icall_fn  g_res_resolve_icall;   /* checked at install time */
static void               **g_res_cache_slot;      /* so we can self-disarm    */
static unsigned             g_res_trace_n;

static void *nx_res_load_trace(void *path, void *type) {
  if (!g_res_load_orig) {
    /* First call: fetch the real icall the thunk would have resolved. */
    g_res_load_orig = (nx_res_load_fn)g_res_resolve_icall(LCGO_RES_LOAD_SIG);
    debugPrintf("[res] resolved real icall -> %p\n", (void *)g_res_load_orig);
    if (!g_res_load_orig) {
      /* SELF-DISARM. Without this, a failed resolve would make every
       * Resources.Load in the process return null for the rest of the run --
       * a tracer that manufactures the exact symptom it was added to diagnose,
       * and one that would read in the log as "Resources is broken". Put the
       * slot back to zero so the thunk resolves itself normally from the next
       * call on; only this single call is lost. */
      if (g_res_cache_slot) *g_res_cache_slot = NULL;
      debugPrintf("[res] *** resolve FAILED -- trace disarmed, thunk restored. "
                  "This one Load returns null; the rest are unaffected. "
                  "Do NOT read this as a Resources failure. ***\n");
      return NULL;
    }
  }
  void *r = g_res_load_orig(path, type);
  if (g_res_trace_n < LCGO_RES_TRACE_MAX) {
    g_res_trace_n++;
    char buf[192];
    nx_il2cpp_str(path, buf, sizeof buf);
    debugPrintf("[res] Load(\"%s\") -> %s\n", buf, r ? "ok" : "NULL");
    if (g_res_trace_n == LCGO_RES_TRACE_MAX)
      debugPrintf("[res] trace cap reached (%u); further loads not logged\n",
                  (unsigned)LCGO_RES_TRACE_MAX);
  }
  return r;
}

static void nx_install_res_trace(void) {
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  /* Verify-first: confirm the thunk still opens with its expected prologue
   * before trusting the cache-slot offset derived from it. A wrong offset here
   * would write a function pointer into arbitrary .bss. */
  const uint32_t w = *(volatile uint32_t *)(b + LCGO_RES_LOAD_THUNK);
  if (w != LCGO_WORD_RES_THUNK_STOCK) {
    debugPrintf("[res] SKIP trace: thunk @il2cpp+0x%x = 0x%08x, expected 0x%08x "
                "-- offset wrong for this libil2cpp\n",
                (unsigned)LCGO_RES_LOAD_THUNK, w,
                (unsigned)LCGO_WORD_RES_THUNK_STOCK);
    return;
  }
  /* Refuse to arm unless we can obtain the resolver we will need to call the
   * ORIGINAL icall through. Checking here rather than on first use means a
   * missing export leaves the game completely untouched instead of breaking
   * every Resources.Load at the worst possible moment. */
  g_res_resolve_icall =
      (nx_resolve_icall_fn)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_resolve_icall");
  if (!g_res_resolve_icall) {
    debugPrintf("[res] SKIP trace: il2cpp_resolve_icall not exported -- cannot "
                "call through to the original, so not arming\n");
    return;
  }
  void **slot = (void **)(b + LCGO_RES_LOAD_CACHE);
  g_res_cache_slot = slot;
  /* If it has already resolved, keep what is there as the original and skip the
   * resolver entirely. Normally we install first and *slot is still null. */
  if (*slot && *slot != (void *)nx_res_load_trace)
    g_res_load_orig = (nx_res_load_fn)*slot;
  *slot = (void *)nx_res_load_trace;
  debugPrintf("[res] Resources.Load trace armed (cache il2cpp+0x%x, was %s)\n",
              (unsigned)LCGO_RES_LOAD_CACHE,
              g_res_load_orig ? "already resolved" : "unresolved");
}
#else
static void nx_install_res_trace(void) { }
#endif

static int g_boot_hacks_done = 0;
static void nx_boot_il2cpp_hacks(void) {
  if (g_boot_hacks_done) return;
  g_boot_hacks_done = 1;
  uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Redirect the managed UnityEngine.Time getters at our frame clock. These are
   * libil2cpp RVAs -- game code -- derived and verified per nx_patch_lcgo.h.
   * Verify-first: each getter's prologue must be the expected
   * `stp x30,x19,[sp,#-0x10]!` before we splice, so a wrong offset logs and
   * skips instead of corrupting whatever else lives there. */
#if LCGO_HAVE_TIME_HOOKS
  {
    static const struct { uint32_t off; void *fn; const char *nm; } th[] = {
#define LCGO_TH_ROW(rva, fn, nm) { (uint32_t)(rva), (void *)&fn, nm },
      LCGO_TIME_HOOK_LIST(LCGO_TH_ROW)
#undef LCGO_TH_ROW
    };
    unsigned done = 0;
    for (unsigned i = 0; i < sizeof(th) / sizeof(th[0]); i++) {
      volatile uint32_t *pw = (volatile uint32_t *)(b + th[i].off);
      if (*pw != LCGO_WORD_TIMEGET_STOCK) {
        debugPrintf("[time] SKIP %s @il2cpp+0x%x: 0x%08x, not `stp x30,x19` "
                    "-- offset wrong for this libil2cpp\n",
                    th[i].nm, th[i].off, *pw);
        continue;
      }
      uint32_t stub[4];
      stub[0] = 0x58000050u; stub[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&stub[2], &th[i].fn, 8);
      so_patch_code((void *)(b + th[i].off), stub, sizeof stub);
      debugPrintf("[time] hooked %s @il2cpp+0x%x\n", th[i].nm, th[i].off);
      done++;
    }
    so_flush_caches(&il2cpp_mod);   /* make the Time-hook code patches live */
    debugPrintf("[time] managed clock hooks installed (%u/%u: 6 Time.get_* + "
                "Application.get_internetReachability)\n",
                done, (unsigned)(sizeof(th) / sizeof(th[0])));
  }
#endif

#if LCGO_HAVE_SCREEN_HOOKS
  {
    static const struct { uint32_t off; void *fn; const char *nm; uint32_t want; } sh[] = {
#define LCGO_SH_ROW(rva, fn, nm, want) { (uint32_t)(rva), (void *)&fn, nm, (uint32_t)(want) },
      LCGO_SCREEN_HOOK_LIST(LCGO_SH_ROW)
#undef LCGO_SH_ROW
    };
    unsigned done = 0;
    for (unsigned i = 0; i < sizeof(sh) / sizeof(sh[0]); i++) {
      volatile uint32_t *pw = (volatile uint32_t *)(b + sh[i].off);
      if (*pw != sh[i].want) {
        debugPrintf("[screen] SKIP %s @il2cpp+0x%x: 0x%08x, want 0x%08x\n",
                    sh[i].nm, sh[i].off, *pw, sh[i].want);
        continue;
      }
      uint32_t stub[4];
      stub[0] = 0x58000050u; stub[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&stub[2], &sh[i].fn, 8);
      so_patch_code((void *)(b + sh[i].off), stub, sizeof stub);
      debugPrintf("[screen] hooked %s @il2cpp+0x%x\n", sh[i].nm, sh[i].off);
      done++;
    }
    so_flush_caches(&il2cpp_mod);
    debugPrintf("[screen] Screen/Display size hooks installed (%u/%u) -> %dx%d\n",
                done, (unsigned)(sizeof(sh) / sizeof(sh[0])),
                (int)nx_screen_width(), (int)nx_screen_height());
  }
#else
  (void)b;
  debugPrintf("[time] managed Time.get_* hooks DISABLED (LCGO_HAVE_TIME_HOOKS=0)\n");
#endif
}

int main(int argc, char *argv[]) {
  /* First: work out which folder we live in. Everything else -- debug.log, the
   * config file, the modules, the assets -- hangs off this, so nothing may
   * touch a path before it runs. */
  game_root_resolve(argc, argv);
  socketInitializeDefault();
  debugPrintf("[boot] === lcgo_nx start ===\n");

  /* Load config.txt. When the file is missing, autogenerate a documented one
   * with the defaults; when it holds retired options -- screen_width,
   * screen_height and language among them now -- rewrite it without them so the
   * file on disk always matches what the port actually reads. */
  {
    int crc = read_config(game_path(CONFIG_NAME));
    if (crc != 0) {
      write_config(game_path(CONFIG_NAME));
      debugPrintf("[boot] %s config.txt\n", crc < 0 ? "created" : "rewrote");
    }
    debugPrintf("[boot] config: handheld=%dp docked=%dp framerate=%d\n",
                config.handheld_res, config.docked_res, config.framerate);
  }

  /* Extract the console's shared fonts and expose them where Unity's dynamic
   * font fallback looks. Must run before the modules load: libunity reads the
   * font config while building its fallback list, and an empty list is what
   * left Cyrillic/CJK blank. See nx_fonts.c. */
  nx_fonts_init(GAME_HOME);

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays
   * from older builds, plus the single hidden scratch the probe is redirected
   * to now (libc_shim.c casetest_redirect). */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, "%s/%s", DATA_ROOT, de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  /* CWD fix (mirrors MMX enter_data_dir): title-override / hbloader leaves the
   * working dir at the .nro folder or the SD root, NOT the game dir. Unity &
   * il2cpp read many files through *relative* paths ("assets/bin/Data/...") and
   * our basename_fallback stats relative to cwd, so a wrong cwd silently yields
   * empty/missing reads -> NULL il2cpp classes. chdir into DATA_ROOT so every
   * relative read resolves under sdmc:/switch/lcgo. (Absolute "sdmc:/..."
   * reads are unaffected.) */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    int reach_assets = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    int reach_meta   = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_guid   = stat("assets/bin/Data/unity_app_guid", &st) == 0;
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(rel): globalgamemanagers=%d metadata=%d unity_app_guid=%d\n",
                reach_assets, reach_meta, reach_guid);
  }

  /* Force libunity to RE-EXTRACT il2cpp resources every boot. Observed: when
   * extraction is skipped (il2cpp/unity.ver present), il2cpp mmaps the extracted
   * global-metadata.dat and crashes in Class::Init(NULL); when extraction RUNS,
   * il2cpp uses the full source it reads for the copy and gets past that point.
   * The extracted copy is bad because our shim doesn't flush a writable
   * file-backed mmap back to disk, so it lands truncated. Removing the extracted
   * markers makes libunity redo the extraction each boot (uses the good source).
   * Proper fix = flush writable file-backed mmaps on munmap (tracked separately). */
  {
    int a = unlink(game_path("il2cpp/unity.ver"));
    int b = unlink(game_path("il2cpp/Metadata/global-metadata.dat"));
    int c = unlink(game_path("il2cpp/Resources/mscorlib.dll-resources.dat"));
    debugPrintf("[boot] force re-extract: unlink unity.ver=%d metadata=%d resources=%d\n", a, b, c);
  }

  /* ---- asset preparation ------------------------------------------------
   * Order matters and each step is idempotent:
   *   1. adopt an existing pack, if the loose tree is already gone
   *   2. join Unity's .splitN chunks (loose installs only)
   *   3. validate -- fail by name here rather than as a black screen later
   *   4. delete Android packaging the port never reads
   *   5. build the pack, then drop the loose tree
   * Steps 2-5 only do work on the first boot after an install. */
  {
    struct stat loose;
    if (stat(game_path("assets/bin/Data/globalgamemanagers"), &loose) != 0 ||
        !S_ISREG(loose.st_mode))
      asset_pack_open_existing(DATA_ROOT);
  }

  if (!asset_pack_active()) {
    startup_status_begin("Preparing game data (first boot)");

    /* Reassemble Unity's 1MB .splitN chunks. The asset pack stores large
     * SerializedFiles chunked and expects libunity's AndroidSplitFile VFS (an
     * APK/AssetManager path) to stitch them; we serve plain files, so we join
     * them ourselves -- and it must happen before the pack is built, or the
     * pack would just contain the fragments. */
    startup_status_update("Joining split asset files");
    nx_join_asset_splits();
  }

  startup_status_update("Validating game data");
  check_data();

  if (!asset_pack_active()) {
    startup_status_update("Removing unused Android files");
    cleanup_android_files();

    startup_status_update("Optimizing game assets (first boot, several minutes)");
    debugPrintf("[pack] building asset pack from %s/assets\n", DATA_ROOT);
    if (!asset_pack_build(game_path("assets"), DATA_ROOT))
      fatal_error("Could not optimize the extracted assets.\n%s\n\n"
                  "The original files were kept, so you can\n"
                  "simply try again.", asset_pack_error());

    /* Only drop the loose tree once the pack is live and verified. */
    struct stat st;
    if (asset_pack_active() &&
        stat(game_path("assets/bin/Data/globalgamemanagers"), &st) == 0 &&
        S_ISREG(st.st_mode)) {
      startup_status_update("Removing unpacked asset files");
      remove_tree(game_path("assets"));
      create_asset_skeleton();
    }
    debugPrintf("[pack] optimization complete: %u entries\n",
                (unsigned)asset_pack_entry_count());
  } else {
    debugPrintf("[pack] using existing pack: %u entries\n",
                (unsigned)asset_pack_entry_count());
  }
  startup_status_update("Starting the game");
  startup_status_end();

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    /* THE line that says whether GFX_RESERVE_MB took effect. `free` must rise
     * from ~3 MB to roughly GFX_RESERVE_MB. Readable on a 720p boot, so the
     * reserve can be verified without going near the 1080p path. */
    debugPrintf("[heap] %s: heap %u MB, free for the graphics driver %u MB "
                "(GFX_RESERVE_MB=%u)%s\n",
                g_gfx_override ? "title override" : "own heap",
                g_gfx_heap_mb, g_gfx_free_mb, (unsigned)GFX_RESERVE_MB,
                g_gfx_free_mb + 16 < (unsigned)GFX_RESERVE_MB
                  ? "  <-- RESERVE DID NOT TAKE EFFECT" : "");
    if (g_gfx_base_moved)
      debugPrintf("[heap] shrink returned a different base -- restored, "
                  "reserve not applied\n");
    if (g_overcommit)
      debugPrintf("[boot] OVERCOMMIT on: heap shrunk to %u MB, freed %u MB physical; "
                  "arena reserved virtual @ %p (commit on demand)\n",
                  g_oc_heap_mb, g_oc_freed_mb, g_mmap_arena_base);
    else
      debugPrintf("[boot] OVERCOMMIT off (heap-backed): system_resource=%u MB "
                  "(svcMapPhysicalMemory needs >0; unsafe pool exhausted). map_hint=%d alias=%u MB\n",
                  (unsigned)(g_oc_sysres >> 20), g_oc_hint_map, g_oc_alias_mb);
  }

  /* Overcommit feasibility probe. Proper PROT_NONE overcommit on Switch needs a
   * physical-backing primitive (svcMapMemory in the stack region, or
   * svcMapPhysicalMemory in the alias region) plus a region large enough to hold
   * Unity's multi-GB reservations. MMX found svcMapMemory caps at ~2-3 pools (the
   * stack region is ~1GB). Log the region sizes + which mapping svc are granted so
   * we can size/choose the real overcommit (or rule it out) from real numbers. */
  {
    struct { const char *nm; int a, s; } R[] = {
      { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
      { "heap",  InfoType_HeapRegionAddress,  InfoType_HeapRegionSize  },
      { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
    };
    for (unsigned i = 0; i < 3; i++) {
      u64 a = 0, s = 0;
      svcGetInfo(&a, R[i].a, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&s, R[i].s, CUR_PROCESS_HANDLE, 0);
      debugPrintf("[probe] region %-5s base=0x%lx size=%u MB\n",
                  R[i].nm, (unsigned long)a, (unsigned)(s >> 20));
    }
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[probe] mem total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20));
    debugPrintf("[probe] svc hinted: MapPhysicalMemory(0x2c)=%d UnmapPhysical(0x2d)=%d "
                "MapMemory(0x24)=%d UnmapMemory(0x25)=%d\n",
                envIsSyscallHinted(0x2c), envIsSyscallHinted(0x2d),
                envIsSyscallHinted(0x24), envIsSyscallHinted(0x25));
  }

  /* Decisive probe: does svcMapMemory accept a dst in the (unmapped upper) HEAP
   * region? The stack region works but is only ~2GB (~8 regions). The heap region
   * is 8GB; its upper ~5GB sits unmapped above our heap. If svcMapMemory works
   * there too, we can host Unity's 256MB pools in ~7GB of backable address space
   * (~28 regions) without any libunity patching. Pure diagnostic: map 1 page,
   * verify the sentinel reads back, unmap. */
  {
    u64 hbase = 0, hsize = 0;
    svcGetInfo(&hbase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&hsize, InfoType_HeapRegionSize,    CUR_PROCESS_HANDLE, 0);
    u64 probe = 0, a = hbase, end = hbase + hsize;
    while (a < end) {
      MemoryInfo mi; u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
      if (mi.addr + mi.size <= a) break;
      if (mi.type == MemType_Unmapped && mi.size >= MMAP_ARENA_ALIGN) {
        u64 al = (mi.addr + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
        if (al + 0x1000 <= mi.addr + mi.size) { probe = al; break; }
      }
      a = mi.addr + mi.size;
    }
    if (probe) {
      void *src = memalign(0x1000, 0x1000);
      if (src) {
        *(volatile u32 *)src = 0xABCD1234;
        Result rc = svcMapMemory((void *)probe, src, 0x1000);
        if (R_SUCCEEDED(rc)) {
          u32 v = *(volatile u32 *)probe;
          Result u = svcUnmapMemory((void *)probe, src, 0x1000);
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx rc=0x%x read=0x%x unmap=0x%x WORKS=%d\n",
                      (unsigned long)probe, rc, v, u, v == 0xABCD1234);
          if (R_SUCCEEDED(u)) free(src);
        } else {
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx FAILED rc=0x%x\n",
                      (unsigned long)probe, rc);
          free(src);
        }
      }
    } else {
      debugPrintf("[heapprobe] no unmapped 256MB-aligned spot found in heap region\n");
    }
  }

  /* Arm the stack-region overcommit arena. The boot probe confirmed svcMapMemory
   * aliases heap pages into the stack region; Unity reserves ~2.8GB of PROT_NONE
   * pools but commits only ~80MB. Reserve a 1280MB stack-region window (cheap
   * address space) + a 256MB heap commit-pool; the OC arena (libc_shim.c) then
   * holds Unity's big reservations there and aliases pool pages in on mprotect.
   * Any failure leaves OC disabled and the engine runs on the heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);   // keep libnx thread stacks out
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      /* __real_memalign, NOT memalign: the wrapper routes any
       * (align >= 0x1000, size >= GPUA_MIN) request into the GPU arena, and this
       * request is both. The pool was therefore being carved out of the GPU
       * arena -- 896 of its 1152 MB -- which is why "arena 1017/1152" looked
       * like GPU pressure when real GPU buffers were only ~120 MB, and why
       * shrinking the pool by 384 MB dropped arena usage by exactly 384 MB.
       * It also poisoned g_gpu_live/bo-live, since the wrapper counts every
       * page-aligned allocation as a nouveau BO. */
      pool = __real_memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES)) {
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB "
                    "(total reserve %u MB)\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20),
                    (unsigned)((winsz + g_mmap_arena_size) >> 20));
        if (g_oc_win2 && g_oc_win2_sz) {
          virtmemLock();
          VirtmemReservation *rv2 = virtmemAddReservation(g_oc_win2, g_oc_win2_sz);
          virtmemUnlock();
          if (rv2 && oc_arena_add_window(g_oc_win2, g_oc_win2_sz))
            debugPrintf("[oc] ARMED window 2: %u MB @ %p (total window VA %u MB)\n",
                        (unsigned)(g_oc_win2_sz >> 20), g_oc_win2,
                        (unsigned)((winsz + g_oc_win2_sz) >> 20));
        }
      }
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* (Removed: an inherited ZOOKEEPER block that forced a PORTRAIT surface here
   * -- 1080x1920 docked / 720x1280 handheld. Lara Croft GO is landscape, and
   * android_native_update_mode() below sets the real geometry from
   * config.handheld_res / config.docked_res. The block was dead in practice
   * because update_mode overwrote it, but it left screen_width/height holding
   * portrait values for anything that read them in between.) */

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  /* (check_data() already ran during asset preparation above, before the pack
   * build -- validating there means a bad install is reported by name instead
   * of after several minutes of packing.) */

  /* load the three modules; libil2cpp resolves its engine calls against libunity
   * module-to-module during relocation. */
  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);
  /* hand the il2cpp exec base to the GC stop-the-world bridge in libc_shim.c so
   * our pthread_kill can ack the Boehm GC's (undeliverable) suspend/restart
   * signals via its semaphore at il2cpp+0x3213828 (see nx_patch_lcgo.h). */
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Firebase native libs are intentionally NOT loaded. The first scene's
   * FirebaseManager only advances when the managed dependency check reports
   * DependencyStatus.Available(0); on a Switch there is no Google Play Services,
   * so the real libs could never report that (they return UnavailableMissing)
   * AND libFirebaseCppApp crashes our loader in its JNI_OnLoad logging path. We
   * instead answer the SDK's native P/Invoke lookups with stubs (firebase_stub.c
   * via dlsym_fake) that make the check resolve to Available. The 4 .so files can
   * be deleted from sdmc:/switch/lcgo/. Firebase is cosmetic here (RemoteConfig
   * banner/news textures), so stubbing it costs only those images. */
  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  /* Resolve the Java->Unity callback channel now that libunity is relocated.
   * Export-based, so nothing to derive -- see unity_sendmessage.h. */
  nx_sendmsg_init();
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed (canary-patch disabled)\n");

  /* Patch libunity AFTER finalize/flush -- the same point every other libunity
   * patch below runs at. so_patch_code aliases the target pages via
   * svcMapProcessMemory, but the module's segments must be finalized (mapped
   * with their final RX perms and relocated) first; doing it right after
   * load_module faulted at boot (fbstub94: stock .so on SD, PC in the patch
   * path before finalize). */
  nx_patch_libunity((uintptr_t)unity_mod.load_virtbase);

  /* Force FMOD to use its native OpenSL ES output instead of Unity's Java
   * AudioTrack driver.
   *
   * Mechanism. Unity's AudioManager FMOD init calls FMOD::System::setOutput with
   * a requested FMOD_OUTPUTTYPE in w1, produced at +0x6f6db0 (`mov w1,w21`) --
   * derived for THIS build, see nx_patch_lcgo.h. setOutput then walks the
   * registered output list and matches the requested constant against each
   * entry's type field:
   *     AudioTrack = 21 (0x15)   <- the default request; needs the JVM run loop
   *     OpenSL ES  = 22 (0x16)   <- callback-driven, self-driving via our shim
   * The selector immediately above the patch site is visible in the disassembly
   * as `cmp w0,#3 / mov w8,#0x15 / mov w9,#0x17 / csel / cmp w0,#2 /
   * mov w9,#0x16 / csel w21,w9,w8,eq`, so without the patch the Java AudioTrack
   * output is selected and, with no JVM consumer, stays silent.
   *
   * Fix: rewrite the requested type at the setOutput call site from
   * "mov w1,w21" to "movz w1,#22", so Unity asks FMOD for OPENSL. FMOD finds the
   * registered OpenSL output (type 22), inits it -> dlopen(libOpenSLES.so) ->
   * slCreateEngine (our opensles.c shim) -> the engine drives its own callback
   * buffer queue. No Java handshake, correct lifecycle. The registration path is
   * left untouched (both AudioTrack and OpenSL register normally with their real
   * type fields). 0x2A1503E1 (mov w1,w21) -> 0x528002C1 (movz w1,#0x16). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    uint32_t req_opensl = LCGO_WORD_FMOD_OPENSL; /* movz w1, #22 (FMOD_OUTPUTTYPE OPENSL) */
    /* Lara Croft GO 40f1 offset 0x6f6db0 (byte-verified: currently 0x2a1503e1,
     * inside AudioManager::InitNormal per the 2022.3.40f1 symbol kit). Verify the
     * expected `mov w1,w21` before writing -- belt-and-suspenders, cannot corrupt. */
    if (*(volatile uint32_t *)(ub + LCGO_OFF_FMOD_OUTPUT) == LCGO_WORD_FMOD_STOCK) {
      so_patch_code((void *)(ub + LCGO_OFF_FMOD_OUTPUT), &req_opensl, sizeof req_opensl);
      debugPrintf("[fmod] output forced to OpenSL(22) @libunity+0x%x\n", LCGO_OFF_FMOD_OUTPUT);
    } else {
      debugPrintf("[fmod] SKIP force-OpenSL: +0x%x = 0x%08x, not `mov w1,w21` -- "
                  "libunity differs from expected 2022.3.40f1\n",
                  LCGO_OFF_FMOD_OUTPUT, *(volatile uint32_t *)(ub + LCGO_OFF_FMOD_OUTPUT));
    }
    /* Frame-pacing (Swappy) force-disable. PvZ registers Swappy (9 JNI entrypoints);
     * its init brings up a Choreographer/vsync-driven thread pool that never completes
     * on Switch -- there is no Android Choreographer to deliver frame callbacks -- so
     * engine-init parks in a pthread_join at frame 0 (verified on hardware: UnityMain
     * state=join for 18s, workers hard-parked in Swappy's 0xd6xxxx wait). libunity+
     * 0x62d8a0 is the cached "is frame-pacing enabled?" getter: 13 call sites, each
     * `bl 0x62d8a0 ; tbz w0,#0,<skip>`. Forcing it to return 0 makes every site take
     * the disabled path -> plain eglSwapBuffers, no pacing threads, no join. This is
     * how the Zookeeper base already boots (it never enables Swappy). Verify-first:
     * patch only if the prologue is the expected `stp x30,x19,[sp,#-0x10]!`. */
    if (*(volatile uint32_t *)(ub + LCGO_OFF_PACING_GETTER) == LCGO_WORD_PACING_STOCK) {
      uint32_t off_pacing[2] = { 0x52800000u /* mov w0,#0 */, 0xD65F03C0u /* ret */ };
      so_patch_code((void *)(ub + LCGO_OFF_PACING_GETTER), off_pacing, sizeof off_pacing);
      debugPrintf("[pace] frame-pacing (Swappy) force-disabled @libunity+0x%x\n", LCGO_OFF_PACING_GETTER);
    } else {
      debugPrintf("[pace] SKIP Swappy-disable: +0x%x = 0x%08x, not `stp x30,x19` -- "
                  "libunity differs (see PORTING)\n", LCGO_OFF_PACING_GETTER,
                  *(volatile uint32_t *)(ub + LCGO_OFF_PACING_GETTER));
    }

    /* Neutralise FMOD's OpenSL buffer-geometry validation.
     *
     * After slCreateEngine succeeds, FMOD's OpenSL init validates the output
     * period against the DSP mixer buffer. It reads {sampleRate,
     * framesPerBuffer} from the AudioManager getProperty values (our jni_fake.c:
     * 48000 / 64) and fails with FMOD error 60 ("Error initializing output
     * device") if sampleRate == 0, OR framesPerBuffer == 0, OR
     * framesPerBuffer > (dspNumBuffers-1)*dspBufferLength even after one halving.
     * dspNumBuffers (w20) / dspBufferLength (w21) come from the game's BAKED
     * AudioSettings. If the baked buffer is degenerate (dspNumBuffers == 1 makes
     * the bound (1-1)*len = 0, which no positive period can satisfy) then
     * reporting a smaller framesPerBuffer cannot help, so we force the bound.
     *
     * The validation in THIS build (derived, see nx_patch_lcgo.h):
     *   +0xdd5818  cbnz w8,+0xdd5828 / b +0xdd5850      <- sampleRate zero-guard
     *   +0xdd5824  cbz  w8,+0xdd5850                    <- (same, other path)
     *   +0xdd582c  cbz  w9,+0xdd5850                    <- framesPerBuffer guard
     *   +0xdd5830  sub w10,w20,#1 / mul w10,w10,w21     <- the bound
     *   +0xdd583c  b.ls +0xdd5848                       <- first check
     *   +0xdd5840  lsr w9,w9,#1 / str w9,[x19,#0x3f8]   <- one halving
     *   +0xdd584c  b.ls +0xdd585c                       <- terminal check (PATCHED)
     *
     * Patch the terminal compare-branch at +0xdd584c from `b.ls +0xdd585c`
     * (0x54000089) to an unconditional `b +0xdd585c` (0x14000004): the success
     * path always runs and builds the audio player from the sane
     * sampleRate/channels. The two zero-guards are left intact and pass (48000
     * and 64 are both non-zero). */
    uint32_t b_uncond = LCGO_WORD_BUFGEO_FORCE; /* b +0x10 (was b.ls) -- same local target */
    /* NOT DERIVED for this build. The Deus Ex GO port located this by an 8-word
     * buffer-halving loop containing `str w?,[x19,#0x3f8]`; that struct offset
     * does not match in this FMOD build, so LCGO_OFF_FMOD_BUFGEO is 0 and the
     * bypass is skipped entirely. Audio-off does not block boot or video, so
     * this is cosmetic until someone derives it. If audio initialises but stays
     * silent, look inside FMOD::OutputOpenSL::init (libunity+0xd0beb0). */
    if (LCGO_OFF_FMOD_BUFGEO &&
        *(volatile uint32_t *)(ub + LCGO_OFF_FMOD_BUFGEO) == LCGO_WORD_BUFGEO_STOCK) {
      so_patch_code((void *)(ub + LCGO_OFF_FMOD_BUFGEO), &b_uncond, sizeof b_uncond);
      debugPrintf("[fmod] OpenSL buffer-geometry check bypassed @libunity+0x%x\n", LCGO_OFF_FMOD_BUFGEO);
    } else {
      debugPrintf("[fmod] SKIP buffer-geometry bypass: +0x%x = 0x%08x, not b.ls -- "
                  "libunity differs from expected 2022.3.40f1\n",
                  LCGO_OFF_FMOD_BUFGEO, *(volatile uint32_t *)(ub + LCGO_OFF_FMOD_BUFGEO));
    }
  }


  /* The main thread runs init_array + the engine lifecycle; give it its own
   * stable bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  /* From here the EGL surface owns the display, so an on-screen fatal message is
   * no longer possible -- fatal_error() logs and exits instead of aborting
   * inside consoleInit(). See error.c. */
  fatal_error_display_busy(1);
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  /* re-assert the guard right before handing control to the engine, so no
   * intervening libnx/jni setup left tpidr in an unexpected state */
  install_bionic_tls(main_tls);

  /* drive the lifecycle the Java UnityPlayer would */
  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST. It runs jni::Initialize(),
   * which caches the JavaVM into libunity's internal JNI manager; without this
   * ScopedJNI/LocalScope inside initJni get a NULL JNIEnv and crash. It also
   * AttachCurrentThread()s and RegisterNatives() for each subsystem (our fake
   * env handles FindClass/RegisterNatives as safe no-ops). */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. il2cpp caches the VM in a
   * global it later checks; without it, il2cpp logs "Java VM not initialized"
   * and every managed AndroidJNI / AndroidJavaObject call (the Twitter SDK +
   * the SWIG-wrapped AppUtil module the first scene initializes) fails, hanging
   * scene load.
   *
   * We do NOT call libil2cpp's JNI_OnLoad: its first action is a log via
   * __android_log_print, whose GOT slot in libil2cpp is mis-bound (resolves to
   * a heap address -> Instruction Abort). Its only *essential* effects are two
   * global stores (verified by disassembling THIS PvZ 62f1c1 libil2cpp's
   * JNI_OnLoad @ 0x12ff760): cache the VM at il2cpp+0x2fff750, and store the JNI
   * handler fn-ptr (il2cpp+0x12ff7a4, which the reg-fn @0x1371358 writes) at
   * il2cpp+0x30007f8. Both land in this build's RW segment. Replicate the two
   * stores. Addresses derived in nx_patch_lcgo.h. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    *(void **)(b + LCGO_IL2CPP_VM_GLOBAL)   = fake_vm;
    *(void **)(b + LCGO_IL2CPP_HANDLER_PTR) = (void *)(b + LCGO_IL2CPP_HANDLER_FN);
    debugPrintf("[boot] il2cpp JavaVM global set (vm=%p)\n", fake_vm);
    nx_boot_il2cpp_hacks();   /* install Time.get_* hooks; GC handled by bridge */
    /* Must be armed here, not from the render loop: this title's first
     * Resources.Load calls happen during boot-scene script init, well before
     * frame 2. Installing into an unresolved cache slot is the expected case --
     * see the LCGO_RES_LOAD_* comment. */
    nx_install_res_trace();
    /* Same timing requirement as the Resources trace: the NRE this title throws
     * happens during boot-scene init, well before frame 1. */
    nx_install_nre_trace();
    nx_install_fps_override();
  }

  /* (Firebase native libs are not loaded; their JNI_OnLoad is neither needed nor
   * safe to call -- see the boot-time note above. The managed SDK's native calls
   * are answered by firebase_stub.c through dlsym_fake.) */

  debugPrintf("[boot] calling initJni...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* CRITICAL: on Android the Unity player loop only advances Update/coroutines/
   * animation when the app is RESUMED and FOCUSED. The Java UnityPlayer drives
   * this from onResume()/onWindowFocusChanged(). We had been calling only
   * initJni + gfx + render, so the engine stayed paused: it loaded the boot
   * scene and ran Awake/Start once (hence Firebase init), then rendered a frozen
   * frame forever without ticking a single Update or coroutine -- which is why
   * StartInitializer.InitUpdate was never called. Issue the resume + focus
   * transitions the lifecycle normally would before the render loop. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);

  /* GC off BEFORE the first frame -- see LCGO_DISABLE_IL2CPP_GC in
   * nx_patch_lcgo.h. This point is deliberate: after surface setup (so the GC
   * has finished its own init and gc_disable will not block on a lock held by a
   * thread that has not started) and before the first nativeRender (so no
   * allocation-triggered collection can race us). */
#if LCGO_DISABLE_IL2CPP_GC
  {
    void (*gc_set_mode)(int) =
        (void (*)(int))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    void (*gc_disable)(void) =
        (void (*)(void))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (gc_set_mode) gc_set_mode(1);      /* 1 == manual */
    if (gc_disable)  gc_disable();
    debugPrintf("[gc] il2cpp GC DISABLED before frame 0 (set_mode=%s disable=%s); "
                "UnloadUnusedAssets will no longer stop the world\n",
                gc_set_mode ? "ok" : "ABSENT", gc_disable ? "ok" : "ABSENT");
  }
#else
  debugPrintf("[gc] il2cpp GC left enabled -- stop-the-world handled by the "
              "pthread_kill bridge in libc_shim.c\n");
#endif

  debugPrintf("[boot] resumed + focus=true; entering render loop\n");
#if LCGO_HAVE_TIME_FIX
  nx_install_time_fix();   /* PvZ: hook Update + start clock thread BEFORE the first (blocking) nativeRender */
#endif

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");   // the thread that drives nativeRender
  /* Watchdog re-enabled (fbstub88) with the snapshot-ordering fix: stacks are
   * now walked while the target thread is PAUSED (the fbstub86 self-crash came
   * from resuming first and walking a live stack). 6s thresholds. Its job now:
   * catch the first-present hang and dump the thread blocked in eglSwapBuffers. */
  diag_watchdog_start();

  int frame = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);   // heartbeat: lets the watchdog see progress (or its absence)
    nx_time_tick();      // advance our managed-Time clock once per frame
    /* fbstub42: the engine clock is now fixed at its true source by the
     * TimeManager::Update entry hook (installed at boot, see nx_install_time_fix):
     * Update is re-driven each frame with newTime = GetTimeSinceStartup(), so all
     * deltaTime variants advance and the PreloadManager can integrate. No per-frame
     * field poking needed here. */
    android_native_update_mode();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame == 0) {
      /* fbstub42: install the native engine-clock fix first thing. Drives
       * TimeManager::Update with a live newTime so deltaTime / m_Time advance for
       * native readers (PreloadManager), unblocking async scene loads. */
      /* nx_install_time_fix() moved before the render loop (installs too late here:
       * PvZ's first nativeRender blocks on the scene load and never returns). */
      /* Time.get_* hooks are installed at boot (nx_boot_il2cpp_hacks, right after
       * the JavaVM global is set). This call is a self-skipping backstop only. */
      nx_boot_il2cpp_hacks();
    }
    /* Assert targetFrameRate once the player is up, then re-assert: a quality
     * change or scene load can reapply Unity's mobile default (30) underneath
     * us. Frame 2 rather than 0 so il2cpp and the player loop are fully live. */
    if (frame == 2 || (frame % 300) == 0) nx_force_target_framerate();
    /* Entitlements: install once the managed runtime is live. Frame 2 for the
     * same reason as the framerate assert -- il2cpp must be fully up before we
     * splice a managed thunk. */
    if (frame == 2) { extern void lcgo_entitlements_init(void); lcgo_entitlements_init(); }
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    /* Ask the managed runtime what it thinks exists. Must be here, after
     * nativeRender returned: this is the Unity main thread. */
    nx_managed_probe(frame);
    /* Opens the send window and runs the one-shot channel self-test. Must be
     * here for the same reason as the probe: this is the Unity main thread. */
    nx_sendmsg_frame(frame);
    /* Runs twice, late, after every other diagnostic has been flushed -- see
     * the LCGO_SCENE_CENSUS_LIST comment on why the timing is deliberate. */
    nx_scene_census(frame);
    frame++;
  }

  /* Commit any sensitivity change made inside the last 3 seconds -- the pointer
   * normally debounces its own save, so quitting quickly after a D-pad tweak
   * would otherwise lose it. */
  android_native_input_shutdown();

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
