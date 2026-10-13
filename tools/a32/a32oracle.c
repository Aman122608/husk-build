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
 * ISOLATION: each case runs in its own forked child, which hands the finished
 * struct a32_result back over a pipe. A case that crashes (SIGSEGV/SIGILL) or
 * runs away is therefore DATA, not a dead job: the parent records it with
 * stop_reason = A32_STOP_FAULT / A32_STOP_LIMIT, names it and its signal on
 * stderr, keeps the goldens file complete and valid, and carries on to the next
 * case. (The golden job's own `assert rec["stop_reason"] == 0, rec["id"]` is
 * what turns any such record into a named, loud failure; the oracle does not
 * weaken it.) Because the parent survives, a crash can no longer cost the
 * stdout/log buffered up to that point.
 *
 * Build (ubuntu):  arm-linux-gnueabi-gcc -static -marm -march=armv7-a \
 *                    -mfpu=vfpv3 -mfloat-abi=soft -D_GNU_SOURCE \
 *                    -I tools/a32 -o a32oracle a32oracle.c a32_cases_gen.S
 * Run:             qemu-arm -cpu cortex-a15 ./a32oracle <outdir>
 *                  A32_TRACE=1 qemu-arm ... # also trace every case id
 *
 * The golden shape is documented at the top of tools/a32/a32_state.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

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

/* Run exactly one case in THIS process and publish the finished record to wfd.
 * Called only in the forked child, so a fault inside m->fn cannot take the
 * goldens loop with it. Returns 0 when the whole struct made it down the pipe. */
static int run_one(const struct a32_case_meta *m, uint8_t *scratch, size_t ms,
                   int wfd)
{
    struct a32_result r;

    memset(&r, 0, sizeof(r));
    r.id = (uint32_t)m->id;
    r.mem_size = (uint32_t)ms;

    mem_seed((uint32_t *)scratch, ms);
    a32_fp_init();
    m->fn(&r, scratch);
    a32_fp_capture(r.fp);
    r.fpscr = a32_fp_fpscr();
    r.mem_hash = mem_hash(scratch, ms);
    r.mem_first_diff = 0;
    r.stop_reason = A32_STOP_RETURN;

    const uint8_t *p = (const uint8_t *)&r;
    size_t done = 0;
    while (done < sizeof(r)) {
        ssize_t n = write(wfd, p + done, sizeof(r) - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    return done == sizeof(r) ? 0 : 3;
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

    /* The scratch window, with mapped, zeroed guard room on BOTH sides. A case's
     * r9 is the window BASE, and the corpus (as the a32_interp reference and its
     * arena agree) treats the addresses just below that base as mapped and zero:
     * STMDA/STMDB park words there (stm_da, stm_wb_da) and negative LDR/STRD
     * offsets read them (t32_ldr_str_neg, t32_ldrd_neg). Handing mmap() exactly
     * the window makes r9 sit on the very start of the mapping, so the first
     * decrement store faults -- which is not what the interpreter's arena does.
     * Only [r9, r9 + A32_MEM_SIZE) is seeded, hashed and compared. */
    size_t ms = A32_MEM_SIZE;
    size_t guard = 0x10000;
    uint8_t *map = mmap(NULL, ms + 2 * guard, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        perror("mmap scratch");
        return 2;
    }
    uint8_t *scratch = map + guard;

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
    const int trace = getenv("A32_TRACE") != NULL;
    size_t incomplete = 0;
    for (size_t i = 0; i < A32_CASE_COUNT; i++) {
        const struct a32_case_meta *m = &a32_cases[i];
        struct a32_result out;
        int pfd[2], status = 0, ok;

        /* A crashed case must still leave a WELL-FORMED record, so start from
         * the "it did not run" answer and let a successful child overwrite it. */
        memset(&out, 0, sizeof(out));
        out.id = (uint32_t)m->id;
        out.mem_size = (uint32_t)ms;
        out.stop_reason = A32_STOP_FAULT;

        if (trace) {
            fprintf(stderr, "a32oracle: case %d/%zu %s\n",
                    m->id, (size_t)A32_CASE_COUNT, m->name);
            fflush(stderr);
        }

        if (pipe(pfd) != 0) {
            perror("pipe");
            return 2;
        }
        fflush(NULL);   /* the child must not inherit a dirty stdio buffer */

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 2;
        }
        if (pid == 0) {
            /* ---- child: exactly one case, then the record down the pipe ---- */
            int rc;
            close(pfd[0]);
            alarm(30);              /* a runaway case is data too, not a hung job */
            rc = run_one(m, scratch, ms, pfd[1]);
            close(pfd[1]);
            _exit(rc);              /* _exit: never flush the parent's gf buffer */
        }

        close(pfd[1]);
        size_t got = 0;
        uint8_t *rb = (uint8_t *)&out;
        while (got < sizeof(out)) {
            ssize_t n = read(pfd[0], rb + got, sizeof(out) - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        close(pfd[0]);

        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return 2;
        }

        ok = got == sizeof(out) && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!ok) {
            int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            incomplete++;
            fprintf(stderr, "a32oracle: case %d (%s) DID NOT COMPLETE",
                    m->id, m->name);
            if (sig)
                fprintf(stderr, " -- killed by signal %d (%s)\n", sig, strsignal(sig));
            else
                fprintf(stderr, " -- child returned %zu/%zu bytes\n",
                        got, sizeof(out));
            memset(&out, 0, sizeof(out));
            out.id = (uint32_t)m->id;
            out.mem_size = (uint32_t)ms;
            out.stop_reason = (sig == SIGALRM) ? A32_STOP_LIMIT : A32_STOP_FAULT;
            fprintf(stderr, "a32oracle:   keeping the case with stop_reason=%u so the "
                            "golden job's stop_reason assertion names it\n",
                    out.stop_reason);
        }

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
        fflush(gf);
    }
    fprintf(gf, " ]\n}\n");
    fclose(gf);

    if (incomplete)
        fprintf(stderr, "a32oracle: %zu/%zu cases did not complete; each is named above and "
                        "carries a non-zero stop_reason in %s\n",
                incomplete, (size_t)A32_CASE_COUNT, gpath);
    printf("a32oracle: %zu cases -> %s\n", (size_t)A32_CASE_COUNT, gpath);
    return 0;
}
