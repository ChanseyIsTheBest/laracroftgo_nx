/* jni_fake.c -- fake JNI environment for the MVGL engine (libcrx.so)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "data.h"
#include "text2bitmap.h"
#include "unity_sendmessage.h"
#include "movie_player.h"
#include "editbox.h"
#include "android_native_unity.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "opensles.h"    /* audio_fmod_open/write: FMOD native-audio output sink */

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

void fmod_audio_start(void); // defined below; launched from FMODAudioDevice.start()

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
  // text2bitmap.h BITMAP_TAG ('BMP1') is also handled by free_ref
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

/* ---- app identity ---------------------------------------------------------
 * Reported to game code through PackageInfo / ApplicationInfo / Build.VERSION.
 * Defined up here because act_object() reads APP_VERSION_NAME long before the
 * field-access section that used to hold these.
 *
 * The version pair WAS a placeholder ("1.0.0"/45) and this title DOES gate on
 * it -- the remote version check on the title screen. They now live in config.h
 * with the full explanation. GAME_PACKAGE (config.h) IS this game's real
 * package and is not a placeholder.
 * -------------------------------------------------------------------------- */
/* APP_VERSION_NAME / APP_VERSION_CODE now live in config.h -- see the block
 * there for why "1.0.0" caused the "your game version is out of date" popup to
 * loop on the title screen. */
#define NX_SDK_INT       33   /* Android 13 -- clears any minSdk gate */

volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// local reference registry (matches the engine's Push/PopLocalFrame brackets)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// interned-string pool: the engine re-creates the same constant strings (class
// names, the activity name) constantly; pool them by content so repeats don't
// fill the local-ref table. Pooled strings are never reg_local'd, and free_ref
// skips them (range check below).
#define MAX_ISTR 512
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static void free_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;  // interned string -- pooled, never freed
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    case BITMAP_TAG: text2bitmap_free((FakeBitmap *)ref); break;
    default: break; // TAG_ID / TAG_CLASS are pooled
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Intern objects by label -- one pooled object per class (TAG_CLASS so free_ref()
// leaves it alone, never reg_local'd) -- so the engine's frequent NewObject calls
// don't fill the local-ref table. Safe: our objects are opaque, stateless handles
// dispatched by method class, not by identity.
#define MAX_IOBJ 128
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&locals_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (istr_count < MAX_ISTR) {
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->utf = strdup(u);
    mutexUnlock(&locals_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&locals_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  s->tag = TAG_STRING;
  s->utf = strdup(u);
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// UTF-16 code-unit count of a modified-UTF-8 string (Java String.length()).
// ASCII -> byte count; astral planes count as a surrogate pair. Used by both
// GetStringLength and the String.length() upcall handler.
static juint utf16_len(const char *str) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    n += (cp >= 0x10000) ? 2u : 1u;
    p += adv;
  }
  return n;
}

// register a text2bitmap result in the local table so the engine's recycle /
// DeleteLocalRef frees it
static void *reg_bitmap(FakeBitmap *b) { return reg_local(b); }

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

#define MAX_CLASSES 128
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) {
    debugPrintf("JNI: *** class pool exhausted at '%s' -> collapsing to '%s' "
                "(distinct classes break instanceof!)\n", name, class_pool[0].name);
    return &class_pool[0];
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  debugPrintf("JNI class: %s\n", c->name);
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// The engine fetches the ClassLoader every frame; hand back a cached singleton
// (pooled, never reg_local'd) so it doesn't fill the local-ref table.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

/* Read a class name back out of whatever FindClass handed the engine. Both
 * FakeClass (TAG_CLASS pool) and the interned FakeObject form carry a name, and
 * ReflectionHelper hands us one of them as its first argument. */
static const char *fake_class_name(void *jcls) {
  if (!jcls) return NULL;
  uint32_t tag = *(uint32_t *)jcls;
  if (tag == TAG_CLASS) return ((FakeClass *)jcls)->name;
  if (tag == TAG_OBJECT) return ((FakeObject *)jcls)->label;
  return NULL;
}

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s.%s)\n", cls, name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  debugPrintf("JNI id: %s.%s %s\n", id->cls, id->name, id->sig);
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

// --- Text2Bitmap ------------------------------------------------------------
// draw methods return a Bitmap; the first arg is the text String, the next int
// is the pixel size. measure methods return I (width or height by name).

static void *t2b_object(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  FakeBitmap *b = text2bitmap_render(text, size);
  (void)id;
  return b ? reg_bitmap(b) : NULL;
}

static juint t2b_int(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  if (name_has(id->name, "Height"))
    return (juint)text2bitmap_measure_height(text, size);
  if (name_has(id->name, "Width"))
    return (juint)text2bitmap_measure_width(text, size);
  return (juint)text2bitmap_measure_width(text, size);
}

// --- MoviePlayer ------------------------------------------------------------

static const char *first_string_arg(const char *sig, va_list va); // defined below

static void mov_void(const FakeID *id, va_list va) {
  if (!strcmp(id->name, "SetMovieDB")) { movie_set_db(first_string_arg(id->sig, va)); return; }
  if (name_has(id->name, "Stop") || name_has(id->name, "stop")) { movie_stop(); return; }
  if (name_has(id->name, "Pause")) { movie_pause(); return; }
  if (name_has(id->name, "Resume")) { movie_resume(); return; }
  if (name_has(id->name, "Play") || name_has(id->name, "play") ||
      name_has(id->name, "Start")) {
    movie_play(first_string_arg(id->sig, va), 0); // the String arg is the movie name
    return;
  }
}

static juint mov_int(const FakeID *id, va_list va) {
  (void)va;
  if (name_has(id->name, "Playing") || name_has(id->name, "playing"))
    return (juint)movie_is_playing();
  return 0;
}

// --- MyNativeActivity / general activity ------------------------------------

// the in-archive base name the engine appends ".android.mvgl" to. "10007" is
// the APK versionCode, matching the shipped main.10007.android.mvgl.

/* Lara Croft GO shipped a broad Western + CJK language set. We map the Switch
 * system language to a 2-letter ISO code; the game falls back to English for
 * anything it does not carry, so an over-broad map is harmless.
 * NOTE: if this build expects a different token (e.g. "zh-CN", "pt-BR", or a
 * numeric id), change the strings here -- see PORTING.md section 5. */
static const char *lang_code(void) {
  /* Always follows the Switch system language. The old config.language override
   * is gone: its default was LANG_AUTO, i.e. exactly this, and a manual token
   * that the build does not carry silently degrades to English with no
   * indication -- worse than simply tracking the console. */
  static const char *cached = NULL;
  if (cached) return cached;
  cached = "en";
  u64 code; SetLanguage sl;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl))) {
      switch (sl) {
        case SetLanguage_FR: case SetLanguage_FRCA: cached = "fr"; break;
        case SetLanguage_DE:                        cached = "de"; break;
        case SetLanguage_ES: case SetLanguage_ES419:cached = "es"; break;
        case SetLanguage_IT:                        cached = "it"; break;
        case SetLanguage_PT: case SetLanguage_PTBR: cached = "pt"; break;
        case SetLanguage_RU:                        cached = "ru"; break;
        case SetLanguage_JA:                        cached = "ja"; break;
        case SetLanguage_KO:                        cached = "ko"; break;
        case SetLanguage_ZHCN: case SetLanguage_ZHTW:
        case SetLanguage_ZHHANS: case SetLanguage_ZHHANT: cached = "zh"; break;
        default:                                    cached = "en"; break;
      }
    }
    setExit();
  }
  return cached;
}

/* Country / ISO3 tokens paired with lang_code(), for java.util.Locale. */
static void lang_locale_tokens(const char **country, const char **iso3l,
                               const char **iso3c, const char **tag) {
  const char *l = lang_code();
  struct { const char *l, *c, *i3l, *i3c, *tag; } t[] = {
    { "en", "US", "eng", "USA", "en_US" }, { "fr", "FR", "fra", "FRA", "fr_FR" },
    { "de", "DE", "deu", "DEU", "de_DE" }, { "es", "ES", "spa", "ESP", "es_ES" },
    { "it", "IT", "ita", "ITA", "it_IT" }, { "pt", "BR", "por", "BRA", "pt_BR" },
    { "ru", "RU", "rus", "RUS", "ru_RU" }, { "ja", "JP", "jpn", "JPN", "ja_JP" },
    { "ko", "KR", "kor", "KOR", "ko_KR" }, { "zh", "CN", "zho", "CHN", "zh_CN" },
  };
  for (unsigned k = 0; k < sizeof(t)/sizeof(t[0]); k++)
    if (!strcmp(l, t[k].l)) {
      *country = t[k].c; *iso3l = t[k].i3l; *iso3c = t[k].i3c; *tag = t[k].tag;
      return;
    }
  *country = "US"; *iso3l = "eng"; *iso3c = "USA"; *tag = "en_US";
}

// Walk a JNI arg list per the signature and return the first non-empty String
// argument's text (used to seed the keyboard from ShowEditBox's initial text).
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

// EditBox / TextBox names the engine drives via JNI (both share our swkbd box)
static int is_editbox_show(const char *n)  { return name_has(n, "ShowEditBox")  || name_has(n, "OpenEditBox")  || name_has(n, "ShowTextBox") || name_has(n, "OpenTextBox")
                                                  || name_has(n, "setKeyboardVisible") || name_has(n, "SetKeyboardVisible")
                                                  || name_has(n, "showSoftInput")      || name_has(n, "ShowSoftInput")
                                                  || name_has(n, "openKeyboard")       || name_has(n, "OpenKeyboard"); }
static int is_editbox_open(const char *n)  { return name_has(n, "IsOpenEditBox") || name_has(n, "IsOpenTextBox"); }
static int is_editbox_text(const char *n)  { return name_has(n, "GetEditBoxText") || name_has(n, "GetTextBoxText")
                                                  || name_has(n, "getKeyboardText") || name_has(n, "GetKeyboardText")
                                                  || name_has(n, "getText"); }
static int is_editbox_close(const char *n) { return name_has(n, "CloseEditBox") || name_has(n, "CloseTextBox")
                                                  || name_has(n, "hideSoftInput") || name_has(n, "HideSoftInput")
                                                  || name_has(n, "closeKeyboard") || name_has(n, "CloseKeyboard"); }
/* Anything keyboard-shaped that we did NOT match: log once so the exact JNI name
 * this game uses is visible and can be added above. */
static void kbd_sniff(const char *cls, const char *n) {
  if (!name_has(n, "eyboard") && !name_has(n, "oftInput") && !name_has(n, "extBox")
      && !name_has(n, "ditBox") && !name_has(cls, "eyboard")) return;
  static unsigned seen; if (seen < 12) { seen++;
    debugPrintf("[kbd] unmatched: %s.%s\n", cls ? cls : "?", n ? n : "?"); }
}

/* jni_string_utf is defined far below (shared with unity_jni.c) and unity_jni.h
 * is included after this point, so forward-declare it for act_object's
 * getProperty()/locale arg reads. */
