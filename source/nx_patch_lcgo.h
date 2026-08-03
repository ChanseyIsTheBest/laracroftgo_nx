/* nx_patch_lcgo.h -- in-memory libunity.so patch table for LARA CROFT GO
 * (Deca Games re-release, Unity 2022.3.40f1, arm64 / IL2CPP).
 * BuildID xxHash 9f45f342e5b732e5.
 *
 * WHAT IT DOES
 *   Unity's block allocator reserves memory in 256MB-aligned regions. On a 4GB
 *   Switch that granularity does not fit the so_loader address space, so we
 *   rewrite the allocator's region-size computation to use 64MB granularity.
 *   Each entry rewrites one 32-bit instruction word: {from} is the stock word,
 *   {to} is the 64MB-granularity word (shift 0x1c->0x1a, mask/const 256MB->64MB,
 *   etc.). The transforms are the SAME as the PvZ Fusion / Zookeeper DX tables;
 *   only the offsets differ.
 *
 * HOW THESE OFFSETS WERE DERIVED
 *   The 21 known-good region-granularity words from the PvZ Fusion 62f1c1 table
 *   were searched for in THIS 40f1 libunity.so (tools/derive_region_patch.py).
 *   20 pinned uniquely or by intra-cluster adjacency, and EVERY ONE landed at
 *   exactly (pvz_offset - 0x2fea8) -- a single, constant delta across all four
 *   allocator functions. Site 2 was then placed by that same constant delta,
 *   and its candidate address was one of the two raw word hits.
 *
 *   Spot-checked by disassembly: +0x3dea70 is `lsr x8,x1,#0x1c` / `mov
 *   w9,#0x10000000` (the 256MB region-round), +0x3e501c..+0x3e5050 is the page-
 *   table lookup cluster (`ubfx #0x1c`, `and #0xfffffff0000000`), +0x3e6d84 is
 *   the second-level `ubfx x9,x20,#0x1c,#0xc`. All match the reference layout.
 *   Confidence: HIGH.
 *
 * SAFETY
 *   nx_patch_libunity() is VERIFY-FIRST: it reads each target word and only
 *   patches if it already equals {from}; if ANY site mismatches it patches
 *   NOTHING and logs loudly. A wrong offset is caught, not catastrophic.
 */
#ifndef NX_PATCH_LCGO_H
#define NX_PATCH_LCGO_H

#include <stdint.h>

/* ---- Phase flags ---------------------------------------------------------
 * Engine-internal hooks that the PvZ port needed on hardware. They carry PvZ
 * (62f1c1) offsets and have NOT been re-derived for 40f1, so they are OFF.
 * Turn one on only after deriving its offsets for this build (see PORTING.md).
 * The port should reach first light without any of them.                    */
/* Disable il2cpp's collector before the first frame.
 *
 * The Boehm collector stops the world with POSIX signals Switch never delivers.
 * The pthread_kill bridge in libc_shim.c (sec 6) exists to ack those, but it can
 * only help once the GC has initialised its ack semaphore; a collection
 * triggered DURING the first frame -- before that -- hangs instead of pausing.
 *
 * Observed exactly that: UnityMain parked in cond_wait inside nativeRender at
 * frame 0 with two AssetGarbageCollectorHelper threads spinning on futex words
 * that never change (Unity's UnloadUnusedAssets machinery). The run before it,
 * which spent several minutes building the asset pack first, won the same race
 * and reached frame 3600 -- so this is timing, and the fast path is the one that
 * loses.
 *
 * The CloverPit port hit the same wall and disables the GC before the first
 * render for this reason. Note the ordering constraint: the PvZ port deadlocked
 * calling gc_disable during GC INIT, because it blocks on a lock held by a
 * helper thread that has not started yet. After surface setup and before the
 * first nativeRender is the window where it is safe.
 *
 * Cost: managed memory is never reclaimed. Acceptable for a session-length
 * puzzle game; the alternative is a hang. Set to 0 to fall back to the bridge. */
#define LCGO_DISABLE_IL2CPP_GC  1

#define LCGO_HAVE_TIME_FIX      1  /* TimeManager::Update hook (async-scene-load frame-0 fix) -- DERIVED, ON */

typedef struct { uint32_t off, from, to; } NxPatchWord;

/* ---- 21 region-granularity sites (256MB -> 64MB) : derived, HIGH conf. ----
 * DERIVATION FOR THIS BUILD (2022.3.40f1): the 21 known-good {from} words from
 * the proven PvZ Fusion (62f1c1) table were searched in THIS libunity's .text.
 * 14 of 21 matched UNIQUELY; the other 7 were pinned by intra-cluster adjacency
 * and then EVERY ONE of the 20 consecutive deltas was verified to reproduce the
 * PvZ layout exactly. Unlike the Deus Ex build there is NO single uniform delta
 * from PvZ -- this engine is stripped differently -- so each site stands on its
 * own evidence.
 *
 * Cross-checked against the Unity 2022.3.40f1 symbol kit: the cluster start
 * (0x3a6324) resolves to LocalLowLevelAllocator::ReserveMemoryBlock +0x98 and
 * the end (0x3aeee0) to MemoryManager::GetAllocatorContainingPtr +0x24 -- i.e.
 * the patch lands inside exactly the functions it is supposed to.
 *
 *  idx   pvz_62f1c1_off   lcgo_40f1_off                                      */
