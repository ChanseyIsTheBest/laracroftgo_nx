/* nx_nre_trace.h -- managed stack trace at the point a NullReferenceException
 * is raised. See nx_nre_trace.c and nx_patch_lcgo.h (LCGO_NRE_*). */
#ifndef NX_NRE_TRACE_H
#define NX_NRE_TRACE_H

/* Splice the detour into libil2cpp's NRE raiser. Verify-first: a mismatched
 * prologue logs and does nothing. Must be called after libil2cpp is loaded and
 * finalized, and before the first managed code runs -- this title throws its
 * NRE during boot-scene init, around frame 0. */
void nx_install_nre_trace(void);

#endif /* NX_NRE_TRACE_H */