const char *jni_string_utf(void *jstr);

/* AudioManager.getProperty(PROPERTY_OUTPUT_*): the engine/FMOD read the native
 * sample rate / frames-per-buffer to size the audio path. Empty -> parse failure
 * -> a 0 config, which makes FMOD's OpenSL output init fail with "Error
 * initializing output device" (60) on the framesPerBuffer==0 guard. Hand back
 * Switch-sane values (48 kHz, 64 frames).
 *
 * The String key argument does NOT reliably reach us: getProperty is invoked
 * through a JNI call path whose positional argument is lost (observed: key
 * resolves to ""), so keying purely off the argument returned "" and FMOD parsed
 * framesPerBuffer 0 -> error 60. The game ALWAYS reads the PROPERTY_OUTPUT_*
 * static field immediately before the matching getProperty() call, so field_object
 * records which one in g_last_output_prop and we fall back to it when the key is
 * missing/unrecognised. 1 = sample rate, 2 = frames-per-buffer. */
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  static int logged[3] = {0, 0, 0};
  if (which >= 0 && which <= 2 && !logged[which]) {
    logged[which] = 1;
    debugPrintf("[jni] getProperty -> %s\n",
                which == 1 ? "24000" : which == 2 ? "256" : "(empty)");
  }
  if (which == 1) return jni_make_string("24000");
  if (which == 2) return jni_make_string("256");
  return jni_make_string("");
}

extern void *fake_env;
typedef void *(*jnibridge_invoke_fn)(void *, void *, long long, void *, void *, void *);
static jnibridge_invoke_fn g_jnibridge_invoke = 0;
static void *j_NewObjectArray(void *env, int len, void *cls, void *init);
/* jni_make_object pools by label, which would collapse every proxy to one object.
 * Give each proxy its own object that embeds its native ptr; identify proxies by
 * address range (safe -- never reads a field on a non-proxy jobject). */
#define MAX_PROXY_OBJ 512
typedef struct { uint32_t tag; uint32_t pad; long long ptr; } FakeProxy;
static FakeProxy g_proxy_pool[MAX_PROXY_OBJ];
static int g_proxy_pool_n = 0;
static FakeProxy *g_last_proxy = 0;
static void *proxy_make(long long ptr) {
  if (g_proxy_pool_n >= MAX_PROXY_OBJ) return jni_make_object("jniproxy");
  FakeProxy *p = &g_proxy_pool[g_proxy_pool_n++];
  p->tag = TAG_CLASS; p->ptr = ptr; g_last_proxy = p;   /* TAG_CLASS -> free_ref ignores it */
  return p;
}
static long long proxy_ptr_of(void *obj) {
  uintptr_t a = (uintptr_t)obj, lo = (uintptr_t)g_proxy_pool, hi = (uintptr_t)(g_proxy_pool + g_proxy_pool_n);
  if (a >= lo && a < hi && ((a - lo) % sizeof(FakeProxy)) == 0) return ((FakeProxy *)obj)->ptr;
  return 0;
}
/* Invoke one method on a proxy through Unity's bridge. ProxyObject dispatch checks
   the jclass matches the interface and the methodID == the method (pointer compare);
   j_FromReflectedMethod passes a TAG_ID methodID straight through so it matches. */
static void proxy_invoke(void *proxy, void *cls, void *method, void *args) {
  long long ptr = proxy_ptr_of(proxy);
  if (!ptr || !g_jnibridge_invoke) return;
  debugPrintf("[jni] proxy invoke %p ptr=%llx .%s\n", proxy, (unsigned long long)ptr, ((FakeID *)method)->name);
  g_jnibridge_invoke(fake_env, proxy, ptr, cls, method, args);
}
static void proxy_run_runnable(void *runnable) {
  proxy_invoke(runnable, intern_class("java/lang/Runnable"), get_id("java/lang/Runnable", "run", "()V"), (void *)0);
}
/* Message.sendToTarget(): the factory's HandlerThread never pumps its Looper, so the
   message is never delivered to callback.handleMessage(msg). Deliver it ourselves to
   the most-recently created proxy (the Handler$Callback the factory just built). */
static void handler_deliver_message(void *callback) {
  void *msg  = jni_make_object("android/os/Message");
  void *args = j_NewObjectArray(fake_env, 1, (void *)0, msg);
  proxy_invoke(callback, intern_class("android/os/Handler$Callback"),
               get_id("android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z"), args);
}
static uint64_t g_frame_ns = 0;   /* frameTimeNanos for boxed-Long unboxing */
static void *g_frame_cb = 0;      /* registered Choreographer FrameCallback proxy */
static void deliver_doframe(void *cb) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  g_frame_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  void *boxed = jni_make_object("java/lang/Long");
  void *args  = j_NewObjectArray(fake_env, 1, (void *)0, boxed);
  static int logged = 0;
  if (logged < 3) { logged++; debugPrintf("[jni] doFrame tick -> %p (vsync pump)\n", cb); }
  /* Report the cadence we are ACTUALLY delivering Choreographer callbacks at.
   * Unity's Android player paces itself off doFrame, so this is the ceiling on
   * the game's frame rate no matter what the swap interval or vSyncCount say --
   * which is how a 30 fps cap survived fixing both of those. */
  {
    static uint64_t win_start = 0; static unsigned win_frames = 0;
    if (!win_start) win_start = g_frame_ns;
    if (++win_frames >= 120) {
      const uint64_t dt = g_frame_ns - win_start;
      if (dt)
        debugPrintf("[fps] doFrame cadence: %u frames in %llu ms = %llu fps "
                    "(config target %d)\n",
                    win_frames, (unsigned long long)(dt / 1000000ull),
                    (unsigned long long)((uint64_t)win_frames * 1000000000ull / dt),
                    config.framerate);
      win_start = g_frame_ns; win_frames = 0;
    }
  }
  proxy_invoke(cb, intern_class("android/view/Choreographer$FrameCallback"),
               get_id("android/view/Choreographer$FrameCallback", "doFrame", "(J)V"), args);
}
/* drain thread: runs posted work off the main thread (no self-deadlock / re-entrancy) */
#define RUNQ_N 128
static void *g_runq[RUNQ_N]; static int g_runq_kind[RUNQ_N]; static int g_runq_head = 0, g_runq_tail = 0;
static Mutex g_runq_lk; static CondVar g_runq_cv; static int g_runq_started = 0;
/* Monotonic nanoseconds, same clock deliver_doframe stamps frames with. */
static uint64_t drain_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void run_drain_thread(void *arg) {
  (void)arg;
  static uint8_t drain_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(drain_tls);
  /* Choreographer tick DEADLINE, not a fixed sleep.
   *
   * WHY. This loop used to do `condvarWaitTimeout(..., 16000000)` and then run
   * the frame, which makes the period 16 ms PLUS however long the frame takes,
   * not a 16 ms period. With a frame costing ~16 ms that is ~33 ms -> 30 fps,
   * which is exactly the cap that survived every other frame-rate fix: the
   * swap interval was already 1 and vSyncCount was already forced, but Unity's
   * Android player waits for Choreographer.doFrame and we were only delivering
   * it every ~33 ms. The pacing was ours, not the game's.
   *
   * Now the next tick is scheduled from the PREVIOUS tick, so frame work
   * overlaps the wait instead of adding to it, and the cadence follows
   * config.framerate. If a frame overruns its budget the deadline is snapped
   * forward to now rather than accumulating debt (which would burst-deliver
   * doFrame to catch up and stutter). */
  uint64_t next_tick = 0;
  for (;;) {
    const uint64_t period = (config.framerate == 30) ? 33333333ull : 16666667ull;
    mutexLock(&g_runq_lk);
    if (g_runq_head == g_runq_tail) {
      if (g_frame_cb) {
        const uint64_t now = drain_now_ns();
        if (next_tick <= now) next_tick = now;      /* first tick, or overrun */
        const uint64_t wait = next_tick - now;
        if (wait) condvarWaitTimeout(&g_runq_cv, &g_runq_lk, wait);
        next_tick += period;
      } else {
        next_tick = 0;                              /* idle: restart the cadence */
        condvarWait(&g_runq_cv, &g_runq_lk);
      }
    }
    void *o = 0; int k = 0, have = 0;
    if (g_runq_head != g_runq_tail) {
      o = g_runq[g_runq_head]; k = g_runq_kind[g_runq_head];
      g_runq_head = (g_runq_head + 1) % RUNQ_N; have = 1;
    }
    void *fcb = g_frame_cb; g_frame_cb = 0;   /* one-shot: doFrame re-registers */
    mutexUnlock(&g_runq_lk);
    if (have) { if (k == 0) proxy_run_runnable(o); else handler_deliver_message(o); }
    if (fcb) deliver_doframe(fcb);
  }
}
static Thread g_runq_thr;
static void runq_post(void *obj, int kind) {
  if (!obj) return;
  mutexLock(&g_runq_lk);
  if (!g_runq_started) {
    g_runq_started = 1;
    if (R_SUCCEEDED(threadCreate(&g_runq_thr, run_drain_thread, NULL, NULL, 0x8000, 0x2C, -2)))
      threadStart(&g_runq_thr);
  }
  int nt = (g_runq_tail + 1) % RUNQ_N;
  if (nt != g_runq_head) { g_runq[g_runq_tail] = obj; g_runq_kind[g_runq_tail] = kind; g_runq_tail = nt; }
  condvarWakeOne(&g_runq_cv);
  mutexUnlock(&g_runq_lk);
}
static void post_runnable(void *runnable) { runq_post(runnable, 0); }
static void post_message(void) { runq_post(g_last_proxy, 1); }
static int g_msg_what = 0;   /* captured from Handler.obtainMessage(what) for msg.what reads */
unsigned g_kbd_trace;      /* set when swkbd closes; counts down as we log */
/* Unity soft-input natives (captured at RegisterNatives, invoked on close). */
void *g_u_setInputString, *g_u_setInputSel, *g_u_softClosed,
     *g_u_softCancel, *g_u_kbdVisible;
