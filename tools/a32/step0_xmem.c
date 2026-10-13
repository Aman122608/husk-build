/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * step0_xmem.c -- STEP 0, the go/no-go for the whole ARM32 harness.
 *
 * The one fact that decides whether an AArch32->AArch64 translator can be
 * developed in CI at all: can a macos-26 runner execute code the process just
 * wrote? Nothing here is new -- it drives the layer's OWN executable-memory code
 * (src/translation-layer-next/husk-tl-xmem.c), whose self-test writes a function
 * through the writable view and calls it through the executable one, and fails
 * loudly if the host forbids that.
 *
 * Two ways of getting there, both meaningful:
 *   TL_XMEM_SELF=1 ./step0      -> husk-tl-xmem.c's grant_self path, which runs
 *                                  the layer's built-in self-test (movz/ret,
 *                                  0x5a5a). This is the loud existing signal.
 *   ./step0                     -> the ordinary Mac two-view path (mmap RW +
 *                                  mach_vm_remap alias + mprotect RX); this
 *                                  program then writes its own movz/ret through
 *                                  the RW view and calls it through the RX view.
 * Either failing is a hard, obvious CI failure, not a silent wrong answer.
 *
 * On top of that, checks on the property the phone actually depends on and
 * that a fetch through the RX view can never show -- the phone died calling a
 * WRITABLE-view address the layer had just said it made executable:
 *   (a) the writable/guest-facing view is executed on purpose. tl_xmem_allow_exec's
 *       verdict for it must match what really happens: a detector that says a page is
 *       executable when a guarded fetch of it faults is exactly the bug that killed the
 *       phone (SIGBUS with pc == fault address), so a mismatch is a hard failure.
 *   (b) FIX A: whether the guest-facing view carries the JIT authorisation, which the
 *       layer reports (tl_xmem_guest_view_is_jit). If it does, the guest's own arena at
 *       that address MUST execute after a write through it, and tl_xmem_allow_exec MUST
 *       succeed on it -- that is the whole change. If the host will not grant the mapping
 *       (a Mac with neither MAP_JIT nor RWX), the layer falls back, and the same address
 *       MUST NOT execute; either is asserted, so this cannot pass vacuously.
 *   (c) the NEGATIVE CONTROL, in a child forced onto the fallback order
 *       (TL_XMEM_NO_JIT_VIEW=1): the guest-facing view is then a vm_remap alias, exactly
 *       the shape that failed on the phone. It MUST come back non-executable. A harness
 *       that cannot fail the way the phone failed proves nothing.
 *   (d) with TL_NO_XMEM_RW_EXEC=1 the detector must refuse, and writing through the
 *       writable view and running through the executable one must still work. The
 *       workflow runs this program with no such variable, so the program runs (d)
 *       itself in a child of its own with the variable set -- CI sees it without a
 *       third invocation to keep in sync.
 *
 * Build: clang -arch arm64 -g -O1 -Wall -I src/translation-layer-next \
 *          -I src/translation-layer tools/a32/step0_xmem.c \
 *          src/translation-layer-next/husk-tl-xmem.c -o /tmp/step0_xmem
 * Run:   TL_XMEM_SELF=1 /tmp/step0_xmem      # then also: /tmp/step0_xmem
 *
 * Exit: 0 the runner executes freshly written code, the guest-facing view's verdict is
 *       honest and matches the fix, and the negative control fails the way the phone did.
 *       1 any of those is false.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "husk-tl-xmem.h"

/* husk-tl-xmem.c calls this; normally the loader provides it. */
void tl_log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* movz w0, #0x5a5a ; ret -- the same bytes husk-tl-xmem.c self-tests with */
static const uint32_t k_code[2] = { 0x528B4B40u, 0xD65F03C0u };

