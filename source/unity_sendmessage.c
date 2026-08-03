/* unity_sendmessage.c -- Java -> Unity callback channel. See the header. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "so_util.h"
#include "unity_sendmessage.h"
#include "util.h"

extern so_module unity_mod;

/* UnitySendMessage(const char *gameObject, const char *method, const char *param)
 *
 * Resolved BY NAME, not by offset: libunity exports it (.dynsym, 0xcc bytes at
 * +0x6b6428 in this build). Using the export means this survives a libunity
 * update, so unlike the rest of nx_patch_lcgo.h there is no constant to
 * re-derive and nothing for verify_offsets.py to check.
 *
 * THREAD SAFETY -- derived, not assumed. The export does not dispatch into
 * scripting; it builds a message struct on its stack and appends it to a queue
 * under a lock. Disassembled:
 *
 *     UnitySendMessage:  ... bl <build msg> ; bl <get queue> ; bl 0x6b65dc
 *     0x6b65dc:          bl 0xb981c8   -> pthread_mutex_lock
 *                        <append entries to the vector at [queue+0x58]>
 *                        b  0xb981d0   -> pthread_mutex_unlock
 *
 * So this is a mutex-guarded deferred enqueue and is safe to call from any
 * thread -- which matters, because on Android these callbacks arrive on Java
 * threads, and our JNI stubs run on whatever thread the game called from. The
 * message is delivered on the main thread during the next player-loop
 * iteration, exactly as on Android.
 */
typedef void (*unity_send_message_fn)(const char *, const char *, const char *);
static unity_send_message_fn g_send;
static int g_send_ready;   /* export resolved */
static int g_send_open;    /* player loop has run a frame -- safe to enqueue */
static int g_selftest_done;

int nx_sendmsg_init(void) {
  if (g_send_ready) return 1;
  g_send = (unity_send_message_fn)so_try_find_addr_rx(&unity_mod, "UnitySendMessage");
  g_send_ready = g_send != NULL;
  if (g_send_ready)
    debugPrintf("[sendmsg] UnitySendMessage resolved @%p -- Java->Unity callback "
                "channel available\n", (void *)g_send);
  else
    debugPrintf("[sendmsg] UnitySendMessage NOT FOUND in libunity -- no plugin "
                "callback can reach managed code\n");
  return g_send_ready;
}

int nx_unity_send_message(const char *obj, const char *method, const char *param) {
  if (!g_send_ready || !g_send) {
    debugPrintf("[sendmsg] DROPPED %s.%s -- channel unavailable\n",
                obj ? obj : "?", method ? method : "?");
    return 0;
  }
  if (!g_send_open) {
    /* Before the first frame the queue's owning singleton may not be
     * constructed, and anything enqueued could be drained before the receiving
     * GameObject exists. Refuse rather than risk it; Android has the same
     * ordering, it just never has to think about it. */
    debugPrintf("[sendmsg] DEFERRED-DROP %s.%s -- called before frame 1\n",
                obj ? obj : "?", method ? method : "?");
    return 0;
  }
  if (!obj || !method) return 0;
  debugPrintf("[sendmsg] -> %s.%s(\"%s\")\n", obj, method, param ? param : "");
  g_send(obj, method, param ? param : "");
  return 1;
}

void nx_sendmsg_frame(int frame) {
  if (frame < 1) return;
  g_send_open = 1;
  if (g_selftest_done || !g_send_ready) return;
  if (frame < 2) return;   /* one clear frame of scripting first */
  g_selftest_done = 1;
  /* End-to-end proof that the channel works, with zero effect on the game.
   *
   * The target GameObject deliberately does not exist. Unity's queue drain logs
   * a "SendMessage: object ... not found" style error when it cannot resolve
   * the target, so the NEXT engine log line after this tells us the message was
   * enqueued, drained on the main thread, and processed. Silence means the
   * queue is not being drained at all -- which would itself be the finding, and
   * would mean no plugin callback could ever land even once we start sending
   * real ones.
   *
   * Sending to a name nothing can match is the point: it exercises the whole
   * path without touching game state. */
  debugPrintf("[sendmsg] self-test: sending to a deliberately absent object; "
              "expect an engine 'not found' log next if the queue drains\n");
  nx_unity_send_message("__nx_sendmsg_selftest", "Receive", "{}");
}

