/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Writing to code, for guests that patch it: Geode, which hooks Geometry Dash by overwriting the first instructions of its
 * functions and building trampolines in executable memory it maps for itself.
 *
 * On a phone, memory can be executable or writable, never both: every library's code is in the executable view of the JIT
 * region (husk-tl-xmem.c), and the same pages are writable only through a second view a fixed distance away. A hooking
 * library does not know that: it mprotects for write and stores straight into the code. So:
 *
 *   - mprotect on the JIT region says yes and changes nothing (its permissions must stay as they are);
 *   - mmap for executable memory hands out a piece of the JIT region;
 *   - a store into the executable view faults, and this guard performs it through the writable twin -- decoding the
 *     instruction from the faulting PC, reading its value from the saved registers -- flushes the instruction cache and
 *     resumes after it. Land it there, NEVER by making the page writable: a vm_protect naming WRITE on these pages is what
 *     revokes the debugger's per-entry execute grant for the whole region (husk-tl-xmem.c, adopt_exec_view;
 *     jit-grant-and-detach.md). A store into a view that is not writable at all is refused honestly, not guessed at.
 *
 * Off unless a driver turns it on (tl_codewrite_enable) OR the arena opens on the executable view, which is exactly the
 * engine that needs it (husk-tl-xmem.c arms it there), so no other game's crash passes through here.
 */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "husk-tl-codewrite.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

static volatile bool g_on;
static struct sigaction g_prev_bus, g_prev_segv;
static volatile long g_emulated;
static volatile long g_emulated_relro;          /* of the carries, the ones whose address is in a RELRO page (measurement) */

bool tl_codewrite_enabled(void) { return g_on; }
long tl_codewrite_count(void) { return g_emulated; }
long tl_codewrite_relro_count(void) { return g_emulated_relro; }

static int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

/* A general register as a store sees it: 31 is the zero register for the value, the stack pointer for the base. */
static uint64_t xreg(const struct __darwin_arm_thread_state64 *ss, unsigned r, bool sp)
{
    if (r == 31) return sp ? ss->__sp : 0;
    if (r == 29) return ss->__fp;
    if (r == 30) return ss->__lr;
    return ss->__x[r];
}
static void set_xreg(struct __darwin_arm_thread_state64 *ss, unsigned r, uint64_t v)
{
    if (r == 31) ss->__sp = v;
    else if (r == 29) ss->__fp = v;
    else if (r == 30) ss->__lr = v;
    else ss->__x[r] = v;
}

/* Store `bytes` of a register's value at `addr`, through whichever view is writable.
 *
 * An address in the EXECUTABLE view (library code a hooking library patches) is written through the writable twin at the
 * fixed delta. An address in the GUEST-FACING view (a JIT's own arena) is written in place, with the thread briefly in
 * write mode where W^X is enforced (tl_xmem_emit_begin/end are no-ops otherwise). Either way the instruction cache is
 * dropped for the bytes changed. */
static bool put(uint64_t addr, const void *value, unsigned bytes)
{
    if (tl_xmem_is_rx((const void *)addr) && tl_xmem_is_rx((const void *)(addr + bytes - 1))) {
        /* The address is in the EXECUTABLE view (RX). Land the store through the writable twin at the fixed delta,
         * the way a JIT substrate does, and leave the executable page executable so the guest's next fetch still runs.
         * Do NOT vm_protect WRITE here -- that ask is what revokes the debugger's per-entry execute grant (the
         * two-run control and the old per-page flip are in jit-grant-and-detach.md). The two views are one set of
         * pages, so no bytes differ; a refusal is a no-op, not a guess. */
        memcpy((void *)(addr + (uint64_t)tl_xmem_delta()), value, bytes);
        tl_xmem_flush((const void *)addr, bytes);
        {
            static int said;
            if (said++ < 4)
                tl_log_line("husk: store into the executable view at %p (%u bytes) carried through the writable twin at %p (delta %td) -- the executable page was NOT made writable and stays executable, and the guest fetches the byte it wrote at %p",
                            (void *)addr, bytes, (void *)(addr + (uint64_t)tl_xmem_delta()), tl_xmem_delta(), (void *)addr);
        }
        return true;
    }
    if (tl_xmem_contains((const void *)addr) && tl_xmem_contains((const void *)(addr + bytes - 1))) {
        tl_xmem_emit_begin();
        memcpy((void *)addr, value, bytes);
        tl_xmem_emit_end();
        tl_xmem_flush((const void *)addr, bytes);
        return true;
    }
    return false;
}

