/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32_shim.c -- a tiny dllexport wrapper so a Python host (ctypes) can drive the
 * real AArch32 reference interpreter (a32_interp.c) without a second copy of it.
 *
 * This adds NO behaviour: it fills a struct a32_interp_case from flat arguments
 * and calls a32_interp_run() exactly as the differential harness does. It exists
 * only because MSVC/clang DLLs do not export plain C symbols without a hint.
 *
 * Build (Windows host, no ARM toolchain needed -- this is the *host* side):
 *   clang -std=c11 -O2 -shared -o a32_interp.dll a32_shim.c a32_interp.c
 */
#include "a32_interp.h"

#include <string.h>

#ifdef _WIN32
#define A32_EXPORT __declspec(dllexport)
#else
#define A32_EXPORT
#endif

/* Run `code_len` bytes of `code` as one interpreter case.
 *
 * in_regs (16 u32, may be NULL -> all zero) seeds r0..r11 (r9/r15 are forced by
 * the interpreter). out_regs (16 u32, may be NULL) receives the final register
 * file. *out_stop receives struct a32_result.stop_reason. err/errn receive the
 * named fault string (which carries the offending instruction word).
 *
 * Returns 0 iff the fragment ran to its end (A32_STOP_RETURN), nonzero
 * otherwise -- the same contract a32_interp_run() has. */
A32_EXPORT int a32_shim_run(const uint8_t *code, uint32_t code_len, int thumb,
                            const uint32_t *in_regs, uint32_t id,
                            uint32_t *out_regs, uint32_t *out_cpsr,
                            uint32_t *out_stop, char *err, size_t errn)
{
    struct a32_interp_case c;
    struct a32_result r;
    int rc, i;

    memset(&c, 0, sizeof(c));
    c.id = id;
    c.code = code;
    c.code_len = code_len;
    c.thumb = thumb ? 1 : 0;
    if (in_regs)
        for (i = 0; i < A32_NREG; i++)
            c.regs[i] = in_regs[i];

    memset(&r, 0, sizeof(r));
    rc = a32_interp_run(&c, &r, err, errn);

    if (out_regs)
        for (i = 0; i < A32_NREG; i++)
            out_regs[i] = r.regs[i];
    if (out_cpsr) *out_cpsr = r.cpsr;
    if (out_stop) *out_stop = r.stop_reason;
    return rc;
}

/* The same run, but also returning the VFP register file and the scratch
 * memory hash, so a host can diff the two fields a VFP load/store case is the
 * whole point of (fp[] and mem_hash) without a second copy of the interpreter.
 * out_fp receives 32 u32 (the low word of each s0..s31; the high word is always
 * zero for a single-precision register file). */
A32_EXPORT int a32_shim_run_full(const uint8_t *code, uint32_t code_len, int thumb,
                                 const uint32_t *in_regs, uint32_t id,
                                 uint32_t *out_regs, uint32_t *out_cpsr,
                                 uint32_t *out_fp, uint32_t *out_mem_hash,
                                 uint32_t *out_stop, char *err, size_t errn)
{
    struct a32_interp_case c;
    struct a32_result r;
    int rc, i;

    memset(&c, 0, sizeof(c));
    c.id = id;
    c.code = code;
    c.code_len = code_len;
    c.thumb = thumb ? 1 : 0;
    if (in_regs)
        for (i = 0; i < A32_NREG; i++)
            c.regs[i] = in_regs[i];

    memset(&r, 0, sizeof(r));
    rc = a32_interp_run(&c, &r, err, errn);

    if (out_regs)
        for (i = 0; i < A32_NREG; i++)
            out_regs[i] = r.regs[i];
    if (out_cpsr) *out_cpsr = r.cpsr;
    if (out_fp)
        for (i = 0; i < A32_NFP; i++)
            out_fp[i] = (uint32_t)r.fp[i];
    if (out_mem_hash) *out_mem_hash = r.mem_hash;
    if (out_stop) *out_stop = r.stop_reason;
    return rc;
}
