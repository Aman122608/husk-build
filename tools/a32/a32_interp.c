/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32_interp.c -- the AArch32 (ARM + Thumb) integer reference interpreter.
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
#define F_T 0x00000020u          /* Thumb state */
/* What a32_case_N's wrapper leaves: N=0 Z=1 C=1 V=0, user mode (0x10). */
#define CPSR_INIT (F_Z | F_C | 0x10u)

struct cpu {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t itstate;           /* Thumb IT block state (cond:mask), 0 = none */
    uint32_t s[32];             /* VFP single-precision view of s0..s31 (d_n = s[2n]:s[2n+1]) */
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
/* 64-bit little-endian access, for the VFP doubleword forms (d_n = s[2n]:s[2n+1]). */
static uint64_t rd64(struct cpu *c, uint32_t a) {
    return (uint64_t)rd32(c, a) | ((uint64_t)rd32(c, a + 4u) << 32);
}
static void wr64(struct cpu *c, uint32_t a, uint64_t v) {
    wr32(c, a, (uint32_t)v);
    wr32(c, a + 4u, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------ helpers */
static uint32_t get_r(struct cpu *c, int n, uint32_t pc)
{
    return (n == 15) ? (pc + 8u) : c->r[n];   /* reading PC yields pc+8 in ARM */
}
static uint32_t t_get_r(struct cpu *c, int n, uint32_t pc)
{
    return (n == 15) ? (pc + 4u) : c->r[n];   /* reading PC yields pc+4 in Thumb */
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
static uint32_t align4(uint32_t x) { return x & ~3u; }

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

/* ==========================================================================
 *                             VFP load / store
 * ==========================================================================
 * The reference interpreter carries no floating-point arithmetic, but a real
 * game executes the VFP load/store forms from its very first prologue:
 * VLDR/VSTR (single and double), VLDM/VSTM (all four addressing modes) and the
 * VPUSH/VPOP aliases. Those are pure register-file <-> memory moves, so they can
 * be executed exactly without a floating-point unit -- only the s0..s31 register
 * file is needed, which is already recorded and compared (the harness diffs
 * fp[]). The VFP *arithmetic* (VADD/VMUL/... = the CDP space) stays a named
 * unimplemented fault.
 *
 * The architectural field layout is common to A32 and T32:
 *   P U D W L Rn | Vd 101{s,d} imm8
 * P==1 && W==0 is VLDR/VSTR (imm8 is a word offset); otherwise VLDM/VSTM with
 * imm8 a register count. The register number is built from Vd and D:
 * a single is S(Vd<<1|D), a double is D(D<<4|Vd). For a doubleword multiple the
 * count is imm8/2 (imm8 counts 32-bit words). VPUSH is STMDB sp! and VPOP is
 * LDMIA sp!; both are just the P/U/W aliases of VLDM/VSTM.
 */
static int vfp_transfer(struct cpu *c, uint32_t pc, uint32_t word,
                        int P, int U, int D, int W, int L, int Rn,
                        int Vd, int dbl, uint32_t imm8, int thumb)
{
    uint32_t base = (Rn == 15)
                    ? (thumb ? (pc + 4u) : (pc + 8u))
                    : c->r[Rn];

    if (P && !W) {                                  /* VLDR / VSTR */
        uint32_t off = imm8 << 2;
        uint32_t addr = U ? base + off : base - off;
        uint32_t size = dbl ? 8u : 4u;
        int first = dbl ? ((D << 4) | Vd) : ((Vd << 1) | D);
        if (!in_arena(addr, size)) { a32_fault(c, pc, word, "VFP load/store outside the arena"); return 1; }
        if (L) {
            if (dbl) { uint64_t v = rd64(c, addr);
                       c->s[first * 2] = (uint32_t)v; c->s[first * 2 + 1] = (uint32_t)(v >> 32); }
            else       c->s[first] = rd32(c, addr);
        } else {
            if (dbl) wr64(c, addr, ((uint64_t)c->s[first * 2 + 1] << 32) | c->s[first * 2]);
            else     wr32(c, addr, c->s[first]);
        }
        return 0;
    }

    /* VLDM / VSTM (and the VPUSH/VPOP aliases) */
    if (imm8 == 0u) {
        a32_fault(c, pc, word, "VFP VLDM/VSTM with an empty register list not implemented");
        return 1;
    }
    if (dbl && (imm8 & 1u)) {
        a32_fault(c, pc, word, "VFP VLDM/VSTM with an odd doubleword count");
        return 1;
    }
    int count = dbl ? (int)(imm8 >> 1) : (int)imm8;
    if (count < 1 || (dbl && count > 16) || (!dbl && count > 32)) {
        a32_fault(c, pc, word, "VFP VLDM/VSTM with an out-of-range register count");
        return 1;
    }
    int first = dbl ? ((D << 4) | Vd) : ((Vd << 1) | D);
    uint32_t esz = dbl ? 8u : 4u;
    uint32_t lo;
    if (P == 0 && U == 1)      lo = base;                              /* IA */
    else if (P == 1 && U == 1) lo = base + esz;                        /* IB */
    else if (P == 0 && U == 0) lo = base - (uint32_t)(count - 1) * esz;/* DA */
    else                       lo = base - (uint32_t)count * esz;      /* DB */

    if (!in_arena(lo, (uint32_t)count * esz)) {
        a32_fault(c, pc, word, "VFP VLDM/VSTM outside the arena");
        return 1;
    }
    uint32_t a = lo;
    for (int k = 0; k < count; k++) {
        int r = first + k;
        if (dbl) {
            if (L) { uint64_t v = rd64(c, a);
                     c->s[r * 2] = (uint32_t)v; c->s[r * 2 + 1] = (uint32_t)(v >> 32); }
            else     wr64(c, a, ((uint64_t)c->s[r * 2 + 1] << 32) | c->s[r * 2]);
        } else {
            if (L) c->s[r] = rd32(c, a);
            else   wr32(c, a, c->s[r]);
        }
        a += esz;
    }
    if (W) c->r[Rn] = U ? base + (uint32_t)count * esz
                        : base - (uint32_t)count * esz;
    return 0;
}

/* A32 coprocessor space (bits[27:25] == 110). Only the VFP load/store
 * encodings (coprocessor 1010/1011) are executed; every other coprocessor --
 * and the legacy FPA/CP10 forms the corpus still shows -- is a named fault. */
static int a32_coproc(struct cpu *c, uint32_t pc, uint32_t insn)
{
    uint32_t coproc = (insn >> 8) & 0xFu;
    if (coproc == 0xAu || coproc == 0xBu) {
        int P = (insn >> 24) & 1, U = (insn >> 23) & 1, D = (insn >> 22) & 1;
        int W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        int Rn = (insn >> 16) & 0xF, Vd = (insn >> 12) & 0xF;
        return vfp_transfer(c, pc, insn, P, U, D, W, L, Rn, Vd,
                            coproc == 0xBu, insn & 0xFFu, 0);
    }
    a32_fault(c, pc, insn, "coprocessor load/store not implemented");
    return 1;
}

/* ==========================================================================
 *                                  THUMB (T32)
 * ========================================================================== */

/* ThumbExpandImm (ARM ARM A7.4.3): the modified immediate of a 32-bit Thumb
 * data-processing instruction, imm12 = i:imm3:imm8. */
static uint32_t thumb_expand_imm(uint32_t imm12)
{
    if ((imm12 & 0xC00u) == 0) {
        uint32_t b = imm12 & 0xFFu;
        switch ((imm12 >> 8) & 3u) {
        case 0: return b;
        case 1: if (b == 0) return b; return b | (b << 16);
        case 2: if (b == 0) return b; return (b << 24) | (b << 8);
        default: return b * 0x01010101u;
        }
    }
    { uint32_t v = 0x80u | (imm12 & 0x7Fu);
      uint32_t rot = (imm12 >> 7) & 0x1Fu;
      if (rot == 0) return v;
      return (v >> rot) | (v << (32 - rot)); }
}

static void flags_nz(struct cpu *c, uint32_t res)
{
    c->cpsr &= ~(F_N | F_Z);
    if (res & F_N) c->cpsr |= F_N;
    if (res == 0)  c->cpsr |= F_Z;
}
static void flags_nzc(struct cpu *c, uint32_t res, int cout)
{
    c->cpsr &= ~(F_N | F_Z | F_C);
    if (res & F_N) c->cpsr |= F_N;
    if (res == 0)  c->cpsr |= F_Z;
    if (cout)      c->cpsr |= F_C;
}
static void flags_add(struct cpu *c, uint32_t a, uint32_t b, uint32_t res, int cin)
{
    uint64_t t = (uint64_t)a + b + (uint32_t)cin;
    c->cpsr &= ~(F_N | F_Z | F_C | F_V);
    if (res & F_N) c->cpsr |= F_N;
    if (res == 0)  c->cpsr |= F_Z;
    if (t >> 32)   c->cpsr |= F_C;
    if ((~(a ^ b) & (a ^ res)) >> 31) c->cpsr |= F_V;
}
static void flags_sub(struct cpu *c, uint32_t a, uint32_t b, uint32_t res, int cin)
{
    /* res == a - b - (1 - cin) */
    uint64_t sub = (uint64_t)b + (uint32_t)(1 - cin);
    c->cpsr &= ~(F_N | F_Z | F_C | F_V);
    if (res & F_N) c->cpsr |= F_N;
    if (res == 0)  c->cpsr |= F_Z;
    if ((uint64_t)a >= sub) c->cpsr |= F_C;
    if (((a ^ b) & (a ^ res)) >> 31) c->cpsr |= F_V;
}

/* The 16-bit and 32-bit Thumb executes (defined below). */
static int thumb_exec16(struct cpu *c, uint32_t pc, uint32_t h, uint32_t *next);
static int thumb_exec32(struct cpu *c, uint32_t pc, uint32_t h1, uint32_t h2,
                        uint32_t ins, uint32_t *next);

/* One Thumb instruction: handles the IT block (skip/execute + advance). */
static uint32_t thumb_step(struct cpu *c, uint32_t pc, int *fault)
{
    uint32_t h1 = rd16(c, pc);
    int is32 = (h1 & 0xF800u) >= 0xE800u;    /* 11101 / 11110 / 11111 */
    uint32_t len = is32 ? 4u : 2u;
    uint32_t next = pc + len;

    if (is32 && !in_arena(pc, 4)) { a32_fault(c, pc, h1, "Thumb-2 fetch outside the arena"); *fault = 1; return 0; }

    *fault = 0;

    /* IT / hint: only a 16-bit opcode. */
    if (!is32 && (h1 & 0xFF00u) == 0xBF00u) {
        uint32_t mask = h1 & 0xFu, firstcond = (h1 >> 4) & 0xFu;
        if (mask == 0u) {
            if (c->itstate) { a32_fault(c, pc, h1, "Thumb hint inside an IT block (UNPREDICTABLE)"); *fault = 1; return 0; }
            return pc + 2u;             /* HINT: no architectural effect */
        }
        if (c->itstate) { a32_fault(c, pc, h1, "nested Thumb IT block (UNPREDICTABLE)"); *fault = 1; return 0; }
        if (firstcond == 0xFu || (firstcond == 0xEu && mask != 0u)) {
            a32_fault(c, pc, h1, "Thumb IT with an invalid condition"); *fault = 1; return 0;
        }
        c->itstate = (firstcond << 4) | mask;
        return pc + 2u;
    }

    /* Conditional execution inside an IT block. */
    if (c->itstate) {
        uint32_t cond = c->itstate >> 4;
        if (cond_true(cond, c->cpsr)) {
            int f = 0;
            uint32_t nx = next;
            /* In an IT block the APSR flag update is suppressed for the
             * instruction (this matches the oracle: qemu's QEMU-TCG suppresses
             * the flag write for a conditional instruction inside an IT block).
             * The flags are saved here and restored after execution. */
            uint32_t saved = c->cpsr & (F_N | F_Z | F_C | F_V | F_Q);
            if (is32) {
                uint32_t h2 = rd16(c, pc + 2);
                f = thumb_exec32(c, pc, h1, h2, ((uint32_t)h1 << 16) | h2, &nx);
            } else {
                f = thumb_exec16(c, pc, h1, &nx);
            }
            if (f) { *fault = 1; return 0; }
            c->cpsr = (c->cpsr & ~(F_N | F_Z | F_C | F_V | F_Q)) | saved;
            next = nx;
        }
        if ((c->itstate & 7u) == 0) c->itstate = 0;
        else c->itstate = (c->itstate & 0xE0u) | ((c->itstate & 0x0Fu) << 1);
        return next;
    }

    if (is32) {
        uint32_t h2 = rd16(c, pc + 2);
        if (thumb_exec32(c, pc, h1, h2, ((uint32_t)h1 << 16) | h2, &next)) { *fault = 1; return 0; }
    } else {
        if (thumb_exec16(c, pc, h1, &next)) { *fault = 1; return 0; }
    }
    return next;
}

/* ---- 16-bit Thumb forms ------------------------------------------------ */
/* One 16-bit Thumb load/store. opc: 0 STR,1 STRH,2 STRB,3 LDRSB,4 LDR,
 * 5 LDRH,6 LDRB,7 LDRSH -- the format-7 register-offset opcode order, which a
 * format-8/9/10 immediate form maps into. addr is the fully formed address. */
static int t_mem(struct cpu *c, uint32_t pc, int opc, uint32_t addr, int rt)
{
    if (rt == 15) { a32_fault(c, pc, 0, "Thumb load/store to PC (Rt=15) not implemented"); return 1; }
    switch (opc) {
    case 0: if (!in_arena(addr, 4)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            wr32(c, addr, c->r[rt]); break;
    case 4: if (!in_arena(addr, 4)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            c->r[rt] = rd32(c, addr); break;
    case 1: if (!in_arena(addr, 2)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            wr16(c, addr, c->r[rt]); break;
    case 5: if (!in_arena(addr, 2)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            c->r[rt] = rd16(c, addr); break;
    case 2: if (!in_arena(addr, 1)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            wr8(c, addr, c->r[rt]); break;
    case 6: if (!in_arena(addr, 1)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            c->r[rt] = rd8(c, addr); break;
    case 3: if (!in_arena(addr, 1)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
            c->r[rt] = sx(rd8(c, addr), 8); break;
    default: if (!in_arena(addr, 2)) { a32_fault(c, pc, 0, "Thumb load/store outside the arena"); return 1; }
             c->r[rt] = sx(rd16(c, addr), 16); break;
    }
    return 0;
}

static int thumb_exec16(struct cpu *c, uint32_t pc, uint32_t h, uint32_t *next)
{
    uint32_t b1513 = h >> 13;

    if (b1513 == 0) {                                  /* 000: format 1/2 */
        uint32_t op = (h >> 11) & 3u;
        if (op != 3u) {                                /* move shifted register */
            uint32_t imm5 = (h >> 6) & 0x1Fu, rm = (h >> 3) & 7u, rd = h & 7u;
            uint32_t v = c->r[rm], res; int cout = (c->cpsr & F_C) ? 1 : 0;
            if (op == 0 && imm5 == 0) { c->r[rd] = v; flags_nz(c, v); return 0; }  /* MOVS */
            { uint32_t amt = imm5; if ((op == 1 || op == 2) && amt == 0) amt = 32;
              res = do_shift(v, (int)op, amt, &cout); }
            c->r[rd] = res; flags_nzc(c, res, cout);
            return 0;
        }
        { uint32_t subop = (h >> 9) & 3u, rn = (h >> 3) & 7u, rd = h & 7u;
          uint32_t b = (subop & 2u) ? ((h >> 6) & 7u) : c->r[(h >> 6) & 7u];
          uint32_t a = c->r[rn], res;
          if ((subop & 1u) == 0u) { res = a + b; c->r[rd] = res; flags_add(c, a, b, res, 0); }
          else                    { res = a - b; c->r[rd] = res; flags_sub(c, a, b, res, 1); } }
        return 0;
    }

    if (b1513 == 1) {                                  /* 001: format 3 imm8 */
        uint32_t op = (h >> 11) & 3u, rd = (h >> 8) & 7u, imm = h & 0xFFu;
        if (op == 0) { c->r[rd] = imm; flags_nz(c, imm); }
        else if (op == 1) { uint32_t a = c->r[rd]; flags_sub(c, a, imm, a - imm, 1); }
        else if (op == 2) { uint32_t a = c->r[rd], res = a + imm; c->r[rd] = res; flags_add(c, a, imm, res, 0); }
        else              { uint32_t a = c->r[rd], res = a - imm; c->r[rd] = res; flags_sub(c, a, imm, res, 1); }
        return 0;
    }

    if ((h & 0xFC00u) == 0x4000u) {                    /* 010000: format 4 ALU */
        uint32_t op = (h >> 6) & 0xFu, rm = (h >> 3) & 7u, rd = h & 7u;
        uint32_t a = c->r[rd], b = c->r[rm], res;
        int cout = (c->cpsr & F_C) ? 1 : 0;
        switch (op) {
        case 0x0: res = a & b; c->r[rd] = res; flags_nzc(c, res, cout); break;
        case 0x1: res = a ^ b; c->r[rd] = res; flags_nzc(c, res, cout); break;
        case 0x2: case 0x3: case 0x4: {
            uint32_t amt = b & 0xFFu; int chg = 0;
            if (amt == 0) { res = a; chg = 0; }
            else { res = do_shift(a, (int)(op - 2), amt, &cout); chg = 1; }
            c->r[rd] = res; if (chg) flags_nzc(c, res, cout); else flags_nz(c, res);
            break; }
        case 0x5: { uint32_t ci = (c->cpsr & F_C) ? 1u : 0u; res = a + b + ci;
                    c->r[rd] = res; flags_add(c, a, b, res, (int)ci); } break;
        case 0x6: { uint32_t ci = (c->cpsr & F_C) ? 1u : 0u; res = a - b - (1u - ci);
                    c->r[rd] = res; flags_sub(c, a, b, res, (int)ci); } break;
        case 0x7: { uint32_t amt = b & 0xFFu;
                    if (amt == 0) { res = a; c->r[rd] = res; flags_nz(c, res); }
                    else { res = do_shift(a, 3, amt, &cout); c->r[rd] = res; flags_nzc(c, res, cout); }
                  } break;
        case 0x8: res = a & b; flags_nzc(c, res, cout); break;
        case 0x9: res = 0u - b; c->r[rd] = res; flags_sub(c, 0u, b, res, 1); break;
        case 0xA: flags_sub(c, a, b, a - b, 1); break;
        case 0xB: { uint32_t r2 = a + b; flags_add(c, a, b, r2, 0); } break;
        case 0xC: res = a | b; c->r[rd] = res; flags_nzc(c, res, cout); break;
        case 0xD: res = a * b; c->r[rd] = res; flags_nz(c, res); break;
        case 0xE: res = a & ~b; c->r[rd] = res; flags_nzc(c, res, cout); break;
        default:  res = ~b; c->r[rd] = res; flags_nzc(c, res, cout); break;
        }
        return 0;
    }

    if ((h & 0xFC00u) == 0x4400u) {                    /* 010001: format 5 hi reg/BX */
        uint32_t op = (h >> 8) & 3u;
        uint32_t rm = ((h >> 3) & 7u) | (((h >> 6) & 1u) << 3);
        uint32_t rd = (h & 7u) | (((h >> 7) & 1u) << 3);
        if (op == 0) { c->r[rd] = t_get_r(c, (int)rd, pc) + t_get_r(c, (int)rm, pc); return 0; }
        if (op == 1) { uint32_t a = t_get_r(c, (int)rd, pc), b = t_get_r(c, (int)rm, pc);
                       flags_sub(c, a, b, a - b, 1); return 0; }
        if (op == 2) { c->r[rd] = t_get_r(c, (int)rm, pc); return 0; }
        { uint32_t v = t_get_r(c, (int)rm, pc);
          if (!((h >> 7) & 1u)) {                      /* BX */
              c->cpsr = (c->cpsr & ~F_T) | ((v & 1u) ? F_T : 0u);
              *next = v & ~1u;
          } else {                                     /* BLX (register) */
              c->r[14] = (pc + 2u) | 1u;
              c->cpsr = (c->cpsr & ~F_T) | ((v & 1u) ? F_T : 0u);
              *next = v & ~1u;
          } }
        return 0;
    }

    if ((h & 0xF800u) == 0x4800u) {                    /* 01001: format 6 LDR literal */
        uint32_t rd = (h >> 8) & 7u, imm8 = h & 0xFFu;
        uint32_t addr = align4(pc + 4u) + (imm8 << 2);
        if (!in_arena(addr, 4)) { a32_fault(c, pc, h, "Thumb LDR literal outside the arena"); return 1; }
        c->r[rd] = rd32(c, addr);
        return 0;
    }

    if ((h & 0xF000u) == 0x5000u) {                    /* 0101: format 7 reg offset */
        uint32_t op = (h >> 9) & 7u, rm = (h >> 6) & 7u, rn = (h >> 3) & 7u, rt = h & 7u;
        return t_mem(c, pc, (int)op, c->r[rn] + c->r[rm], (int)rt);
    }

    if ((h & 0xF000u) == 0x6000u) {                    /* 0110: LDR/STR word imm5 */
        uint32_t l = (h >> 11) & 1u, imm5 = (h >> 6) & 0x1Fu, rn = (h >> 3) & 7u, rt = h & 7u;
        return t_mem(c, pc, l ? 4 : 0, c->r[rn] + (imm5 << 2), (int)rt);
    }

    if ((h & 0xF000u) == 0x7000u) {                    /* 0111: LDRB/STRB imm5 (byte) */
        uint32_t l = (h >> 11) & 1u, imm5 = (h >> 6) & 0x1Fu, rn = (h >> 3) & 7u, rt = h & 7u;
        return t_mem(c, pc, l ? 6 : 2, c->r[rn] + imm5, (int)rt);
    }

    if ((h & 0xF000u) == 0x8000u) {                    /* 1000: LDRH/STRH imm5 (halfword) */
        uint32_t l = (h >> 11) & 1u, imm5 = (h >> 6) & 0x1Fu, rn = (h >> 3) & 7u, rt = h & 7u;
        return t_mem(c, pc, l ? 5 : 1, c->r[rn] + (imm5 << 1), (int)rt);
    }

    if ((h & 0xF000u) == 0x9000u) {                    /* 1001: format 11 SP-relative */
        uint32_t l = (h >> 11) & 1u, rd = (h >> 8) & 7u, imm8 = h & 0xFFu;
        return t_mem(c, pc, l ? 4 : 0, c->r[13] + (imm8 << 2), (int)rd);
    }

    if ((h & 0xF000u) == 0xA000u) {                    /* 1010: format 12 load address */
        uint32_t sp = (h >> 11) & 1u, rd = (h >> 8) & 7u, imm8 = h & 0xFFu;
        uint32_t base = sp ? c->r[13] : align4(pc + 4u);
        c->r[rd] = base + (imm8 << 2);
        return 0;
    }

    if ((h & 0xF000u) == 0xB000u) {                    /* 1011: format 13/14 (and CBZ/CBNZ/BKPT) */
        if ((h & 0xFF00u) == 0xB000u) {                /* ADD/SUB SP, #imm7*4 */
            /* The mask must be 0xFF00, not 0xFF80: SUB SP is 1011 0000 1xxx xxxx
               (0xB080-0xB0FF) and can never equal 0xB000 under a 0xFF80 mask, so
               every Thumb-1 SUB SP went undecoded and faulted -- the single
               most-executed refused encoding in the dynmix run over the real
               corpus games (2466 occurrences, more than everything else on the
               list combined). The sign bit that distinguishes ADD from SUB is
               read on the next line, so one mask serves both. */
            uint32_t sign = (h >> 7) & 1u, imm7 = h & 0x7Fu;
            if (sign) c->r[13] -= (imm7 << 2); else c->r[13] += (imm7 << 2);
            return 0;
        }
        if ((h & 0xFD00u) == 0xB100u || (h & 0xFD00u) == 0xB900u) {   /* CBZ / CBNZ */
            uint32_t nz = ((h >> 11) & 1u);            /* 0 = CBZ, 1 = CBNZ */
            uint32_t i = (h >> 9) & 1u, imm5 = (h >> 3) & 0x1Fu, rn = h & 7u;
            uint32_t off = ((i << 5) | imm5) << 1;
            int zero = (c->r[rn] == 0);
            if ((nz == 0u) == zero) *next = pc + 4u + off;
            return 0;
        }
        if ((h & 0xFE00u) == 0xB400u || (h & 0xFE00u) == 0xBC00u) {  /* PUSH / POP */
            uint32_t is_pop = (h >> 11) & 1u, m = (h >> 8) & 1u, list = h & 0xFFu;
            int n = popcount16(list) + (m ? 1 : 0);
            uint32_t sp = c->r[13];
            if (sp & 3u) { a32_fault(c, pc, h, "Thumb PUSH/POP with an unaligned SP"); return 1; }
            if (n == 0) { a32_fault(c, pc, h, "Thumb PUSH/POP with an empty list"); return 1; }
            if (!in_arena(is_pop ? sp : sp - (uint32_t)n * 4u, (uint32_t)n * 4u)) {
                a32_fault(c, pc, h, "Thumb PUSH/POP outside the arena"); return 1;
            }
            if (!is_pop) {
                uint32_t a = sp - (uint32_t)n * 4u;
                for (int r = 0; r < 8; r++) if (list & (1u << r)) { wr32(c, a, c->r[r]); a += 4; }
                if (m) wr32(c, a, c->r[14]);
                c->r[13] = sp - (uint32_t)n * 4u;
            } else {
                uint32_t a = sp;
                for (int r = 0; r < 8; r++) if (list & (1u << r)) { c->r[r] = rd32(c, a); a += 4; }
                if (m) { *next = rd32(c, a) & ~1u; }
                c->r[13] = sp + (uint32_t)n * 4u;
            }
            return 0;
        }
        /* 1011 0010 01xx: SXTH/SXTB/UXTH/UXTB. */
        if ((h & 0xFF00u) == 0xB200u) {
            uint32_t op = (h >> 6) & 3u, rm = (h >> 3) & 7u, rd = h & 7u;
            if (op == 0u)      c->r[rd] = sx(c->r[rm] & 0xFFFFu, 16);   /* SXTH */
            else if (op == 1u) c->r[rd] = sx(c->r[rm] & 0xFFu, 8);      /* SXTB */
            else if (op == 2u) c->r[rd] = c->r[rm] & 0xFFFFu;           /* UXTH */
            else               c->r[rd] = c->r[rm] & 0xFFu;             /* UXTB */
            return 0;
        }
        /* 1011 1010 00xx: REV/REV16/REVSH. */
        if ((h & 0xFF00u) == 0xBA00u) {
            uint32_t op = (h >> 6) & 3u, rm = (h >> 3) & 7u, rd = h & 7u;
            uint32_t v = c->r[rm];
            if (op == 0u) {                                             /* REV */
                c->r[rd] = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
                           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
                return 0;
            }
            if (op == 1u) {                                             /* REV16 */
                c->r[rd] = ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu) |
                           ((v & 0xFF0000u) << 8) | ((v >> 8) & 0xFF0000u);
                return 0;
            }
            if (op == 3u) {                                             /* REVSH */
                c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
                return 0;
            }
            a32_fault(c, pc, h, "Thumb REVSH encoding 1011 1010 10xx is undefined");
            return 1;
        }
        if ((h & 0xFF00u) == 0xBE00u) { a32_fault(c, pc, h, "Thumb BKPT not implemented"); return 1; }
        a32_fault(c, pc, h, "unimplemented Thumb 1011 encoding"); return 1;
    }

    if ((h & 0xF000u) == 0xC000u) {                    /* 1100: format 15 LDMIA/STMIA */
        uint32_t l = (h >> 11) & 1u, rn = (h >> 8) & 7u, list = h & 0xFFu;
        int n = popcount16(list);
        if (list == 0) { a32_fault(c, pc, h, "Thumb LDM/STM with an empty register list"); return 1; }
        if (list & (1u << (int)rn)) {
            a32_fault(c, pc, h, "Thumb LDM/STM with the base in the list (UNPREDICTABLE)"); return 1;
        }
        uint32_t base = c->r[rn];
        if (!in_arena(base, (uint32_t)n * 4u)) { a32_fault(c, pc, h, "Thumb LDM/STM outside the arena"); return 1; }
        uint32_t a = base;
        for (int r = 0; r < 8; r++) {
            if (!(list & (1u << r))) continue;
            if (l) c->r[r] = rd32(c, a); else wr32(c, a, c->r[r]);
            a += 4;
        }
        c->r[rn] = base + (uint32_t)n * 4u;
        return 0;
    }

    if ((h & 0xF000u) == 0xD000u) {                    /* 1101: format 16 cond branch */
        uint32_t cond = (h >> 8) & 0xFu;
        if (cond == 0xEu) { a32_fault(c, pc, h, "Thumb UDF (permanently undefined) executed"); return 1; }
        if (cond == 0xFu) { a32_fault(c, pc, h, "Thumb SVC not implemented (user mode)"); return 1; }
        { uint32_t off = sx(h & 0xFFu, 8) << 1;
          if (cond_true(cond, c->cpsr)) *next = pc + 4u + off; }
        return 0;
    }

    if ((h & 0xF800u) == 0xE000u) {                    /* 11100: format 18 B */
        uint32_t off = sx(h & 0x7FFu, 11) << 1;
        *next = pc + 4u + off;
        return 0;
    }

    a32_fault(c, pc, h, "unimplemented 16-bit Thumb encoding"); return 1;
}

/* Wide Thumb-2 load/store single (11111 group). Every form real code shows is
 * decoded here: the imm12 form, the imm8 form in all four P/U/W addressing
 * combinations (pre/post-indexed, writeback or not) and the register-offset
 * form, for word, byte and halfword plus the signed byte/half loads. The
 * opcode is h1[11:4] (bits[11:8]=1000 unsigned / 1001 signed, bits[7:4] the
 * size and kind). */
static int thumb_wide_ldst(struct cpu *c, uint32_t pc, uint32_t h1, uint32_t h2)
{
    uint32_t key = (h1 >> 4) & 0xFFu;       /* bits[11:4]: the load/store opcode */
    uint32_t op = key & 0xFu;               /* size / kind */
    uint32_t sn = (key >> 4) & 1u;          /* 1 = the 1111 1001 signed space */
    uint32_t rn = h1 & 0xFu;
    uint32_t rt = (h2 >> 12) & 0xFu;
    uint32_t base = c->r[rn];
    int sz, load, sign = 0, imm12_form;

    if (rt == 15) { a32_fault(c, pc, h1, "Thumb-2 load/store to PC not implemented"); return 1; }

    if (sn) {
        if (op == 0x1u || op == 0x9u) { load = 1; sign = 1; sz = 1; }       /* LDRSB */
        else if (op == 0x3u || op == 0xBu) { load = 1; sign = 1; sz = 2; }  /* LDRSH */
        else { a32_fault(c, pc, h1, "unimplemented Thumb-2 load/store encoding"); return 1; }
        imm12_form = (op == 0x9u || op == 0xBu);
    } else {
        switch (op) {
        case 0x0u: load = 0; sz = 1; break;   /* STRB      */
        case 0x1u: load = 1; sz = 1; break;   /* LDRB      */
        case 0x2u: load = 0; sz = 2; break;   /* STRH      */
        case 0x3u: load = 1; sz = 2; break;   /* LDRH      */
        case 0x4u: load = 0; sz = 4; break;   /* STR       */
        case 0x5u: load = 1; sz = 4; break;   /* LDR       */
        case 0x8u: load = 0; sz = 1; break;   /* STRB imm12 */
        case 0x9u: load = 1; sz = 1; break;   /* LDRB imm12 */
        case 0xAu: load = 0; sz = 2; break;   /* STRH imm12 */
        case 0xBu: load = 1; sz = 2; break;   /* LDRH imm12 */
        case 0xCu: load = 0; sz = 4; break;   /* STR  imm12 */
        case 0xDu: load = 1; sz = 4; break;   /* LDR  imm12 */
        default: a32_fault(c, pc, h1, "unimplemented Thumb-2 load/store encoding"); return 1;
        }
        imm12_form = (op >= 0x8u && op <= 0xDu);
    }

    uint32_t addr, wb_addr = 0;
    int writeback = 0;
    if (imm12_form) {
        addr = base + (h2 & 0xFFFu);
    } else if (h2 & 0x0800u) {                 /* Rt 1 P U W imm8 */
        uint32_t P = (h2 >> 10) & 1u, U = (h2 >> 9) & 1u, W = (h2 >> 8) & 1u;
        uint32_t imm8 = h2 & 0xFFu;
        uint32_t off_addr = U ? base + imm8 : base - imm8;
        addr = P ? off_addr : base;
        if (!P || W) { writeback = 1; wb_addr = off_addr; }
    } else {                                   /* Rt 000000 imm2 Rm */
        if (h2 & 0x0FC0u) { a32_fault(c, pc, h1, "unimplemented Thumb-2 load/store encoding (1101 form)"); return 1; }
        uint32_t rm = h2 & 0xFu, imm2 = (h2 >> 4) & 3u;
        addr = base + (c->r[rm] << imm2);
    }

    if (!in_arena(addr, (uint32_t)sz)) { a32_fault(c, pc, h1, "Thumb-2 load/store outside the arena"); return 1; }
    if (load) {
        if (sz == 4)      c->r[rt] = rd32(c, addr);
        else if (sz == 2) c->r[rt] = sign ? sx(rd16(c, addr), 16) : rd16(c, addr);
        else              c->r[rt] = sign ? sx(rd8(c, addr), 8) : rd8(c, addr);
    } else {
        if (sz == 4)      wr32(c, addr, c->r[rt]);
        else if (sz == 2) wr16(c, addr, c->r[rt]);
        else              wr8(c, addr, c->r[rt]);
    }
    if (writeback) c->r[rn] = wb_addr;
    return 0;
}

/* Wide Thumb-2 multiply (1111 1011 xxxx). The op nibble is h1[7:4]:
 *   0 = MUL / MLA / MLS, 8 = SMULL, A = UMULL, C = SMLAL, E = UMLAL.
 * MUL is the Ra==15 alias of MLA; MLS has h2[7:4]==1. The media variants
 * (SMULBB/SMLAWx/SMMLA, op 1..6) and the dual multiplies stay named faults. */
static int thumb_mulwide(struct cpu *c, uint32_t pc, uint32_t h1, uint32_t h2)
{
    uint32_t op = (h1 >> 4) & 0xFu, rn = h1 & 0xFu;
    uint32_t ra = (h2 >> 12) & 0xFu, rd = (h2 >> 8) & 0xFu;
    uint32_t sub = (h2 >> 4) & 0xFu, rm = h2 & 0xFu;
    uint32_t a = c->r[rn], b = c->r[rm];

    if (op == 0x0u) {
        if (sub == 0u && ra == 15u) { c->r[rd] = a * b; return 0; }       /* MUL */
        if (sub == 0u) { c->r[rd] = c->r[ra] + a * b; return 0; }         /* MLA */
        if (sub == 1u) { c->r[rd] = c->r[ra] - a * b; return 0; }         /* MLS */
        a32_fault(c, pc, h1, "unimplemented Thumb-2 multiply encoding");
        return 1;
    }
    /* Long multiplies: bits[15:12] = RdLo, bits[11:8] = RdHi. */
    if (op == 0x8u || op == 0xAu || op == 0xCu || op == 0xEu) {
        if (sub != 0u) { a32_fault(c, pc, h1, "unimplemented Thumb-2 multiply encoding"); return 1; }
        int uns = (int)((op >> 1) & 1u), acc = (int)((op >> 2) & 1u);
        uint32_t rdlo = ra, rdhi = rd;
        if (uns) {
            uint64_t p = (uint64_t)a * (uint64_t)b;
            uint64_t r = acc ? ((((uint64_t)c->r[rdhi] << 32) | c->r[rdlo]) + p) : p;
            c->r[rdlo] = (uint32_t)r; c->r[rdhi] = (uint32_t)(r >> 32);
        } else {
            int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
            uint64_t r = acc ? (uint64_t)((int64_t)(((uint64_t)c->r[rdhi] << 32) |
                                                    c->r[rdlo]) + p)
                             : (uint64_t)p;
            c->r[rdlo] = (uint32_t)r; c->r[rdhi] = (uint32_t)(r >> 32);
        }
        return 0;
    }
    a32_fault(c, pc, h1, "unimplemented Thumb-2 multiply encoding");
    return 1;
}

/* 32-bit Thumb-2 execute. `ins` = h1:h2 as a 32-bit instruction word. */
static int thumb_exec32(struct cpu *c, uint32_t pc, uint32_t h1, uint32_t h2,
                        uint32_t ins, uint32_t *next)
{
    uint32_t op1 = (h1 >> 11) & 0x1Fu;   /* 11101 / 11110 / 11111 */
    (void)ins;

    /* ---- BL / BLX immediate (11110 ... | 11 ...) -------------------------- */
    if ((h1 & 0xF800u) == 0xF000u && (h2 & 0xC000u) == 0xC000u) {
        uint32_t S  = (h1 >> 10) & 1u;
        uint32_t j1 = (h2 >> 13) & 1u, j2 = (h2 >> 11) & 1u;
        uint32_t i1 = ((j1 ^ S) ^ 1u), i2 = ((j2 ^ S) ^ 1u);
        uint32_t imm = (S << 24) | (i1 << 23) | ((h1 & 0x3FFu) << 13) |
                       (i2 << 12) | (h2 & 0x7FFu);
        int32_t v = (int32_t)(imm << 7) >> 7;        /* sign-extend the 25-bit offset */
        int32_t off = v * 2;
        int is_blx = ((h2 >> 12) & 1u) == 0u;
        if (is_blx) {
            /* BLX (immediate) executed in Thumb state switches to ARM. */
            c->r[14] = (pc + 4u) | 1u;
            c->cpsr &= ~F_T;
            *next = align4(pc + 4u) + (uint32_t)off;
        } else {
            c->r[14] = (pc + 4u) | 1u;
            *next = pc + 4u + (uint32_t)off;
        }
        return 0;
    }

    if (op1 == 0x1Eu) {                          /* 11110 */
        /* MOVW / MOVT */
        if ((h1 & 0xFBF0u) == 0xF240u || (h1 & 0xFBF0u) == 0xF2C0u) {
            uint32_t i = (h1 >> 10) & 1u, imm4 = h1 & 0xFu;
            uint32_t imm3 = (h2 >> 12) & 7u, rd = (h2 >> 8) & 0xFu, imm8 = h2 & 0xFFu;
            uint32_t imm16 = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
            if ((h1 & 0xFBF0u) == 0xF240u) c->r[rd] = imm16;
            else c->r[rd] = (c->r[rd] & 0xFFFFu) | (imm16 << 16);
            return 0;
        }
        /* Misc: MRS / MSR / SSAT / USAT / BFI / BFC / UBFX / SBFX (1111 0011 ..) */
        if ((h1 & 0xFF00u) == 0xF300u) {
            /* DMB / DSB / ISB (1111 0011 1011 1111 | 1000 option 1111): a
             * memory barrier is a no-op for a single sequentially-consistent
             * observer, which is what this interpreter is. */
            if ((h1 & 0xFFF0u) == 0xF3B0u && (h2 & 0xFF00u) == 0x8F00u) return 0;
            if (h1 == 0xF3EFu) {                              /* MRS Rd, APSR/CPSR */
                uint32_t rd = (h2 >> 8) & 0xFu;
                uint32_t sysm = h2 & 0xFFu;
                if (sysm != 0u) { a32_fault(c, pc, h1, "Thumb MRS of a sysm other than APSR/CPSR not implemented"); return 1; }
                c->r[rd] = c->cpsr;
                return 0;
            }
            if ((h1 & 0xFFF0u) == 0xF380u && (h2 & 0xF000u) == 0x8000u) {   /* MSR APSR, Rn */
                uint32_t mask = (h2 >> 8) & 0xFu, rn = h1 & 0xFu;
                if (mask & 0x7u) { a32_fault(c, pc, h1, "Thumb MSR of a field other than flags not implemented"); return 1; }
                c->cpsr = (c->cpsr & ~(F_N | F_Z | F_C | F_V)) | (c->r[rn] & (F_N | F_Z | F_C | F_V));
                return 0;
            }
            if ((h1 & 0xFFF0u) == 0xF300u) {                  /* SSAT */
                uint32_t rn = h1 & 0xFu, rd = (h2 >> 8) & 0xFu;
                uint32_t sat = (h2 & 0x1Fu) + 1u, lsb = (((h2 >> 12) & 7u) << 2) | ((h2 >> 6) & 3u);
                int32_t v = (int32_t)(c->r[rn] << lsb);
                int32_t max = (int32_t)((1u << (sat - 1u)) - 1u), min = -(int32_t)(1u << (sat - 1u));
                int satd = 0;
                if (v > max) { v = max; satd = 1; } else if (v < min) { v = min; satd = 1; }
                c->r[rd] = (uint32_t)v;
                if (satd) c->cpsr |= F_Q;
                return 0;
            }
            if ((h1 & 0xFFF0u) == 0xF380u) {                  /* USAT */
                uint32_t rn = h1 & 0xFu, rd = (h2 >> 8) & 0xFu;
                uint32_t sat = h2 & 0x1Fu, lsb = (((h2 >> 12) & 7u) << 2) | ((h2 >> 6) & 3u);
                int64_t v = (int64_t)(int32_t)(c->r[rn] << lsb);
                int64_t max = (int64_t)((1u << sat) - 1u);
                int satd = 0;
                if (v < 0) { v = 0; satd = 1; } else if (v > max) { v = max; satd = 1; }
                c->r[rd] = (uint32_t)v;
                if (satd) c->cpsr |= F_Q;
                return 0;
            }
            if ((h1 & 0xFFF0u) == 0xF360u) {                  /* BFI / BFC */
                uint32_t rn = h1 & 0xFu, rd = (h2 >> 8) & 0xFu;
                uint32_t lsb = (((h2 >> 12) & 7u) << 2) | ((h2 >> 6) & 3u);
                uint32_t msb = h2 & 0x1Fu, width = msb - lsb + 1u;
                if (width == 0u || width > 32u || msb > 31u) { a32_fault(c, pc, h1, "Thumb-2 BFI/BFC with an invalid field"); return 1; }
                uint32_t mask = (width == 32u) ? 0xFFFFFFFFu : (((1u << width) - 1u) << lsb);
                uint32_t src = (rn == 15u) ? 0u : c->r[rn];
                c->r[rd] = (c->r[rd] & ~mask) | ((src << lsb) & mask);
                return 0;
            }
            if ((h1 & 0xFFF0u) == 0xF3C0u) {                  /* UBFX */
                uint32_t rn = h1 & 0xFu, rd = (h2 >> 8) & 0xFu;
                uint32_t lsb = (((h2 >> 12) & 7u) << 2) | ((h2 >> 6) & 3u);
                uint32_t width = (h2 & 0x1Fu) + 1u;
                if (lsb + width > 32u) { a32_fault(c, pc, h1, "Thumb-2 UBFX with an invalid field"); return 1; }
                uint32_t mask = (width == 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u);
                c->r[rd] = (c->r[rn] >> lsb) & mask;
                return 0;
            }
            if ((h1 & 0xFFF0u) == 0xF340u) {                  /* SBFX */
                uint32_t rn = h1 & 0xFu, rd = (h2 >> 8) & 0xFu;
                uint32_t lsb = (((h2 >> 12) & 7u) << 2) | ((h2 >> 6) & 3u);
                uint32_t width = (h2 & 0x1Fu) + 1u;
                if (lsb + width > 32u) { a32_fault(c, pc, h1, "Thumb-2 SBFX with an invalid field"); return 1; }
                c->r[rd] = sx((c->r[rn] >> lsb) & ((width == 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)), (int)width);
                return 0;
            }
            a32_fault(c, pc, h1, "unimplemented Thumb-2 11110 011 encoding"); return 1;
        }
        /* ADDW / SUBW (plain imm12) */
        if ((h1 & 0xFB00u) == 0xF200u) {
            uint32_t op = (h1 >> 5) & 0xFu, rn = h1 & 0xFu;
            uint32_t i = (h1 >> 10) & 1u, imm3 = (h2 >> 12) & 7u;
            uint32_t rd = (h2 >> 8) & 0xFu, imm8 = h2 & 0xFFu;
            uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
            if (op == 0u) c->r[rd] = c->r[rn] + imm12;
            else if (op == 5u) c->r[rd] = c->r[rn] - imm12;
            else { a32_fault(c, pc, h1, "unimplemented Thumb-2 11110 encoding"); return 1; }
            return 0;
        }
        /* Data-processing (modified immediate): bits[15:11]==11110, bit9==0. */
        if ((h1 & 0xFA00u) == 0xF000u) {
            uint32_t op = (h1 >> 5) & 0xFu, s = (h1 >> 4) & 1u, rn = h1 & 0xFu;
            uint32_t i = (h1 >> 10) & 1u, imm3 = (h2 >> 12) & 7u;
            uint32_t rd = (h2 >> 8) & 0xFu, imm8 = h2 & 0xFFu;
            uint32_t op2 = thumb_expand_imm((i << 11) | (imm3 << 8) | imm8);
            /* map to the A32 4-bit opcode and reuse dp_execute */
            int opc = -1, is_test = 0;
            switch (op) {
            case 0x0: opc = 0x0; break;                 /* AND */
            case 0x1: opc = 0xE; break;                 /* BIC */
            case 0x2:                                   /* ORR, or MOV if Rn==15 */
                opc = (rn == 15u) ? 0xD : 0xC; break;
            case 0x3:                                   /* ORN, or MVN if Rn==15 */
                if (rn == 15u) { opc = 0xF; }
                else { opc = 0xC; op2 = ~op2; }
                break;
            case 0x4: opc = 0x1; break;                 /* EOR */
            case 0x8: opc = 0x4; break;                 /* ADD */
            case 0xA: opc = 0x5; break;                 /* ADC */
            case 0xB: opc = 0x6; break;                 /* SBC */
            case 0xD: opc = 0x2; break;                 /* SUB */
            case 0xE: opc = 0x3; break;                 /* RSB */
            default: a32_fault(c, pc, h1, "unimplemented Thumb-2 modified-immediate opcode"); return 1;
            }

            if (s && rd == 15u) {                       /* TST/TEQ/CMN/CMP */
                is_test = 1;
                switch (op) {
                case 0x0: opc = 0x8; break;             /* TST */
                case 0x4: opc = 0x9; break;             /* TEQ */
                case 0x8: opc = 0xB; break;             /* CMN */
                case 0xD: opc = 0xA; break;             /* CMP */
                default: a32_fault(c, pc, h1, "unimplemented Thumb-2 test encoding"); return 1;
                }
            }
            (void)is_test;
            if (dp_execute(c, pc, opc, (int)s, (int)rn, (int)rd, op2, (c->cpsr & F_C) ? 1 : 0))
                return 1;
            return 0;
        }
        a32_fault(c, pc, h1, "unimplemented Thumb-2 11110 encoding"); return 1;
    }

    if (op1 == 0x1Du) {                          /* 11101: DP register / dual / table / multi */
        if ((h1 & 0xFE00u) == 0xEA00u) {         /* data-processing (register) */
            uint32_t op = (h1 >> 5) & 0xFu, s = (h1 >> 4) & 1u, rn = h1 & 0xFu;
            uint32_t rd = (h2 >> 8) & 0xFu, imm3 = (h2 >> 12) & 7u;
            uint32_t imm2 = (h2 >> 6) & 3u, st = (h2 >> 4) & 3u, rm = h2 & 0xFu;
            uint32_t amt = (imm3 << 2) | imm2;
            int cout = (c->cpsr & F_C) ? 1 : 0;
            uint32_t op2;
            int type = (int)st;
            if ((h2 & 0x8000u) == 0u && (imm3 & 4u) == 0u && st == 0 && amt == 0 && (h1 & 0xFu) != 0xFu && (h2 & 0x0080u)) {
                /* shift-by-register form is not generated; fall through to imm path */
            }
            if (st == 3) {                       /* ROR with amt==0 => RRX */
                if (amt == 0) { uint32_t v = c->r[rm];
                    op2 = (uint32_t)(((c->cpsr & F_C) ? 1u : 0u) << 31) | (v >> 1);
                    cout = (int)(v & 1u);
                } else op2 = do_shift(c->r[rm], 3, amt, &cout);
            } else {
                op2 = do_shift(c->r[rm], type, amt, &cout);
            }
            int opc = -1;
            switch (op) {
            case 0x0: opc = 0x0; break;             /* AND */
            case 0x1: opc = 0xE; break;             /* BIC */
            case 0x2:                                   /* ORR, or MOV if Rn==15 */
                opc = (rn == 15u) ? 0xD : 0xC; break;
            case 0x3:                                   /* ORN, or MVN if Rn==15 */
                if (rn == 15u) { opc = 0xF; }
                else { opc = 0xC; op2 = ~op2; }
                break;
            case 0x4: opc = 0x1; break;             /* EOR */
            case 0x8: opc = 0x4; break;             /* ADD */
            case 0xA: opc = 0x5; break;             /* ADC */
            case 0xB: opc = 0x6; break;             /* SBC */
            case 0xD: opc = 0x2; break;             /* SUB */
            case 0xE: opc = 0x3; break;             /* RSB */
            default: a32_fault(c, pc, h1, "unimplemented Thumb-2 register opcode"); return 1;
            }

            if (s && rd == 15u) {
                switch (op) {
                case 0x0: opc = 0x8; break;
                case 0x4: opc = 0x9; break;
                case 0x8: opc = 0xB; break;
                case 0xD: opc = 0xA; break;
                default: a32_fault(c, pc, h1, "unimplemented Thumb-2 register test"); return 1;
                }
            }
            if (dp_execute(c, pc, opc, (int)s, (int)rn, (int)rd, op2, cout)) return 1;
            return 0;
        }
        /* VFP load/store (1110 110x ... | Vd 101{s,d} imm8). Covers VLDR/VSTR
         * (P==1,W==0) and VLDM/VSTM/VPUSH/VPOP (P/U/W as a block transfer). */
        if ((h1 & 0xFE00u) == 0xEC00u) {
            uint32_t coproc = (h2 >> 8) & 0xFu;
            if (coproc == 0xAu || coproc == 0xBu) {
                int P = (h1 >> 8) & 1, U = (h1 >> 7) & 1, D = (h1 >> 6) & 1;
                int W = (h1 >> 5) & 1, L = (h1 >> 4) & 1;
                int rn = h1 & 0xF, Vd = (h2 >> 12) & 0xF;
                return vfp_transfer(c, pc, ((uint32_t)h1 << 16) | h2,
                                    P, U, D, W, L, rn, Vd, coproc == 0xBu,
                                    h2 & 0xFFu, 1);
            }
            a32_fault(c, pc, h1, "Thumb-2 coprocessor load/store not implemented");
            return 1;
        }
        /* TBB / TBH */
        if ((h1 & 0xFFF0u) == 0xE8D0u && (h2 & 0xFFE0u) == 0xF000u) {
            uint32_t rn = h1 & 0xFu, hw = (h2 >> 4) & 1u, rm = h2 & 0xFu;
            uint32_t base = c->r[rn] + c->r[rm];
            if (!in_arena(base, hw ? 2u : 1u)) { a32_fault(c, pc, h1, "Thumb TBB/TBH table outside the arena"); return 1; }
            uint32_t idx = hw ? rd16(c, base) : rd8(c, base);
            *next = pc + 4u + (idx << 1);   /* both scale the entry by 2 */
            return 0;
        }
        /* LDRD / STRD (immediate). Rt:Rt2 = h2[15:12]:h2[11:8], imm8 = h2[7:0]. */
        {
            uint32_t hw = h1 & 0xFFF0u;
            static const struct { uint32_t v; int load, u, w; } d[] = {
                { 0xE9D0u, 1, 1, 0 }, { 0xE950u, 1, 0, 0 },
                { 0xE9F0u, 1, 1, 1 }, { 0xE970u, 1, 0, 1 },
                { 0xE9C0u, 0, 1, 0 }, { 0xE940u, 0, 0, 0 },
                { 0xE9E0u, 0, 1, 1 }, { 0xE960u, 0, 0, 1 },
            };
            for (unsigned k = 0; k < sizeof(d) / sizeof(d[0]); k++) {
                if (hw == d[k].v) {
                    uint32_t rn = h1 & 0xFu, rt = (h2 >> 12) & 0xFu, rt2 = (h2 >> 8) & 0xFu;
                    uint32_t imm8 = h2 & 0xFFu, off = imm8 << 2;
                    uint32_t base = c->r[rn];
                    uint32_t addr = d[k].u ? base + off : base - off;
                    if (rt & 1u || rt2 != rt + 1u) { a32_fault(c, pc, h1, "Thumb-2 LDRD/STRD with an odd or non-consecutive register pair"); return 1; }
                    if (addr & 3u) { a32_fault(c, pc, h1, "Thumb-2 unaligned LDRD/STRD"); return 1; }
                    if (!in_arena(addr, 8)) { a32_fault(c, pc, h1, "Thumb-2 LDRD/STRD outside the arena"); return 1; }
                    if (d[k].load) { c->r[rt] = rd32(c, addr); c->r[rt2] = rd32(c, addr + 4); }
                    else           { wr32(c, addr, c->r[rt]); wr32(c, addr + 4, c->r[rt2]); }
                    if (d[k].w) c->r[rn] = d[k].u ? base + off : base - off;
                    return 0;
                }
            }
        }
        /* LDM/STM wide (1110 100x ...): not generated; fault by name. */
        if ((h1 & 0xFE00u) == 0xE800u) {
            a32_fault(c, pc, h1, "Thumb-2 LDM/STM (wide) not implemented"); return 1;
        }
        a32_fault(c, pc, h1, "unimplemented Thumb-2 11101 encoding"); return 1;
    }

    if (op1 == 0x1Fu) {                          /* 11111 */
        /* Extend / reverse / CLZ: 1111 1010 xxxx ... */
        if ((h1 & 0xFF00u) == 0xFA00u) {
            uint32_t rn = h1 & 0xFu, rm = h2 & 0xFu, rd = (h2 >> 8) & 0xFu, op = (h2 >> 4) & 0xFu;
            uint32_t add = (rn == 15u) ? 0u : c->r[rn];   /* SXTAB/UXTAH add Rn */
            switch (h1 & 0xFFF0u) {
            case 0xFA40u: c->r[rd] = add + sx(c->r[rm] & 0xFFu, 8); return 0;    /* SXTB/SXTAB */
            case 0xFA00u: c->r[rd] = add + sx(c->r[rm] & 0xFFFFu, 16); return 0; /* SXTH/SXTAH */
            case 0xFA50u: c->r[rd] = add + (c->r[rm] & 0xFFu); return 0; /* UXTB/UXTAB */
            case 0xFA10u: c->r[rd] = add + (c->r[rm] & 0xFFFFu); return 0;/* UXTH/UXTAH */
            case 0xFAB0u: {                                              /* CLZ */
                uint32_t v = c->r[rm], n = 0;
                while (n < 32 && !((v >> (31 - n)) & 1u)) n++;
                c->r[rd] = n; return 0; }
            case 0xFA90u:                                                /* REV/REV16/REVSH */
                if (op == 0x8u) {
                    uint32_t v = c->r[rm];
                    c->r[rd] = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
                               ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
                    return 0;
                }
                if (op == 0x9u) {
                    uint32_t v = c->r[rm];
                    c->r[rd] = ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu) |
                               ((v & 0xFF0000u) << 8) | ((v >> 8) & 0xFF0000u);
                    return 0;
                }
                if (op == 0xBu) {
                    uint32_t v = c->r[rm];
                    c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
                    return 0;
                }
                break;
            default: break;
            }
            a32_fault(c, pc, h1, "unimplemented Thumb-2 extend/reverse encoding"); return 1;
        }
        /* wide multiply (1111 1011 xxxx) */
        if ((h1 & 0xFF00u) == 0xFB00u) return thumb_mulwide(c, pc, h1, h2);
        /* load/store single */
        if ((h1 & 0xFE00u) == 0xF800u) return thumb_wide_ldst(c, pc, h1, h2);
        a32_fault(c, pc, h1, "unimplemented Thumb-2 11111 encoding"); return 1;
    }


    a32_fault(c, pc, h1, "unimplemented Thumb-2 instruction"); return 1;
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
    c->code_end = CODE_BASE + in->code_len;
    c->err = err;
    c->errn = errn;
    if (err && errn) err[0] = 0;

    if (in->code_len == 0 || in->code_len > (SCRATCH - CODE_BASE)) {
        if (err) snprintf(err, errn, "case %u: bad fragment length (%u bytes)",
                          c->case_id, in->code_len);
        free(mem);
        out->stop_reason = A32_STOP_FAULT;
        return 1;
    }
    for (uint32_t i = 0; i < in->code_len; i++)
        mem[(CODE_BASE - ARENA_BASE) + i] = in->code[i];

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
    c->cpsr = CPSR_INIT | (in->thumb ? F_T : 0u);
    /* The harness's agreed VFP init: gen_cases.py's a32_fp_init writes
     * `movw r1,#i` + `movt r1,#0x3f80`, so s_i starts at 0x3f800000 + i
     * (1.0f + i ulp). Integer cases never touch the register file, so a case
     * that does not reach VFP records exactly this -- the same value the old
     * hard-coded out->fp[] produced, which is why the 500 A32 cases are
     * unchanged. A VFP case writes real values over it. */
    for (int i = 0; i < 32; i++) c->s[i] = 0x3f800000u + (uint32_t)i;

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

        /* ---- Thumb state ------------------------------------------------ */
        if (c->cpsr & F_T) {
            if (pc & 1u) { a32_fault(c, pc, 0, "pc is not halfword-aligned in Thumb state"); break; }
            int f = 0;
            pc = thumb_step(c, pc, &f);
            if (f) break;
            continue;
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
                c->cpsr |= F_T;                          /* BLX immediate enters Thumb */
                pc = pc + 8u + sx(off << 2, 26);
                continue;
            }
            /* DMB / DSB / ISB. A barrier orders memory accesses between
             * observers; this interpreter is a single sequentially-consistent
             * observer, so the architecturally correct effect here is no
             * effect. (The other unconditional encodings -- PLD, the legacy
             * coprocessor stc2/ldc2 -- stay named faults.) */
            if ((insn & 0xFFFF0FF0u) == 0xF57F0040u ||   /* DSB */
                (insn & 0xFFFF0FF0u) == 0xF57F0050u ||   /* DMB */
                (insn & 0xFFFF0FF0u) == 0xF57F0060u) {   /* ISB */
                pc = next; continue;
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
                c->cpsr = (c->cpsr & ~F_T) | ((v & 1u) ? F_T : 0u);
                pc = v & ~1u; continue;
            }
            if ((insn & 0x0FFFFFF0u) == 0x012FFF30u) {   /* BLX (register) */
                uint32_t v = get_r(c, (int)(insn & 0xF), pc);
                c->r[14] = pc + 4u;
                c->cpsr = (c->cpsr & ~F_T) | ((v & 1u) ? F_T : 0u);
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

        /* ================= coprocessor load/store (g == 6) ================ */
        if (g == 6) {
            if (a32_coproc(c, pc, insn)) break;
            pc = next; continue;
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
    /* The register file as the case left it (s0..s31 zero-extended into u64),
     * whether it is still the agreed init or a VFP load/store has overwritten
     * it. See the init note above. */
    for (int i = 0; i < A32_NFP; i++) out->fp[i] = c->s[i];

    free(mem);
    return failed;
}