static void reg_bytes(const mcontext_t mc, bool simd, unsigned r, unsigned bytes, uint8_t out[16])
{
    memset(out, 0, 16);
    if (simd) memcpy(out, &mc->__ns.__v[r], bytes);
    else { uint64_t v = xreg(&mc->__ss, r, false); memcpy(out, &v, bytes > 8 ? 8 : bytes); }
}

/* The store at the faulting PC, done through the writable view. False -- and `*why` says why -- if it is not a store
 * this understands; the caller must then chain the fault to the previous handler exactly as it did before. */
static bool emulate(mcontext_t mc, const char **why)
{
    struct __darwin_arm_thread_state64 *ss = &mc->__ss;
    uint32_t insn = *(const uint32_t *)ss->__pc;
    uint8_t buf[16];

#define REFUSE(txt) do { *why = (txt); return false; } while (0)

    /* DC ZVA: memset zeroes whole blocks this way. */
    if ((insn & 0xFFFFFFE0u) == 0xD50B7420u) {
        uint64_t dczid;
        __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
        unsigned block = 4u << (dczid & 0xF);
        uint64_t a = xreg(ss, insn & 31, false) & ~(uint64_t)(block - 1);
        static const uint8_t zero[256];
        for (unsigned done = 0; done < block; done += 16)
            if (!put(a + done, zero, 16)) REFUSE("the DC ZVA block is in neither view of the JIT region, so there is no writable twin for it");
        return true;
    }

    unsigned rt = insn & 31, rn = (insn >> 5) & 31;
    bool simd = (insn >> 26) & 1;

    /* Atomics (the C++ runtime's std::atomic, Dart's BSS set-up). A fault here is start-up initialisation on one thread,
     * so the store is done plainly. */
    {
        unsigned bytes = 1u << (insn >> 30);
        uint64_t a = xreg(ss, rn, true);
        if ((insn & 0x3FE07C00u) == 0x08007C00u) {                 /* STXR / STLXR Ws, Rt, [Xn]: store, and report success in Ws */
            reg_bytes(mc, false, rt, bytes, buf);
            if (!put(a, buf, bytes)) REFUSE("an exclusive store whose address is in neither view of the JIT region");
            set_xreg(ss, (insn >> 16) & 31, 0);
            return true;
        }
        if ((insn & 0x3FFFFC00u) == 0x089FFC00u) {                 /* STLR Rt, [Xn] */
            reg_bytes(mc, false, rt, bytes, buf);
            if (!put(a, buf, bytes)) REFUSE("an ordered store (STLR) whose address is in neither view of the JIT region");
            return true;
        }
        if ((insn & 0x3FA07C00u) == 0x08A07C00u) {                 /* CAS{A}{L} Rs, Rt, [Xn]: store Rt if [Xn] == Rs; Rs gets the old value */
            unsigned rs = (insn >> 16) & 31;
            uint64_t old = 0, want = xreg(ss, rs, false), mask = bytes == 8 ? ~0ull : ((1ull << (8 * bytes)) - 1);
            memcpy(&old, (const void *)a, bytes);
            if ((old & mask) == (want & mask)) {
                reg_bytes(mc, false, rt, bytes, buf);
                if (!put(a, buf, bytes)) REFUSE("a compare-and-swap store whose address is in neither view of the JIT region");
            }
            set_xreg(ss, rs, old);
            return true;
        }
    }

    /* ----------------------------------------------------- Advanced SIMD structure stores (ST1 / ST2 / ST3 / ST4)
     *
     * Both families live outside the "load/store register" class the cases below cover, so none of their masks matches
     * them; that is the form this guard once refused (e.g. `st1 {v1.s}[1], [x9]`). They share one shape: Rn holds the
     * address, the register(s) the value(s), there is no pre-index form, and the post-index bit (23) either names a
     * register (Rm, bits[20:16]) or, with Rm == 31, an immediate of exactly the bytes transferred. Single structure (one
     * lane) spreads its lane index over Q (bit 30 -- so Q is not a vector width here), S (bit 12) and size
     * (bits[11:10]); multiple structure writes whole vectors of Q ? 16 : 8 bytes and takes its register count from
     * bits[15:12]. Only the store direction is carried. The validity rules encoded below were checked with capstone
     * over both families' whole bit space (5709 encodings). See codewrite-store-guard.md. */
    {
        unsigned fam = insn & 0xBF000000u;
        if (fam == 0x0C000000u || fam == 0x0D000000u) {
            unsigned post = (insn >> 23) & 1, rm = (insn >> 16) & 31, q = (insn >> 30) & 1, size = (insn >> 10) & 3;
            unsigned nreg, per, index = 0;

            if ((insn >> 22) & 1) REFUSE("an Advanced SIMD structure LOAD on a store fault");
            if (!post && rm != 0) REFUSE("an Advanced SIMD structure store with no post-index but a non-zero Rm");

            if (fam == 0x0D000000u) {                               /* single structure: ONE lane */
                unsigned op = (insn >> 13) & 7, rbit = (insn >> 21) & 1, base = op >> 1, s = (insn >> 12) & 1;
                if (op > 5) REFUSE("an unallocated Advanced SIMD single-structure opcode");
                nreg = ((op & 1) ? 3u : 1u) + rbit;
                if (base == 0) {                                    /* 8-bit elements: 16 lanes */
                    per = 1; index = (q << 3) | (s << 2) | size;
                } else if (base == 1) {                             /* 16-bit elements: 8 lanes */
                    if (size & 1) REFUSE("an unallocated Advanced SIMD single-structure encoding (16-bit element with size<0> set)");
                    per = 2; index = (q << 2) | (s << 1) | (size >> 1);
                } else {                                            /* 32- or 64-bit elements: 4 or 2 lanes */
                    if (size > 1) REFUSE("an unallocated Advanced SIMD single-structure encoding (element wider than a doubleword)");
                    if (size == 0) { per = 4; index = (q << 1) | s; }
                    else { if (s) REFUSE("an unallocated Advanced SIMD single-structure encoding (64-bit element with the S bit set)");
                           per = 8; index = q; }
                }
            } else {                                                /* multiple structures: WHOLE vectors */
                unsigned opc = (insn >> 12) & 0xF;
                if ((insn >> 21) & 1) REFUSE("an Advanced SIMD multiple-structure store with the R bit set, which no such form encodes");
                nreg = opc == 0x0 ? 4u : opc == 0x4 ? 3u : opc == 0x8 ? 2u :     /* ST4 / ST3 / ST2 */
                       opc == 0x2 ? 4u : opc == 0x6 ? 3u : opc == 0x7 ? 1u : opc == 0xA ? 2u : 0u;   /* ST1 of 4 / 3 / 1 / 2 */
                if (!nreg) REFUSE("an unallocated Advanced SIMD multiple-structure opcode");
                if ((opc == 0x0 || opc == 0x4 || opc == 0x8) && !q && size == 3)
                    REFUSE("an unallocated Advanced SIMD multiple-structure encoding (single-doubleword element)");
                per = q ? 16u : 8u;
            }

            uint64_t base_addr = xreg(ss, rn, true);
            for (unsigned k = 0; k < nreg; k++) {
                unsigned r = (rt + k) & 31;
                memset(buf, 0, 16);
                memcpy(buf, (const uint8_t *)&mc->__ns.__v[r] + (fam == 0x0D000000u ? (size_t)index * per : 0), per);
                if (!put(base_addr + (uint64_t)k * per, buf, per))
                    REFUSE("an Advanced SIMD structure store whose address is in neither view of the JIT region");
            }
            if (post) {                                             /* Rm == 31 is the immediate, the bytes transferred */
                uint64_t step = rm == 31 ? (uint64_t)(nreg * per) : xreg(ss, rm, false);
                set_xreg(ss, rn, base_addr + step);
            }
            return true;
        }
    }

    /* STP / STNP: a pair. */
    if ((insn & 0x3A000000u) == 0x28000000u) {
        if ((insn >> 22) & 1) REFUSE("a load/store PAIR with the L bit set (a load), on a store fault");
        unsigned opc = insn >> 30, type = (insn >> 23) & 3;
        unsigned scale = simd ? 2 + opc : (opc == 0 ? 2 : opc == 2 ? 3 : 99);
        if (scale > 4) REFUSE("a load/store PAIR whose element width this decoder does not know");
        unsigned rt2 = (insn >> 10) & 31, bytes = 1u << scale;
        int64_t off = sext((insn >> 15) & 0x7F, 7) << scale;
        uint64_t base = xreg(ss, rn, true);
        uint64_t a = type == 1 ? base : base + (uint64_t)off;       /* post-index stores at the base */
        reg_bytes(mc, simd, rt, bytes, buf);
        if (!put(a, buf, bytes)) REFUSE("the first half of a store PAIR whose address is in neither view of the JIT region");
        reg_bytes(mc, simd, rt2, bytes, buf);
        if (!put(a + bytes, buf, bytes)) REFUSE("the second half of a store PAIR whose address is in neither view of the JIT region");
        if (type == 1 || type == 3) set_xreg(ss, rn, base + (uint64_t)off);
        return true;
    }

    /* STR / STRB / STRH and their SIMD forms. */
    unsigned size = insn >> 30, opc = (insn >> 22) & 3;
    bool is_store = opc == 0 || (simd && opc == 2 && size == 0);    /* SIMD opc 2 with size 0 is the 128-bit store */
    unsigned scale = (simd && opc == 2) ? 4 : size;
    unsigned bytes = 1u << scale;

    if ((insn & 0x3B000000u) == 0x39000000u) {                      /* unsigned immediate offset */
        if (!is_store) REFUSE("a load (unsigned immediate offset), on a store fault");
        uint64_t a = xreg(ss, rn, true) + ((uint64_t)((insn >> 10) & 0xFFF) << scale);
        reg_bytes(mc, simd, rt, bytes, buf);
        if (!put(a, buf, bytes)) REFUSE("a store whose address is in neither view of the JIT region");
        return true;
    }
    if ((insn & 0x3B200000u) == 0x38000000u) {                      /* unscaled / pre-index / post-index / unprivileged */
        if (!is_store) REFUSE("an unscaled/pre-/post-index LOAD, on a store fault");
        unsigned type = (insn >> 10) & 3;
        int64_t off = sext((insn >> 12) & 0x1FF, 9);
        uint64_t base = xreg(ss, rn, true);
        uint64_t a = type == 1 ? base : base + (uint64_t)off;
        reg_bytes(mc, simd, rt, bytes, buf);
        if (!put(a, buf, bytes)) REFUSE("a store whose address is in neither view of the JIT region");
        if (type == 1 || type == 3) set_xreg(ss, rn, base + (uint64_t)off);
        return true;
    }
    if ((insn & 0x3B200C00u) == 0x38200800u) {                      /* register offset */
        if (!is_store) REFUSE("a register-offset LOAD, on a store fault");
        unsigned rm = (insn >> 16) & 31, option = (insn >> 13) & 7, s = (insn >> 12) & 1;
        uint64_t idx = xreg(ss, rm, false);
        if (option == 2) idx = (uint32_t)idx;                       /* UXTW */
        else if (option == 6) idx = (uint64_t)(int64_t)(int32_t)idx;   /* SXTW */
        idx <<= s ? scale : 0;
        uint64_t a = xreg(ss, rn, true) + idx;
        reg_bytes(mc, simd, rt, bytes, buf);
        if (!put(a, buf, bytes)) REFUSE("a store whose address is in neither view of the JIT region");
        return true;
    }
    REFUSE("no case in this guard's decoder matches the instruction word");
#undef REFUSE
}

