/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32_interp.c -- the A32 (ARM state) integer reference interpreter.
 *
 * One instruction in, one instruction out, in C, with no tricks. See
 * a32_interp.h for the scope, the memory model and the exact initial state it
 * reproduces so a run is comparable to a golden.
 *
 * The one rule that matters here: an instruction this file does not implement,
 * or a memory access outside the arena, is a NAMED fault (a32_fault), never a
 * silent wrong answer. The harness counts a faulted case as NOT passing.
 */
#include "a32_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ arena */
#define ARENA_BASE  A32_INTERP_ARENA_BASE
#define ARENA_SIZE  A32_INTERP_ARENA_SIZE
#define ARENA_END   (A32_INTERP_ARENA_BASE + A32_INTERP_ARENA_SIZE)
#define CODE_BASE   A32_INTERP_ARENA_BASE
#define SCRATCH     A32_INTERP_SCRATCH_BASE
#define STEP_LIMIT  100000u

/* CPSR flag bits (the only ones this interpreter gives meaning to). */
#define F_N 0x80000000u
#define F_Z 0x40000000u
#define F_C 0x20000000u
#define F_V 0x10000000u
#define F_Q 0x08000000u
/* What a32_case_N's wrapper leaves: N=0 Z=1 C=1 V=0, user mode (0x10). */
#define CPSR_INIT (F_Z | F_C | 0x10u)

struct cpu {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t base;
    uint8_t *mem;
    uint32_t case_id;
    uint32_t steps;
    uint32_t code_end;
    int      tfault;
    char    *err;
    size_t   errn;
};

static void a32_fault(struct cpu *c, uint32_t pc, uint32_t insn, const char *why)
{
    if (c->tfault) return;
    c->tfault = 1;
    if (c->err)
        snprintf(c->err, c->errn,
                 "case %u: %s at pc=0x%08x (instruction word 0x%08x)",
                 c->case_id, why, pc, insn);
}