/*
 * Write the movz/ret through the WRITABLE view at `at` and call it THERE, under a sigsetjmp guard
 * so a page that is not really executable faults into the guard instead of killing the CI step.
 * The bytes at `at` are the guest's, so they are put back whether the call returns or faults:
 * nothing here unmaps, ends a thread, or leaves a stub where the guest's code was.
 * Returns true only if the call ran and returned 0x5a5a.
 */
static sigjmp_buf g_jump;
static volatile sig_atomic_t g_in_guard;
static void step0_fault(int sig)
{
    if (g_in_guard) siglongjmp(g_jump, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}
static bool guarded_call(uint8_t *at)
{
    uint32_t saved[2];
    memcpy(saved, at, sizeof(saved));
    struct sigaction sa, ob, os, oi;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = step0_fault; sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &ob); sigaction(SIGSEGV, &sa, &os); sigaction(SIGILL, &sa, &oi);
    bool ran = false;
    g_in_guard = 1;
    if (sigsetjmp(g_jump, 1) == 0) {
        tl_xmem_emit_begin();                       /* a MAP_JIT view is writable only in write mode */
        memcpy(at, k_code, sizeof(k_code));
        tl_xmem_emit_end();                         /* and executable only back in execute mode */
        tl_xmem_flush(at, sizeof(k_code));
        ran = ((int (*)(void))(void *)at)() == 0x5a5a;
    }
    g_in_guard = 0;
    sigaction(SIGBUS, &ob, NULL); sigaction(SIGSEGV, &os, NULL); sigaction(SIGILL, &oi, NULL);
    tl_xmem_emit_begin();
    memcpy(at, saved, sizeof(saved));
    tl_xmem_emit_end();
    tl_xmem_flush(at, sizeof(saved));
    return ran;
}

/*
 * Check (c): the negative control, in a child forced onto the fallback order
 * (TL_XMEM_NO_JIT_VIEW=1). The guest-facing view is then a vm_remap of the code view -- exactly
 * the shape that failed on the phone -- and it MUST come back non-executable: a harness that
 * cannot fail the way the phone failed proves nothing about the detector or the fix.
 */
static int negative_control_checks(void)
{
    char err[256] = "";
    if (!tl_xmem_open(64u << 20, err, sizeof(err))) {
        fprintf(stderr, "STEP0 FAIL (negative control child): tl_xmem_open: %s\n", err);
        return 1;
    }
    if (tl_xmem_guest_view_is_jit()) {
        fprintf(stderr,
                "STEP0 FAIL (negative control child): TL_XMEM_NO_JIT_VIEW=1 but the guest-facing view "
                "is still reported as the authorised one; the control is not testing the fallback.\n");
        return 1;
    }
    uint8_t *rx = NULL, *rw = NULL;
    if (!tl_xmem_alloc(4096, &rx, &rw)) {
        fprintf(stderr, "STEP0 FAIL (negative control child): tl_xmem_alloc failed\n");
        return 1;
    }
    uint8_t *rw_page = (uint8_t *)((uintptr_t)rw & ~(uintptr_t)(TL_XMEM_PAGE - 1));
    bool verdict = tl_xmem_allow_exec(rw_page, 4096);
    bool ran = guarded_call(rw_page);
    if (verdict != ran) {
        fprintf(stderr, "STEP0 FAIL (negative control child): the detector lied -- verdict %s, guarded fetch %s\n",
                verdict ? "executable" : "non-executable", ran ? "ran" : "faulted");
        return 1;
    }
    if (verdict || ran) {
        fprintf(stderr,
                "STEP0 FAIL (negative control child): the fallback guest-facing view (a vm_remap alias) "
                "came back executable. That is the shape that failed on the phone; if this host cannot "
                "fail the way the phone failed, the harness proves nothing.\n");
        return 1;
    }
    printf("STEP0 OK (negative control child): in the fallback two-view order the guest-facing view "
           "is non-executable and the detector refuses a writable-view arena\n");
    return 0;
}