/*
 * ------------------------------------------------------------------ the refusal names itself
 *
 * A refusal NAMES the instruction it declines: the pc, WHICH GUEST IMAGE it lies in (the same "+0x..." the crash
 * report prints), the raw bytes, the mnemonic where the decoder knows the form, and the decoder's own reason. So the
 * next occurrence needs no probe build. Named ONCE per distinct refusal -- a hook loop faults on the same instruction
 * hundreds of times -- in a fixed static table, because this runs on the fault path where nothing may allocate. The
 * path itself is unchanged: `chain` below is the same call, with the same arguments, at the same place.
 */
#define TL_CW_REFUSALS 8
static struct { uintptr_t pc; uint32_t insn; } g_refused[TL_CW_REFUSALS];
static volatile int g_refused_n;
static volatile long g_refused_total;

static bool refusal_is_new(uintptr_t pc, uint32_t insn)
{
    int n = g_refused_n < TL_CW_REFUSALS ? g_refused_n : TL_CW_REFUSALS;
    for (int i = 0; i < n; i++)
        if (g_refused[i].pc == pc && g_refused[i].insn == insn) return false;
    if (g_refused_n < TL_CW_REFUSALS) {
        g_refused[g_refused_n].pc = pc;
        g_refused[g_refused_n].insn = insn;
        g_refused_n++;
    }
    return true;
}