/* Push the swkbd result into Unity exactly as its Java keyboard would. */
void kbd_push_result(const char *text, int cancelled) {
  typedef void (*fn_str)(void *, void *, void *);
  typedef void (*fn_ii)(void *, void *, int, int);
  typedef void (*fn_v)(void *, void *);
  typedef void (*fn_z)(void *, void *, int);
  void *cls = intern_class("com/unity3d/player/UnityPlayer");
  if (!text) text = "";
  int len = (int)strlen(text);
  debugPrintf("[kbd] push \"%s\" cancelled=%d (str=%p closed=%p)\n",
              text, cancelled, g_u_setInputString, g_u_softClosed);
  if (!cancelled && g_u_setInputString)
    ((fn_str)g_u_setInputString)(fake_env, cls, jni_make_string(text));
  if (!cancelled && g_u_setInputSel)
    ((fn_ii)g_u_setInputSel)(fake_env, cls, len, len);
  if (g_u_kbdVisible) ((fn_z)g_u_kbdVisible)(fake_env, cls, 0);
  if (cancelled) { if (g_u_softCancel) ((fn_v)g_u_softCancel)(fake_env, cls); }
  else           { if (g_u_softClosed) ((fn_v)g_u_softClosed)(fake_env, cls); }
}
static void *act_object(const FakeID *id, va_list va) {
  if (g_kbd_trace) { g_kbd_trace--;
    debugPrintf("[kbd] after-kbd obj call: %s.%s sig=%s\n",
                id->cls[0] ? id->cls : "?", id->name[0] ? id->name : "?",
                id->sig[0] ? id->sig : "?"); }
  /* A String-returning keyboard call: show it, then hand back what was typed.
   * This is the path this game uses -- it never polls a getter afterwards. */
  if (is_editbox_show(id->name) && sig_returns(id->sig, "Ljava/lang/String;")) {
    editbox_show(first_string_arg(id->sig, va), 64);
    const char *t = editbox_text();
    debugPrintf("[kbd] show-returns-text %s.%s -> \"%s\"\n",
                id->cls[0] ? id->cls : "?", id->name, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  if (is_editbox_text(id->name)) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text getter(obj) %s.%s -> \"%s\"\n",
                id->cls[0] ? id->cls : "?", id->name, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  /* JNIBridge.newInterfaceProxy(long nativePtr, Class[] ifaces) -> remember the
   * proxy so a later Handler.post(runnable) can invoke its run() (see post_runnable). */
  if (name_has(id->name, "newInterfaceProxy")) {
    long long ptr = va_arg(va, long long);
    void *proxy = proxy_make(ptr);
    debugPrintf("[jni] newInterfaceProxy ptr=%llx -> proxy=%p\n", (unsigned long long)ptr, proxy);
    return proxy;
  }
  /* Handler.obtainMessage(what): must return a REAL Message or the game's C++
   * wrapper null-checks and silently skips msg.sendToTarget() -- the UI-manager
   * factory then waits forever for handleMessage's side effect. Capture 'what'
   * so the callback's msg.what field read sees the right value. */
  if (name_has(id->name, "obtainMessage")) {
    if (id->sig[1] == 'I') g_msg_what = va_arg(va, int);
    debugPrintf("[jni] obtainMessage what=%d -> Message\n", g_msg_what);
    return jni_make_object("android/os/Message");
  }
  if (name_has(id->name, "getLooper") || name_has(id->name, "getMainLooper"))
    return jni_make_object("android/os/Looper");
  if (name_has(id->name, "getInstance") && name_has(id->cls, "Choreographer"))
    return jni_make_object("android/view/Choreographer");
  /* Unity launch args: libunity reads currentActivity.getIntent().getStringExtra("unity").
   * Serve -job-worker-count 0 to run jobs inline on main: diagnostic for the
   * UIGeometryJob garbage-input crash (if it persists inline, the corruption
   * predates the job and the crash stack shows the producer; if it vanishes,
   * it's a job/fence lifetime race) -- and a potential playable workaround.
   * The JobSystem log line 'Creating JobQueue using job-worker-count value %d'
   * confirms the effective value. */
  if (name_has(id->name, "getIntent"))
    return jni_make_object("android/content/Intent");
  if (name_has(id->name, "getExtras"))
    return jni_make_object("android/os/Bundle");
  if (name_has(id->cls, "Bundle") && (name_has(id->name, "getString") || name_has(id->name, "getCharSequence"))) {
    debugPrintf("[jni] Bundle.%s -> launch args served\n", id->name);
    return jni_make_string("");
  }
  if (name_has(id->name, "getStringExtra")) {
    const char *k = first_string_arg(id->sig, va);
    if (k && !strcmp(k, "unity")) {
      debugPrintf("[jni] getStringExtra(unity) -> launch args served\n");
      return jni_make_string("");
    }
    return NULL;
  }
  /* Uri.encode/decode: Unity round-trips PlayerPrefs keys through these. We are
   * the storage, so identity (return the input string) round-trips correctly and
   * keeps keys non-empty. Must precede the generic handlers. */
  if (name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return va_arg(va, void *);
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string(APP_VERSION_NAME);
  if (name_has(id->name, "PackageName")) return jni_make_string(GAME_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  /* The reference port answered a set of OBB / .mvgl archive-name getters here.
   * Lara Croft GO has no OBB: it ships an App Bundle whose install-time asset pack
   * (split_UnityDataAssetPack) is merged into assets/bin/Data by
   * tools/install_assets.py, so every archive is already a plain file under
   * data_dir(). Those getters are removed rather than left returning another
   * game's archive names -- they matched by substring, so a same-named method
   * here would have been handed another game's archive name and failed in a
   * confusing way. */
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  // Environment.getExternalStorageState() must return the SAME token as the
  // Environment.MEDIA_MOUNTED field ("mounted", see field_object) or the engine
  // decides external storage is unavailable and the save path never initialises.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // java.util.Locale. The game's LocalizationController ctor reads the device
  // locale to pick its language, via the standard three-call sequence:
  //
  //     Locale.getDefault()  ->  .getLanguage()  /  .getCountry()
  //
  // getDefault() is STATIC and its return type is rewritten to
  // Ljava/lang/Object; by the reflection bridge, so it matched none of the
  // string cases below, fell through act_object entirely, and returned NULL --
  // and the managed side then called .getLanguage() on a null AndroidJavaObject.
  // That is NRE #2 in the log (Technology.Core.Localization.LocalizationController
  // ::.ctor +0x4c0, immediately after `getMethodID(java/util/Locale.getDefault)`).
  // Localization dying in its constructor takes every localized UI string with
  // it, which is why the menu music loads and plays while nothing renders.
  //
  // getLanguage() was missing too: it returns Ljava/lang/String; so it hit the
  // generic "" fallback at the end of act_object rather than lang_code(). Even
  // with a non-null Locale that would have left the language blank.
  //
  // Any non-null object works as the Locale: subsequent getLanguage/getCountry
  // calls carry id->cls from the method ID the game minted against
  // java/util/Locale, so they re-enter this same block regardless of the
  // object's own label.
  if (name_has(id->cls, "Locale")) {
    const char *cc, *i3l, *i3c, *tag;
    lang_locale_tokens(&cc, &i3l, &i3c, &tag);
    if (!strcmp(id->name, "getDefault") || !strcmp(id->name, "getLocale"))
      return jni_make_object("java/util/Locale");
    if (!strcmp(id->name, "getLanguage"))    return jni_make_string(lang_code());
    if (!strcmp(id->name, "getCountry"))     return jni_make_string(cc);
    if (!strcmp(id->name, "getISO3Language"))return jni_make_string(i3l);
    if (!strcmp(id->name, "getISO3Country")) return jni_make_string(i3c);
    if (!strcmp(id->name, "toLanguageTag")) {
      /* BCP-47 uses a hyphen ("en-US"); lang_locale_tokens' tag is the POSIX
       * underscore form ("en_US"). Converting rather than returning tag raw
       * matters because managed code parses this with Split('-'). */
      static char bcp[32];
      snprintf(bcp, sizeof bcp, "%s", tag);
      for (char *q = bcp; *q; q++) if (*q == '_') *q = '-';
      return jni_make_string(bcp);
    }
    if (!strcmp(id->name, "toString") || name_has(id->name, "getDisplayName") ||
        name_has(id->name, "getDisplayLanguage"))
      return jni_make_string(tag);
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(data_dir()));
  // text the user typed on the Switch software keyboard
  if (is_editbox_text(id->name)) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text getter %s.%s -> \"%s\"\n",
                id->cls[0] ? id->cls : "?", id->name, t ? t : "(null)");
    return jni_make_string(t);
  }
  kbd_sniff(id->cls, id->name);   /* a jstring getter lands here, not in act_void */
  // Android object getters that must NOT be null, or the engine aborts the
  // chain. getPackageInfo()/getApplicationInfo() are how Unity reaches
  // PackageInfo.versionName/versionCode (-> Application.version); returning null
  // here is why the version stayed blank even with field access fixed -- Unity
  // got a null PackageInfo and never read the field. Hand back live (opaque)
  // objects; the subsequent field reads then resolve via field_object/field_int.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  /* --- accessors that must not be null, found by the UNMATCHED log ---------
   *
   * Every one of these was returning NULL or "" silently before §16e made the
   * fallbacks visible. They are all the same shape: the game (or Unity) asks
   * for a handle it will immediately call another method on, so a null turns
   * into a NullReferenceException one frame later, far from the cause.
   *
   * ArmoryActivity.getActivity() is the game's own bridge to its Java layer.
   * It has a fallback (it reads UnityPlayer.currentActivity next, which we do
   * answer), which is why boot survived -- but everything routed through the
   * primary path was being dropped. Hand back the same MyNativeActivity object
   * the rest of the port uses, so both routes agree on one identity. */
  /* ---- VERIFIED FOR LARA CROFT GO -------------------------------------------
   * The three Armory/Prime31 handlers below were inherited from the Deus Ex GO
   * port, where each was found the hard way (null handle -> silent drop or
   * unhandled managed throw). All three were re-checked against THIS game's
   * classes3.dex and dump.cs before the fork, and all three apply:
   *
   *   ArmoryActivity.getActivity()      -> Lcom/squareenixmontreal/armory/ArmoryActivity;
   *   ArmoryActivity.getPermissionPlugin() -> Lcom/.../PermissionPlugin;
   *   PermissionPlugin.hasPermission(String)Z      SYNCHRONOUS
   *   PermissionPlugin.canRequestPermissions()Z    SYNCHRONOUS
   *   PermissionPlugin.requestPermissions(String)V async -- only reached when
   *        the synchronous check says a permission is missing
   *   GoogleIABPluginBase.instance()    -> Lcom/prime31/GoogleIABPlugin;
   *
   * The managed caller is identical too (dump.cs:193589):
   *   internal sealed class LoginGooglePlayGames : IAuthenticationState
   *     Handle(Action<object>) / HasPermissions() / RequestPermissions() /
   *     OnPermissionResult(object, PermissionRequestArgs)
   * and the shop cascade behind the IAB plugin is present as well
   *   GoogleIAB (static AndroidJavaObject _plugin) -> AbstractShop.RefreshOffers
   *   with GoogleShop / OfflineShop / OnlineSuiteShop all deriving AbstractShop.
   *
   * So: DO NOT "simplify" these back to null. That is the exact regression the
   * Deus Ex port made twice.
   * ------------------------------------------------------------------------ */
  if (name_has(id->cls, "ArmoryActivity") && !strcmp(id->name, "getActivity"))
    return jni_make_activity_object();
  /* ArmoryActivity.getPermissionPlugin() -> PermissionPlugin.
   *
   * THIS REVERSES AN EARLIER DECISION (§20c), and the reason is worth keeping.
   * It was left null on the argument that a handle would make the game request
   * permissions and then wait on a UnitySendMessage callback we cannot deliver.
   * Reading the real classes2.dex shows that argument was wrong:
   *
   *   PermissionPlugin.hasPermission(String)      -> Z   SYNCHRONOUS
   *   PermissionPlugin.canRequestPermissions()    -> Z   SYNCHRONOUS
   *   PermissionPlugin.requestPermissions(String) -> V   async (SendMessage)
   *
   * The async path is only entered when the SYNCHRONOUS check says a permission
   * is missing. Answer hasPermission() truthfully-for-this-platform (see
   * act_int) and the game never reaches the callback path at all.
   *
   * Why it matters: the caller is
   *   internal sealed class LoginGooglePlayGames : IAuthenticationState
   *     public void Handle(Action<object> resultAction)
   *     private bool HasPermissions() / private void RequestPermissions()
   *     private void OnPermissionResult(object, PermissionRequestArgs)
   *     private void Authenticate()
   * -- an authentication STATE MACHINE that only advances when `resultAction`
   * is invoked. With a null plugin, HasPermissions() is false (or throws),
   * RequestPermissions() does nothing, OnPermissionResult never fires, and the
   * state never completes. Nothing downstream of login ever runs. */
  if (name_has(id->cls, "ArmoryActivity") && !strcmp(id->name, "getPermissionPlugin"))
    return jni_make_object("com/squareenixmontreal/armory/PermissionPlugin");

  /* com.prime31.GoogleIABPlugin.instance() -> the IAB plugin singleton.
   *
   * SECOND REVERSAL OF THE SAME MISTAKE (the first was getPermissionPlugin,
   * §21b). This was left null on the identical argument -- "a handle would push
   * the game into dead code" -- and boot 6 printed that rationale one line
   * before the exception it caused:
   *
   *   [jni] UNMATCHED -> null: ...GoogleIABPlugin.instance() [REVIEWED: no IAB;
   *         a handle would push the game into dead code]
   *   [nre] #2  frame 0 il2cpp+0x140a850   <- Prime31.GoogleIAB::init
   *
   * The cascade that follows is what actually blacks out the screen:
   *
   *   instance() -> null
   *     -> Prime31.GoogleIAB::init throws (caught)
   *       -> the shop's offer collection is never initialised
   *         -> Technology.Shop.AbstractShop::RefreshOffers throws -- UNHANDLED
   *           -> the [AutoInstantiate] singleton pass ABORTS
   *             -> ViewManager (Resources.Load "UI/ViewManager") is never created
   *               -> no ViewController ever exists, and the only object on
   *                  screen is the scene's "BlackPanel" overlay.
   *
   * Boot 6 evidence: AudioManager / NotificationManager / MusicManager -- all
   * [AutoInstantiate] singletons like ViewManager -- load fine BEFORE the
   * unhandled throw, and "UI/ViewManager" is never requested at all.
   *
   * Safe to hand back a handle: every initialisation method on the real class
   * (classes2.dex) returns void -- init(String), enableLogging(Z),
   * connectBillingService(), queryInventory(...), consumeProduct(String),
   * unbindService(). Nothing downstream needs a value we would have to invent.
   * The two that do return data, areSubscriptionsSupported() -> Z and
   * getPurchaseHistory() -> String, are not called during boot; if they appear,
   * the UNMATCHED log will name them. */
  if (name_has(id->cls, "GoogleIABPlugin") && !strcmp(id->name, "instance"))
    return jni_make_object("com/prime31/GoogleIABPlugin");
  /* Activity.getWindow() -> Window -> getDecorView() -> View. Unity walks this
   * chain during display setup; a null Window ends it at the first step. */
  if (!strcmp(id->name, "getWindow"))    return jni_make_object("android/view/Window");
  if (!strcmp(id->name, "getDecorView")) return jni_make_object("android/view/View");
  /* getContentResolver() feeds Settings$Secure.getString(resolver, "android_id").
   * The device id degrades to "" either way, but a null resolver throws on the
   * way there instead of degrading. */
  if (!strcmp(id->name, "getContentResolver"))
    return jni_make_object("android/content/ContentResolver");
  /* Object.getClass(). Unity's _AndroidJNIHelper.GetSignature(object) calls
   * this on a live Java object to build a JNI signature; a null result throws
   * inside GetSignature, which is NREs #1 and #4 in the boot-3 log (both
   * immediately after `getMethodID(java/lang/Object.getClass)`). The class had
   * no handler at all, and its return type is Ljava/lang/Class; or
   * Ljava/lang/Object; depending on the call site, so neither string fallback
   * caught it either.
   *
   * Returning the class the method id was minted against is the honest answer
   * we can give: the reflection bridge has already collapsed most receivers to
   * java/lang/Object, so this yields a valid, interned, self-consistent Class
   * rather than a null. Exact strcmp, NOT name_has -- "getClass" is a prefix of
   * "getClassLoader" and swallowing that would be wrong. */
  if (!strcmp(id->name, "getClass"))
    return intern_class(id->cls[0] ? id->cls : "java/lang/Object");
  /* ClassLoader handles are only ever passed back to us (Class.forName's third
   * argument, findLibrary), so any non-null interned object serves. Deliberately
   * NOT handling forName itself: returning a Class for a type that genuinely is
   * not present (PlayGames, IAB) would push the game INTO code that cannot work
   * here, which is worse than the ClassNotFound path it takes today. */
  if (!strcmp(id->name, "getClassLoader"))
    return jni_make_object("java/lang/ClassLoader");

  /* Settings.Secure.getString(resolver, "android_id") and friends. The generic
   * "" fallback below is a bad answer specifically here: the device id is used
   * to key analytics identity and, in some titles, the local profile. Code that
   * treats "" as "no id yet" can retry or refuse to initialise. Hand back a
   * stable, syntactically plausible 64-bit hex id -- constant across boots so
   * anything cached against it stays valid. */
  if (name_has(id->cls, "Settings$Secure") && name_has(id->name, "getString"))
    return jni_make_string("a1b2c3d4e5f60718");

  /* ANY array-returning method that reached here. Java callers do arr.length or
   * a foreach immediately; an EMPTY array is always a safe answer and a null
   * never is. This is a general rule rather than a per-method case because the
   * boot-4 UNMATCHED list produced four of these at once (getDevices,
   * getDeviceIds, getObbDirs, getSupportedModes) and the next binary will
   * produce more. "There are none" is both true and harmless on this platform. */
  {
    const char *ret = strchr(id->sig, ')');
    if (ret && ret[1] == '[') {
      debugPrintf("[jni] UNMATCHED -> empty array: %s.%s%s\n",
                  id->cls[0] ? id->cls : "?", id->name, id->sig);
      /* [I / [F / [B ... primitives carry an element size; object arrays don't. */
      if (ret[2] == 'L' || ret[2] == '[')
        return j_NewObjectArray(NULL, 0, NULL, NULL);
      return make_pri_array_adopt(calloc(1, 1), 0,
                                  (ret[2] == 'J' || ret[2] == 'D') ? 8 :
                                  (ret[2] == 'S' || ret[2] == 'C') ? 2 :
                                  (ret[2] == 'Z' || ret[2] == 'B') ? 1 : 4);
    }
  }

  /* Nothing matched. Both fallbacks below are silent wrong answers, and this
   * is exactly the shape of bug that cost the most time on this port:
   * Locale.getDefault() fell through to `return NULL`, the managed side called
   * .getLanguage() on it, and LocalizationController's constructor died --
   * taking every localized UI string with it. Locale.getLanguage() fell through
   * to the "" branch and would have left the language blank even once the null
   * was fixed. Neither produced a single line of log.
   *
   * So name them. One boot now lists every Java call we answer with a guess,
   * which is the work list for the next one -- the same approach as the UNIMPL
   * slot stubs (PORTING §8a) and the ARGS DROPPED warning (§14e). Deduplicated
   * per (class, method); costs nothing after the first hit. */
  {
    /* Entries we have looked at and DECIDED to leave as a guess. Without this,
     * every boot re-presents the same settled questions and the genuinely new
     * ones get lost in the noise -- the list is only useful if it shrinks.
     * A line WITHOUT "REVIEWED" is new work; a line with it has a rationale in
     * PORTING §20 and should not be re-litigated without new evidence. */
    static const struct { const char *cls_frag; const char *name; const char *why; }
    reviewed[] = {
      { "StringBuilder", "toString",
        "launch args are genuinely empty (Bundle.containsKey(unity) -> 0)" },
      { "ClassLoader", "findLibrary",
        "dlopen is intercepted; the returned path is never used" },
      { "PlayGames", "getGamesSignInClient",
        "no Play Services; a handle would push the game into dead code" },
      { "NotificationClient", "requestGcmToken",
        "no push token exists on this platform" },
      { "MediaRouter", "getSelectedRoute",
        "audio routing is fixed; audio works" },
      { "PlayAssetDeliveryUnityWrapper", "init",
        "PAD is absent and null is the signal Unity expects (playCoreApiMissing=1)" },
    };
    const char *why = NULL;
    for (unsigned i = 0; i < sizeof reviewed / sizeof reviewed[0]; i++)
      if (name_has(id->cls, reviewed[i].cls_frag) && !strcmp(id->name, reviewed[i].name)) {
        why = reviewed[i].why; break;
      }
    static struct { char cls[96]; char name[64]; } seen[64];
    static unsigned n;
    int dup = 0;
    for (unsigned i = 0; i < n; i++)
      if (!strcmp(seen[i].cls, id->cls) && !strcmp(seen[i].name, id->name)) { dup = 1; break; }
    if (!dup) {
      if (n < sizeof seen / sizeof seen[0]) {
        snprintf(seen[n].cls, sizeof seen[n].cls, "%s", id->cls);
        snprintf(seen[n].name, sizeof seen[n].name, "%s", id->name);
        n++;
      }
      const char *what = sig_returns(id->sig, "Ljava/lang/String;") ? "\"\"" : "null";
      if (why)
        debugPrintf("[jni] UNMATCHED -> %s: %s.%s%s [REVIEWED: %s]\n",
                    what, id->cls[0] ? id->cls : "?", id->name, id->sig, why);
      else if (sig_returns(id->sig, "Ljava/lang/String;"))
        debugPrintf("[jni] UNMATCHED -> \"\": %s.%s%s -- guessed empty string\n",
                    id->cls[0] ? id->cls : "?", id->name, id->sig);
      else
        debugPrintf("[jni] UNMATCHED -> null: %s.%s%s -- managed code calling a "
                    "method on this result will throw\n",
                    id->cls[0] ? id->cls : "?", id->name, id->sig);
    }
  }
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string(""); // UUID, asset-pack name, etc.
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  if (name_has(id->cls, "Handler") && name_has(id->name, "post")) {   /* post/postDelayed(Runnable) */
    post_runnable(va_arg(va, void *)); return 1;
  }
  /* Bundle.containsKey(key): the launch-args probe -- true for the 'unity' extra so
   * Unity proceeds to Bundle.getString(unity) (see act_object) and applies args. */
  if (name_has(id->cls, "Bundle") && name_has(id->name, "containsKey")) {
    const char *k = first_string_arg(id->sig, va);
    /* PVZ_UNITY_LAUNCH_ARGS is empty now: the -job-worker-count 0 injection was a
     * crash-era experiment. Starving the job workers stops Unity building
     * culling/batching/UI geometry -> frames advance but nothing renders.
     * Report the extra as ABSENT so Unity uses its normal defaults. */
    int hit = 0; (void)k;
    debugPrintf("[jni] Bundle.containsKey(%s) -> %d\n", k ? k : "?", hit);
    return hit ? 1 : 0;
  }
  // java.lang.Integer.parseInt(String[,radix]) / Long.parseLong: FMOD's audio
  // path parses getProperty()'s "48000"/"64" results through these. The old
  // act_int fall-through returned 0 -> framesPerBuffer parsed to 0 -> FMOD's
  // OpenSL output init failed with "Error initializing output device" (60).
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u\n", id->name, s ? s : "", v);
    return v;
  }
  if (is_editbox_open(id->name)) return (juint)editbox_is_open();
  // some builds expose Show/Open as an int (success) call rather than void
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 32); return 1; }
  // Play Asset Delivery: with NO Play Core on Switch, the engine MUST take the
  // "missing" path, where it treats every asset pack as install-time/local and
  // reads assets synchronously from the APK/bundle. Returning false (the old
  // default) tells Unity Play Core IS present, so it uses the ASYNC AssetPackManager
  // path -- it calls getAssetPackState() with a callback that we never invoke and
  // then waits forever for the pack, so the resident scene never loads and
  // ResidentSystem.Awake never runs (the live boot stall). Return true.
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  /* Android runtime permissions -- report everything as already granted.
   *
   * Derived from the real classes2.dex, not guessed. PermissionPlugin exposes
   * hasPermission(String)/canRequestPermissions() as SYNCHRONOUS booleans, and
   * only enters the asynchronous requestPermissions() ->
   * onRequestPermissionsResult -> SendMessage path when one of them says no.
   *
   * Returning false (the old act_int fall-through) therefore pushed the game
   * into a callback we have no way to deliver. Its caller is
   * `LoginGooglePlayGames : IAuthenticationState`, whose Handle(resultAction)
   * only advances the boot state machine once the permission result arrives --
   * so a false here stalls login permanently, and everything after it.
   *
   * True is also the honest answer. The manifest asks for INTERNET,
   * ACCESS_NETWORK_STATE and WAKE_LOCK (normal permissions, auto-granted on
   * Android) plus ACCESS_COARSE_LOCATION and ACCOUNT_MANAGER, none of which
   * mean anything on Switch -- there is no permission system to deny them, and
   * the capabilities behind them are stubbed regardless.
   *
   * shouldShowRequestPermissionRationale stays false: with everything granted
   * there is no rationale UI to show, and true would ask the game to display a
   * dialog for a permission it already has. */
  if (name_has(id->name, "hasPermission") ||
      name_has(id->name, "canRequestPermissions") ||
      name_has(id->name, "checkSelfPermission") ||
      name_has(id->name, "hasUserAuthorizedPermission")) {
    debugPrintf("[jni] %s.%s -> granted\n", id->cls[0] ? id->cls : "?", id->name);
    return 1;
  }
  if (name_has(id->name, "shouldShowRequestPermissionRationale")) return 0;

  /* Unity-as-a-Library: we are a normal player, not an embedded view. */
  if (name_has(id->name, "isUaaLUseCase")) return 0;
  /* No ARCore on Switch. Returning 1 here would have the engine wait on an AR
   * session that never starts. */
  if (name_has(id->name, "initializeGoogleAr")) return 0;
  // The other Google check whose default (0 == ConnectionResult.SUCCESS) wrongly
  // means "Play Services available": isGooglePlayServicesAvailable(). Return
  // SERVICE_MISSING (1) so the Play Games plugin cleanly disables itself instead
  // of trying to sign in against GMS that isn't there. (Not hit during boot --
  // Play Games activates on user action -- but correct for when it is.)
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  (void)va;
  // every other "is something open / clicked / ok" probe -> false/0
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  if (name_has(id->name, "runOnUiThread")) { post_runnable(va_arg(va, void *)); return; }
  if (name_has(id->name, "sendToTarget")) { post_message(); return; }
  if (name_has(id->name, "postFrameCallback")) {
    void *cb = va_arg(va, void *);
    mutexLock(&g_runq_lk);
    if (!g_runq_started) {
      g_runq_started = 1;
      if (R_SUCCEEDED(threadCreate(&g_runq_thr, run_drain_thread, NULL, NULL, 0x8000, 0x2C, -2)))
        threadStart(&g_runq_thr);
    }
    g_frame_cb = cb;
    condvarWakeOne(&g_runq_cv);
    mutexUnlock(&g_runq_lk);
    static int fclog = 0; if (fclog < 3) { fclog++; debugPrintf("[jni] postFrameCallback cb=%p (vsync pump on)\n", cb); }
    return;
  }
  if (name_has(id->name, "removeFrameCallback")) {
    mutexLock(&g_runq_lk); g_frame_cb = 0; mutexUnlock(&g_runq_lk); return;
  }
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 64); return; }
  if (is_editbox_close(id->name)) { editbox_close(); return; }
  kbd_sniff(id->cls, id->name);
  (void)va;
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

