/* unity_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so's JNI_OnLoad  (LARA CROFT GO (Deca Games re-release), Unity
 * 2022.3.40f1, arm64 / IL2CPP).
 *
 * Auto-extracted from THIS build's libunity.so with
 *   python3 tools/extract_entrypoints.py libunity.so
 * JNI_OnLoad is at 0x6021b8 and calls 10 RegisterNatives helpers registering 43
 * native methods in total. Helper #1 registers the 27 UnityPlayer natives (the
 * drive surface); the others register Swappy frame pacing (we run our own loop),
 * ARCore, Camera2, HFP, audio-volume, orientation lock and soft-input -- stub
 * SDK classes we never invoke.
 *
 * IMPORTANT: these offsets are LINK-TIME addresses for THIS EXACT libunity.so
 * (BuildID xxHash 9f45f342e5b732e5, .text 0x345de0..0x101076c). If the game is
 * patched or updated, re-run the extractor and paste the new offsets here.
 *
 * Runtime address = unity_mod.load_virtbase + offset (the .so links at base 0).
 * The drive-critical method set is identical to the Deus Ex GO / PvZ Fusion
 * ports -- only the offsets differ -- so main.c's drive code is unchanged.
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- UnityPlayer native method offsets (link-time vaddr) ---------------- */
/* JNI_OnLoad */
#define OFF_JNI_OnLoad                        0x6021b8 /* (JavaVM*,reserved)->jint  caches VM, registers natives */

/* drive-critical */
#define OFF_initJni                           0x601368 /* (env,thiz,Context)                 */
#define OFF_nativeRecreateGfxState            0x60159c /* (env,thiz,int,Surface)  set surface*/
#define OFF_nativeSendSurfaceChangedEvent     0x601604 /* (env,thiz)                         */
#define OFF_nativeRender                      0x60165c /* (env,thiz)->Z   per-frame; false=stop */
#define OFF_nativeInjectEvent                 0x6016bc /* (env,thiz,InputEvent,int)->Z input */
#define OFF_nativePause                       0x601404 /* (env,thiz)->Z                      */
#define OFF_nativeResume                      0x601468 /* (env,thiz)                         */
#define OFF_nativeFocusChanged                0x601548 /* (env,thiz,Z)                       */
#define OFF_nativeDone                        0x601374 /* (env,thiz)->Z   shutdown           */
#define OFF_nativeApplicationUnload           0x6014f8 /* (env,thiz)                         */
#define OFF_nativeLowMemory                   0x6014b0 /* (env,thiz)                         */
#define OFF_nativeOrientationChanged          0x602100 /* (env,thiz,int,int)                 */

/* secondary / usually unused for a port */
#define OFF_nativeUnitySendMessage            0x601ccc /* (env,thiz,String,String,byte[])    */
#define OFF_nativeMuteMasterAudio             0x601edc /* (env,thiz,Z)                       */
#define OFF_nativeGetNoWindowMode             0x602160 /* (env,thiz)->Z                      */
#define OFF_nativeIsAutorotationOn            0x601e7c /* (env,thiz)->Z                      */
#define OFF_nativeSetLaunchURL                0x601f80 /* (env,thiz,String)                  */
#define OFF_nativeHidePreservedContent        0x6020b8 /* (env,thiz)                         */

/* soft keyboard (route via SoftInputProvider stub; not needed for first boot) */
#define OFF_nativeSetInputArea                0x6019b4
#define OFF_nativeSetKeyboardIsVisible        0x601a34
#define OFF_nativeSetInputString              0x601a8c
#define OFF_nativeSetInputSelection           0x601b2c
#define OFF_nativeSoftInputClosed             0x601c7c
#define OFF_nativeSoftInputCanceled           0x601b94
#define OFF_nativeSoftInputLostFocus          0x601be4
#define OFF_nativeReportKeyboardConfigChanged 0x601c34
#define OFF_nativeSendSurfaceChanged      OFF_nativeSendSurfaceChangedEvent

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) ----- */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; you do it in main.c):
 *
 *   initJni(env, thiz, fake_context);                 // early init
 *   nativeRecreateGfxState(env, thiz, 0, fake_surface);// give it the surface
 *   nativeSendSurfaceChangedEvent(env, thiz);          // engine builds GL state
 *   for (;;) {
 *       // input: nativeInjectEvent(env,thiz, motionEvent, deviceId);  // see NOTE
 *       if (!nativeRender(env, thiz)) break;            // false == engine wants out
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 *
 * NOTE on input: nativeInjectEvent takes a Java InputEvent/MotionEvent jobject,
 * which the engine then queries back via JNI (getActionMasked/getX/getY/
 * getPointerId/getPointerCount...). Feeding touch/keys needs the fake
 * MotionEvent/KeyEvent handles in jni_fake.c (stateful handles whose getters
 * return the values you stashed). PvZ Fusion is a touch game, so this path is
 * load-bearing -- see PORTING.md "Input".
 * =========================================================================== */

/* ---- Non-UnityPlayer native tables also present in this build (FYI) -------
 * We do NOT register/drive these; listed only so nobody re-hunts them. All
 * offsets below are from THIS build's JNI_OnLoad (tools/extract_entrypoints.py).
 *   choreographer   nOnChoreographer                     @0xb18ffc
 *   swappy          nOnRefreshPeriodChanged              @0xb1b2fc
 *                   nSetSupportedRefreshPeriods          @0xb1b11c
 *   ARCore          initializeARCore/pause/resume        @0x5dceb8/0x5dcf1c/0x5dcf70
 *   Camera2         initCamera2Jni/deinit                @0x5fa498/0x5fa4e4
 *                   nativeFrameReady/nativeSurfaceTexReady @0x5fec3c/0x5fead4
 *   HFP audio       initHFPStatusJni/deinit              @0x5e015c/0x5e01a8
 *   audio volume    onAudioVolumeChanged                 @0x5e655c
 *   query status    nativeStatusQueryResult              @0x5df31c
 *   orient lock     nativeUpdateOrientationLockState     @0x5e67a0
 *   softinput type  nativeGetSoftInputType               @0x5d9a2c
 * -------------------------------------------------------------------------- */

#endif /* UNITY_ENTRYPOINTS_H */