/* The instruction as one short line, appended by hand into the caller's buffer (the fault path allocates nothing): the
 * bytes always, and the mnemonic and operands where THIS decoder knows the form. */
static void describe_insn(uint32_t insn, char *out, size_t n)
{
    static const char *const elem[4] = { "b", "h", "s", "d" };
    unsigned rt = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, sz = (insn >> 10) & 3;
    bool post = (insn >> 23) & 1, load = (insn >> 22) & 1, simd = (insn >> 26) & 1;
    unsigned fam = insn & 0xBF000000u;
    size_t k = 0;

#define APP(...) do { if (k < n) { k += (size_t)snprintf(out + k, n - k, __VA_ARGS__); if (k > n) k = n; } } while (0)

    if (fam == 0x0D000000u || fam == 0x0C000000u) {
        unsigned nreg, per, index = 0, q = (insn >> 30) & 1;
        char arr[8];
        if (fam == 0x0D000000u) {                                   /* single structure: one lane */
            unsigned op = (insn >> 13) & 7, rbit = (insn >> 21) & 1, base = op >> 1, s = (insn >> 12) & 1;
            nreg = ((op & 1) ? 3u : 1u) + rbit;
            if (base == 0)      { per = 1; index = (q << 3) | (s << 2) | sz; snprintf(arr, sizeof arr, "b"); }
            else if (base == 1) { per = 2; index = (q << 2) | (s << 1) | (sz >> 1); snprintf(arr, sizeof arr, "h"); }
            else if (sz == 0)   { per = 4; index = (q << 1) | s; snprintf(arr, sizeof arr, "s"); }
            else                { per = 8; index = q; snprintf(arr, sizeof arr, "d"); }
            APP("%s%u {", load ? "ld" : "st", nreg);
            for (unsigned j = 0; j < nreg; j++) APP("%sv%u.%s", j ? ", " : "", (rt + j) & 31, arr);
            APP("}[%u], [x%u]", index, rn);
        } else {                                                    /* multiple structures: whole vectors */
            unsigned opc = (insn >> 12) & 0xF;
            per = q ? 16u : 8u;
            nreg = opc == 0x0 ? 4u : opc == 0x4 ? 3u : opc == 0x8 ? 2u :
                   opc == 0x2 ? 4u : opc == 0x6 ? 3u : opc == 0x7 ? 1u : 2u;
            snprintf(arr, sizeof arr, "%u%s", (q ? 16u : 8u) >> sz, elem[sz]);
            APP("%s%u {", load ? "ld" : "st", nreg);
            for (unsigned j = 0; j < nreg; j++) APP("%sv%u.%s", j ? ", " : "", (rt + j) & 31, arr);
            APP("}, [x%u]", rn);
        }
        if (post) { if (rm == 31) APP(", #%u", nreg * per); else APP(", x%u", rm); }
    } else if ((insn & 0x3FE07C00u) == 0x08007C00u) {
        APP("stxr/stlxr: a %u-byte exclusive store from w%u to [x%u]", 1u << (insn >> 30), rt, rn);
    } else if ((insn & 0x3FFFFC00u) == 0x089FFC00u) {
        APP("stlr: a %u-byte ordered store of x%u to [x%u]", 1u << (insn >> 30), rt, rn);
    } else if ((insn & 0x3FA07C00u) == 0x08A07C00u) {
        APP("cas/casa/casal: a %u-byte compare-and-swap on [x%u]", 1u << (insn >> 30), rn);
    } else if ((insn & 0xFFFFFFE0u) == 0xD50B7420u) {
        APP("dc zva: zero a block at [x%u]", insn & 31);
    } else if ((insn & 0x3A000000u) == 0x28000000u) {
        unsigned popc = insn >> 30, pscale = simd ? 2 + popc : (popc == 0 ? 2 : popc == 2 ? 3 : 99);
        APP("%s%s%s: a pair of %u-byte elements at [x%u]", load ? "ld" : "st", simd ? " (SIMD)" : "",
            (insn >> 23) & 1 ? " (post-index)" : (insn >> 24) & 1 ? " (pre-index)" : "", pscale > 4 ? 0u : (1u << pscale), rn);
    } else if ((insn & 0x3B000000u) == 0x39000000u || (insn & 0x3B200000u) == 0x38000000u || (insn & 0x3B200C00u) == 0x38200800u) {
        unsigned opc = (insn >> 22) & 3;
        APP("%s%s: a %u-byte access at [x%u]", opc ? "ld" : "st", simd ? " (SIMD)" : "",
            (opc == 2 && !(insn >> 30)) ? 16u : (1u << (insn >> 30)), rn);
    } else {
        APP("(not a form this decoder knows)");
    }
#undef APP
}