/* ---------------------------------------------------------------------------
 * "What would Android have sent?"
 *
 * Each entry is a plugin entry point the game calls, paired with the managed
 * callback Android's real implementation answers it with. Derived from the JNI
 * ids the game actually requested (debug.log) cross-referenced against dump.cs.
 *
 * `receiver` is the GameObject name UnitySendMessage would target. It is
 * recorded as the managed TYPE name, which is the usual convention for these
 * singletons but is NOT verified against the scene -- that is exactly why
 * nothing here is sent. Treat the receiver column as a lead, not a fact.
 * ------------------------------------------------------------------------- */
typedef struct {
  const char *cls_frag;   /* substring match, as the JNI layer names classes */
  const char *name_frag;
  const char *receiver;
  const char *method;
  const char *shape;
} CallbackPoint;

static const CallbackPoint k_points[] = {
  /* Runtime permissions. The game fetches the plugin during boot (debug.log:
   * "app upcall: ArmoryActivity.getPermissionPlugin()"), then requests
   * permissions on it. Android answers asynchronously -- the dialog result, or
   * the cached grant set, arrives as JSON. dump.cs has the receiver:
   *   AndroidPermissionMessageReceiver : GameSingleton<...>
   *     public void Receive(string json)
   *     private List<string> ParsePermissions(JSONNode)
   *     private List<bool>   ParseGrants(JSONNode)
   * so the payload carries parallel permission-name and grant-bool arrays. */
  { "ArmoryActivity", "getPermissionPlugin", "AndroidPermissionMessageReceiver",
    "Receive", "JSON: parallel arrays of permission names + granted bools" },

  /* Push token. requestGcmToken returns a String immediately on Android but the
   * real token is delivered later by the messaging service. */
  { "NotificationClient", "requestGcmToken", "<unknown>", "<token callback>",
    "registration token string, delivered after the call returns" },

  /* Deep links. getDeepLinkIntentData is answered null on purpose (see
   * jni_fake.c), which is correct for a cold launch with no link. Android also
   * pushes links that arrive WHILE running, which we can never generate. */
  { "DeepLinkClient", "getDeepLinkIntentData", "<unknown>", "<deep link callback>",
    "URL string, only when a link arrives while the app is running" },
};

static void note(const char *site, const char *receiver, const char *method,
                 const char *shape) {
  nx_sendmsg_would_have(site, receiver, method, shape);
}

void nx_sendmsg_would_have(const char *site, const char *receiver,
                           const char *method, const char *shape) {
  static struct { char site[96]; char method[64]; } seen[32];
  static unsigned n;
  if (!site || !method) return;
  for (unsigned i = 0; i < n; i++)
    if (!strcmp(seen[i].site, site) && !strcmp(seen[i].method, method)) return;
  if (n < sizeof seen / sizeof seen[0]) {
    snprintf(seen[n].site, sizeof seen[n].site, "%s", site);
    snprintf(seen[n].method, sizeof seen[n].method, "%s", method);
    n++;
  }
  debugPrintf("[sendmsg] WOULD-HAVE: %s -> Android calls %s.%s | payload: %s "
              "(not sent)\n",
              site, receiver ? receiver : "<unknown>", method,
              shape ? shape : "?");
}

int nx_sendmsg_note_upcall(const char *cls, const char *name, const char *sig) {
  (void)sig;
  if (!cls || !name) return 0;
  for (unsigned i = 0; i < sizeof k_points / sizeof k_points[0]; i++) {
    const CallbackPoint *p = &k_points[i];
    if (strstr(cls, p->cls_frag) && strstr(name, p->name_frag)) {
      char site[160];
      snprintf(site, sizeof site, "%s.%s", p->cls_frag, p->name_frag);
      note(site, p->receiver, p->method, p->shape);
      return 1;
    }
  }
  return 0;
}