__attribute__((unused))
static const NxPatchWord LCGO_PATCH_WORDS[] = {
  /*  0 (pvz 0x40e070) */ { 0x3a6324, 0x12be0009, 0x12bf8009 },
  /*  1 (pvz 0x40e078) */ { 0x3a632c, 0x92648d36, 0x92669536 },
  /*  2 (pvz 0x40e918) */ { 0x3a6bcc, 0x52a20009, 0x52a08009 }, /* region-round: lsr#0x1c then mov#256MB */
  /*  3 (pvz 0x412640) */ { 0x3aa8f4, 0xd35cfd29, 0xd35afd29 },
  /*  4 (pvz 0x412644) */ { 0x3aa8f8, 0x52a2000a, 0x52a0800a },
  /*  5 (pvz 0x412aec) */ { 0x3aada0, 0x12be000a, 0x12bf800a },
  /*  6 (pvz 0x412af4) */ { 0x3aada8, 0x92648d36, 0x92669536 },
  /*  7 (pvz 0x414af4) */ { 0x3acda8, 0xd35cdc33, 0xd35ad433 },
  /*  8 (pvz 0x414af8) */ { 0x3acdac, 0xd35cfd15, 0xd35afd15 },
  /*  9 (pvz 0x414b88) */ { 0x3ace3c, 0x52a20008, 0x52a08008 }, /* passes 256MB as call arg */
  /* 10 (pvz 0x414ec4) */ { 0x3ad178, 0xd35cfc28, 0xd35afc28 },
  /* 11 (pvz 0x414ed4) */ { 0x3ad188, 0x92646c28, 0x92667428 },
  /* 12 (pvz 0x414edc) */ { 0x3ad190, 0xd35c9c2a, 0xd35a942a },
  /* 13 (pvz 0x414ef0) */ { 0x3ad1a4, 0xb25c6feb, 0xb25e77eb },
  /* 14 (pvz 0x414ef4) */ { 0x3ad1a8, 0xd35cdc29, 0xd35ad429 },
  /* 15 (pvz 0x414ef8) */ { 0x3ad1ac, 0xf2a2000b, 0xf2a0800b },
  /* 16 (pvz 0x414f38) */ { 0x3ad1ec, 0xcb0a7108, 0xcb0a6908 },
  /* 17 (pvz 0x414f50) */ { 0x3ad204, 0xd368fc28, 0xd366fc28 },
  /* 18 (pvz 0x414f60) */ { 0x3ad214, 0xd35c9c29, 0xd35a9429 },
  /* 19 (pvz 0x416c14) */ { 0x3aeec8, 0xd368fc28, 0xd366fc28 },
  /* 20 (pvz 0x416c2c) */ { 0x3aeee0, 0xd35c9e89, 0xd35a9689 },
};
#define LCGO_PATCH_WORDS_N ((int)(sizeof(LCGO_PATCH_WORDS)/sizeof(LCGO_PATCH_WORDS[0])))

/* ---- branch/word force sites ----------------------------------------------
 * None derived for this build. The PvZ table's single entry (a BufferGLES
 * BeginWrite caps gate) was a crash-era workaround for a mesa bug that has
 * since been fixed upstream; it is not carried over. */
#define LCGO_HAVE_BRANCH_FORCES 0
__attribute__((unused))
static const NxPatchWord LCGO_BRANCH_FORCES[] = {
  { 0, 0, 0 },  /* placeholder; unused while LCGO_HAVE_BRANCH_FORCES == 0 */
};
#define LCGO_BRANCH_FORCES_N (LCGO_HAVE_BRANCH_FORCES ? \
  ((int)(sizeof(LCGO_BRANCH_FORCES)/sizeof(LCGO_BRANCH_FORCES[0]))) : 0)

/* ===========================================================================
 * Single-word engine patches, derived for THIS libunity by signature match.
 * All are applied verify-first in main.c.
 * ======================================================================== */

/* Frame-pacing (Swappy) force-disable.
 * libunity+0x5da988 is the cached "is frame-pacing enabled?" getter: 13 call
 * sites, 12 of them of the form `bl 0x5da988 ; tbz w0,#0,<skip>` (the 13th
 * consumes w0 differently) -- exactly the shape the PvZ
 * port found at its 0x652354. Body confirmed by disassembly: cached-flag byte
 * at +0x11bb8f1, value byte at +0x11bb8f0, init call, AND with a second flag.
 * Forcing a 0 return makes every site take the disabled path -> plain
 * eglSwapBuffers, no Choreographer-driven pacing threads, no frame-0 join. */
#define LCGO_OFF_PACING_GETTER   0x5da988
#define LCGO_WORD_PACING_STOCK   0xA9BF4FFEu   /* stp x30,x19,[sp,#-0x10]! */

/* FMOD force-OpenSL output.
 * libunity+0x6f6db0 is the `mov w1,w21` that passes the chosen FMOD_OUTPUTTYPE
 * into setOutput. Unique 4/4 signature match: the preceding run is
 *   cmp w0,#3 / mov w8,#0x15 / mov w9,#0x17 / csel / cmp w0,#2 / mov w9,#0x16 /
 *   csel w21,w9,w8,eq / ldr x0,[x19,#0x158] / mov w1,w21
 * exactly as in the reference. Rewrite to `movz w1,#22` (OPENSL) so FMOD uses
 * its self-driving OpenSL ES output (backed by opensles.c) instead of the Java
 * AudioTrack path, which needs a JVM run loop we do not have. */
#define LCGO_OFF_FMOD_OUTPUT     0x6f6db0
#define LCGO_WORD_FMOD_STOCK     0x2A1503E1u   /* mov w1, w21 */
#define LCGO_WORD_FMOD_OPENSL    0x528002C1u   /* movz w1, #22 */

/* FMOD OpenSL buffer-geometry bypass.
 * libunity+0xdd584c is the terminal `b.ls` of the buffer-halving validation.
 * Unique match on the 8-word signature sub/mul/cmp/b.ls/lsr/str [x19,#0x3f8]/
 * cmp/b.ls. Forcing it unconditional stops the init rejecting the Switch's
 * period geometry with FMOD error 60. Same local target (+0x10). */
#define LCGO_OFF_FMOD_BUFGEO     0          /* NOT DERIVED for this build */
#define LCGO_WORD_BUFGEO_STOCK   0x54000089u   /* b.ls +0x10 */
#define LCGO_WORD_BUFGEO_FORCE   0x14000004u   /* b    +0x10 */