/* ZOOKEEPER DX port: delegate Unity/input classes to our modules */
#include "unity_jni.h"
#include "unity_input.h"

static int is_t2b(const char *cls)  { return name_has(cls, "Text2Bitmap"); }
static int is_mov(const char *cls)  { return name_has(cls, "MoviePlayer"); }

// Breadcrumb: the game's own Java side is reached only through JNI upcalls. The
// DEX shows which classes exist but not which the C# actually invokes, or in
// what order. Log each unique app-class upcall once, so the first run that
// reaches a plugin tells us precisely what to implement instead of guessing.
// Behaviour is unchanged: after logging the call still falls through to act_*.
//
// The filter list is THIS game's plugin surface (from its dex), not the
// reference port's -- an unmatched filter here silently produces an empty work
// list, which is the worst possible failure for a diagnostic. See PORTING.md
// section 8.
static const char *const APP_PKGS[] = {
  "squareenixmontreal",   /* Armory: dialogs, GPGS, notifications, profile   */
  "prime31",              /* Etcetera + InAppBilling                          */
  "GooglePlayGames", "google/games", "gms/games",   /* GPGS: Java side is
                             com.google.android.gms.games.*, C# side is
                             GooglePlayGames.* -- both need catching */
  "play/core",            /* Play Asset Delivery / integrity                  */
  "firebase",
  "billingclient", "unity/purchasing",
};
static void log_app_upcall(const FakeID *id) {
  int hit = 0;
  for (unsigned i = 0; i < sizeof(APP_PKGS)/sizeof(APP_PKGS[0]); i++)
    if (name_has(id->cls, APP_PKGS[i])) { hit = 1; break; }
  if (!hit) return;
  static const char *seen[64]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return; // interned name ptr
  if (seen_n < 64) seen[seen_n++] = id->name;
  debugPrintf("[jni] app upcall: %s.%s%s\n", id->cls, id->name, id->sig);
  /* If Android would have answered this one with a UnitySendMessage callback,
   * record what we are failing to deliver. Logs only -- see unity_sendmessage.h
   * for why nothing is fabricated and sent. */
  nx_sendmsg_note_upcall(id->cls, id->name, id->sig);
}

