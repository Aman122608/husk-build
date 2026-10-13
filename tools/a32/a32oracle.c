/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32oracle.c -- the AArch32 ground-truth producer.
 *
 * Built as an armhf (AArch32) ELF and run under qemu-arm on ubuntu-latest: it
 * executes the hand-written cases compiled in from a32_cases_gen.S and writes
 * the architectural state each one left behind. That state is the golden. The
 * oracle is a test fixture: never linked into the app, never shipped, and its
 * only job is to be right. Its AArch32 emulation is upstream QEMU's, not our own
 * patched fork, so a translator bug cannot be masked by a matching fork bug.
 *
 * It must never be the only judge of "is this right": QEMU decides undefined
 * behaviour, and differential testing inherits the oracle's bugs.
 *
 * Usage:  a32oracle [outdir]          (default outdir ".")
 * Writes: <outdir>/a32_cases.json     the case manifest (id/name/group/source)
 *         <outdir>/a32_goldens.json   per-case architectural state
 *
 * Build (ubuntu):  arm-linux-gnueabi-gcc -static -marm -march=armv7-a \
 *                    -mfpu=vfpv3 -mfloat-abi=soft -D_GNU_SOURCE \
 *                    -I tools/a32 -o a32oracle a32oracle.c a32_cases_gen.S
 * Run:             qemu-arm -cpu cortex-a15 ./a32oracle <outdir>
 *
 * The golden shape is documented at the top of tools/a32/a32_state.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "a32_state.h"
#include "a32_cases_gen.h"

/* Provided by the generated assembly. */
extern void     a32_fp_init(void);
extern void     a32_fp_capture(uint64_t *fp);
extern uint32_t a32_fp_fpscr(void);

/* FNV-1a over the whole scratch window: a STR write anywhere in the window moves
 * the hash, so a store bug shows up in one number per case, not in a byte dump. */
static uint32_t mem_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Word k of the scratch window starts as 0x10000000 + k, so a load reads a value
 * that is neither zero nor an address, and a store is visible in mem_hash. */
static void mem_seed(uint32_t *w, size_t bytes)
{
    for (size_t k = 0; k < bytes / 4; k++)
        w[k] = 0x10000000u + (uint32_t)k;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char cpath[1024], gpath[1024];
    snprintf(cpath, sizeof(cpath), "%s/a32_cases.json", dir);
    snprintf(gpath, sizeof(gpath), "%s/a32_goldens.json", dir);

    FILE *cf = fopen(cpath, "w");
    FILE *gf = fopen(gpath, "w");
    if (!cf || !gf) {
        perror(dir);
        return 2;
    }

    size_t ms = A32_MEM_SIZE;
    uint8_t *scratch = mmap(NULL, ms, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED) {
        perror("mmap scratch");
        return 2;
    }

    /* ---- the case manifest ------------------------------------------------- */
    fprintf(cf, "{\n \"format\": \"a32-cases/1\",\n \"count\": %zu,\n \"cases\": [\n",
            (size_t)A32_CASE_COUNT);
    for (size_t i = 0; i < A32_CASE_COUNT; i++) {
        const struct a32_case_meta *m = &a32_cases[i];
        fprintf(cf, "  {\"id\":%d,\"name\":\"%s\",\"group\":\"%s\",\"mode\":\"%s\","
                    "\"source\":\"%s\",\"desc\":\"%s\"}%s\n",
                m->id, m->name, m->group, m->mode, m->source, m->desc,
                i + 1 < A32_CASE_COUNT ? "," : "");
    }
    fprintf(cf, " ]\n}\n");
    fclose(cf);

    /* ---- the goldens ------------------------------------------------------- */
    fprintf(gf, "{\n \"format\": \"a32-goldens/1\",\n \"oracle\": \"qemu-arm\",\n"
                " \"arch\": \"armv7-a\",\n \"count\": %zu,\n \"cases\": [\n",
            (size_t)A32_CASE_COUNT);
    for (size_t i = 0; i < A32_CASE_COUNT; i++) {
        const struct a32_case_meta *m = &a32_cases[i];
        static struct a32_result out;

        memset(&out, 0, sizeof(out));
        out.id = (uint32_t)m->id;
        out.mem_size = (uint32_t)ms;

        mem_seed((uint32_t *)scratch, ms);
        a32_fp_init();
        m->fn(&out, scratch);
        a32_fp_capture(out.fp);
        out.fpscr = a32_fp_fpscr();
        out.mem_hash = mem_hash(scratch, ms);
        out.mem_first_diff = 0;
        out.stop_reason = A32_STOP_RETURN;

        fprintf(gf, "  {\"id\":%u,\"name\":\"%s\",\"regs\":[", out.id, m->name);
        for (int r = 0; r < A32_NREG; r++)
            fprintf(gf, "%s\"0x%08x\"", r ? "," : "", out.regs[r]);
        fprintf(gf, "],\"cpsr\":\"0x%08x\",\"nzcvq\":\"0x%08x\","
                    "\"fpscr\":\"0x%08x\",\"fpscr_masked\":\"0x%08x\",\"fp\":[",
                out.cpsr, out.cpsr & A32_CPSR_MASK,
                out.fpscr, out.fpscr & A32_FPSCR_MASK);
        for (int r = 0; r < A32_NFP; r++)
            fprintf(gf, "%s\"0x%016llx\"", r ? "," : "",
                    (unsigned long long)out.fp[r]);
        fprintf(gf, "],\"mem_size\":%u,\"mem_hash\":\"0x%08x\","
                    "\"mem_first_diff\":\"0x%08x\",\"stop_reason\":%u}%s\n",
                out.mem_size, out.mem_hash, out.mem_first_diff, out.stop_reason,
                i + 1 < A32_CASE_COUNT ? "," : "");
    }
    fprintf(gf, " ]\n}\n");
    fclose(gf);

    printf("a32oracle: %zu cases -> %s\n", (size_t)A32_CASE_COUNT, gpath);
    return 0;
}
