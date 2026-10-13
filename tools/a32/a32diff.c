/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32diff.c -- the macos-26 side of the differential harness.
 *
 * In THIS increment the translator does not exist yet, so a32diff does not run
 * cases through one. What it does, and proves, is the whole rest of the pipeline:
 *
 *   (a) it loads and executes a *generated* AArch64 function on the runner --
 *       through the layer's own executable-memory code (husk-tl-xmem.c), the
 *       same two-view shape the translator will use. This is the compare job's
 *       own copy of the Step-0 fact;
 *   (b) it loads the case manifest and the goldens the ubuntu job produced,
 *       validates their structure, and runs its diff/report path on a
 *       trivially-true comparison (every golden against itself), so the format,
 *       the diff and the failure reporting are exercised end to end;
 *   (c) with --self-check it plants ONE mismatched field in a copy of a golden
 *       and requires the report to name exactly that case and that field. A
 *       deliberate red here is the point: it is how we know a real red will say
 *       which case and which field disagreed. It is OFF by default.
 *
 * Usage:  a32diff <goldens-dir> [--self-check]
 * Exit:   0 ok; 1 a mismatch was reported (the red a real translator run wants);
 *         2 usage/IO/parse error, or a self-check that did not behave.
 *
 * Build (macos-26):
 *   clang -arch arm64 -g -O1 -Wall -I src/translation-layer-next \
 *     -I src/translation-layer tools/a32/a32diff.c \
 *     src/translation-layer-next/husk-tl-xmem.c -o /tmp/a32diff
 *
 * What is compared, per case: id, the comparable GPRs (r0-r8, r10, r11), nzcvq
 * (cpsr masked to NZCV+Q), fpscr_masked (never raw fpscr), fp[0..31] (raw bits,
 * never floats), mem_size, mem_hash and stop_reason. regs[9] holds the scratch
 * address and regs[12..15] are forced 0, so they are not compared. The field
 * names in the report are the JSON key names, so a failure is actionable without
 * decoding anything.
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

/* ====================================================================== JSON
 * A small, purpose-built JSON reader. The golden files are well-formed and have
 * no string escapes (the generator emits none), so this reads them exactly and
 * nothing more. Values that can lose precision (any 32/64-bit state) are hex
 * strings in the file and are read back with strtoull, never as doubles.
 */
enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jv jv;
struct jv {
    int    t;
    double num;            /* J_NUM */
    char  *str;            /* J_STR */
    jv   **items; size_t n;                     /* J_ARR */
    char **keys; jv **vals; size_t nk;          /* J_OBJ */
};

static const char *P;

static void jskip(void) { while (*P == ' ' || *P == '\t' || *P == '\n' || *P == '\r') P++; }
static jv *jnew(int t) { jv *v = calloc(1, sizeof(*v)); v->t = t; return v; }

static char *jstr(void)
{
    P++;                                   /* opening quote */
    size_t cap = 16, n = 0;
    char *s = malloc(cap);
    while (*P && *P != '"') {
        if (n + 1 >= cap) { cap *= 2; s = realloc(s, cap); }
        s[n++] = *P++;
    }
    if (*P == '"') P++;
    s[n] = 0;
    return s;
}

static jv *jval(void)
{
    jskip();
    if (*P == '{') {
        P++;
        jv *v = jnew(J_OBJ);
        size_t cap = 8;
        v->keys = malloc(sizeof(char *) * cap);
        v->vals = malloc(sizeof(jv *) * cap);
        jskip();
        if (*P == '}') { P++; return v; }
        for (;;) {
            jskip();
            if (*P != '"') break;
            char *k = jstr();
            jskip();
            if (*P != ':') break;
            P++;
            jv *val = jval();
            if (v->nk >= cap) {
                cap *= 2;
                v->keys = realloc(v->keys, sizeof(char *) * cap);
                v->vals = realloc(v->vals, sizeof(jv *) * cap);
            }
            v->keys[v->nk] = k;
            v->vals[v->nk] = val;
            v->nk++;
            jskip();
            if (*P == ',') { P++; continue; }
            if (*P == '}') { P++; break; }
            break;
        }
        return v;
    }
    if (*P == '[') {
        P++;
        jv *v = jnew(J_ARR);
        size_t cap = 8;
        v->items = malloc(sizeof(jv *) * cap);
        jskip();
        if (*P == ']') { P++; return v; }
        for (;;) {
            jv *it = jval();
            if (v->n >= cap) { cap *= 2; v->items = realloc(v->items, sizeof(jv *) * cap); }
            v->items[v->n++] = it;
            jskip();
            if (*P == ',') { P++; continue; }
            if (*P == ']') { P++; break; }
            break;
        }
        return v;
    }
    if (*P == '"') { jv *v = jnew(J_STR); v->str = jstr(); return v; }
    if (!strncmp(P, "true", 4))  { P += 4; return jnew(J_BOOL); }
    if (!strncmp(P, "false", 5)) { P += 5; return jnew(J_BOOL); }
    if (!strncmp(P, "null", 4))  { P += 4; return jnew(J_NULL); }
    { char *e; double d = strtod(P, &e); P = e; jv *v = jnew(J_NUM); v->num = d; return v; }
}