/* Mint the FakeID that Unity's reflection bridge expects back from
 * ReflectionHelper.getMethodID/getFieldID/getConstructorID.
 * `cls_obj` is the java.lang.Class, `a0`/`a1` the name/signature strings (for
 * getConstructorID there is no name, only a signature). */
static void *reflect_make_id(const FakeID *id, void *cls_obj, void *a0, void *a1) {
  const char *cname = fake_class_name(cls_obj);
  const char *mname, *msig;
  if (name_has(id->name, "getConstructorID")) { mname = "<init>"; msig = jni_string_utf(a0); }
  else                                        { mname = jni_string_utf(a0); msig = jni_string_utf(a1); }
  FakeID *made = get_id(cname ? cname : "", mname ? mname : "", msig ? msig : "()V");
  debugPrintf("[jni] ReflectionHelper.%s(%s.%s %s) -> id %p\n", id->name,
              cname ? cname : "?", mname ? mname : "?", msig ? msig : "?", (void *)made);
  return made;
}

static int reflect_wants_id(const FakeID *id) {
  return LCGO_JNI_REFLECTION && name_has(id->cls, "ReflectionHelper") &&
         (name_has(id->name, "getMethodID") || name_has(id->name, "getConstructorID") ||
          name_has(id->name, "getFieldID"));
}

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  log_app_upcall(id);

  /* ---- reflection objects arriving as receivers -------------------------
   * ReflectionHelper.getMethodID/getFieldID hand back a FakeID so that
   * AndroidJNI.FromReflectedMethod can round-trip it into a jmethodID. Unity
   * ALSO treats that same value as a live java.lang.reflect.Field/Method and
   * calls methods on it. A FakeID is {tag, cls[96], name[64], sig[160]}; the
   * generic paths below assume FakeString {tag, char *utf} and read offset 8 as
   * a pointer -- which lands inside the cls string. That is the observed crash:
   * far=0x6374696f00000000, ASCII "ctio" from a class name, dereferenced.
   *
   * So handle a TAG_ID receiver here and never fall through with one. */
  if (recv && *(uint32_t *)recv == TAG_ID) {
    const FakeID *r = (const FakeID *)recv;
    if (name_has(id->name, "getDeclaringClass"))
      return jni_make_object(r->cls[0] ? r->cls : "java/lang/Object");
    if (name_has(id->name, "getName"))
      return jni_make_string(r->name);
    if (name_has(id->name, "getSignature") || name_has(id->name, "toGenericString"))
      return jni_make_string(r->sig);
    if (name_has(id->name, "toString"))
      return jni_make_string(r->name);
    debugPrintf("[jni] reflect obj %s.%s on %s.%s -> null (unhandled)\n",
                id->cls, id->name, r->cls, r->name);
    return NULL;
  }

  /* ---- Unity's reflection bridge ---------------------------------------
   * Every AndroidJavaObject.Call()/Get() in managed code resolves through
   * com.unity3d.player.ReflectionHelper: getMethodID(Class,name,sig,static)
   * hands back a java.lang.reflect.Method, which AndroidJNI.FromReflectedMethod
   * then converts to a jmethodID. Our j_FromReflectedMethod already passes a
   * TAG_ID straight through, so the right move is to mint the FakeID here and
   * return it as the "Method" object -- the round trip then lands on a real id
   * carrying cls/name/sig, and the call dispatches normally.
   *
   * Unhandled, this returned a generic object: the game reflected into
   * ArmoryActivity (log: getConstructorID/getMethodID/getFieldID right after
   * FindClass) and every plugin call it made afterwards went nowhere. */
  if (reflect_wants_id(id)) {
    void *c = va_arg(va, void *);
    void *a0 = va_arg(va, void *);
    void *a1 = name_has(id->name, "getConstructorID") ? NULL : va_arg(va, void *);
    return reflect_make_id(id, c, a0, a1);
  }

  /* Getters that must be genuinely absent. A non-null placeholder here is worse
   * than null: getLaunchURL feeding a junk string into the deep-link parser, or
   * a fake proxy string making the game try to route traffic. */
  if (name_has(id->name, "getLaunchURL") ||
      name_has(id->name, "getNetworkProxySettings") ||
      name_has(id->name, "getDeepLink")) {
    debugPrintf("[jni] %s -> null (deliberate)\n", id->name);
    return NULL;
  }

  /* Anything under java.lang.reflect that is not one of our FakeIDs still must
   * not fall through: the generic getters below assume shapes these objects do
   * not have. Answer plausibly or return null. */
  if (name_has(id->cls, "java/lang/reflect")) {
    debugPrintf("[jni] reflect %s.%s -> null (unmodelled receiver)\n", id->cls, id->name);
    return NULL;
  }

  /* ReflectionHelper.getFieldSignature(Field) / getMethodSignature(Method):
   * the argument is one of ours, so answer from the FakeID rather than letting
   * it reach a path that dereferences it. */
  if (name_has(id->cls, "ReflectionHelper") && name_has(id->name, "Signature")) {
    void *a = va_arg(va, void *);
    if (a && *(uint32_t *)a == TAG_ID)
      return jni_make_string(((const FakeID *)a)->sig);
    return jni_make_string("");
  }
  /* MotionEvent.obtain(MotionEvent): copy factory. The engine copies our
   * injected event and reads the copy after inject returns; return a real
   * UEvent copy so getSource/getX/getY on it hit our handlers (else they read
   * 0, getSource looks non-touch, and the event is dropped before getX/getY). */
  if (input_owns_class(id->cls) && !strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes([charset]) -> byte[] of the string's UTF-8 bytes. Unity's
   * PlayerPrefs key-encoding is key.getBytes() -> new String([B,charset) ->
   * Uri.encode(...); without real bytes the whole chain collapsed to "" and
   * every encoded-key pref collided. Route by the FakeString receiver. */
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  /* Class.getName() on one of our interned class handles. A CONSEQUENCE of the
   * getClass() fix: code that used to die on a null Class now proceeds and asks
   * for its name, which fell to act_object's "" fallback (visible as
   * `UNMATCHED -> "": java/lang/Object.getName()` in the boot-4 log). Must live
   * here rather than in act_object, because only this layer still has `recv` --
   * the name belongs to the receiver, not to the method id's class.
   * class_name_of() yields "" for anything that is not one of ours, so a
   * getName() on some other object falls through untouched. */
  if (!strcmp(id->name, "getName")) {
    const char *cn = class_name_of(recv);
    if (cn && cn[0]) return jni_make_string(cn);
  }
  if (unity_owns_class(id->cls)) return unity_dispatch_object(recv, id, va);
  // any method returning a Bitmap is text rendering (Char2Bitmap / getShadowBitmap
  // / ...): the loaded class always reads back as java/lang/Object, so route by
  // return type rather than class name.
  const int wants_bitmap = sig_returns(id->sig, "Landroid/graphics/Bitmap;");
  return (is_t2b(id->cls) || wants_bitmap) ? t2b_object(id, va) : act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  log_app_upcall(id);
  // java.lang.String instance methods reached via CallIntMethod (Unity's
  // java::lang::String::length() does this to size path buffers). The receiver
  // is our FakeString; GetObjectClass reports it as java/lang/Object, so route
  // on the receiver tag + method name, NOT id->cls. Returning 0 here (the old
  // act_int fall-through) undersizes the OBB-path sprintf buffer and overflows.
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
  }
  /* Boxed PlayerPrefs value (Integer/Long/Boolean) from getAll(): unbox by the
   * receiver so only our own boxes are affected. intValue/longValue/booleanValue
   * all land here (CallInt/Long/BooleanMethod -> dispatch_int). */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* MotionEvent/KeyEvent getters: the engine resolves these via
   * GetObjectClass(event) -> java/lang/Object, so id->cls is NOT the real
   * class. Route on the receiver tag (mirrors the FakeString case above), or
   * touch getters silently fall through to act_int and return 0. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  if (is_t2b(id->cls)) return t2b_int(id, va);
  if (is_mov(id->cls)) return mov_int(id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  log_app_upcall(id);
  if (name_has(id->cls, "FMODAudioDevice")) {
    debugPrintf("[fmod] FMODAudioDevice.%s() CALLED\n", id->name);
    /* Path A (driving fmodProcess) is blocked: the mixer's source buffer is
     * allocated only by the AudioTrack output start sequence the Java run() loop
     * drives, which never runs here -- so fmodProcess always copies from a null
     * source (Data Abort at +0x28), confirmed even after a 120-frame warmup.
     * Pump left in the tree but disabled; audio is moving to FMOD OutputOpenSL
     * (Path B), which FMOD drives natively via opensles.c. */
    if (0 && !strcmp(id->name, "start")) fmod_audio_start();
  }
  if (unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  if (is_mov(id->cls)) { mov_void(id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (obj && *(uint32_t *)obj == BITMAP_TAG) return intern_class("android/graphics/Bitmap");
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* String(byte[][,charset]) constructor: Unity builds PlayerPrefs keys as
 * bytes -> new String(bytes, charset) -> Uri.encode(...). Returning a
 * content-less object made every encoded key empty, so all such prefs collided
 * under "" (corrupted Screenmanager resolution prefs -> bad resolution ->
 * crash). Decode the byte array (UTF-8) into a real FakeString. Other ctors are
 * unaffected (still a labelled object). */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  /* new Scanner(InputStream, charset): hand the stream to unity_jni.c, which
   * owns the InputStream handles. Unity uses this to read boot.config -- see
   * unity_make_scanner(). Without it the constructor produced a label-only
   * object and every later call fell through to null/"". */
  if (cn && strstr(cn, "java/util/Scanner"))
    return unity_make_scanner(first_arg);
  if (cn && strstr(cn, "java/lang/String")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid); void *a0 = va_arg(va, void *); va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; void *a0 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf (slot 32). The unimpl stub returned 0 (false), trapping the game
 * in a per-frame retry loop on the black screen: it does obj=jniCall(); if
 * (IsInstanceOf(obj, Expected)) proceed; else retry. We can't track the runtime
 * type of opaque fake jobjects, so answer optimistically: per the JNI spec a
 * NULL object is an instance of any class, and for our fake objects assuming the
 * cast succeeds lets the game move forward instead of spinning. Logged (capped)
 * so we can see which class it is keying on. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* nativeInjectEvent classifies the event by instanceof KeyEvent / MotionEvent
   * and picks its handler accordingly. If we blindly return 1, the KeyEvent
   * check (which it does first) matches our touch event and it gets read as a
   * key (getKeyCode) and dropped. Answer by the handle's real kind. */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    /* InputEvent base class, or class names collapsed by pool overflow: both
     * kinds are InputEvents, so 1 is safe for the base; overflow is now logged. */
    return 1;
  }
  /* Boxed PlayerPrefs values from getAll(): Unity reads each value with
   * IsInstanceOf(value, Integer/Long/Float/Boolean/String) then unboxes. These
   * MUST be exact or every value is misread as the first type checked. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
    /* other classes: fall through to the optimistic answer below */
  }
  static int logn = 0;
  if (logn < 16) { logn++;
    debugPrintf("JNI: IsInstanceOf(obj=%p, clazz=%s) -> 1\n", obj, cn); }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

static uint64_t dispatch_long(void *recv, const FakeID *id, va_list va) {
  if (name_has(id->name, "nanoTime")) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
  if (name_has(id->name, "currentTimeMillis")) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
  }
  if (!unity_is_boxed(recv) && name_has(id->name, "longValue")) return g_frame_ns;
  return (uint64_t)dispatch_int(recv, id, va);
}
CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, uint64_t, dispatch_long)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}
/* CallNonvirtual<Object>Method{,V,A}: (env, obj, clazz, methodID, args) -- like the
 * virtual object call but with an extra clazz arg our dispatch ignores. Slot 65
 * (V form) is called on the PlayAssetDelivery/package path and was UNIMPL -> null. */