/* ------------------------------------------------------------ memory */
static int in_arena(uint32_t a, uint32_t n)
{
    return a >= ARENA_BASE && (uint64_t)a + (uint64_t)n <= (uint64_t)ARENA_END;
}
static uint32_t rd8(struct cpu *c, uint32_t a)  { return c->mem[a - c->base]; }
static uint32_t rd16(struct cpu *c, uint32_t a) {
    return (uint32_t)c->mem[a - c->base] | ((uint32_t)c->mem[a - c->base + 1] << 8);
}
static uint32_t rd32(struct cpu *c, uint32_t a) {
    return (uint32_t)c->mem[a - c->base]
         | ((uint32_t)c->mem[a - c->base + 1] << 8)
         | ((uint32_t)c->mem[a - c->base + 2] << 16)
         | ((uint32_t)c->mem[a - c->base + 3] << 24);
}
static void wr8(struct cpu *c, uint32_t a, uint32_t v)  { c->mem[a - c->base] = (uint8_t)v; }
static void wr16(struct cpu *c, uint32_t a, uint32_t v) {
    c->mem[a - c->base]     = (uint8_t)v;
    c->mem[a - c->base + 1] = (uint8_t)(v >> 8);
}
static void wr32(struct cpu *c, uint32_t a, uint32_t v) {
    c->mem[a - c->base]     = (uint8_t)v;
    c->mem[a - c->base + 1] = (uint8_t)(v >> 8);
    c->mem[a - c->base + 2] = (uint8_t)(v >> 16);
    c->mem[a - c->base + 3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------ helpers */
static uint32_t get_r(struct cpu *c, int n, uint32_t pc)
{
    return (n == 15) ? (pc + 8u) : c->r[n];   /* reading PC yields pc+8 in ARM */
}

static int cond_true(uint32_t cond, uint32_t cpsr)
{
    int n = (cpsr & F_N) != 0, z = (cpsr & F_Z) != 0;
    int cc = (cpsr & F_C) != 0, v = (cpsr & F_V) != 0;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return cc;
    case 0x3: return !cc;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return cc && !z;
    case 0x9: return !cc || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return 1;
    default:  return 0;
    }
}

static uint32_t do_shift(uint32_t v, int type, uint32_t amount, int *cout)
{
    switch (type) {
    case 0: /* LSL */
        if (amount == 0) return v;
        if (amount < 32) { *cout = (v >> (32 - amount)) & 1u; return v << amount; }
        if (amount == 32) { *cout = v & 1u; return 0; }
        *cout = 0; return 0;
    case 1: /* LSR */
        if (amount == 0) return v;
        if (amount < 32) { *cout = (v >> (amount - 1)) & 1u; return v >> amount; }
        if (amount == 32) { *cout = (v >> 31) & 1u; return 0; }
        *cout = 0; return 0;
    case 2: /* ASR */
        if (amount == 0) return v;
        if (amount < 32) { *cout = (v >> (amount - 1)) & 1u; return (uint32_t)((int32_t)v >> amount); }
        *cout = (v >> 31) & 1u;
        return (uint32_t)((int32_t)v >> 31);
    default: /* ROR */
        if (amount == 0) return v;
        { uint32_t a = amount & 31u;
          if (a == 0) { *cout = (v >> 31) & 1u; return v; }
          *cout = (v >> (a - 1)) & 1u;
          return (v >> a) | (v << (32 - a)); }
    }
}

static uint32_t sx(uint32_t x, int n)
{
    uint32_t m = 1u << (n - 1);
    return (x ^ m) - m;
}
static uint32_t ror8(uint32_t imm8, uint32_t rot)
{
    if (rot == 0) return imm8;
    return (imm8 >> (2 * rot)) | (imm8 << (32 - 2 * rot));
}
static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static int popcount16(uint32_t x)
{
    int n = 0;
    while (x) { n += (int)(x & 1u); x >>= 1; }
    return n;
}

/* --------------------------------------------------- data-processing exec */
/* opcode is the 4-bit DP opcode (0x0 AND .. 0xF MVN). Returns 1 if the
 * instruction faulted (a name was recorded), else 0. */
static int dp_execute(struct cpu *c, uint32_t pc, int opcode, int S,
                      int rn, int rd, uint32_t op2, int cout)
{
    uint32_t a = get_r(c, rn, pc);
    uint32_t res = 0;
    int is_test = 0, is_logical = 0;
    uint32_t newc = c->cpsr & F_C, newv = c->cpsr & F_V;

    switch (opcode) {
    case 0x0: res = a & op2;  is_logical = 1; break;
    case 0x1: res = a ^ op2;  is_logical = 1; break;
    case 0x2: res = (uint32_t)((uint64_t)a - op2);
              newc = (a >= op2) ? F_C : 0;
              newv = (((a ^ op2) & (a ^ res)) >> 31) ? F_V : 0; break;
    case 0x3: res = (uint32_t)((uint64_t)op2 - a);
              newc = (op2 >= a) ? F_C : 0;
              newv = (((op2 ^ a) & (op2 ^ res)) >> 31) ? F_V : 0; break;
    case 0x4: { uint64_t r = (uint64_t)a + op2; res = (uint32_t)r;
                newc = (r >> 32) ? F_C : 0;
                newv = ((~(a ^ op2) & (a ^ res)) >> 31) ? F_V : 0; } break;
    case 0x5: { uint32_t ci = (c->cpsr & F_C) ? 1u : 0u;
                uint64_t r = (uint64_t)a + op2 + ci; res = (uint32_t)r;
                newc = (r >> 32) ? F_C : 0;
                newv = ((~(a ^ op2) & (a ^ res)) >> 31) ? F_V : 0; } break;
    case 0x6: { uint32_t nci = (c->cpsr & F_C) ? 0u : 1u;
                uint64_t sub = (uint64_t)op2 + nci;
                res = (uint32_t)((uint64_t)a - sub);
                newc = ((uint64_t)a >= sub) ? F_C : 0;
                newv = (((a ^ op2) & (a ^ res)) >> 31) ? F_V : 0; } break;
    case 0x7: { uint32_t nci = (c->cpsr & F_C) ? 0u : 1u;
                uint64_t sub = (uint64_t)a + nci;
                res = (uint32_t)((uint64_t)op2 - sub);
                newc = ((uint64_t)op2 >= sub) ? F_C : 0;
                newv = (((op2 ^ a) & (op2 ^ res)) >> 31) ? F_V : 0; } break;
    case 0x8: res = a & op2; is_test = 1; is_logical = 1; break;   /* TST */
    case 0x9: res = a ^ op2; is_test = 1; is_logical = 1; break;   /* TEQ */
    case 0xA: res = (uint32_t)((uint64_t)a - op2);
              newc = (a >= op2) ? F_C : 0;
              newv = (((a ^ op2) & (a ^ res)) >> 31) ? F_V : 0;
              is_test = 1; break;                                  /* CMP */
    case 0xB: { uint64_t r = (uint64_t)a + op2; res = (uint32_t)r;
                newc = (r >> 32) ? F_C : 0;
                newv = ((~(a ^ op2) & (a ^ res)) >> 31) ? F_V : 0;
                is_test = 1; } break;                              /* CMN */
    case 0xC: res = a | op2;  is_logical = 1; break;
    case 0xD: res = op2;      is_logical = 1; break;
    case 0xE: res = a & ~op2; is_logical = 1; break;
    default:  res = ~op2;     is_logical = 1; break;               /* MVN */
    }

    if (is_logical) newc = cout ? F_C : 0;

    if (!is_test) {
        if (rd == 15) {
            a32_fault(c, pc, 0, S
                      ? "data-processing with Rd=PC and S set (return/mode change) not implemented"
                      : "data-processing writing PC (register branch) not implemented");
            return 1;
        }
        c->r[rd] = res;
    }
    if (S || is_test) {
        uint32_t f = 0;
        if (res & F_N) f |= F_N;
        if (res == 0)  f |= F_Z;
        f |= newc | newv;
        c->cpsr = (c->cpsr & ~(F_N | F_Z | F_C | F_V)) | f;
    }
    return 0;
}

/* --------------------------------------------- load/store word and byte */
static int decode_ldst_word(struct cpu *c, uint32_t pc, uint32_t insn, int is_reg)
{
    int P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1;
    int W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    uint32_t off;

    if (is_reg) {
        int rm = insn & 0xF, type = (insn >> 5) & 3;
        uint32_t amt = (insn >> 7) & 0x1Fu;
        int cout = (c->cpsr & F_C) ? 1 : 0;
        if (type == 3 && amt == 0) {                 /* RRX */
            uint32_t v = get_r(c, rm, pc);
            off = (uint32_t)(((c->cpsr & F_C) ? 1u : 0u) << 31) | (v >> 1);
        } else {
            if ((type == 1 || type == 2) && amt == 0) amt = 32;
            off = do_shift(get_r(c, rm, pc), type, amt, &cout);
        }
    } else {
        off = insn & 0xFFFu;
    }

    if (rd == 15) { a32_fault(c, pc, insn, "load/store to PC not implemented"); return 1; }
    uint32_t base = get_r(c, rn, pc);
    uint32_t off_addr = U ? base + off : base - off;
    uint32_t addr = P ? off_addr : base;
    uint32_t sz = B ? 1u : 4u;
    if (!in_arena(addr, sz)) { a32_fault(c, pc, insn, "load/store outside the arena"); return 1; }
    if (L) c->r[rd] = B ? rd8(c, addr) : rd32(c, addr);
    else   { if (B) wr8(c, addr, c->r[rd]); else wr32(c, addr, c->r[rd]); }
    if (!P || W) {
        if (rn == 15) { a32_fault(c, pc, insn, "writeback to PC not implemented"); return 1; }
        c->r[rn] = off_addr;
    }
    return 0;
}

/* ---------------------------------- halfword / signed byte / doubleword */
static int decode_extra_ldst(struct cpu *c, uint32_t pc, uint32_t insn)
{
    int P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1;
    int W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    int n74 = (insn >> 4) & 0xF;
    uint32_t base = get_r(c, rn, pc);
    uint32_t off, off_addr, addr;

    if (rd == 15) { a32_fault(c, pc, insn, "extra load/store to PC not implemented"); return 1; }

    if (I) {
        off = ((insn >> 8) & 0xFu) << 4 | (insn & 0xFu);
    } else {
        if (((insn >> 8) & 0xFu) != 0) {
            a32_fault(c, pc, insn, "extra load/store register offset with bits[11:8] != 0");
            return 1;
        }
        off = get_r(c, (int)(insn & 0xF), pc);
    }
    off_addr = U ? base + off : base - off;
    addr = P ? off_addr : base;

    if (n74 == 0xB) {                              /* LDRH / STRH */
        if (!in_arena(addr, 2)) { a32_fault(c, pc, insn, "LDRH/STRH outside the arena"); return 1; }
        if (L) c->r[rd] = rd16(c, addr);
        else   wr16(c, addr, c->r[rd]);
    } else if (n74 == 0xD && L) {                  /* LDRSB */
        if (!in_arena(addr, 1)) { a32_fault(c, pc, insn, "LDRSB outside the arena"); return 1; }
        c->r[rd] = sx(rd8(c, addr), 8);
    } else if (n74 == 0xF && L) {                  /* LDRSH */
        if (!in_arena(addr, 2)) { a32_fault(c, pc, insn, "LDRSH outside the arena"); return 1; }
        c->r[rd] = sx(rd16(c, addr), 16);
    } else {                                       /* LDRD (1101) / STRD (1111) */
        int load = (n74 == 0xD);
        if (rd & 1) { a32_fault(c, pc, insn, "LDRD/STRD with an odd Rt"); return 1; }
        if (addr & 3u) { a32_fault(c, pc, insn, "unaligned LDRD/STRD"); return 1; }
        if (I && (off & 3u)) { a32_fault(c, pc, insn, "unaligned LDRD/STRD offset"); return 1; }
        if (!in_arena(addr, 8)) { a32_fault(c, pc, insn, "LDRD/STRD outside the arena"); return 1; }
        if (load) { c->r[rd] = rd32(c, addr); c->r[rd + 1] = rd32(c, addr + 4); }
        else      { wr32(c, addr, c->r[rd]); wr32(c, addr + 4, c->r[rd + 1]); }
    }

    if (!P || W) {
        if (rn == 15) { a32_fault(c, pc, insn, "writeback to PC not implemented"); return 1; }
        c->r[rn] = off_addr;
    }
    return 0;
}

/* --------------------------------------------------- block transfer */
static int decode_block(struct cpu *c, uint32_t pc, uint32_t insn)
{
    int P = (insn >> 24) & 1, U = (insn >> 23) & 1, S = (insn >> 22) & 1;
    int W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    int rn = (insn >> 16) & 0xF;
    uint32_t list = insn & 0xFFFFu;

    if (list == 0) { a32_fault(c, pc, insn, "LDM/STM with an empty register list"); return 1; }
    if (S) { a32_fault(c, pc, insn, "LDM/STM with the S bit (user bank / CPSR transfer) not implemented"); return 1; }
    if (rn == 15) { a32_fault(c, pc, insn, "LDM/STM with PC as the base not implemented"); return 1; }
    if (list & (1u << 15)) { a32_fault(c, pc, insn, "LDM/STM with PC in the list not implemented"); return 1; }

    int n = popcount16(list);
    uint32_t base = c->r[rn];
    uint32_t lo;
    if (P == 0 && U == 1) lo = base;                     /* IA */
    else if (P == 1 && U == 1) lo = base + 4u;           /* IB */
    else if (P == 0 && U == 0) lo = base - (uint32_t)(n - 1) * 4u;  /* DA */
    else lo = base - (uint32_t)n * 4u;                   /* DB */

    if (!in_arena(lo, (uint32_t)n * 4u)) { a32_fault(c, pc, insn, "LDM/STM outside the arena"); return 1; }

    uint32_t addr = lo;
    for (int r = 0; r < 16; r++) {
        if (!(list & (1u << r))) continue;
        if (L) c->r[r] = rd32(c, addr);
        else   wr32(c, addr, c->r[r]);
        addr += 4;
    }
    if (W) {
        if (U) c->r[rn] = base + (uint32_t)n * 4u;
        else   c->r[rn] = base - (uint32_t)n * 4u;
    }
    return 0;
}

/* ==================================================================== run */

int a32_interp_run(const struct a32_interp_case *in, struct a32_result *out,
                   char *err, size_t errn)
{
    struct cpu cpu, *c = &cpu;
    uint8_t *mem;
    int failed = 0;

    memset(c, 0, sizeof(*c));
    mem = (uint8_t *)malloc(ARENA_SIZE);
    if (!mem) {
        if (err) snprintf(err, errn, "out of memory for the interpreter arena");
        return 1;
    }
    memset(mem, 0, ARENA_SIZE);

    c->mem = mem;
    c->base = ARENA_BASE;
    c->case_id = in->id;
    c->code_end = CODE_BASE + in->code_len * 4u;
    c->err = err;
    c->errn = errn;
    if (err && errn) err[0] = 0;

    if (in->code_len == 0 || in->code_len > (SCRATCH - CODE_BASE) / 4u) {
        if (err) snprintf(err, errn, "case %u: bad fragment length (%u words)",
                          c->case_id, in->code_len);
        free(mem);
        out->stop_reason = A32_STOP_FAULT;
        return 1;
    }
    for (uint32_t i = 0; i < in->code_len; i++) {
        uint32_t w = in->code[i];
        mem[(CODE_BASE - ARENA_BASE) + i * 4 + 0] = (uint8_t)w;
        mem[(CODE_BASE - ARENA_BASE) + i * 4 + 1] = (uint8_t)(w >> 8);
        mem[(CODE_BASE - ARENA_BASE) + i * 4 + 2] = (uint8_t)(w >> 16);
        mem[(CODE_BASE - ARENA_BASE) + i * 4 + 3] = (uint8_t)(w >> 24);
    }

    /* The scratch window's agreed seed: word k = 0x10000000 + k, exactly what
     * a32oracle.c's mem_seed writes. */
    for (uint32_t k = 0; k < A32_MEM_SIZE / 4u; k++)
        wr32(c, SCRATCH + k * 4u, 0x10000000u + k);

    /* Initial architectural state (see a32_interp.h). */
    memset(c->r, 0, sizeof(c->r));
    for (int i = 0; i < 16; i++)
        if (i != 9 && i < 12)
            c->r[i] = in->regs[i];
    c->r[9] = SCRATCH;
    c->r[15] = CODE_BASE;
    c->cpsr = CPSR_INIT;

    uint32_t pc = CODE_BASE;
    while (!c->tfault) {
        if (c->steps++ >= STEP_LIMIT) {
            if (err) snprintf(err, errn, "case %u: step limit (%u) exceeded at pc=0x%08x",
                              c->case_id, STEP_LIMIT, pc);
            failed = 1;
            out->stop_reason = A32_STOP_LIMIT;
            break;
        }
        if (pc == c->code_end) { out->stop_reason = A32_STOP_RETURN; break; }
        if (pc < CODE_BASE || pc >= c->code_end) {
            a32_fault(c, pc, 0, "branch left the fragment");
            break;
        }
        if (pc & 3u) { a32_fault(c, pc, 0, "pc is not word-aligned"); break; }

        uint32_t insn = rd32(c, pc);
        uint32_t next = pc + 4u;
        uint32_t cond = insn >> 28;

        /* ---- unconditional space (cond == 0xF) --------------------------- */
        if (cond == 0xF) {
            if ((insn & 0xFE000000u) == 0xFA000000u) {   /* BLX immediate */
                uint32_t off = ((insn & 0x01000000u) >> 24) << 24 | (insn & 0x00FFFFFFu);
                c->r[14] = pc + 4u;
                pc = pc + 8u + sx(off << 2, 26);
                a32_fault(c, pc, insn,
                          "BLX immediate enters Thumb state, which is not implemented "
                          "(A32-only interpreter)");
                break;
            }
            a32_fault(c, pc, insn, "unimplemented unconditional instruction");
            break;
        }

        if (!cond_true(cond, c->cpsr)) { pc = next; continue; }

        uint32_t g = (insn >> 25) & 7u;

        /* ================= data-processing register space (g == 0) ======== */
        if (g == 0) {
            uint32_t n74 = (insn >> 4) & 0xFu;

            if (n74 == 0x9) {                            /* multiply / swap */
                uint32_t top = (insn >> 23) & 0x1Fu;
                if (top == 0x01u) {                      /* long multiply */
                    int S = (insn >> 20) & 1, U = (insn >> 22) & 1, A = (insn >> 21) & 1;
                    int rdh = (insn >> 16) & 0xF, rdl = (insn >> 12) & 0xF;
                    int rs = (insn >> 8) & 0xF, rm = insn & 0xF;
                    uint32_t a = get_r(c, rm, pc), b = get_r(c, rs, pc);
                    uint64_t acc = ((uint64_t)c->r[rdh] << 32) | c->r[rdl];
                    uint64_t res;
                    if (U) {
                        int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
                        res = A ? (uint64_t)((int64_t)acc + p) : (uint64_t)p;
                    } else {
                        uint64_t p = (uint64_t)a * (uint64_t)b;
                        res = A ? acc + p : p;
                    }
                    c->r[rdl] = (uint32_t)res;
                    c->r[rdh] = (uint32_t)(res >> 32);
                    if (S) c->cpsr = (c->cpsr & ~(F_N | F_Z)) |
                                     ((res >> 63) ? F_N : 0u) |
                                     ((((uint32_t)(res >> 32) | (uint32_t)res) == 0) ? F_Z : 0u);
                    pc = next; continue;
                }
                if (top == 0x02u && ((insn >> 8) & 0xFu) == 0) {   /* SWP / SWPB */
                    int rm = insn & 0xF, rd = (insn >> 12) & 0xF, rn = (insn >> 16) & 0xF;
                    int B = (insn >> 22) & 1;
                    uint32_t addr = get_r(c, rn, pc);
                    uint32_t v = get_r(c, rm, pc);
                    uint32_t sz = B ? 1u : 4u;
                    if (!in_arena(addr, sz)) { a32_fault(c, pc, insn, "SWP outside the arena"); break; }
                    uint32_t old = B ? rd8(c, addr) : rd32(c, addr);
                    if (B) wr8(c, addr, v); else wr32(c, addr, v);
                    c->r[rd] = old;
                    pc = next; continue;
                }
                {                                        /* MUL / MLA / MLS */
                    int S = (insn >> 20) & 1, A = (insn >> 21) & 1;
                    int rd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF;
                    int rs = (insn >> 8) & 0xF, rm = insn & 0xF;
                    uint32_t a = get_r(c, rm, pc), b = get_r(c, rs, pc);
                    uint32_t res;
                    if ((insn >> 22) & 1) res = get_r(c, rn, pc) - (a * b);   /* MLS */
                    else if (A)           res = (a * b) + get_r(c, rn, pc);   /* MLA */
                    else                  res = a * b;                        /* MUL */
                    c->r[rd] = res;
                    if (S) c->cpsr = (c->cpsr & ~(F_N | F_Z)) |
                                     ((res >> 31) ? F_N : 0u) |
                                     (res == 0 ? F_Z : 0u);
                    pc = next; continue;
                }
            }

            if (n74 == 0xB || n74 == 0xD || n74 == 0xF) {
                if (decode_extra_ldst(c, pc, insn)) break;
                pc = next; continue;
            }

            if ((insn & 0x0FFF0FF0u) == 0x016F0F10u) {   /* CLZ */
                uint32_t v = get_r(c, (int)(insn & 0xF), pc);
                uint32_t n = 0;
                while (n < 32 && !((v >> (31 - n)) & 1u)) n++;
                c->r[(insn >> 12) & 0xF] = n;
                pc = next; continue;
            }

            if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) {   /* BX */
                uint32_t v = get_r(c, (int)(insn & 0xF), pc);
                if (v & 1u) {
                    a32_fault(c, pc, insn, "BX to a Thumb address (Thumb is not implemented)");
                    break;
                }
                pc = v & ~1u; continue;
            }
            if ((insn & 0x0FFFFFF0u) == 0x012FFF30u) {   /* BLX (register) */
                uint32_t v = get_r(c, (int)(insn & 0xF), pc);
                c->r[14] = pc + 4u;
                if (v & 1u) {
                    a32_fault(c, pc, insn, "BLX register to a Thumb address (Thumb is not implemented)");
                    break;
                }
                pc = v & ~1u; continue;
            }

            /* MRS has bits[19:16]==1111 and bits[21:20]==00; MSR (register) has
             * bits[15:12]==1111 and bits[21:20]==10. Both share
             * bits[27:23]==00010, bits[11:4]==0 and bit20==0, and every
             * data-processing opcode with bits[27:23]==00010 (TST/TEQ/CMP/CMN)
             * has S=1 (bit20) in any valid encoding, so bit20==0 identifies the
             * PSR pair and nothing else. */
            if (((insn >> 23) & 0x1Fu) == 0x02u &&
                (insn & 0x0FF0u) == 0u &&
                ((insn >> 20) & 1u) == 0u) {         /* MRS / MSR (register) */
                uint32_t f2120 = (insn >> 20) & 3u;
                if (f2120 == 0u) {
                    if ((insn >> 22) & 1) {
                        a32_fault(c, pc, insn, "MRS of SPSR not implemented (user mode)");
                        break;
                    }
                    c->r[(insn >> 12) & 0xF] = c->cpsr;
                    pc = next; continue;
                }
                if (f2120 == 2u) {
                    uint32_t field = (insn >> 16) & 0xFu;
                    if (field & ~0x8u) {
                        a32_fault(c, pc, insn, "MSR field other than _f not implemented in user mode");
                        break;
                    }
                    c->cpsr = (c->cpsr & 0x0FFFFFFFu) |
                              (get_r(c, (int)(insn & 0xF), pc) & 0xF0000000u);
                    pc = next; continue;
                }
                a32_fault(c, pc, insn, "unimplemented PSR-transfer encoding");
                break;
            }

            {   /* data-processing, register form */
                int opcode = (insn >> 21) & 0xFu;
                int S = (insn >> 20) & 1;
                int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
                int rm = insn & 0xF, type = (insn >> 5) & 3u;
                int cout = (c->cpsr & F_C) ? 1 : 0;
                uint32_t op2;
                if (insn & 0x10u) {                       /* register-shifted */
                    uint32_t amt = get_r(c, (insn >> 8) & 0xF, pc) & 0xFFu;
                    op2 = do_shift(get_r(c, rm, pc), type, amt, &cout);
                } else {
                    uint32_t amt = (insn >> 7) & 0x1Fu;
                    if (type == 3 && amt == 0) {          /* RRX */
                        uint32_t v = get_r(c, rm, pc);
                        op2 = (uint32_t)(((c->cpsr & F_C) ? 1u : 0u) << 31) | (v >> 1);
                        cout = (int)(v & 1u);
                    } else {
                        if ((type == 1 || type == 2) && amt == 0) amt = 32;
                        op2 = do_shift(get_r(c, rm, pc), type, amt, &cout);
                    }
                }
                if (dp_execute(c, pc, opcode, S, rn, rd, op2, cout)) break;
                pc = next; continue;
            }
        }

        /* ================= data-processing immediate (g == 1) ============= */
        if (g == 1) {
            /* MOVW/MOVT/MSR-immediate share bits[27:24]==0011 with the
             * DP-immediate opcodes 8-15, but the compares (8-11) always have
             * S=1 (bit20) and ORR/MOV/BIC/MVN (12-15) always have bit23=1, so
             * (bit24=1, bit23=0, bit20=0) identifies exactly this group and
             * nothing else. */
            if (((insn >> 24) & 1u) && (((insn >> 23) & 1u) == 0u) &&
                (((insn >> 20) & 1u) == 0u)) {
                uint32_t op20 = (insn >> 20) & 0xFu;      /* bits[23:20] */
                int rd = (insn >> 12) & 0xF;
                uint32_t imm16 = (((insn >> 16) & 0xFu) << 12) | (insn & 0xFFFu);
                if (op20 == 0x0u) {                       /* MOVW */
                    c->r[rd] = imm16;
                    pc = next; continue;
                }
                if (op20 == 0x4u) {                       /* MOVT */
                    c->r[rd] = (c->r[rd] & 0xFFFFu) | (imm16 << 16);
                    pc = next; continue;
                }
                if ((op20 & 0xBu) == 0x2u) {              /* MSR immediate */
                    uint32_t field = (insn >> 16) & 0xFu;
                    uint32_t rot = (insn >> 8) & 0xFu;
                    uint32_t imm = ror8(insn & 0xFFu, rot);
                    if (field & ~0x8u) {
                        a32_fault(c, pc, insn, "MSR immediate field other than _f not implemented");
                        break;
                    }
                    c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (imm & 0xF0000000u);
                    pc = next; continue;
                }
                a32_fault(c, pc, insn, "unimplemented immediate-form instruction");
                break;
            }
            {   /* data-processing immediate, rotated 8-bit constant */
                int opcode = (insn >> 21) & 0xFu;
                int S = (insn >> 20) & 1;
                int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
                uint32_t rot = (insn >> 8) & 0xFu;
                uint32_t imm8 = insn & 0xFFu;
                uint32_t op2 = ror8(imm8, rot);
                int cout = (c->cpsr & F_C) ? 1 : 0;
                if (rot != 0) cout = (int)((imm8 >> (2 * rot - 1)) & 1u);
                if (dp_execute(c, pc, opcode, S, rn, rd, op2, cout)) break;
                pc = next; continue;
            }
        }

        /* ================= load/store word and byte (g == 2/3) ============ */
        if (g == 2 || g == 3) {
            if (decode_ldst_word(c, pc, insn, g == 3)) break;
            pc = next; continue;
        }

        /* ================= block transfer (g == 4) ======================== */
        if (g == 4) {
            if (decode_block(c, pc, insn)) break;
            pc = next; continue;
        }

        /* ================= branch (g == 5) ================================ */
        if (g == 5) {
            int L = (insn >> 24) & 1;
            uint32_t off = sx((insn & 0x00FFFFFFu) << 2, 26);
            if (L) c->r[14] = pc + 4u;
            pc = pc + 8u + off;
            continue;
        }

        a32_fault(c, pc, insn, "unimplemented instruction");
        break;
    }

    if (c->tfault) { out->stop_reason = A32_STOP_FAULT; failed = 1; }

    for (int i = 0; i < 16; i++) {
        if (i == 9)       out->regs[i] = SCRATCH;
        else if (i >= 12) out->regs[i] = 0;
        else              out->regs[i] = c->r[i];
    }
    out->cpsr = c->cpsr;
    out->fpscr = 0;
    out->mem_size = A32_MEM_SIZE;
    out->mem_hash = fnv1a(mem + (SCRATCH - ARENA_BASE), A32_MEM_SIZE);
    out->mem_first_diff = 0;
    for (int i = 0; i < A32_NFP; i++) out->fp[i] = 0x3f800000ull;

    free(mem);
    return failed;
}