/* Check (d): the TL_NO_XMEM_RW_EXEC=1 refusal path, run in a child so CI exercises it too. */
static int refusal_checks(void)
{
    char err[256] = "";
    if (!tl_xmem_open(64u << 20, err, sizeof(err))) {
        fprintf(stderr, "STEP0 FAIL (refusal child): tl_xmem_open: %s\n", err);
        return 1;
    }
    uint8_t *rx = NULL, *rw = NULL;
    if (!tl_xmem_alloc(4096, &rx, &rw)) {
        fprintf(stderr, "STEP0 FAIL (refusal child): tl_xmem_alloc failed\n");
        return 1;
    }
    uint8_t *rw_page = (uint8_t *)((uintptr_t)rw & ~(uintptr_t)(TL_XMEM_PAGE - 1));
    /* Writing through the writable view and running through the executable one must survive the refusal. */
    memcpy(rw, k_code, sizeof(k_code));
    tl_xmem_flush(rx, sizeof(k_code));
    if (((int (*)(void))(void *)rx)() != 0x5a5a) {
        fprintf(stderr, "STEP0 FAIL (refusal child): the executable view stopped working under TL_NO_XMEM_RW_EXEC=1\n");
        return 1;
    }
    if (tl_xmem_allow_exec(rw_page, 4096)) {
        fprintf(stderr, "STEP0 FAIL (refusal child): TL_NO_XMEM_RW_EXEC=1 but tl_xmem_allow_exec said the writable view is executable\n");
        return 1;
    }
    if (guarded_call(rw_page)) {
        fprintf(stderr, "STEP0 FAIL (refusal child): the writable view executed a page the detector refused\n");
        return 1;
    }
    printf("STEP0 OK (refusal child): TL_NO_XMEM_RW_EXEC=1 refused execute, rx still runs, rw still does not\n");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    if (getenv("TL_STEP0_REFUSE_CHILD")) return refusal_checks();
    if (getenv("TL_STEP0_NEGCTL_CHILD")) return negative_control_checks();

    const char *self = getenv("TL_XMEM_SELF");
    char err[256] = "";

    /* Open the writable+executable region. With TL_XMEM_SELF=1 this is the
     * layer's grant_self path, which itself executes freshly written code
     * (its own self-test) before returning; the loud message above is the verdict. */
    if (!tl_xmem_open(64u << 20, err, sizeof(err))) {
        fprintf(stderr,
                "STEP0 FAIL: this runner cannot make executable memory for "
                "itself; an AArch32 translator cannot be developed in CI here.\n"
                "  husk-tl-xmem says: %s\n", err);
        return 1;
    }
    printf("STEP0: tl_xmem_open ok (%s path), region %zu MiB, guest-facing view %s\n",
           self ? "grant_self/self-test" : "Mac two-view", tl_xmem_size() >> 20,
           tl_xmem_guest_view_is_jit() ? "CARRIES the JIT authorisation (Fix A)"
                                       : "is NOT the authorised mapping (fallback)");

    /* Take one page and write a function through the RW view, then execute it
     * through the RX view. This is exactly the shape the translator will use. */
    uint8_t *rx = NULL, *rw = NULL;
    if (!tl_xmem_alloc(4096, &rx, &rw)) {
        fprintf(stderr, "STEP0 FAIL: tl_xmem_alloc failed\n");
        return 1;
    }
    memcpy(rw, k_code, sizeof(k_code));
    tl_xmem_flush(rx, sizeof(k_code));

    int rc = ((int (*)(void))(void *)rx)();
    if (rc != 0x5a5a) {
        fprintf(stderr,
                "STEP0 FAIL: executed a function this process just wrote and it "
                "returned 0x%x, expected 0x5a5a\n", rc);
        return 1;
    }
    printf("STEP0 OK: this process wrote a function and executed it through the "
           "executable view; it returned 0x%x\n", rc);

    /* The two views are one set of pages: a value stored through the guest-facing view is the
     * same value read through the code view. This is what lets the loader write image bytes
     * through one view and the guest fetch them through the other. */
    uint64_t probe = 0x0123456789abcdefull, back = 0;
    memcpy(rw, &probe, sizeof(probe));
    memcpy(&back, rx, sizeof(back));
    if (back != probe) {
        fprintf(stderr, "STEP0 FAIL: the two views are not one set of pages (wrote %llx through the "
                "guest view, read %llx through the code view)\n",
                (unsigned long long)probe, (unsigned long long)back);
        return 1;
    }

    /* (a) The guest-facing view, executed on purpose; and (b) the fix: whether that view carries
     *     the JIT authorisation. Ask the layer, make the page executable the way a guest does,
     *     then really fetch it. Verdict and fetch must agree, and the fix must show in one branch
     *     or the other -- never pass vacuously. */
    uint8_t *rw_page = (uint8_t *)((uintptr_t)rw & ~(uintptr_t)(TL_XMEM_PAGE - 1));
    bool guest_jit = tl_xmem_guest_view_is_jit();
    bool verdict = tl_xmem_allow_exec(rw_page, 4096);
    bool wr_ran = guarded_call(rw_page);
    if (verdict != wr_ran) {
        fprintf(stderr,
                "STEP0 FAIL: the execute detector lied -- tl_xmem_allow_exec on the guest-facing view "
                "said %s, but a guarded fetch of it %s. This is the phone's bug: the layer reported "
                "'made executable' and the fetch faulted.\n",
                verdict ? "EXECUTABLE" : "non-executable", wr_ran ? "ran" : "faulted");
        return 1;
    }
    if (guest_jit && !wr_ran) {
        fprintf(stderr,
                "STEP0 FAIL: FIX A -- the guest-facing view is the authorised mapping "
                "(tl_xmem_guest_view_is_jit) yet a guarded fetch of a page in it faulted. The guest's "
                "own arena at the address it already holds must execute.\n");
        return 1;
    }
    if (!guest_jit && wr_ran) {
        fprintf(stderr,
                "STEP0 FAIL: the guest-facing view is NOT the authorised mapping yet it executed; the "
                "layer's own account of which view carries the JIT authorisation is wrong.\n");
        return 1;
    }
    printf("STEP0 OK: guest-facing view -> authorisation %s, detector says %s, guarded fetch %s (%s path)%s\n",
           guest_jit ? "yes" : "no", verdict ? "yes" : "no", wr_ran ? "ran" : "faulted",
           self ? "grant_self" : "Mac two-view",
           guest_jit ? "; the guest's own arena is genuinely executable"
                     : "; fallback: a writable-view arena is refused, as the phone needed");

    /* (c) The negative control, in a child forced onto the fallback order, so it is deterministic
     *     whatever this host granted; then (d) the refusal path, in another child. Both run here so
     *     the workflow needs no extra invocation to keep in sync. */
    pid_t pid = fork();
    if (pid == 0) {
        setenv("TL_XMEM_NO_JIT_VIEW", "1", 1);
        setenv("TL_STEP0_NEGCTL_CHILD", "1", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "STEP0 FAIL: fork for the forced-fallback negative-control child failed\n");
        return 1;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "STEP0 FAIL: the forced-fallback negative-control child failed\n");
        return 1;
    }

    pid = fork();
    if (pid == 0) {
        setenv("TL_NO_XMEM_RW_EXEC", "1", 1);
        setenv("TL_STEP0_REFUSE_CHILD", "1", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "STEP0 FAIL: fork for the TL_NO_XMEM_RW_EXEC=1 refusal-path child failed\n");
        return 1;
    }
    st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "STEP0 FAIL: the TL_NO_XMEM_RW_EXEC=1 refusal-path child failed\n");
        return 1;
    }

    printf("STEP0 OK (all): rx executes, the two views alias, the guest-facing view's verdict is honest "
           "and matches %s, the negative control fails the way the phone did, and the refusal path refuses.\n",
           guest_jit ? "the fix (its own arena executes)" : "the fallback (a writable-view arena is refused)");
    return 0;
}