static void *j_CallNonvirtualObjectMethodV(void *env, void *recv, void *clazz, FakeID *id, va_list va) {
  (void)env; (void)clazz; return dispatch_object(recv, id, va);
}
static void *j_CallNonvirtualObjectMethod(void *env, void *recv, void *clazz, FakeID *id, ...) {
  (void)env; (void)clazz; va_list va; va_start(va, id);
  void *r = dispatch_object(recv, id, va); va_end(va); return r;
}
static void *j_CallNonvirtualObjectMethodA(void *env, void *recv, void *clazz, FakeID *id, const void *a) {
  (void)a; return j_CallNonvirtualObjectMethod(env, recv, clazz, id);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
// SWIG bindings and AndroidJavaObject.CallStatic<T>()/Call<T>() marshal their
// arguments into a jvalue[] array and invoke the "A" variants. A va_list cannot
// be reconstructed from jvalue[] portably, so we forward to the variadic form
// with no varargs: the dispatch keys off the resolved method name and the
// object/value getters ignore positional args (defaulting to a non-null handle
// of the right class), which is what these init paths need. Previously these
// slots fell through to the unimplemented stub and returned 0/null, hanging the
// first scene (e.g. UNIMPL slot 116 == CallStaticObjectMethodA).
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  /* Unity's AndroidJavaObject bridge calls ReflectionHelper through the "A"
   * form, so the arguments arrive as a jvalue[] and the forward below drops
   * them. Reading them as varargs downstream picks up stale stack -- which is
   * how a class-name string ended up dereferenced as a pointer (far="ctio").
   * Handle it here, where the array is actually in hand. */
  if (a && reflect_wants_id(id)) {
    void *const *jv = (void *const *)a;
    return reflect_make_id(id, jv[0], jv[1],
                           name_has(id->name, "getConstructorID") ? NULL : jv[2]);
  }
  if (a && name_has(id->cls, "ReflectionHelper") && name_has(id->name, "Signature")) {
    void *const *jv = (void *const *)a;
    if (jv[0] && *(uint32_t *)jv[0] == TAG_ID)
      return jni_make_string(((const FakeID *)jv[0])->sig);
    return jni_make_string("");
  }
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return (void *)((void *const *)a)[0];        /* identity, jvalue path */
  return j_CallObjectMethod(e, r, id);
}
/* The "A" (jvalue[]) forms below forward to the varargs dispatchers, which read
 * their arguments with va_arg -- so any argument that arrived in the jvalue[]
 * array is DROPPED, and the dispatcher reads stale stack instead. Where that
 * mattered it has been special-cased above (getProperty, ReflectionHelper,
 * parseInt, Uri.encode...). The rest fall through silently, which is the wrong
 * failure mode: on Android both calling conventions carry identical arguments,
 * so a call that works through CallBooleanMethodV can return garbage through
 * CallBooleanMethodA with no indication anything happened.
 *
 * This warns once per (class, method) whenever a call with a non-empty argument
 * signature takes the dropping path, so the log names exactly which bindings
 * need a jvalue-aware case -- the same "loud, specific failure" approach the
 * UNIMPL slot stubs use. It does not change behaviour. */