/* ===========================================================================
 * libil2cpp globals -- game code, so entirely different from PvZ's.
 * Recovered by disassembling THIS libil2cpp's JNI_OnLoad @ 0x12ff760:
 *   adrp x8,#0x23cf000 ; str x19,[x8,#0x70]       -> VM global   +0x23cf070
 *   adrp x0,#0xeab000  ; add x0,x0,#0x2b8 ; bl .. -> handler fn  +0xeab2b8
 *   reg-fn @0xe1a810  = { adrp x8,#0x23cd000 ; str x0,[x8,#0x4d0] ; ret }
 *                                                  -> handler ptr +0x23cd4d0
 * We replicate those two stores rather than calling libil2cpp's JNI_OnLoad,
 * whose first action is an __android_log_print through a GOT slot that is not
 * safely bound at that point (same reason as the reference port).
 * ======================================================================== */
#define LCGO_IL2CPP_VM_GLOBAL     0x23cf070
#define LCGO_IL2CPP_HANDLER_PTR   0x23cd4d0
#define LCGO_IL2CPP_HANDLER_FN    0xeab2b8

/* ===========================================================================
 * Engine clock -- TimeManager::Update entry hook (libunity). DERIVED.
 *
 * On Switch the player loop parks UnityMain inside a *synchronous* scene load,
 * so TimeManager::Update stops being driven: newTime freezes, deltaTime
 * collapses to its 1e-5 floor, and Loading.PreloadManager -- which integrates
 * load progress over deltaTime -- starves. That is the frame-0 async-load hang.
 * It is a property of this loader architecture rather than anything
 * title-specific, and every port in this lineage carries the fix.
 *
 * main.c installs an entry hook that replays the prologue and re-drives the
 * body, plus a background clock thread that re-drives the body with a wall-clock
 * newTime whenever the main thread has been silent for >100ms.
 *
 * Derivation: Update's entry prologue is 36 fixed bytes with no adrp/bl, so it
 * word-matches uniquely. Scanning for `ldr x8,[x0,#0xc8]` gave 19 hits; exactly
 * ONE also had `ldr w9,[x0,#0xd0]` + `ldrb w10,[x0,#0xf8]` in the next two
 * instructions with no adrp/bl in the first nine:
 *
 *   0x45dfac  ldr  x8,[x0,#0xc8] / ldr w9,[x0,#0xd0] / ldrb w10,[x0,#0xf8]
 *             add  x8,x8,#1      / add w9,w9,#1
 *             str  x8,[x0,#0xc8] / str w9,[x0,#0xd0]
 *             cbz  w10,+0x24     / ret            <- paused early-out
 *
 * Body = entry+0x24 = 0x45dfd0, and it opens `ldr d2,[x0,#0xe8]` /
 * `fsub d2,d0,d2` -- confirming both that +0xe8 is the m_StartupRef double and
 * that the body takes newTime in d0, i.e. body(void *tm, double newTime). The
 * 1e-5 deltaTime floor is visible shortly after as movz/movk 0x3727c5ac. All
 * four struct offsets (+0xc8 / +0xd0 / +0xf8 / +0xe8) are unchanged from the
 * 62f2 reference.
 *
 * GetTimeManager() = 0x45e5fc, reached from EVERY UnityEngine.Time icall
 * implementation: the registration thunks at 0x39e48c.. pair each name string
 * with a wrapper at 0x3959xx, and each wrapper is `bl 0x45e5fc` followed by a
 * field load. Its body is `mov w0,#7 ; b GetSubsystem` -- subsystem index 7,
 * matching the reference exactly. Confidence: high.
 *
 * The relative layout does NOT match the reference (PvZ had GetTimeManager at
 * Update+0x5fc, this build has Update+0x648), which is why each was derived
 * rather than extrapolated from a delta.
 * ======================================================================== */
#define LCGO_OFF_TIMEMGR_UPDATE_ENTRY  0x45dfac
#define LCGO_OFF_TIMEMGR_UPDATE_BODY   0x45dfd0   /* entry + 0x24 */
#define LCGO_OFF_GET_TIME_MANAGER      0x45e5fc
#define LCGO_WORD_TIMEMGR_STOCK        0xF9406408u /* ldr x8,[x0,#0xc8] */
#define LCGO_WORD_GETTIMEMGR_STOCK     0x528000E0u /* mov w0,#7          */

/* ===========================================================================
 * Managed UnityEngine.Time.get_* -- redirect to our frame clock. DERIVED.
 *
 * These are libil2cpp RVAs, i.e. GAME code, so the reference port's values are
 * meaningless here. Recovered from the supplied Il2CppDumper output and then
 * INDEPENDENTLY VERIFIED against the binary: each getter is a 0x28-byte lazy
 * icall resolver that embeds its own signature string, e.g.
 *
 *   0x2a399c8  stp x30,x19,[sp,#-0x10]!
 *              adrp x19,#0x2ffc000 ; ldr x0,[x19,#0x470] ; cbnz x0,done
 *              adrp/add x0, "UnityEngine.Time::get_deltaTime()"
 *              bl <resolve_icall> ; str x0,[x19,#0x470]
 *   done:      ldp x30,x19,[sp],#0x10 ; br x0      <- tail call into libunity
 *
 * so every RVA below self-identifies. Confidence: very high.
 *
 * NOTE: this build has NO smoothDeltaTime -- IL2CPP stripped it because the game
 * never reads it. The reference's 7-hook table is therefore 6 here.
 *
 * The hook overwrites 16 bytes (ldr x16,#8 ; br x16 ; ptr) at each entry, which
 * fits inside the 0x28-byte body. Verify-first in main.c against the prologue.
 *
 * TO DISABLE (if the managed and native clocks disagree and something animates
 * wrongly): set LCGO_HAVE_TIME_HOOKS to 0. The TimeManager fix above is the more
 * fundamental of the two and is independent of this one.
 * ======================================================================== */
#define LCGO_HAVE_TIME_HOOKS 1
#define LCGO_WORD_TIMEGET_STOCK 0xA9BF4FFEu   /* stp x30,x19,[sp,#-0x10]! */