static jv *jget(jv *o, const char *k)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->nk; i++)
        if (!strcmp(o->keys[i], k)) return o->vals[i];
    return NULL;
}
static uint64_t jhex(jv *v) { return (v && v->t == J_STR) ? strtoull(v->str, NULL, 0) : 0; }
static long     jnum(jv *v) { return v ? (long)v->num : 0; }

static jv *jclone(jv *v)
{
    if (!v) return NULL;
    jv *c = jnew(v->t);
    c->num = v->num;
    if (v->t == J_STR) c->str = v->str ? strdup(v->str) : NULL;
    if (v->t == J_ARR) {
        c->items = malloc(sizeof(jv *) * (v->n ? v->n : 1));
        c->n = v->n;
        for (size_t i = 0; i < v->n; i++) c->items[i] = jclone(v->items[i]);
    }
    if (v->t == J_OBJ) {
        c->keys = malloc(sizeof(char *) * (v->nk ? v->nk : 1));
        c->vals = malloc(sizeof(jv *) * (v->nk ? v->nk : 1));
        c->nk = v->nk;
        for (size_t i = 0; i < v->nk; i++) {
            c->keys[i] = strdup(v->keys[i]);
            c->vals[i] = jclone(v->vals[i]);
        }
    }
    return c;
}
static void jfreev(jv *v)
{
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->n; i++) jfreev(v->items[i]);
    for (size_t i = 0; i < v->nk; i++) { free(v->keys[i]); jfreev(v->vals[i]); }
    free(v->items); free(v->keys); free(v->vals); free(v);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return NULL; }
    buf[len] = 0;
    fclose(f);
    return buf;
}

/* ================================================================ the diff */

static char g_last_field[64];   /* the last field a mismatch named, for --self-check */

static int cmp_hex(jv *a, jv *b, const char *field, int id, const char *name)
{
    uint64_t x = jhex(jget(a, field)), y = jhex(jget(b, field));
    if (x == y) return 0;
    snprintf(g_last_field, sizeof(g_last_field), "%s", field);
    printf("  MISMATCH case %d (%s) field %s: golden=0x%llx translated=0x%llx\n",
           id, name, field, (unsigned long long)x, (unsigned long long)y);
    return 1;
}
static int cmp_num(jv *a, jv *b, const char *field, int id, const char *name)
{
    long x = jnum(jget(a, field)), y = jnum(jget(b, field));
    if (x == y) return 0;
    snprintf(g_last_field, sizeof(g_last_field), "%s", field);
    printf("  MISMATCH case %d (%s) field %s: golden=%ld translated=%ld\n",
           id, name, field, x, y);
    return 1;
}
static int cmp_arr(jv *a, jv *b, const char *field, int id, const char *name)
{
    jv *aa = jget(a, field), *bb = jget(b, field);
    int diffs = 0;
    size_t n = (aa && aa->t == J_ARR) ? aa->n : 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t x = jhex(aa->items[i]);
        uint64_t y = (bb && i < bb->n) ? jhex(bb->items[i]) : 0;
        if (x == y) continue;
        diffs++;
        snprintf(g_last_field, sizeof(g_last_field), "%s[%zu]", field, i);
        printf("  MISMATCH case %d (%s) field %s[%zu]: golden=0x%llx translated=0x%llx\n",
               id, name, field, i, (unsigned long long)x, (unsigned long long)y);
    }
    return diffs;
}

/* The comparable general-purpose registers. regs[9] holds the scratch window's
 * ADDRESS and regs[12..15] are forced 0 by the case wrapper: they are structural,
 * not architectural, and are deliberately not diffed. */