static int sig_has_args(const FakeID *id) {
  if (!id) return 0;
  const char *o = strchr(id->sig, '(');
  return o && o[1] && o[1] != ')';
}
static void warn_dropped_args(const FakeID *id, const char *form) {
  if (!sig_has_args(id)) return;
  static struct { char cls[96]; char name[64]; } seen[48];
  static unsigned n;
  for (unsigned i = 0; i < n; i++)
    if (!strcmp(seen[i].cls, id->cls) && !strcmp(seen[i].name, id->name)) return;
  if (n < sizeof(seen) / sizeof(seen[0])) {
    snprintf(seen[n].cls, sizeof seen[n].cls, "%s", id->cls);
    snprintf(seen[n].name, sizeof seen[n].name, "%s", id->name);
    n++;
  }
  debugPrintf("[jni] ARGS DROPPED (%s): %s.%s%s -- jvalue[] path has no "
              "jvalue-aware case; args read as stale stack\n",
              form, id->cls, id->name, id->sig);
}

static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){
  (void)a; warn_dropped_args(id, "Boolean"); return j_CallBooleanMethod(e, r, id); }
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u [A]\n", id->name, s ? s : "", v);
    return v;
  }
  (void)a; warn_dropped_args(id, "Int"); return j_CallIntMethod(e, r, id);
}
static uint64_t j_CallLongMethodA(void *e, void *r, FakeID *id, const void *a){
  (void)a; warn_dropped_args(id, "Long"); return j_CallLongMethod(e, r, id); }
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){
  (void)a; warn_dropped_args(id, "Float"); return j_CallFloatMethod(e, r, id); }
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){
  (void)a; warn_dropped_args(id, "Void"); j_CallVoidMethod(e, r, id); }
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) { // naive UTF-16 -> UTF-8 (BMP)
    const uint32_t c = u[i];
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 variant; widen ASCII bytes into jchar (uint16) buf.
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  for (int i = 0; i < len; i++) buf[i] = (uint8_t)s[start + i];
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// GetStringChars / ReleaseStringChars -- JNIEnv slots 165 and 166.
//
// These were missing from env_table, so both fell through to the UNIMPL stub
// and returned 0. That is not a cosmetic gap: libunity's *native* C++ reads
// strings through the UTF-8 accessors above (which is why most of the JNI
// surface worked), but Unity's *managed* AndroidJavaObject / AndroidReflection
// bridge marshals every Java string return through this UTF-16 pair, because
// C# strings are UTF-16. With them stubbed, managed code saw an empty string
// for every Java string it asked for. In debug.log that showed up as:
//
//   [jni] ReflectionHelper.getFieldID(...UnityPlayer.currentActivity Ljava/lang/Object;)
//   JNI: UNIMPL slot 165
//   JNI: UNIMPL slot 166
//   JNI id: com/unity3d/player/UnityPlayer.currentActivity      <- signature blank
//
// i.e. the reflected field's type name came back empty, the signature was
// rebuilt from nothing, and the lookup that followed produced the log's only
// NullReferenceException.
//
// Contract notes: the returned buffer must stay valid until the matching
// ReleaseStringChars, so it is heap-allocated rather than pointed at shared
// storage. The code-unit count matches j_GetStringLength exactly (same
// utf16_len decode, surrogate pairs included), because callers size their
// reads with GetStringLength and index the buffer up to it.
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 1;              /* always a copy -- we own the buffer */
  const char *s = obj_str(jstr);
  const juint n = utf16_len(s);
  uint16_t *out = malloc(((size_t)n + 1) * sizeof(uint16_t));
  if (!out) return NULL;                  /* JNI allows NULL on alloc failure */
  const unsigned char *p = (const unsigned char *)s;
  juint o = 0;
  while (*p && o < n) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    if (cp >= 0x10000 && o + 1 < n) {     /* astral plane -> surrogate pair */
      cp -= 0x10000;
      out[o++] = (uint16_t)(0xD800 | (cp >> 10));
      out[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    } else {
      out[o++] = (uint16_t)(cp > 0xFFFF ? 0xFFFD : cp);
    }
    p += adv;
  }
  out[o] = 0;
  return out;
}
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and the game DO read Java fields: android.os.Build.* (device id),
// Build.VERSION.SDK_INT (API gating), PackageInfo.versionName/versionCode (the
// Application.version the boot path logs as blank today), DisplayMetrics.*, and
// Configuration.*. Returning null/0 universally (the old stub) blanks the app
// version -- which can throw in version-parsing boot code -- and zeroes display
// metrics. Route every field read through a name-based dispatcher. fid is the
// FakeID GetFieldID handed back, so cls/name/sig are all available.
// (APP_VERSION_NAME / APP_VERSION_CODE / NX_SDK_INT are defined near the top of
// this file, since act_object() needs them too.)

static int fld_is(const FakeID *id, const char *cls_sub, const char *name) {
  return name_has(id->cls, cls_sub) && !strcmp(id->name, name);
}

// One-line-per-unique-field diagnostic: tells the next run exactly which Java
// fields the game reads (and lets us confirm versionName/currentActivity/etc.
// are being exercised). Dedup by interned name pointer, like log_app_upcall.
static void log_field_read(const FakeID *id, char kind) {
  static const void *seen[128]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return;
  if (seen_n < 128) seen[seen_n++] = id->name;
  debugPrintf("[jni] field(%c): %s.%s %s\n", kind, id->cls, id->name, id->sig);
}

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  /* software-keyboard result: the engine reads it as a String field */
  if (n && (!strcmp(n, "text") || !strcmp(n, "mText") ||
            !strcmp(n, "inputText") || !strcmp(n, "m_Text") ||
            name_has(n, "KeyboardText") || name_has(n, "EditBoxText"))) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text FIELD %s.%s -> \"%s\"\n", c ? c : "?", n, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);

  /* ApplicationInfo path fields.
   *
   * This game ships as an App Bundle: the engine bootstrap is in the base APK
   * and the bulk of assets/bin/Data is an install-time Play Asset Delivery pack
   * (split_UnityDataAssetPack). libunity resolves that at runtime by calling
   * getApplicationInfo() and reading `splitPublicSourceDirs` -- confirmed: both
   * "getApplicationInfo" and "splitPublicSourceDirs" are literals in libunity.
   *
   * We do not have APKs; tools/install_assets.py merges both halves into a plain
   * directory on the SD card, so the honest answer is "there are no split APKs"
   * and the data lives at data_dir(). Returning an EMPTY String[] rather than
   * NULL matters: Unity iterates the array, and a null here is a crash in native
   * code rather than a clean fallback to the normal data path. */
  if (!strcmp(n, "splitPublicSourceDirs") || !strcmp(n, "splitSourceDirs") ||
      !strcmp(n, "splitNames")) {
    debugPrintf("[data] ApplicationInfo.%s -> empty (assets are a plain dir)\n", n);
    return j_NewObjectArray(fake_env, 0, (void *)0, (void *)0);
  }
  if (!strcmp(n, "sourceDir") || !strcmp(n, "publicSourceDir") ||
      !strcmp(n, "dataDir")   || !strcmp(n, "deviceProtectedDataDir")) {
    debugPrintf("[data] ApplicationInfo.%s -> %s\n", n, managed_path(data_dir()));
    return jni_make_string(managed_path(data_dir()));
  }
  if (!strcmp(n, "nativeLibraryDir"))
    return jni_make_string(managed_path(data_dir()));
  // UnityPlayer statics: currentActivity is THE Activity -- null here NPEs every
  // UnityPlayer.currentActivity.getXxx() in managed code, so hand back a live
  // (opaque) Activity that our method dispatch then services.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
  }
  // AudioManager.PROPERTY_OUTPUT_* are static String field keys the engine reads
  // just before AudioManager.getProperty(key) to size FMOD's audio path. Return
  // the real Android property-name strings AND record which one was read
  // (g_last_output_prop) so getProperty() can answer even when the key argument
  // is lost on the JNI call path (see getproperty_value). These are AudioManager
  // fields, NOT UnityPlayer -- gating them on the wrong class meant they never
  // matched, getProperty saw "", and FMOD got framesPerBuffer 0 -> error 60.
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "what") && name_has(c, "Message")) return (juint)g_msg_what;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return screen_width;   /* real panel (landscape) */
    if (!strcmp(n, "heightPixels"))return screen_height;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  if (name_has(c, "Configuration") && !strcmp(n, "orientation")) return 2;  /* LANDSCAPE */
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return screen_width;   /* real panel (landscape) */
    if (!strcmp(n, "heightPixels")) return screen_height;
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  (void)fld_is;
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  log_field_read((const FakeID *)fid, 'O');
  { const FakeID *f = (const FakeID *)fid;   /* name the field the game really reads */
    if (editbox_text() && editbox_text()[0]) {
      static unsigned seen;
      if (seen < 16) { seen++;
        debugPrintf("[kbd] objfield read: %s.%s sig=%s\n",
                    f->cls[0] ? f->cls : "?", f->name[0] ? f->name : "?",
                    f->sig[0] ? f->sig : "?"); } } }
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  log_field_read((const FakeID *)fid, 'I');
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }

/* GetDoubleField / GetStaticDoubleField. These were falling through to the
 * UNIMPL stub, which is not a harmless no-op for a floating-point return: the
 * stub returns an integer in x0, while the caller reads its double out of d0.
 * That hands back whatever happened to be in d0 -- unrelated garbage, not the
 * 0.0 the stub appears to promise. An explicit 0.0 return is ABI-correct.
 * No fake object carries a double field today, so there is nothing to look up. */
static double j_GetDoubleField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; (void)fid; return 0.0; }

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy / JNIBridge.newInterfaceProxy converts the reflected
// Method/Field objects of an interface into jmethod/jfieldIDs via these. Slot 7
// (FromReflectedMethod) and slot 8 (FromReflectedField) were unimplemented, so
// the proxy couldn't bind its methods (the "UNIMPL slot 7" lines). We don't
// carry real reflection, but returning a non-null opaque ID lets the proxy set
// up and be stored; if such a proxy callback is ever actually invoked it routes
// through act_* and no-ops, which is the right behaviour for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env;
  if (m && *(uint32_t *)m == TAG_ID) return m;   /* proxy_run passes the real run() id */
  return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env; (void)f; return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