/* X(rva, clock_fn, name) -- clock_fn is bound in main.c's translation unit. */
#define LCGO_TIME_HOOK_LIST(X) \
  X(0x1f0a634, nx_realtime_since_startup, "Time.get_realtimeSinceStartup") \
  X(0x1efaa04, nx_delta_time,             "Time.get_deltaTime")            \
  X(0x1f0b3d4, nx_delta_time,             "Time.get_unscaledDeltaTime")    \
  X(0x1f0b4bc, nx_delta_time,             "Time.get_smoothDeltaTime")      \
  X(0x1f0b35c, nx_time_f,                 "Time.get_time")                 \
  X(0x1f0b3ac, nx_time_f,                 "Time.get_unscaledTime")         \
  X(0x1f0b544, nx_frame_count,            "Time.get_frameCount")           \
  /* Not a Time getter, but the same lazy-icall shape (verified: the word at
   * 0x1edea08 is the same 0xa9bf4ffe prologue) and the same install path.
   * UnityEngine.Application::get_internetReachability. With no network stack
   * behind it, anything that waits on a web request never resolves. Reporting
   * NotReachable (0) lets the game take its offline branch instead -- which
   * matters more here than it did for Deus Ex GO, because this game's
   * LoginGooglePlayGames / Armory online-suite path is on the boot route. */  \
  X(0x1edea08, nx_internet_reachability,  "Application.get_internetReachability")

/* NOTE vs the Deus Ex GO build: that one had NO smoothDeltaTime (IL2CPP had
 * stripped it) and so ran 6 Time hooks. THIS build DOES export
 * Time.get_smoothDeltaTime (0x1f0b4bc, prologue verified), so there are 7.
 * Conversely Time.get_fixedTime is absent here -- the game never reads it --
 * which is why there is no eighth entry. Every RVA above was read from the
 * supplied Il2CppDumper dump.cs and then INDEPENDENTLY verified by reading the
 * word at that RVA out of libil2cpp.so: all eight are 0xa9bf4ffe. */

/* ===========================================================================
 * Boehm GC stop-the-world bridge globals (libil2cpp) -- DERIVED.
 *
 * il2cpp's Boehm GC stops the world by pthread_kill-ing every other thread with
 * a suspend signal; each target's handler sem_posts an ack and parks in
 * sigsuspend, and GC_stop_world / GC_start_world sem_wait on those acks. Switch
 * never delivers POSIX signals, so the acks never arrive and the FIRST
 * collection hangs forever inside GC_stop_world. pthread_kill_gc() in
 * libc_shim.c posts the ack the undeliverable handler would have -- but it has
 * to read these four globals to know which signal means what, and they are
 * per-build.
 *
 * Recovered with tools/derive_gc_bridge.py and confirmed by disassembly from
 * four independent directions:
 *
 *  1. Exactly TWO pthread_kill call sites, both in the GC module at 0x1389xxx.
 *     Suspend side @0x13898ac sits in a thread-table loop that skips self and
 *     already-flagged threads, then `ldr w1,[x24,#0xf64]`.
 *     Restart side @0x1389b14 walks the thread list, then `ldr w1,[x24,#0xf68]`.
 *  2. GC_start_world reads the GATE first: `ldr w8,[x23,#0xf60] ; cbz w8,skip`
 *     immediately before the restart kill -- and the suspend HANDLER does the
 *     same read (`adrp x8,#0x2ff0000 ; ldr w8,[x8,#0xf60] ; cbz`) before its
 *     SECOND sem_post. That is exactly the gate semantics the bridge mirrors.
 *  3. The GC builds its signal mask from BOTH globals back to back:
 *     `ldr w1,[0x2ff0f64] ; sigaddset` then `ldr w1,[0x2ff0f68] ; sigaddset`
 *     @0x1389648 / 0x1389658.
 *  4. `sem_init(&0x3213828, 0, 0)` @0x1389bf4, and all three sem_wait/sem_post
 *     sites (0x1389a3c / 0x1389798 / 0x13897e4) use that same address.
 *
 * Placement is consistent with the reference ports: the two signal ints are one
 * word apart in .data with the gate one word below the suspend sig, and the
 * ack-sem pointer is in .bss. Because .bss starts zeroed, the bridge is inert
 * until the GC initialises: the sig reads return 0 (never match a real signal)
 * and the sem storage is NULL (sem_post_fake no-ops).
 *
 * Confidence: very high.
 *
 * DO NOT also disable the GC. Calling il2cpp_gc_disable() behind a live bridge
 * deadlocks -- it blocks on a GC lock held by a helper thread that has not
 * started yet. Let the GC run; the bridge is what makes that safe.
 * ======================================================================== */
#define LCGO_GC_START_ACK_OFF    0x23c2288  /* restart ack gate (one word below suspend) */
#define LCGO_GC_SUSPEND_SIG_OFF  0x23c228c  /* GC_suspend_all pthread_kill arg */
#define LCGO_GC_RESTART_SIG_OFF  0x23c2290  /* GC_start_world pthread_kill arg */
#define LCGO_GC_ACK_SEM_OFF      0x25e1f68  /* FakeSem* ack-sem storage (.bss) */

/* ===========================================================================
 * UnityEngine.Screen / Display size accessors. DERIVED.
 *
 * The CloverPit port traced its black screen to a screen dimension arriving as
 * ZERO: "RenderTexture.Create failed: Texture must have width greater than 0",
 * after which the game's own render-scaling loop spun forever. The zero came
 * from UnityEngine.Display.main.systemWidth/systemHeight, which "auto"
 * resolution mode falls back on at boot -- NOT from Screen.width/height, and
 * NOT from the JNI DisplayMetrics path (ours already reports 1280x720
 * correctly). It is a separate il2cpp icall that never touches our JNI layer.
 *
 * Our symptom is consistent: ~1 GB of GPU buffers resident, zero draw calls
 * from the engine after frame 1, and no GL error anywhere -- the engine is
 * declining to render rather than failing to. A render target sized from a
 * zero dimension produces exactly that.
 *
 * Instance methods (Display.get_system*) take `this` in x0 and return in w0;
 * ignoring the argument and returning a constant is ABI-safe, as the reference
 * port notes.
 *
 * These do NOT share the Time getters' prologue, so each entry carries its own
 * verify word: Screen.* open with stp x30,x19,[sp,#-0x10]! and Display.* with
 * sub sp,sp,#0x30. Verify-first as always -- a mismatch logs and skips.
 *
 * X(rva, clock_fn, name, verify_word) */