static const int a32_cmp_reg[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11 };
static int cmp_regs(jv *a, jv *b, int id, const char *name)
{
    jv *aa = jget(a, "regs"), *bb = jget(b, "regs");
    int diffs = 0;
    for (size_t k = 0; k < sizeof(a32_cmp_reg) / sizeof(a32_cmp_reg[0]); k++) {
        int r = a32_cmp_reg[k];
        uint64_t x = (aa && (size_t)r < aa->n) ? jhex(aa->items[r]) : 0;
        uint64_t y = (bb && (size_t)r < bb->n) ? jhex(bb->items[r]) : 0;
        if (x == y) continue;
        diffs++;
        snprintf(g_last_field, sizeof(g_last_field), "regs[%d]", r);
        printf("  MISMATCH case %d (%s) field regs[%d]: golden=0x%llx translated=0x%llx\n",
               id, name, r, (unsigned long long)x, (unsigned long long)y);
    }
    return diffs;
}

static int cmp_records(jv *a, jv *b, int id, const char *name)
{
    g_last_field[0] = 0;
    int d = 0;
    d += cmp_num(a, b, "id", id, name);
    d += cmp_regs(a, b, id, name);
    d += cmp_hex(a, b, "nzcvq", id, name);          /* cpsr masked to NZCV+Q */
    d += cmp_hex(a, b, "fpscr_masked", id, name);   /* fpscr masked, never raw */
    d += cmp_arr(a, b, "fp", id, name);
    d += cmp_num(a, b, "mem_size", id, name);
    d += cmp_hex(a, b, "mem_hash", id, name);
    d += cmp_num(a, b, "stop_reason", id, name);
    return d;
}

/* ============================================================== validation */

static int validate(const char *what, jv *doc, const char *format, const char *file)
{
    if (!doc || doc->t != J_OBJ) { fprintf(stderr, "compare: %s: %s is not a JSON object\n", what, file); return 1; }
    jv *fmt = jget(doc, "format");
    if (!fmt || fmt->t != J_STR || strcmp(fmt->str, format)) {
        fprintf(stderr, "compare: %s: %s has format '%s', expected '%s'\n",
                what, file, fmt && fmt->t == J_STR ? fmt->str : "(none)", format);
        return 1;
    }
    jv *cases = jget(doc, "cases");
    if (!cases || cases->t != J_ARR) {
        fprintf(stderr, "compare: %s: %s has no \"cases\" array\n", what, file);
        return 1;
    }
    long count = jnum(jget(doc, "count"));
    if (count != (long)cases->n) {
        fprintf(stderr, "compare: %s: %s says count=%ld but has %zu cases\n",
                what, file, count, cases->n);
        return 1;
    }
    return 0;
}

static int validate_golden_case(jv *c, const char *file, size_t i)
{
    int bad = 0;
    if (!jget(c, "id"))  { fprintf(stderr, "compare: %s: case %zu has no id\n", file, i); bad = 1; }
    if (!jget(c, "name")) { fprintf(stderr, "compare: %s: case %zu has no name\n", file, i); bad = 1; }
    jv *regs = jget(c, "regs");
    if (!regs || regs->t != J_ARR || regs->n != 16) {
        fprintf(stderr, "compare: %s: case %zu regs must be 16 entries (is %zu)\n",
                file, i, regs && regs->t == J_ARR ? regs->n : 0);
        bad = 1;
    }
    jv *fp = jget(c, "fp");
    if (!fp || fp->t != J_ARR || fp->n != 32) {
        fprintf(stderr, "compare: %s: case %zu fp must be 32 entries (is %zu)\n",
                file, i, fp && fp->t == J_ARR ? fp->n : 0);
        bad = 1;
    }
    static const char *need[] = { "nzcvq", "fpscr_masked", "mem_size",
                                  "mem_hash", "mem_first_diff", "stop_reason" };
    for (size_t k = 0; k < sizeof(need) / sizeof(need[0]); k++) {
        if (!jget(c, need[k])) {
            fprintf(stderr, "compare: %s: case %zu has no \"%s\"\n", file, i, need[k]);
            bad = 1;
        }
    }
    return bad;
}

/* ============================================================== (a) RX proof */

static int rx_proof(void)
{
    char err[256] = "";
    if (!tl_xmem_open(64u << 20, err, sizeof(err))) {
        fprintf(stderr, "compare: RX proof FAILED: %s\n", err);
        return 1;
    }
    uint8_t *rx = NULL, *rw = NULL;
    if (!tl_xmem_alloc(4096, &rx, &rw)) {
        fprintf(stderr, "compare: RX proof FAILED: tl_xmem_alloc\n");
        return 1;
    }
    static const uint32_t code[2] = { 0x528B4B40u, 0xD65F03C0u }; /* movz; ret */
    memcpy(rw, code, sizeof(code));
    tl_xmem_flush(rx, sizeof(code));
    int rc = ((int (*)(void))(void *)rx)();
    if (rc != 0x5a5a) {
        fprintf(stderr, "compare: RX proof FAILED: generated function returned 0x%x\n", rc);
        return 1;
    }
    printf("compare: RX proof OK -- executed a function this process just wrote (0x%x)\n", rc);
    return 0;
}