/* JNI registers org/fmod/FMODAudioDevice's native bridge (fmodGetInfo /
 * fmodProcess / fmodProcessMicData) here -- these are file-local in libunity, so
 * RegisterNatives is the only place their addresses are exposed. Capture them so
 * a native playback thread can pull PCM from FMOD (the Java run() loop never runs
 * because we have no JVM). */
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
void *g_fmod_getinfo = 0, *g_fmod_process = 0, *g_fmod_micdata = 0;
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  int is_fmod = name_has(cn, "fmod") || name_has(cn, "FMOD");
  debugPrintf("[jni] RegisterNatives %s (%d methods)%s\n", cn, n, is_fmod ? "  <-- fmod" : "");
  if (m && name_has(cn, "unity3d/player/UnityPlayer")) {
    for (int i = 0; i < n; i++) {
      if (!m[i].name) continue;
      if (!strcmp(m[i].name, "nativeSetInputString"))    g_u_setInputString  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetInputSelection")) g_u_setInputSel = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputClosed"))   g_u_softClosed  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputCanceled")) g_u_softCancel  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetKeyboardIsVisible")) g_u_kbdVisible = m[i].fn;
    }
    debugPrintf("[kbd] captured Unity soft-input natives: str=%p sel=%p closed=%p cancel=%p vis=%p\n",
                g_u_setInputString, g_u_setInputSel, g_u_softClosed,
                g_u_softCancel, g_u_kbdVisible);
  }
  if (m) {   /* dump the whole table once per class: the keyboard callback is in here */
    static unsigned dumped;
    for (int i = 0; i < n && dumped < 200; i++, dumped++)
      debugPrintf("[natives] %s.%s %s -> %p\n", cn,
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
  }
  if ((name_has(cn, "jnibridge") || name_has(cn, "JNIBridge")) && m) {
    for (int i = 0; i < n; i++) if (m[i].name && name_has(m[i].name, "invoke")) g_jnibridge_invoke = (jnibridge_invoke_fn)m[i].fn;
    debugPrintf("[jni] captured JNIBridge invoke=%p\n", (void *)g_jnibridge_invoke);
  }
  if (is_fmod && m) {
    for (int i = 0; i < n; i++) {
      debugPrintf("[jni]   %s %s -> %p\n",
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
      if (!m[i].name) continue;
      if      (!strcmp(m[i].name, "fmodGetInfo"))        g_fmod_getinfo = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcess"))        g_fmod_process = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcessMicData")) g_fmod_micdata = m[i].fn;
    }
    debugPrintf("[fmod] captured getInfo=%p process=%p micData=%p\n",
                g_fmod_getinfo, g_fmod_process, g_fmod_micdata);
  }
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// FMOD native-audio pump
// ---------------------------------------------------------------------------
// fmodProcess(env, this, ByteBuffer) renders one fixed-size FMOD mixer block
// (size comes from the output singleton set up at start(), NOT from the buffer
// capacity) straight into env->GetDirectBufferAddress(ByteBuffer), then returns
// 0. We have no JVM, so the Java FMODAudioDevice.run() loop never calls it --
// this native thread does instead, and pushes the PCM to the SDL sink.
//
// The only JNIEnv entry fmodProcess uses is GetDirectBufferAddress (slot 230);
// fmodGetInfo(which) uses none. So the shim only has to hand back our staging
// buffer and feed the captured function pointers a non-NULL `this`/buffer token.

#define FMOD_STAGING_BYTES (64 * 1024)   // generous: must exceed one mixer block
static unsigned char g_fmod_staging[FMOD_STAGING_BYTES];
static int  g_fmod_bb_token   = 0;       // stand-in jobject for the ByteBuffer
static int  g_fmod_this_token = 0;       // stand-in jobject for `this`
static int  g_fmod_started    = 0;

typedef int (*fmod_getinfo_fn)(void *env, void *thiz, int which);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *bytebuffer);

// slot 230: every ByteBuffer we ever pass is our own staging buffer.
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; (void)buf; return g_fmod_staging;
}
// slot 231 (defensive -- the disasm shows fmodProcess never calls it).
static long j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; (void)buf; return (long)FMOD_STAGING_BYTES;
}

// Discover how many bytes fmodProcess actually wrote, once, by sentinel-fill.
// Silence (0x0000) still differs from the 0xCD fill, so a silent first block is
// detected correctly.
static int probe_block_bytes(fmod_process_fn process, int frame_bytes) {
  memset(g_fmod_staging, 0xCD, FMOD_STAGING_BYTES);
  process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
  int last = -1;
  for (int i = FMOD_STAGING_BYTES - 1; i >= 0; i--) {
    if (g_fmod_staging[i] != 0xCD) { last = i; break; }
  }
  if (last < 0) return 0;
  int bytes = last + 1;
  if (frame_bytes > 0)                    // round up to a whole frame
    bytes = ((bytes + frame_bytes - 1) / frame_bytes) * frame_bytes;
  if (bytes > FMOD_STAGING_BYTES) bytes = FMOD_STAGING_BYTES;
  return bytes;
}

static int16_t block_peak(int bytes) {
  const int16_t *s = (const int16_t *)g_fmod_staging;
  int n = bytes / 2; int16_t peak = 0;
  for (int i = 0; i < n; i++) {
    int16_t v = s[i] < 0 ? (int16_t)-s[i] : s[i];
    if (v > peak) peak = v;
  }
  return peak;
}

static void *fmod_audio_thread(void *arg) {
  (void)arg;
  fmod_getinfo_fn getinfo = (fmod_getinfo_fn)g_fmod_getinfo;
  fmod_process_fn process = (fmod_process_fn)g_fmod_process;
  if (!process) { debugPrintf("[fmod] pump: no process ptr, abort\n"); return NULL; }

  int rate = 48000, channels = 2;
  if (getinfo) {
    int r = getinfo(fake_env, &g_fmod_this_token, 0);
    int c = getinfo(fake_env, &g_fmod_this_token, 1);
    debugPrintf("[fmod] getInfo: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d\n",
                r, c, getinfo(fake_env, &g_fmod_this_token, 2),
                getinfo(fake_env, &g_fmod_this_token, 3),
                getinfo(fake_env, &g_fmod_this_token, 4));
    if (r >= 8000 && r <= 192000) rate = r;
    if (c == 1 || c == 2 || c == 6) channels = c;
  }
  const int frame_bytes = channels * 2; // S16

  // CRITICAL: start() fires before Unity's render loop has driven a single
  // System::update(), so the FMOD mixer's DSP buffers aren't allocated yet --
  // calling fmodProcess now faults (null deref deep in the mix/copy path). Wait
  // for the engine to tick a batch of frames (each drives a System::update that
  // finalizes the mixer) before the first call. A faulting call can't be caught
  // (no working SEH here), so this warmup is the only protection.
  extern uint32_t port_frame_count(void);
  #define FMOD_WARMUP_FRAMES 120u
  uint32_t f0 = port_frame_count();
  debugPrintf("[fmod] warmup: waiting %u frames (start frame=%u)\n", FMOD_WARMUP_FRAMES, f0);
  for (int guard = 0; guard < 1500; guard++) {            // ~15s hard cap
    if (port_frame_count() - f0 >= FMOD_WARMUP_FRAMES) break;
    svcSleepThread(10000000ULL);                          // 10 ms
  }
  debugPrintf("[fmod] warmup done at frame=%u, probing\n", port_frame_count());

  // start() may still be wiring the FMOD output singleton; fmodProcess writes
  // nothing until it's live. Retry the probe briefly before giving up.
  int block = 0;
  for (int tries = 0; tries < 100 && block <= 0; tries++) {
    block = probe_block_bytes(process, frame_bytes);
    if (block <= 0) svcSleepThread(10000000ULL); // 10 ms
  }
  debugPrintf("[fmod] pump start: %d Hz, %d ch, block=%d bytes (%d frames)\n",
              rate, channels, block, block / (frame_bytes ? frame_bytes : 1));
  if (block <= 0) {
    debugPrintf("[fmod] pump: fmodProcess wrote nothing after retries, abort\n");
    return NULL;
  }

  int dev_rate = audio_fmod_open(rate, channels);
  if (!dev_rate) { debugPrintf("[fmod] pump: device open failed, abort\n"); return NULL; }

  // pace to realtime via the device queue; target ~4 blocks buffered.
  const uint32_t hi = (uint32_t)block * 6;
  const uint32_t lo = (uint32_t)block * 3;
  long iters = 0;
  for (;;) {
    while (audio_fmod_queued() > hi)
      svcSleepThread(2000000ULL); // 2 ms
    // refill toward the low watermark
    do {
      process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
      uint32_t q = audio_fmod_write(g_fmod_staging, block);
      if (iters < 4) {
        debugPrintf("[fmod] block %ld: peak=%d queued=%u\n",
                    iters, (int)block_peak(block), q);
      }
      iters++;
      if (q > hi) break;
    } while (audio_fmod_queued() < lo);
    svcSleepThread(2000000ULL); // 2 ms
  }
  return NULL;
}

// Called from dispatch_void when FMODAudioDevice.start() fires (pointers are
// already captured by then -- RegisterNatives precedes start()).
void fmod_audio_start(void) {
  if (g_fmod_started) return;
  if (!g_fmod_process) { debugPrintf("[fmod] start(): process ptr not captured yet\n"); return; }
  g_fmod_started = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, fmod_audio_thread, NULL) != 0) {
    debugPrintf("[fmod] pthread_create failed\n");
    g_fmod_started = 0;
    return;
  }
  pthread_detach(th);
  debugPrintf("[fmod] native playback thread launched\n");
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* ZOOKEEPER DX port: accessors so unity_jni.c/unity_input.c can read into the
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  jni_fill_unimpl(env_table); // indexed stubs: log the exact unimplemented slot

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[64]  = (void *)j_CallNonvirtualObjectMethod;    // was UNIMPL
  env_table[65]  = (void *)j_CallNonvirtualObjectMethodV;   // was UNIMPL slot 65 (PAD path)
  env_table[66]  = (void *)j_CallNonvirtualObjectMethodA;   // was UNIMPL
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[97]  = (void *)j_GetIntField;            // GetByteField   (returns in w0)
  env_table[98]  = (void *)j_GetIntField;            // GetCharField   (returns in w0)
  env_table[99]  = (void *)j_GetIntField;            // GetShortField  (returns in w0)
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[103] = (void *)j_GetDoubleField;         // GetDoubleField (returns in d0)
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[147] = (void *)j_GetIntField;            // GetStaticByteField
  env_table[148] = (void *)j_GetIntField;            // GetStaticCharField
  env_table[149] = (void *)j_GetIntField;            // GetStaticShortField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[153] = (void *)j_GetDoubleField;         // GetStaticDoubleField (d0)
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;      /* was UNIMPL: managed string marshalling */
  env_table[166] = (void *)j_ReleaseStringChars;  /* was UNIMPL: pairs with 165 */
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  /* GetStringCritical/ReleaseStringCritical are the same UTF-16 contract as
   * 165/166 -- same signatures, and a copy is a legal implementation. Wiring
   * them to the same code closes the identical failure mode on the alternate
   * path some marshalling routines take. */
  env_table[224] = (void *)j_GetStringChars;          // GetStringCritical
  env_table[225] = (void *)j_ReleaseStringChars;      // ReleaseStringCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[230] = (void *)j_GetDirectBufferAddress;  // fmodProcess drains via this
  env_table[231] = (void *)j_GetDirectBufferCapacity; // defensive (unused by fmodProcess)

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