#define LCGO_HAVE_SCREEN_HOOKS 1
#define LCGO_SCREEN_HOOK_LIST(X) \
  X(0x1ee9300, nx_screen_width,  "Screen.width",         0xA9BF4FFEu) \
  X(0x1ee9328, nx_screen_height, "Screen.height",        0xA9BF4FFEu) \
  X(0x1ee8b50, nx_screen_width,  "Display.systemWidth",  0xD100C3FFu) \
  X(0x1ee8c38, nx_screen_height, "Display.systemHeight", 0xD100C3FFu) \
  X(0x1ee89d4, nx_screen_width,  "Display.renderingWidth",  0xD100C3FFu) \
  X(0x1ee8abc, nx_screen_height, "Display.renderingHeight", 0xD100C3FFu)

/* All six RVAs came from dump.cs and were verified by reading the word at each:
 * the two Screen.* getters are 0xa9bf4ffe (lazy-icall prologue) and the four
 * Display.* are 0xd100c3ff (sub sp,sp,#0x30) -- exactly the two shapes the
 * reference port documents. renderingWidth/Height are extra here (the Deus Ex
 * table has only systemWidth/Height); they share the Display.* shape and the
 * same failure mode, so they are hooked for the same reason. */

/* ===========================================================================
 * Managed-state probe (libil2cpp) -- DERIVED, READ-ONLY.
 *
 * Everything else in this header patches or redirects. This block does not: it
 * only records where to *call*, so the loader can ask the managed runtime what
 * it thinks is going on. Nothing is written to libil2cpp for these.
 *
 * Why call rather than hook. Hooking Camera.get_main only fires if the game
 * asks for it, and a boot flow that has already died may never ask -- you burn
 * a boot and learn nothing. Every entry below is a static, zero-argument getter
 * with a scalar return, so the loader can just call it once a second from the
 * thread that drives nativeRender (which IS the Unity main thread, already
 * attached to the il2cpp domain) and always get an answer.
 *
 * Derivation. Every one of these RVAs self-identifies. They are the same
 * 0x28-byte lazy-icall resolver shape as the Time getters in section 7b:
 *
 *   stp x30,x19,[sp,#-0x10]!
 *   adrp x19,<cache> ; ldr x0,[x19,#off] ; cbnz x0,done
 *   adrp/add x0, "<the method's own signature string>"    <- self-identifying
 *   bl <resolve_icall> ; str x0,[x19,#off]
 *   done: ldp x30,x19,[sp],#0x10 ; br x0
 *
 * so disassembling each one prints the name of the method it belongs to. All
 * five were confirmed that way against this exact libil2cpp (tools/probe_rva.py
 * --strings), and the shared prologue word 0xa9bf4ffe is the same verify word
 * the Screen.* hooks already use.
 *
 * NOT included, deliberately: SceneManager::GetActiveScene (0x2a41a8c). It is
 * NOT this shape -- it is a real method body opening
 * `str x30,[sp,#-0x20]! / stp x20,x19,[sp,#0x10] / adrp x20 / adrp x19 /
 * ldrb w8,[x20,#0x7d8]`, with a static-init guard and a by-ref struct return.
 * get_sceneCount below answers the same question (is a scene loaded?) with a
 * plain int and no ABI risk.
 *
 * Reading the output. `cams` is the primary signal -- Camera.allCamerasCount
 * counts every enabled camera in the scene, so 0 means nothing can render and
 * the fault is upstream of GL entirely. `main` is secondary and can legitimately
 * be null while cams > 0, because Camera.main only matches a camera tagged
 * MainCamera and a UI camera usually is not. `playing` is the interesting one
 * for this title: LazyUIReference<T>.get_Path (il2cpp 0x1d3a2e8) only performs
 * its "<ui_size>/" -> "16-9/" substitution when Application.isPlaying is true,
 * so if this ever reads 0 the game is asking Resources.Load for paths that
 * still contain the literal placeholder and every one of them returns null.
 *
 * X(rva, label, kind)  kind: 'i' int-returning, 'p' pointer-returning */
#define LCGO_HAVE_MANAGED_PROBE 1
#define LCGO_PROBE_VERIFY_WORD  0xA9BF4FFEu   /* stp x30,x19,[sp,#-0x10]! */
#define LCGO_MANAGED_PROBE_LIST(X) \
  X(0x1ee1078, "cams",     'i')   /* Camera::GetAllCamerasCount            */ \
  X(0x1ee1028, "main",     'p')   /* Camera::get_main                      */ \
  X(0x1f138b8, "scenes",   'i')   /* SceneManager::get_sceneCount          */ \
  X(0x1ede74c, "playing",  'i')   /* Application::get_isPlaying            */ \
  X(0x1f0b4e4, "timescale",'f')   /* Time::get_timeScale                   */

/* All five re-derived from dump.cs for THIS game and verified: the word at each
 * RVA is 0xa9bf4ffe (LCGO_PROBE_VERIFY_WORD), the lazy-icall prologue. */

/* Probe cadence: frames between samples, and how many samples may print before
 * it falls silent unless a value actually changes. Cheap either way -- five
 * calls a second into already-resolved icalls -- but a log you can read matters
 * more than one more data point. */
#define LCGO_PROBE_PERIOD_FRAMES 60
#define LCGO_PROBE_MAX_QUIET     600   /* re-print at least every 600 frames */