/* One line per DISTINCT refusal, printed from the fault path. */
static void name_refusal(uintptr_t pc, uint32_t insn, bool have_insn, const char *why, uintptr_t fault)
{
    g_refused_total++;
    if (!refusal_is_new(pc, have_insn ? insn : 0)) return;
    {
        char where[80], text[192];
        const tl_lib *lib = tl_ld_lib_of((const void *)pc);
        if (lib && tl_ld_lib_base(lib))
            snprintf(where, sizeof where, "%s+%#llx", tl_ld_lib_name(lib),
                     (unsigned long long)(pc - (uintptr_t)tl_ld_lib_base(lib)));
        else
            snprintf(where, sizeof where, "no guest image at that pc");
        if (have_insn) {
            describe_insn(insn, text, sizeof text);
            tl_log_line("codewrite: REFUSED a store at pc %p (%s), fault address %p: bytes %02x %02x %02x %02x (word %08x)"
                        " = %s -- %s; nothing was carried and the fault is chained to the previous handler unchanged"
                        " (distinct refusal %d of %d, %ld refused in all)",
                        (void *)pc, where, (void *)fault,
                        (unsigned)(insn & 0xFFu), (unsigned)((insn >> 8) & 0xFFu),
                        (unsigned)((insn >> 16) & 0xFFu), (unsigned)((insn >> 24) & 0xFFu),
                        (unsigned)insn, text, why, g_refused_n, TL_CW_REFUSALS, g_refused_total);
        } else {
            tl_log_line("codewrite: REFUSED a store at pc %p (%s), fault address %p: the instruction word could not be"
                        " read -- %s; nothing was carried and the fault is chained to the previous handler unchanged"
                        " (distinct refusal %d of %d, %ld refused in all)",
                        (void *)pc, where, (void *)fault, why, g_refused_n, TL_CW_REFUSALS, g_refused_total);
        }
    }
}

