/* unity_sendmessage.h -- the Java -> Unity callback channel.
 *
 * Everything else in this port's JNI layer is REACTIVE: the game asks Java a
 * question and we answer it. On Android the Java side is also an ACTIVE
 * participant -- plugins call back into the game on their own schedule. There
 * are two channels for that, and until now we implemented only one:
 *
 *   1. AndroidJavaProxy interface proxies. Supported: jni_fake.c mints proxies
 *      and dispatches "[jni] proxy invoke ... .run / .doFrame".
 *   2. UnityPlayer.UnitySendMessage(gameObject, method, param). NOT supported.
 *      unity_entrypoints.h has carried OFF_nativeUnitySendMessage since the
 *      port began, but nothing ever resolved or called it, so no plugin
 *      callback could ever reach managed code.
 *
 * This module implements (2), and -- more importantly for now -- records every
 * callback the game WOULD have received on Android but did not.
 *
 * See PORTING.md §15.
 */

#ifndef UNITY_SENDMESSAGE_H
#define UNITY_SENDMESSAGE_H

/* Resolve UnitySendMessage out of libunity. Safe to call once the module is
 * loaded and finalized; does not need scripting to be up yet. Returns 1 if the
 * export was found. Logs either way. */
int nx_sendmsg_init(void);

/* Enqueue a message for delivery to managed code, exactly as a Java plugin
 * would. Thread-safe (see the note in unity_sendmessage.c). Returns 1 if it was
 * enqueued, 0 if the channel is unavailable or not yet safe to use.
 *
 * `param` may be NULL, which is sent as an empty string -- Unity's signature
 * takes a non-null char*. */
int nx_unity_send_message(const char *obj, const char *method, const char *param);

/* Called from the render loop once per frame, after nativeRender returns.
 * Opens the send window (see unity_sendmessage.c) and runs the one-shot
 * self-test that proves the channel works end to end. */
void nx_sendmsg_frame(int frame);

/* Record that Android would have delivered a callback here.
 *
 * `site` names the plugin entry point the game just called (e.g.
 * "ArmoryActivity.getPermissionPlugin"). `receiver`/`method` are the managed
 * target Android would have sent to, and `shape` describes the payload without
 * inventing one.
 *
 * This LOGS ONLY -- it never sends. Firing a fabricated payload into a boot
 * flow we do not yet understand can do more damage than the silence it
 * replaces; a JSON blob with the wrong schema throws inside the receiver's
 * parser rather than doing nothing. The point of this pass is to produce the
 * list, not to act on it. Deduplicated per (site, method). */
void nx_sendmsg_would_have(const char *site, const char *receiver,
                           const char *method, const char *shape);

/* Convenience: called from the JNI layer for any plugin method that is known to
 * be callback-driven on Android. Looks `cls`.`name` up in the table in
 * unity_sendmessage.c and calls nx_sendmsg_would_have() if it matches.
 * Returns 1 if a callback point was recognised. */
int nx_sendmsg_note_upcall(const char *cls, const char *name, const char *sig);

#endif /* UNITY_SENDMESSAGE_H */