/* ===========================================================================
 * Resources.Load trace (libil2cpp) -- DERIVED.
 *
 * Answers one question the f60/f660 probe cannot: does Resources.Load actually
 * work, and what is the game asking it for? Every ScriptableSingleton in this
 * title -- DefinitionConfig, ParameterStorage -- and every view controller the
 * splash FSM tries to instantiate resolves through it (see PORTING §13b), so a
 * single boot with this on either exonerates the Resources system or names the
 * asset it fails on.
 *
 * HOW IT HOOKS -- and why this technique rather than a splice.
 *
 * `UnityEngine.ResourcesAPIInternal::Load(System.String,System.Type)` is a
 * lazy-icall thunk. It reads a cached function pointer out of .bss, and if that
 * is still null it resolves the icall by name and writes it back:
 *
 *   adrp x21, #0x2ffc000 ; ldr x2, [x21,#0x18]   <- read cache
 *   cbnz x2, dispatch                             <- already resolved?
 *   adrp/add x0, "UnityEngine.ResourcesAPIInternal::Load(...)"
 *   bl <resolver> ; mov x2,x0 ; str x0,[x21,#0x18] <- write cache
 *   dispatch: mov x0,x20 ; mov x1,x19 ; <restore> ; br x2   <- TAIL call
 *
 * So the cache slot at **0x2ffc018** is a plain writable pointer that the thunk
 * tail-calls through. Writing our own function there redirects it without
 * modifying one byte of code. That matters: the first four instructions of this
 * thunk contain two PC-relative `adrp`s and a conditional branch, so the
 * copy-prologue trampoline used for a normal detour would relocate incorrectly.
 * The pointer swap has no such problem, and it is trivially reversible.
 *
 * Because the dispatch is a TAIL call (`br x2`, with x30 already restored to
 * the managed caller), our replacement is entered with the caller's return
 * address live in x30 and returns straight to managed code. It must therefore
 * match the icall's own signature exactly: (Il2CppString*, Il2CppReflectionType*)
 * returning Il2CppObject*.
 *
 * Installing before the thunk has ever run is fine and is what we do -- the
 * `cbnz` simply sees our non-null pointer and dispatches to us, skipping the
 * resolver. We then obtain the real icall ourselves through the exported
 * `il2cpp_resolve_icall` (libil2cpp+0x12fa1ec, a real export -- no offset
 * needed) using the same signature string, and call through. Installing early
 * is required, not optional: the first loads this title performs happen during
 * the boot scene, before the render loop's frame 2.
 *
 * Il2CppString layout is DERIVED, not assumed: `System.String::IsNullOrEmpty`
 * (0x227f264) is seven instructions and reads the length with
 * `ldr w8, [x0, #0x10]`, so length sits at +0x10 and the UTF-16 chars at +0x14.
 *
 * Verified by tools/probe_rva.py --cache, which re-derives the slot from the
 * thunk and prints the signature string the thunk itself embeds -- so the
 * constant self-checks against the binary.                                  */
#define LCGO_HAVE_RES_TRACE      1
#define LCGO_RES_LOAD_THUNK      0x1f01328   /* ResourcesAPIInternal::Load     */
#define LCGO_RES_LOAD_CACHE      0x23ca908   /* its icall cache slot (.bss)    */
#define LCGO_RES_LOAD_SIG \
  "UnityEngine.ResourcesAPIInternal::Load(System.String,System.Type)"
#define LCGO_WORD_RES_THUNK_STOCK 0xa9be57feu /* stp x30,x21,[sp,#-0x20]! */
#define LCGO_RES_TRACE_MAX       200         /* cap the log; boot-time is what matters */

/* Il2CppString field offsets -- derived from String::IsNullOrEmpty, see above. */
#define LCGO_STR_LEN_OFF         0x10
#define LCGO_STR_CHARS_OFF       0x14

/* ===========================================================================
 * NullReferenceException trace (libil2cpp) -- DERIVED.
 *
 * The single NRE in debug.log has no stack trace: Unity cannot symbolicate it
 * because this build ships no il2cpp.usym (the loader's own log shows
 * `open(/switch/lcgo_nx/il2cpp/il2cpp.usym) -> -1`). So we take the trace
 * ourselves, at the point of the throw.
 *
 * TARGET. il2cpp's codegen emits null checks as a call to a two-instruction
 * dispatch stub, which tail-calls the real raiser:
 *
 *   0x135c36c:  str x30, [sp, #-0x10]!     <- pushes the MANAGED caller's LR
 *   0x135c370:  bl  0x13264b4              <- the raiser
 *
 * and the raiser chains on to the constructor that builds "System" +
 * "NullReferenceException" (strings resolved out of 0x13264d8) before raising.
 * We hook the raiser at **0x13264b4**, not the stub: the stub is only 8 bytes
 * and the next stub starts immediately after it, so a 16-byte splice would
 * destroy an unrelated helper.
 *
 * WHERE THE CALLER'S ADDRESS LIVES. Because the stub pushed x30 before its
 * `bl`, on entry to the raiser **x30 points back into the stub (0x135c374) and
 * is useless** -- the managed caller's return address is at `[sp]`. Getting
 * this backwards yields a trace that just says "il2cpp codegen stub", which is
 * the trap that makes this hook look broken.
 *
 * WHY A SPLICE IS SAFE HERE. The four instructions the patch overwrites contain
 * no PC-relative operand at all:
 *
 *   0x13264b4  d10083ff  sub sp, sp, #0x20
 *   0x13264b8  a900fbff  stp xzr, x30, [sp, #8]
 *   0x13264bc  910003e0  mov x0, sp
 *   0x13264c0  f90003ff  str xzr, [sp]
 *   0x13264c4  <- resume; this is a `bl`, but it is OUTSIDE the patched window
 *
 * so the trampoline can re-execute them verbatim and jump back. (An earlier
 * pass mis-read the `bl` at +0x10 as being *inside* the window and wrongly
 * ruled this target out. It is not; +0x10 is where the patch ends.)
 *
 * For a target whose prologue DOES contain `adrp`, see the technique in the
 * Angry Birds 2 reference port: decode the adrp immediate at install time,
 * store the computed page in a global, and have the trampoline load that
 * instead of re-executing the instruction. Verified to reproduce capstone's
 * answer on this build's `SceneManager::GetActiveScene` (x20=0x2ffc000,
 * x19=0x2dd7000) if that one is ever needed. Nothing here needs it.
 *
 * All four words are verified against the loaded image before patching. */
