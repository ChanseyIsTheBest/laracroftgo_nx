/* nx_exception_dump.c -- user exception handler: on any fault, dump symbolized
 * PC/LR + all GPRs + the faulting thread's stack frame, straight into debug.log.
 * Then break so Atmosphere's creport still fires. Every read is
 * svcQueryMemory-guarded.
 *
 * Symbolization is dynamic (so_find_module_by_addr), so this is not tied to any
 * build: a fault anywhere in libunity/libil2cpp prints as "libX.so+0xOFFSET",
 * which is directly comparable against nx_patch_lcgo.h and a disassembler.
 *
 * The extra [sp] slots dumped below were chosen by the reference port while
 * chasing a specific Unity UI job crash. They are harmless to dump on any
 * fault -- treat them as generic stack context rather than as meaningful field
 * names for this title.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "so_util.h"

/* libnx user exception handling: providing these symbols + the handler enables it */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static int xd_readable(uintptr_t addr, size_t len) {
  if (!addr || addr < 0x1000) return 0;
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == MemType_Unmapped) return 0;
    if ((mi.perm & Perm_R) == 0) return 0;
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

/* "libunity.so+0x1234" style annotation; falls back to raw hex */
static const char *xd_sym(u64 v, char *buf, size_t n) {
  so_module *m = so_find_module_by_addr((const void *)v);
  if (m)
    snprintf(buf, n, "%s+0x%lx", m->name, (unsigned long)(v - (uintptr_t)m->load_virtbase));
  else
    snprintf(buf, n, "%016lx", (unsigned long)v);
  return buf;
}

static void xd_dump_range(const char *tag, uintptr_t base, size_t bytes) {
  if (!xd_readable(base, bytes)) {
    debugPrintf("[xd] %s @%p: UNREADABLE\n", tag, (void *)base);
    return;
  }
  char s[64];
  for (size_t off = 0; off < bytes; off += 0x20) {
    const u64 *q = (const u64 *)(base + off);
    debugPrintf("[xd] %s+%03zx: %016lx %016lx %016lx %016lx\n",
                tag, off, (unsigned long)q[0], (unsigned long)q[1],
                (unsigned long)q[2], (unsigned long)q[3]);
    (void)s;
  }
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  char b1[96], b2[96], b3[96];
  debugPrintf("[xd] ================= USER EXCEPTION =================\n");
  debugPrintf("[xd] desc=0x%x pc=%s far=%016lx esr=%08x\n",
              ctx->error_desc, xd_sym(ctx->pc.x, b1, sizeof b1),
              (unsigned long)ctx->far.x, ctx->esr);
  debugPrintf("[xd] lr=%s sp=%016lx fp=%016lx\n",
              xd_sym(ctx->lr.x, b2, sizeof b2),
              (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
  for (int i = 0; i < 28; i += 4)
    debugPrintf("[xd] x%-2d %016lx  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx\n",
                i, (unsigned long)ctx->cpu_gprs[i].x,
                i + 1, (unsigned long)ctx->cpu_gprs[i + 1].x,
                i + 2, (unsigned long)ctx->cpu_gprs[i + 2].x,
                i + 3, (unsigned long)ctx->cpu_gprs[i + 3].x);
  debugPrintf("[xd] x28 %016lx\n", (unsigned long)ctx->cpu_gprs[28].x);

  /* frame-pointer backtrace (same walk as the watchdog) */
  uintptr_t fp = (uintptr_t)ctx->fp.x;
  for (int d = 0; d < 12 && fp; d++) {
    if (!xd_readable(fp, 16)) break;
    uintptr_t nfp = ((uintptr_t *)fp)[0];
    uintptr_t rlr = ((uintptr_t *)fp)[1];
    if (!rlr) break;
    debugPrintf("[xd]   bt[%d] %s\n", d, xd_sym(rlr, b3, sizeof b3));
    if (nfp <= fp) break;
    fp = nfp;
  }

  /* the whole stack frame: every [sp+slot] the crash block reads */
  uintptr_t sp = (uintptr_t)ctx->sp.x;
  xd_dump_range("SP", sp, 0x200);

  /* UIGeometryJob specifics (harmless if this is a different crash):
   * [sp+0x48]=element base, [sp+0x50]=element index (stride 0x70),
   * [sp+0x28]=job data, [sp+0x10]=output base. */
  if (xd_readable(sp + 0x58, 8)) {
    uintptr_t elem_base = ((uintptr_t *)(sp + 0x48))[0];
    uintptr_t elem_idx  = ((uintptr_t *)(sp + 0x50))[0];
    uintptr_t jobdata   = ((uintptr_t *)(sp + 0x28))[0];
    uintptr_t outbase   = ((uintptr_t *)(sp + 0x10))[0];
    debugPrintf("[xd] elem_base=%016lx idx=%lu jobdata=%016lx outbase=%016lx\n",
                (unsigned long)elem_base, (unsigned long)elem_idx,
                (unsigned long)jobdata, (unsigned long)outbase);
    if (elem_idx < 0x10000 && elem_base)
      xd_dump_range("ELEM", elem_base + elem_idx * 0x70, 0x70);
    if (jobdata) xd_dump_range("JOB", jobdata, 0x60);
  }
  debugPrintf("[xd] ============== END EXCEPTION DUMP ==============\n");

  /* re-raise so the process still aborts and Atmosphere writes its report */
  svcBreak(BreakReason_Panic, 0, 0);
  for (;;) svcSleepThread(1000000000ULL);
}