long tl_codewrite_refused_count(void) { return g_refused_total; }

static void chain(struct sigaction *prev, int sig, siginfo_t *info, void *uc)
{
    if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) { prev->sa_sigaction(sig, info, uc); return; }
    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN && prev->sa_handler) { prev->sa_handler(sig); return; }
    signal(sig, SIG_DFL);                                           /* nothing else wants it: let it fault again and end */
}

static void on_fault(int sig, siginfo_t *info, void *uc)
{
    ucontext_t *u = uc;
    /* A store into the JIT region faults in either view: the executable view (library code a hooking library patches)
     * or the guest-facing view (a JIT's own arena, needing only the thread back in write mode). Elsewhere it is not ours. */
    if (g_on && info) {
        uintptr_t pc = (uintptr_t)u->uc_mcontext->__ss.__pc;
        bool have_insn = (pc & 3) == 0;
        const char *why = NULL;
        if (tl_xmem_contains(info->si_addr)) {
            if (emulate(u->uc_mcontext, &why)) {
                u->uc_mcontext->__ss.__pc += 4;
                g_emulated++;
                /* MEASUREMENT, not a behaviour: is this carried store into a PT_GNU_RELRO page? tl_page_plan keeps
                 * RELRO out of TL_PAGE_W, so the loader names those pages through the executable view and a store
                 * there reaches this guard even though the bytes are really data. Counted so a run says whether
                 * naming RELRO through the writable view is worth the change; no line per store, no lock. */
                if (tl_ld_va_relro(info->si_addr) == 1) g_emulated_relro++;
                return;
            }
        } else {
            why = "the fault address is in neither view of the JIT region, so there is no twin to write through";
        }
        /* A refusal names the instruction it declined, once per distinct one, then chains exactly as before. */
        name_refusal(pc, have_insn ? *(const uint32_t *)pc : 0u, have_insn, why, (uintptr_t)info->si_addr);
    }
    chain(sig == SIGBUS ? &g_prev_bus : &g_prev_segv, sig, info, uc);
}

void tl_codewrite_enable(void)
{
    if (g_on) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g_prev_bus);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    g_on = true;
    tl_log_line("codewrite: stores into code go through the writable view from now on");
}
