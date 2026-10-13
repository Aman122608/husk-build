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
 *                                  the layer's built-in self_selftest (movz/ret,
 *                                  0x5a5a). This is the loud existing signal.
 *   ./step0                     -> the ordinary Mac two-view path (mmap RW +
 *                                  mach_vm_remap alias + mprotect RX); this
 *                                  program then writes its own movz/ret through
 *                                  the RW view and calls it through the RX view.
 * Either failing is a hard, obvious CI failure, not a silent wrong answer.
 *
 * On top of that, three checks on the property the phone actually depends on and
 * that a fetch through the RX view can never show -- the phone died calling a
 * WRITABLE-view address the layer had just said it made executable:
 *   (a) the writable view is executed on purpose. tl_xmem_allow_exec's verdict for
 *       it must match what really happens: a detector that says a page is executable
 *       when a guarded fetch of it faults is exactly the bug that killed the phone
 *       (SIGBUS with pc == fault address), so a mismatch is a hard failure.
 *   (b) the NEGATIVE CONTROL: on the ordinary Mac two-view path the writable view
 *       must come back NON-executable, because that path is the shape that failed on
 *       the phone. A harness that cannot fail the way the phone failed proves
 *       nothing, so "reported executable here" is itself a failure.
 *   (c) with TL_NO_XMEM_RW_EXEC=1 the detector must refuse, and writing through the
 *       writable view and running through the executable one must still work. The
 *       workflow runs this program with no such variable, so the program runs (c)
 *       itself in a child of its own with the variable set -- CI sees it without a
 *       third invocation to keep in sync.
 *
 * Build: clang -arch arm64 -g -O1 -Wall -I src/translation-layer-next \
 *          -I src/translation-layer tools/a32/step0_xmem.c \
 *          src/translation-layer-next/husk-tl-xmem.c -o /tmp/step0_xmem
 * Run:   TL_XMEM_SELF=1 /tmp/step0_xmem      # then also: /tmp/step0_xmem
 *
 * Exit: 0 the runner executes freshly written code and the writable-view verdict is
 *       honest; 1 either is false.
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
        memcpy(at, k_code, sizeof(k_code));
        tl_xmem_flush(at, sizeof(k_code));
        ran = ((int (*)(void))(void *)at)() == 0x5a5a;
    }
    g_in_guard = 0;
    sigaction(SIGBUS, &ob, NULL); sigaction(SIGSEGV, &os, NULL); sigaction(SIGILL, &oi, NULL);
    memcpy(at, saved, sizeof(saved));
    tl_xmem_flush(at, sizeof(saved));
    return ran;
}

/* Check (c): the TL_NO_XMEM_RW_EXEC=1 refusal path, run in a child so CI exercises it too. */
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

    const char *self = getenv("TL_XMEM_SELF");
    char err[256] = "";

    /* Open the writable+executable region. With TL_XMEM_SELF=1 this is the
     * layer's grant_self path, which itself executes freshly written code
     * (self_selftest) before returning; the loud message above is the verdict. */
    if (!tl_xmem_open(64u << 20, err, sizeof(err))) {
        fprintf(stderr,
                "STEP0 FAIL: this runner cannot make executable memory for "
                "itself; an AArch32 translator cannot be developed in CI here.\n"
                "  husk-tl-xmem says: %s\n", err);
        return 1;
    }
    printf("STEP0: tl_xmem_open ok (%s path), region %zu MiB\n",
           self ? "grant_self/self-test" : "Mac two-view", tl_xmem_size() >> 20);

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

    /* (a) The writable view. Ask the layer to make it executable, then see what really
     *     happens: the detector's verdict and a guarded fetch of the same page must agree.
     *     (b) On the ordinary Mac two-view path the writable view must be non-executable --
     *     that is the phone's failing shape and the whole point of the negative control. */
    uint8_t *rw_page = (uint8_t *)((uintptr_t)rw & ~(uintptr_t)(TL_XMEM_PAGE - 1));
    bool verdict = tl_xmem_allow_exec(rw_page, 4096);
    bool wr_ran = guarded_call(rw_page);
    if (verdict != wr_ran) {
        fprintf(stderr,
                "STEP0 FAIL: the execute detector lied -- tl_xmem_allow_exec on the writable view "
                "said %s, but a guarded fetch of it %s. This is the phone's bug: the layer reported "
                "'made executable' and the fetch faulted.\n",
                verdict ? "EXECUTABLE" : "non-executable", wr_ran ? "ran" : "faulted");
        return 1;
    }
    if (!self && verdict) {
        fprintf(stderr,
                "STEP0 FAIL: NEGATIVE CONTROL -- the Mac two-view writable view was reported executable "
                "(and really executed). That is the shape that failed on the phone; if the Mac cannot "
                "fail the way the phone failed, this harness proves nothing about the detector.\n");
        return 1;
    }
    printf("STEP0 OK: writable view execute -> the detector says %s and a guarded fetch %s (%s path)%s\n",
           verdict ? "yes" : "no", wr_ran ? "ran" : "faulted",
           self ? "grant_self" : "Mac two-view",
           (!self && !verdict) ? "; negative control holds" : "");

    /* (c) The refusal path, in a child with TL_NO_XMEM_RW_EXEC=1 set, so the workflow need not
     *     add a third invocation for CI to see it. */
    pid_t pid = fork();
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
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "STEP0 FAIL: the TL_NO_XMEM_RW_EXEC=1 refusal-path child failed\n");
        return 1;
    }

    printf("STEP0 OK (all): rx executes, the writable-view verdict is honest, "
           "the negative control holds, and the refusal path refuses.\n");
    return 0;
}
