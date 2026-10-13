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

/* Run one REAL function in a region-mapped image (see a32_interp_run_image).
 * The region descriptors are flat arrays so a ctypes host can pass them without
 * a second struct definition: nregions entries of (vaddr[i], size[i], exec[i])
 * with host[i] a writable buffer of size[i] bytes. Returns 0 iff the function
 * returned (reached ret_vaddr); otherwise err names the reason. */
A32_EXPORT int a32_shim_run_image(
    const uint32_t *vaddr, const uint32_t *size, const uint32_t *exec,
    uint8_t **host, uint32_t nregions,
    uint32_t pc, int thumb,
    const uint32_t *in_regs, uint32_t sp, uint32_t lr, uint32_t ret_vaddr,
    uint32_t step_limit, uint32_t id,
    uint32_t *out_regs, uint32_t *out_cpsr, uint32_t *out_stop,
    char *err, size_t errn)
{
    struct a32_interp_image im;
    struct a32_result r;
    int rc, i;

    memset(&im, 0, sizeof(im));
    im.id = id;
    im.pc = pc;
    im.thumb = thumb ? 1 : 0;
    im.sp = sp;
    im.lr = lr;
    im.ret_vaddr = ret_vaddr;
    im.step_limit = step_limit;
    if (nregions > (uint32_t)A32_MAX_REGIONS)
        nregions = (uint32_t)A32_MAX_REGIONS;
    im.nregions = (int)nregions;
    for (i = 0; i < (int)nregions; i++) {
        im.regions[i].vaddr = vaddr[i];
        im.regions[i].size  = size[i];
        im.regions[i].exec  = exec[i];
        im.regions[i].host  = host[i];
    }
    if (in_regs)
        for (i = 0; i < A32_NREG; i++)
            im.regs[i] = in_regs[i];

    memset(&r, 0, sizeof(r));
    rc = a32_interp_run_image(&im, &r, err, errn);

    if (out_regs)
        for (i = 0; i < A32_NREG; i++)
            out_regs[i] = r.regs[i];
    if (out_cpsr) *out_cpsr = r.cpsr;
    if (out_stop) *out_stop = r.stop_reason;
    return rc;
}