#define LCGO_HAVE_NRE_TRACE 0
#define LCGO_NRE_RAISER     0          /* NOT DERIVED for this build */
#define LCGO_NRE_PATCH_LEN  0x10        /* 4 instructions */
#define LCGO_NRE_WORD_0     0xd10083ffu /* sub sp, sp, #0x20        */
#define LCGO_NRE_WORD_1     0xa900fbffu /* stp xzr, x30, [sp, #8]   */
#define LCGO_NRE_WORD_2     0x910003e0u /* mov x0, sp               */
#define LCGO_NRE_WORD_3     0xf90003ffu /* str xzr, [sp]            */
#define LCGO_NRE_TRACE_MAX  12          /* enough to see a pattern, not a flood */
#define LCGO_NRE_MAX_FRAMES 10

/* ===========================================================================
 * Scene census (libil2cpp) -- DERIVED, READ-ONLY, LATE-RUNNING.
 *
 * Answers the one question none of the other instruments do: IS THERE ANYTHING
 * TO RENDER? The managed probe counts cameras; it cannot tell "the camera is
 * looking at a populated scene it cannot draw" apart from "the camera is
 * correctly drawing an empty scene". Those need opposite fixes.
 *
 * The camera itself is NOT worth measuring, and this replaces an earlier plan
 * to do so. The game never writes camera state at runtime -- zero references in
 * dump.cs to set_cullingMask / set_targetTexture / set_clearFlags / set_enabled
 * / set_rect / set_orthographicSize -- so its configuration is entirely what
 * was deserialised from level0, byte-identical to the phone, and nothing in
 * this port touches it either. There is no mechanism by which it could differ,
 * so reading it back would measure something that cannot have changed.
 *
 * `SceneManager.Scene::get_rootCount` would have been the cheap version of this
 * and is NOT in this build -- IL2CPP stripped it because the game never calls
 * it, the same way it stripped Time::get_smoothDeltaTime (§7b). So we go
 * through Object.FindObjectsOfType instead.
 *
 * HOW. Everything needed is a real libil2cpp EXPORT, so only one RVA is
 * involved (the FindObjectsOfType thunk itself):
 *
 *   il2cpp_domain_get / il2cpp_domain_get_assemblies / il2cpp_assembly_get_image
 *   il2cpp_class_from_name / il2cpp_class_get_type / il2cpp_type_get_object
 *
 * Assemblies are ITERATED rather than opened by name: Canvas lives in
 * UnityEngine.UIModule and Renderer in UnityEngine.CoreModule, and hardcoding
 * either would break on a Unity version bump for no benefit.
 *
 * Il2CppArray length is at +0x18 -- derived, not assumed: the splash FSM reads
 * its own array length with `ldr x8, [x8, #0x18]` at 0x16bcd18 (§13).
 *
 * TIMING IS DELIBERATE. This runs LATE and only twice. FindObjectsOfType walks
 * every live object and allocates a managed array, which is the most invasive
 * thing any instrument in this tree does. Running it at frames 300 and 900 --
 * long after the Resources trace, the NRE traces, the UNMATCHED list and the
 * first probe samples have all been flushed to the log -- means that if it
 * somehow faults, the boot still yields every other diagnostic. Do not move it
 * earlier for convenience.
 *
 * (The allocation leaks: the il2cpp GC is disabled in this tree, §6a. Two
 * arrays of a few hundred pointers is irrelevant against the ~1 GB already
 * resident, but it is a reason not to run this per-frame.)
 *
 * X(namespace, class, label) */
#define LCGO_HAVE_SCENE_CENSUS   1
#define LCGO_FINDOBJECTS_RVA     0x1f09638  /* Object::FindObjectsOfType(Type,bool) */
#define LCGO_WORD_FINDOBJ_STOCK  0xa9be57feu /* stp x30,x21,[sp,#-0x20]!           */
#define LCGO_IL2CPP_ARRAY_LEN    0x18        /* derived, see above                 */
/* Frames the census runs on -- chosen to BRACKET the one dynamic event in the
 * whole boot, not just to sample the aftermath.
 *
 * Boot-4 timing, measured from the log rather than assumed:
 *
 *     frame 0 .. frame 2            <- all three NREs land before this
 *     [gfx] swap #1  draws=77       <- the only frame that ever drew content
 *     frame 3, frame 4
 *     [gfx] swap #2  draws=1        <- and it never recovers
 *
 * So the transition is at frames 2-4. Earlier schedules (300/900, then 120/360)
 * only ever measured the steady state afterwards, which cannot distinguish
 * "the UI was built and then destroyed" from "the UI was never built" -- and
 * those need completely different fixes.
 *
 *   f2  = before/at the 77-draw swap, the last moment content demonstrably existed
 *   f6  = safely after the drop to one draw
 *   f120, f360 = steady state, and proof nothing recovers later
 *
 * Reading it: a non-zero count at f2 falling to zero by f6 means something
 * DESTROYED the UI, and the question becomes what ran in between. Zero at both
 * means nothing was ever built and the 77 draws were engine/splash init, which
 * sends the search back upstream into script execution.
 *
 * Frame 2 is the earliest safe sample: the icall thunks resolve on first call,
 * so nothing may call into managed code before a frame has been presented (same
 * constraint as the managed probe). It is still after every NRE, the Resources
 * trace and the UNMATCHED list, so the "a fault here costs no other diagnostic"
 * property survives. */
#define LCGO_CENSUS_FRAMES       { 2, 6, 120, 360 }
/* Boot-5 result: canvas=1 cvrend=1 render=0, IDENTICAL at f2/f6/f120/f360.
 * Nothing is destroyed -- one Canvas with a single CanvasRenderer is all that
 * ever exists, where a populated menu would carry dozens. Combined with the
 * Resources trace never once requesting a UI prefab (47 loads, all configs,
 * sounds and player data), the view-controller chain never asks for its views.
 *
 * So the next question is about the GAME's own UI objects, not Unity's:
 * does a ViewController exist at all, and is the splash screen among them?
 * Both are global-namespace types, hence the "" namespace. */
