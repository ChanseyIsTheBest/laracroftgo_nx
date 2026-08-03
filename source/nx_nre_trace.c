/* nx_nre_trace.c -- take a managed stack trace at the point a
 * NullReferenceException is raised.
 *
 * This build ships no il2cpp.usym, so Unity prints the exception with no stack
 * (debug.log: three blank lines after "NullReferenceException: Object reference
 * not set to an instance of an object."). This module hooks the raiser itself
 * and logs the return addresses, which tools/il2cpp_symbols.py turns back into
 * C# method names.
 *
 * See nx_patch_lcgo.h (LCGO_NRE_*) for the derivation of the target, why a
 * splice is safe there, and -- importantly -- why the managed caller's address
 * is at [sp] rather than in x30.
 *
 * Reading the output:
 *
 *   [nre] #1 NullReferenceException raised
 *   [nre]   frame 0  il2cpp+0x16bcd14
 *   [nre]   frame 1  il2cpp+0x16c1f80
 *   ...
 *   tools/il2cpp_symbols.py sym dump.cs 0x16bcd14 0x16c1f80
 *
 * Frame 0 is exact -- it is the return address the codegen stub pushed, so it
 * points into the method that dereferenced null. Frames 1+ come from walking
 * the x29 frame-pointer chain and are BEST EFFORT: il2cpp does not guarantee a
 * frame pointer in every compiled method, so a short or truncated chain is
 * normal and a missing frame does not mean the walk is broken. Trust frame 0.
 */

#include <stdint.h>
#include <stdio.h>

#include "nx_patch_lcgo.h"
#include "so_util.h"
#include "util.h"

#if LCGO_HAVE_NRE_TRACE

extern so_module il2cpp_mod;

/* Written once at install time; the trampoline's final `br` reads it. Absolute,
 * so the jump back needs no relocation. */
void     *g_nre_resume;
uintptr_t g_nre_il2cpp_lo, g_nre_il2cpp_hi;

static unsigned g_nre_count;
static int      g_nre_in_report;   /* guard: never recurse into ourselves */

/* Is `p` a readable-looking address inside the loaded libil2cpp image? Frames
 * outside it are engine or loader code and are not worth printing -- and more
 * to the point, refusing to dereference anything outside a known-mapped range
 * is what keeps a diagnostic from becoming a crash. */
static int in_il2cpp(uintptr_t p) {
  return p >= g_nre_il2cpp_lo && p < g_nre_il2cpp_hi;
}

/* A frame pointer is only followed if it is 16-byte aligned AND lies inside a
 * bounded window above the stack pointer we were entered with. Alignment alone
 * is NOT enough: il2cpp does not guarantee a frame pointer in every compiled
 * method, so x29 can hold an ordinary aligned value that is not a frame at all,
 * and dereferencing it would fault -- turning a diagnostic into a crash at
 * exactly the moment the game is already in trouble. Anchoring to the live
 * stack keeps every dereference inside memory we know is mapped. */
#define NRE_STACK_WINDOW (256u * 1024u)
static int followable_fp(uintptr_t fp, uintptr_t sp) {
  return fp && (fp & 0xf) == 0 && fp >= sp && (fp - sp) < NRE_STACK_WINDOW;
}

/* Called from the trampoline. `managed_lr` is [sp] at the raiser's entry -- the
 * return address the codegen stub pushed, i.e. inside the method that threw.
 * `fp` is x29 as the managed method left it, and `sp` is the raiser's entry
 * stack pointer, which anchors the frame walk to mapped memory. */
__attribute__((used)) void nx_nre_report(uintptr_t managed_lr, uintptr_t fp,
                                         uintptr_t sp) {
  if (g_nre_in_report) return;
  if (g_nre_count >= LCGO_NRE_TRACE_MAX) return;
  g_nre_in_report = 1;
  g_nre_count++;

  const uintptr_t b = g_nre_il2cpp_lo;
  debugPrintf("[nre] #%u NullReferenceException raised\n", g_nre_count);

  if (in_il2cpp(managed_lr))
    debugPrintf("[nre]   frame 0  il2cpp+0x%lx\n", (unsigned long)(managed_lr - b));
  else
    debugPrintf("[nre]   frame 0  %p (outside libil2cpp)\n", (void *)managed_lr);

  /* Best-effort walk of the AAPCS frame chain: [x29] = caller's x29,
   * [x29+8] = caller's return address. Bounded, alignment-checked, and
   * required to ascend, so a broken chain stops the walk instead of faulting
   * or spinning. */
  unsigned n = 1;
  uintptr_t cur = fp, prev = 0;
  while (n < LCGO_NRE_MAX_FRAMES && followable_fp(cur, sp) && cur > prev) {
    const uintptr_t next = *(const uintptr_t *)cur;
    const uintptr_t ret  = *(const uintptr_t *)(cur + 8);
    if (in_il2cpp(ret)) {
      debugPrintf("[nre]   frame %u  il2cpp+0x%lx\n", n, (unsigned long)(ret - b));
      n++;
    }
    prev = cur;
    cur  = next;
  }
  if (n == 1)
    debugPrintf("[nre]   (no further frames -- il2cpp omitted the frame pointer; "
                "frame 0 is still exact)\n");
  debugPrintf("[nre]   symbolicate: tools/il2cpp_symbols.py sym dump.cs <addrs>\n");
  if (g_nre_count == LCGO_NRE_TRACE_MAX)
    debugPrintf("[nre] trace cap reached (%u); further NREs not logged\n",
                (unsigned)LCGO_NRE_TRACE_MAX);

  g_nre_in_report = 0;
}

