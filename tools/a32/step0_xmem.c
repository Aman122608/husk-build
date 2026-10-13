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
 * Build: clang -arch arm64 -g -O1 -Wall -I src/translation-layer-next \
 *          -I src/translation-layer tools/a32/step0_xmem.c \
 *          src/translation-layer-next/husk-tl-xmem.c -o /tmp/step0_xmem
 * Run:   TL_XMEM_SELF=1 /tmp/step0_xmem      # then also: /tmp/step0_xmem
 *
 * Exit: 0 the runner executes freshly written code; 1 it does not.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
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
    /* movz w0, #0x5a5a ; ret -- the same bytes husk-tl-xmem.c self-tests with */
    static const uint32_t code[2] = { 0x528B4B40u, 0xD65F03C0u };
    memcpy(rw, code, sizeof(code));
    tl_xmem_flush(rx, sizeof(code));

    int rc = ((int (*)(void))(void *)rx)();
    if (rc != 0x5a5a) {
        fprintf(stderr,
                "STEP0 FAIL: executed a function this process just wrote and it "
                "returned 0x%x, expected 0x5a5a\n", rc);
        return 1;
    }

    printf("STEP0 OK: this process wrote a function and executed it; it "
           "returned 0x%x\n", rc);
    return 0;
}