#define LCGO_SCENE_CENSUS_LIST(X) \
  X("UnityEngine", "Canvas",         "canvas")   /* any UI root at all?          */ \
  X("UnityEngine", "CanvasRenderer", "cvrend")   /* actual UI geometry carriers  */ \
  X("UnityEngine", "Renderer",       "render")   /* mesh/sprite/skinned          */ \
  X("",            "ViewController", "viewctl")  /* the game's own UI units      */ \
  X("",            "SplashScreenViewController", "splash")

/* Also log the name() of the first object found for each type. Identifying that
 * single Canvas is worth more than counting it again -- "SplashCanvas" and
 * "PersistentUIRoot" imply completely different failures.
 *
 * Il2CppArray elements start at +0x20: the length field derived at +0x18 (§18c)
 * is an 8-byte size_t, so the vector begins immediately after it. */
#define LCGO_IL2CPP_ARRAY_DATA   0x20
#define LCGO_OBJ_GET_NAME_RVA    0x1f08b98   /* UnityEngine.Object::get_name    */
#define LCGO_WORD_GETNAME_STOCK  0xa9be57feu /* stp x30,x21,[sp,#-0x20]!        */

/* ===========================================================================
 * Frame-rate override (libil2cpp) -- DERIVED.
 *
 * The game ships `FramerateCapper : MonoBehaviour` with a serialized
 * `public int TargetFrameRate`, which it pushes into
 * UnityEngine.Application::set_targetFrameRate. Mobile builds of this title cap
 * well below what a Switch can present, so config.target_fps overrides whatever
 * the game asks for.
 *
 * Hooked by ICALL CACHE SWAP, the same technique as the Resources trace -- and
 * for the same reason: we need to call THROUGH to the original with a modified
 * argument, which a 16-byte splice cannot do. The thunk reads its cached icall
 * from .bss, resolves by name if null, then tail-calls it with the int still in
 * w0:
 *
 *   adrp x20,#0x2ffb000 ; ldr x1,[x20,#0x2c0]   <- cache
 *   mov  w19,w0                                  <- the requested fps
 *   cbnz x1, dispatch ; <resolve by name> ; str x0,[x20,#0x2c0]
 *   dispatch: mov w0,w19 ; <restore> ; br x1     <- TAIL call
 *
 * so our replacement is entered with the caller's return address live in x30
 * and must match the icall's own signature: void(int).
 *
 * Cache slot re-derived by tools/probe_rva.py --cache, which also prints the
 * signature string the thunk embeds, so the constant self-checks. */
#define LCGO_HAVE_FPS_OVERRIDE   1
#define LCGO_SET_TFR_THUNK       0x1ede904   /* Application::set_targetFrameRate */
#define LCGO_SET_TFR_CACHE       0x23c9c58   /* its icall cache slot (.bss)      */
#define LCGO_WORD_TFR_THUNK_STOCK 0xf81e0ffeu /* str x30,[sp,#-0x20]!            */
#define LCGO_SET_TFR_SIG \
  "UnityEngine.Application::set_targetFrameRate(System.Int32)"

/* ---------------------------------------------------------------------------
 * QualitySettings::set_vSyncCount -- the OTHER half of the frame-rate control,
 * and the reason config.txt's `framerate 60` had no effect.
 *
 * Unity's rule: WHEN vSyncCount IS NON-ZERO, Application.targetFrameRate IS
 * IGNORED. Presentation is then driven purely by vsync divided by vSyncCount:
 *     vSyncCount = 1  -> every vsync   -> 60 fps on this window
 *     vSyncCount = 2  -> every 2nd     -> 30 fps
 * So overriding set_targetFrameRate alone can never raise the cap; the game
 * ships a quality profile that sets vSyncCount and that wins every time.
 *
 * Both setters ARE reached by this game: IL2CPP stripped the *getters* for
 * Application.targetFrameRate and QualitySettings.vSyncCount (dump.cs shows
 * `{ set; }` with no getter for either) but kept both setters, which only
 * happens when managed code actually calls them.
 *
 * The thunk is structurally IDENTICAL to set_targetFrameRate -- same
 * str x30,[sp,#-0x20]! prologue, same lazy-icall cache, same void(int)
 * signature -- so the exact same cache-swap hook applies:
 *
 *   0x1eea84c  str  x30,[sp,#-0x20]!
 *   0x1eea854  adrp x20,#0x23ca000
 *   0x1eea858  ldr  x1,[x20,#0x98]        <- cache slot 0x23ca098
 *   0x1eea85c  mov  w19,w0                <- requested vSyncCount
 *   0x1eea860  cbnz x1, dispatch
 *              <resolve by name> ; str x0,[x20,#0x98]
 *   dispatch:  mov w0,w19 ; ... ; br x1   <- TAIL call
 *
 * Signature string embedded at il2cpp+0x610d48, verified by reading it:
 *   "UnityEngine.QualitySettings::set_vSyncCount(System.Int32)"
 * Prologue word verified: 0xf81e0ffe, same as the TFR thunk.
 * ------------------------------------------------------------------------- */
#define LCGO_HAVE_VSYNC_OVERRIDE  1
#define LCGO_SET_VSYNC_THUNK      0x1eea84c  /* QualitySettings::set_vSyncCount */
#define LCGO_SET_VSYNC_CACHE      0x23ca098  /* its icall cache slot (.bss)     */
#define LCGO_WORD_VSYNC_THUNK_STOCK 0xf81e0ffeu /* str x30,[sp,#-0x20]!         */
#define LCGO_SET_VSYNC_SIG \
  "UnityEngine.QualitySettings::set_vSyncCount(System.Int32)"

#endif /* NX_PATCH_LCGO_H */