/* The detour.
 *
 * Entered with the stack exactly as the raiser found it, so [sp] still holds
 * the managed caller's return address. Saves every caller-saved register plus
 * x30, reports, restores, re-executes the four instructions the patch replaced,
 * and jumps to the instruction after them.
 *
 * The four re-executed instructions are byte-for-byte the originals (see
 * LCGO_NRE_WORD_0..3). Order matters: `mov x0, sp` must observe the stack after
 * `sub sp, sp, #0x20`, and `stp xzr, x30` must store the ORIGINAL x30 -- which
 * is why x30 is restored from the save area before that point rather than being
 * left as whatever `bl nx_nre_report` set it to. */
__asm__(
".text\n.align 2\n.global nx_nre_trampoline\n"
".type nx_nre_trampoline,%function\n"
"nx_nre_trampoline:\n"
"  stp x0,  x1,  [sp, #-0xa0]!\n"
"  stp x2,  x3,  [sp, #0x10]\n"
"  stp x4,  x5,  [sp, #0x20]\n"
"  stp x6,  x7,  [sp, #0x30]\n"
"  stp x8,  x9,  [sp, #0x40]\n"
"  stp x10, x11, [sp, #0x50]\n"
"  stp x12, x13, [sp, #0x60]\n"
"  stp x14, x15, [sp, #0x70]\n"
"  stp x16, x17, [sp, #0x80]\n"
"  stp x18, x30, [sp, #0x90]\n"
"  add x2, sp, #0xa0\n"            /* the raiser's original sp             */
"  ldr x0, [x2]\n"                 /* [sp] = managed caller's return addr  */
"  mov x1, x29\n"                  /* managed frame pointer, for the walk  */
"  bl  nx_nre_report\n"
"  ldp x18, x30, [sp, #0x90]\n"
"  ldp x16, x17, [sp, #0x80]\n"
"  ldp x14, x15, [sp, #0x70]\n"
"  ldp x12, x13, [sp, #0x60]\n"
"  ldp x10, x11, [sp, #0x50]\n"
"  ldp x8,  x9,  [sp, #0x40]\n"
"  ldp x6,  x7,  [sp, #0x30]\n"
"  ldp x4,  x5,  [sp, #0x20]\n"
"  ldp x2,  x3,  [sp, #0x10]\n"
"  ldp x0,  x1,  [sp], #0xa0\n"
/* --- the four replaced instructions, verbatim --- */
"  sub sp, sp, #0x20\n"
"  stp xzr, x30, [sp, #8]\n"
"  mov x0, sp\n"
"  str xzr, [sp]\n"
/* --- back into the raiser, one instruction past the patch --- */
"  adrp x16, g_nre_resume\n"
"  add  x16, x16, :lo12:g_nre_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n");

extern void nx_nre_trampoline(void);

void nx_install_nre_trace(void) {
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  volatile uint32_t *p = (volatile uint32_t *)(b + LCGO_NRE_RAISER);

  /* Verify all four words before touching anything. A wrong offset here would
   * splice a branch into the middle of an unrelated function. */
  if (p[0] != LCGO_NRE_WORD_0 || p[1] != LCGO_NRE_WORD_1 ||
      p[2] != LCGO_NRE_WORD_2 || p[3] != LCGO_NRE_WORD_3) {
    debugPrintf("[nre] SKIP trace: raiser @il2cpp+0x%x = %08x %08x %08x %08x, "
                "expected %08x %08x %08x %08x -- offset wrong for this libil2cpp\n",
                (unsigned)LCGO_NRE_RAISER, p[0], p[1], p[2], p[3],
                (unsigned)LCGO_NRE_WORD_0, (unsigned)LCGO_NRE_WORD_1,
                (unsigned)LCGO_NRE_WORD_2, (unsigned)LCGO_NRE_WORD_3);
    return;
  }

  g_nre_resume    = (void *)(b + LCGO_NRE_RAISER + LCGO_NRE_PATCH_LEN);
  g_nre_il2cpp_lo = b;
  g_nre_il2cpp_hi = b + il2cpp_mod.load_size;

  /* ldr x16, #8 ; br x16 ; <64-bit absolute target>. The literal sits inside
   * the 16-byte window, which is why the patch needs all four instructions and
   * why the trampoline has to re-execute them rather than fewer. */
  const uint32_t stub[4] = {
    0x58000050u,                                                  /* ldr x16, #8 */
    0xd61f0200u,                                                  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_nre_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_nre_trampoline >> 32),
  };
  so_patch_code((void *)p, stub, sizeof stub);
  debugPrintf("[nre] NRE trace armed @il2cpp+0x%x -> trampoline %p, resume %p\n",
              (unsigned)LCGO_NRE_RAISER, (void *)&nx_nre_trampoline, g_nre_resume);
}

#else
void nx_install_nre_trace(void) { }
#endif