/* ==================================================================== main */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <goldens-dir> [--self-check]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    int self_check = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--self-check")) self_check = 1;

    if (rx_proof()) return 1;

    char cpath[1024], gpath[1024];
    snprintf(cpath, sizeof(cpath), "%s/a32_cases.json", dir);
    snprintf(gpath, sizeof(gpath), "%s/a32_goldens.json", dir);
    char *ctext = read_file(cpath), *gtext = read_file(gpath);
    if (!ctext || !gtext) {
        fprintf(stderr, "compare: cannot read %s or %s\n", cpath, gpath);
        return 2;
    }

    P = ctext;
    jv *cdoc = jval();
    P = gtext;
    jv *gdoc = jval();

    if (validate("cases", cdoc, "a32-cases/1", cpath) ||
        validate("goldens", gdoc, "a32-goldens/1", gpath))
        return 2;

    jv *ccases = jget(cdoc, "cases"), *gcases = jget(gdoc, "cases");
    if (ccases->n != gcases->n) {
        fprintf(stderr, "compare: %zu cases but %zu goldens\n", ccases->n, gcases->n);
        return 2;
    }
    for (size_t i = 0; i < gcases->n; i++) {
        if (validate_golden_case(gcases->items[i], gpath, i)) return 2;
        /* "loads the same cases": the manifest and the goldens must name the same
         * case, in the same order. */
        jv *c = ccases->items[i], *g = gcases->items[i];
        if (jnum(jget(c, "id")) != jnum(jget(g, "id"))) {
            fprintf(stderr, "compare: case %zu id differs between manifest (%ld) and goldens (%ld)\n",
                    i, jnum(jget(c, "id")), jnum(jget(g, "id")));
            return 2;
        }
    }
    printf("compare: loaded %zu cases and %zu goldens from %s\n",
           ccases->n, gcases->n, dir);

    /* -------------------------------------------------------------------
     * (b) the diff/report path, on a trivially-true comparison: every golden
     * against itself. If this reports anything, the comparer is broken, not
     * the input -- so it is a hard error, not a mismatch.
     * ------------------------------------------------------------------- */
    int self_diffs = 0;
    for (size_t i = 0; i < gcases->n; i++) {
        int id = (int)jnum(jget(gcases->items[i], "id"));
        const char *name = jget(gcases->items[i], "name")->str;
        self_diffs += cmp_records(gcases->items[i], gcases->items[i], id, name);
    }
    if (self_diffs) {
        fprintf(stderr, "compare: INTERNAL: a golden differs from itself (%d diffs)\n", self_diffs);
        return 2;
    }
    printf("compare: trivial self-comparison: all %zu cases equal (diff/report path OK)\n",
           gcases->n);

    if (!self_check) {
        printf("compare: PASS (this increment validates structure and the report "
               "path; the translator is not wired in yet)\n");
        return 0;
    }

    /* -------------------------------------------------------------------
     * (c) --self-check: plant one field and require the report to name exactly
     * it. This is the deliberate red; it is not enabled by default.
     * ------------------------------------------------------------------- */
    jv *a = gcases->items[0];
    jv *b = jclone(a);
    jv *regs = jget(b, "regs");
    int id = (int)jnum(jget(a, "id"));
    const char *name = jget(a, "name")->str;
    uint64_t was = jhex(regs->items[3]);
    free(regs->items[3]->str);
    regs->items[3]->str = strdup(was == 0xdeadbeefu ? "0x00000001" : "0xdeadbeef");

    printf("compare: --self-check: planted a mismatch in case %d (%s) regs[3]\n", id, name);
    int planted = cmp_records(a, b, id, name);
    if (planted != 1 || strcmp(g_last_field, "regs[3]") != 0) {
        fprintf(stderr, "compare: SELF-CHECK FAILED: expected exactly 1 diff naming "
                        "regs[3], got %d diff(s) naming '%s'\n", planted, g_last_field);
        return 2;
    }
    printf("compare: --self-check: the report named case %d and field %s -- the "
           "report path works\n", id, g_last_field);
    printf("compare: --self-check PASS\n");

    jfreev(cdoc); jfreev(gdoc); jfreev(b);
    free(ctext); free(gtext);
    return 0;
}
