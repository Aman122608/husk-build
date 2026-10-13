/* Local probe (NOT part of CI): run generated cases through the reference
 * interpreter.  `_interp_probe dump <dir>` writes a a32_cases.json/a32_goldens.json
 * pair from the INTERPRETER ITSELF, so a32diff can be exercised end-to-end on a
 * plain host with no qemu (the placeholders are then edited to oracle-like
 * addresses for the placement-dependent cases by _compare_e2e.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a32_state.h"
#include "a32_interp.h"
#include "a32_cases_gen.h"

static void run_case(const struct a32_case_words *w, struct a32_result *res,
                     char *err, size_t errn)
{
    struct a32_interp_case ic;
    memset(&ic, 0, sizeof(ic));
    ic.id = (uint32_t)w->id;
    for (int r = 0; r < A32_NREG; r++) ic.regs[r] = w->init[r];
    ic.code = w->code;
    ic.code_len = w->code_len;
    ic.thumb = (strcmp(w->mode, "thumb") == 0);
    memset(res, 0, sizeof(*res));
    res->id = (uint32_t)w->id;
    a32_interp_run(&ic, res, err, errn);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "dump")) {
        char cp[1024], gp[1024];
        snprintf(cp, sizeof(cp), "%s/a32_cases.json", argv[2]);
        snprintf(gp, sizeof(gp), "%s/a32_goldens.json", argv[2]);
        FILE *cf = fopen(cp, "w"), *gf = fopen(gp, "w");
        if (!cf || !gf) { perror(argv[2]); return 2; }
        fprintf(cf, "{\n \"format\": \"a32-cases/1\",\n \"count\": %zu,\n \"cases\": [\n",
                (size_t)A32_WORDS_COUNT);
        for (size_t i = 0; i < A32_WORDS_COUNT; i++) {
            const struct a32_case_words *w = &a32_case_words[i];
            fprintf(cf, "  {\"id\":%d,\"name\":\"%s\",\"group\":\"%s\",\"mode\":\"%s\","
                        "\"source\":\"handwritten\",\"desc\":\"%s\"}%s\n",
                    w->id, w->name, w->group, w->mode, w->desc,
                    i + 1 < A32_WORDS_COUNT ? "," : "");
        }
        fprintf(cf, " ]\n}\n");
        fprintf(gf, "{\n \"format\": \"a32-goldens/1\",\n \"oracle\": \"qemu-arm\",\n"
                    " \"arch\": \"armv7-a\",\n \"count\": %zu,\n \"cases\": [\n",
                (size_t)A32_WORDS_COUNT);
        for (size_t i = 0; i < A32_WORDS_COUNT; i++) {
            const struct a32_case_words *w = &a32_case_words[i];
            struct a32_result r;
            char err[256]; err[0] = 0;
            run_case(w, &r, err, sizeof(err));
            fprintf(gf, "  {\"id\":%u,\"name\":\"%s\",\"regs\":[", r.id, w->name);
            for (int k = 0; k < A32_NREG; k++)
                fprintf(gf, "%s\"0x%08x\"", k ? "," : "", r.regs[k]);
            fprintf(gf, "],\"cpsr\":\"0x%08x\",\"nzcvq\":\"0x%08x\",\"fpscr\":\"0x%08x\","
                        "\"fpscr_masked\":\"0x%08x\",\"fp\":[",
                    r.cpsr, r.cpsr & A32_CPSR_MASK, r.fpscr, r.fpscr & A32_FPSCR_MASK);
            for (int k = 0; k < A32_NFP; k++)
                fprintf(gf, "%s\"0x%016llx\"", k ? "," : "",
                        (unsigned long long)r.fp[k]);
            fprintf(gf, "],\"mem_size\":%u,\"mem_hash\":\"0x%08x\","
                        "\"mem_first_diff\":\"0x%08x\",\"stop_reason\":%u}%s\n",
                    r.mem_size, r.mem_hash, r.mem_first_diff, r.stop_reason,
                    i + 1 < A32_WORDS_COUNT ? "," : "");
        }
        fprintf(gf, " ]\n}\n");
        fclose(cf); fclose(gf);
        printf("dumped %zu cases to %s\n", (size_t)A32_WORDS_COUNT, argv[2]);
        return 0;
    }
    int want = argc > 1 ? atoi(argv[1]) : -1;
    int n = 0;
    for (size_t i = 0; i < A32_WORDS_COUNT; i++) {
        const struct a32_case_words *w = &a32_case_words[i];
        if (want >= 0 && w->id != want) continue;
        struct a32_result res; char err[256]; err[0] = 0;
        run_case(w, &res, err, sizeof(err));
        printf("case %3d %-16s r0=%08x r1=%08x r2=%08x r3=%08x r9=%08x "
               "nzcvq=%08x stop=%u %s\n",
               w->id, w->name, res.regs[0], res.regs[1], res.regs[2], res.regs[3],
               res.regs[9], res.cpsr & A32_CPSR_MASK, res.stop_reason, err);
        n++;
    }
    if (want >= 0 && n == 0) { fprintf(stderr, "no such case %d\n", want); return 2; }
    return 0;
}
