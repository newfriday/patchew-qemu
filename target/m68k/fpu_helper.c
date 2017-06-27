/*
 *  m68k FPU helpers
 *
 *  Copyright (c) 2006-2007 CodeSourcery
 *  Written by Paul Brook
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/exec-all.h"

static const floatx80 fpu_rom[128] = {
    [0x00] = floatx80_pi,                                   /* Pi */
    [0x0b] = make_floatx80(0x3ffd, 0x9a209a84fbcff798ULL),  /* Log10(2) */
    [0x0c] = make_floatx80(0x4000, 0xadf85458a2bb4a9aULL),  /* e        */
    [0x0d] = make_floatx80(0x3fff, 0xb8aa3b295c17f0bcULL),  /* Log2(e)  */
    [0x0e] = make_floatx80(0x3ffd, 0xde5bd8a937287195ULL),  /* Log10(e) */
    [0x0f] = floatx80_zero,                                 /* Zero     */
    [0x30] = floatx80_ln2,                                  /* ln(2)    */
    [0x31] = make_floatx80(0x4000, 0x935d8dddaaa8ac17ULL),  /* ln(10)   */
    [0x32] = floatx80_one,                                  /* 10^0     */
    [0x33] = make_floatx80(0x4002, 0xa000000000000000ULL),  /* 10^1     */
    [0x34] = make_floatx80(0x4005, 0xc800000000000000ULL),  /* 10^2     */
    [0x35] = make_floatx80(0x400c, 0x9c40000000000000ULL),  /* 10^4     */
    [0x36] = make_floatx80(0x4019, 0xbebc200000000000ULL),  /* 10^8     */
    [0x37] = make_floatx80(0x4034, 0x8e1bc9bf04000000ULL),  /* 10^16    */
    [0x38] = make_floatx80(0x4069, 0x9dc5ada82b70b59eULL),  /* 10^32    */
    [0x39] = make_floatx80(0x40d3, 0xc2781f49ffcfa6d5ULL),  /* 10^64    */
    [0x3a] = make_floatx80(0x41a8, 0x93ba47c980e98ce0ULL),  /* 10^128   */
    [0x3b] = make_floatx80(0x4351, 0xaa7eebfb9df9de8eULL),  /* 10^256   */
    [0x3c] = make_floatx80(0x46a3, 0xe319a0aea60e91c7ULL),  /* 10^512   */
    [0x3d] = make_floatx80(0x4d48, 0xc976758681750c17ULL),  /* 10^1024  */
    [0x3e] = make_floatx80(0x5a92, 0x9e8b3b5dc53d5de5ULL),  /* 10^2048  */
    [0x3f] = make_floatx80(0x7525, 0xc46052028a20979bULL),  /* 10^4096  */
};

int32_t HELPER(reds32)(CPUM68KState *env, FPReg *val)
{
    return floatx80_to_int32(val->d, &env->fp_status);
}

float32 HELPER(redf32)(CPUM68KState *env, FPReg *val)
{
    return floatx80_to_float32(val->d, &env->fp_status);
}

void HELPER(exts32)(CPUM68KState *env, FPReg *res, int32_t val)
{
    res->d = int32_to_floatx80(val, &env->fp_status);
}

void HELPER(extf32)(CPUM68KState *env, FPReg *res, float32 val)
{
    res->d = float32_to_floatx80(val, &env->fp_status);
}

void HELPER(extf64)(CPUM68KState *env, FPReg *res, float64 val)
{
    res->d = float64_to_floatx80(val, &env->fp_status);
}

float64 HELPER(redf64)(CPUM68KState *env, FPReg *val)
{
    return floatx80_to_float64(val->d, &env->fp_status);
}

void HELPER(firound)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_round_to_int(val->d, &env->fp_status);
}

static void m68k_restore_precision_mode(CPUM68KState *env)
{
    switch (env->fpcr & FPCR_PREC_MASK) {
    case FPCR_PREC_X: /* extended */
        set_floatx80_rounding_precision(80, &env->fp_status);
        break;
    case FPCR_PREC_S: /* single */
        set_floatx80_rounding_precision(32, &env->fp_status);
        break;
    case FPCR_PREC_D: /* double */
        set_floatx80_rounding_precision(64, &env->fp_status);
        break;
    case FPCR_PREC_U: /* undefined */
    default:
        break;
    }
}

static void cf_restore_precision_mode(CPUM68KState *env)
{
    if (env->fpcr & FPCR_PREC_S) { /* single */
        set_floatx80_rounding_precision(32, &env->fp_status);
    } else { /* double */
        set_floatx80_rounding_precision(64, &env->fp_status);
    }
}

static void restore_rounding_mode(CPUM68KState *env)
{
    switch (env->fpcr & FPCR_RND_MASK) {
    case FPCR_RND_N: /* round to nearest */
        set_float_rounding_mode(float_round_nearest_even, &env->fp_status);
        break;
    case FPCR_RND_Z: /* round to zero */
        set_float_rounding_mode(float_round_to_zero, &env->fp_status);
        break;
    case FPCR_RND_M: /* round toward minus infinity */
        set_float_rounding_mode(float_round_down, &env->fp_status);
        break;
    case FPCR_RND_P: /* round toward positive infinity */
        set_float_rounding_mode(float_round_up, &env->fp_status);
        break;
    }
}

void cpu_m68k_set_fpcr(CPUM68KState *env, uint32_t val)
{
    env->fpcr = val & 0xffff;

    if (m68k_feature(env, M68K_FEATURE_CF_FPU)) {
        cf_restore_precision_mode(env);
    } else {
        m68k_restore_precision_mode(env);
    }
    restore_rounding_mode(env);
}

void HELPER(fitrunc)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    int rounding_mode = get_float_rounding_mode(&env->fp_status);
    set_float_rounding_mode(float_round_to_zero, &env->fp_status);
    res->d = floatx80_round_to_int(val->d, &env->fp_status);
    set_float_rounding_mode(rounding_mode, &env->fp_status);
}

void HELPER(set_fpcr)(CPUM68KState *env, uint32_t val)
{
    cpu_m68k_set_fpcr(env, val);
}

#define PREC_BEGIN(prec)                                        \
    do {                                                        \
        int old;                                                \
        old = get_floatx80_rounding_precision(&env->fp_status); \
        set_floatx80_rounding_precision(prec, &env->fp_status)  \

#define PREC_END()                                              \
        set_floatx80_rounding_precision(old, &env->fp_status);  \
    } while (0)

void HELPER(fsround)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(32);
    res->d = floatx80_round(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdround)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(64);
    res->d = floatx80_round(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsqrt)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_sqrt(val->d, &env->fp_status);
}

void HELPER(fssqrt)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(32);
    res->d = floatx80_sqrt(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdsqrt)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(64);
    res->d = floatx80_sqrt(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fabs)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_round(floatx80_abs(val->d), &env->fp_status);
}

void HELPER(fsabs)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(32);
    res->d = floatx80_round(floatx80_abs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fdabs)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(64);
    res->d = floatx80_round(floatx80_abs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fneg)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_round(floatx80_chs(val->d), &env->fp_status);
}

void HELPER(fsneg)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(32);
    res->d = floatx80_round(floatx80_chs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fdneg)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(64);
    res->d = floatx80_round(floatx80_chs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fadd)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_add(val0->d, val1->d, &env->fp_status);
}

void HELPER(fsadd)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(32);
    res->d = floatx80_add(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdadd)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(64);
    res->d = floatx80_add(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsub)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_sub(val1->d, val0->d, &env->fp_status);
}

void HELPER(fssub)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(32);
    res->d = floatx80_sub(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdsub)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(64);
    res->d = floatx80_sub(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_mul(val0->d, val1->d, &env->fp_status);
}

void HELPER(fsmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(32);
    res->d = floatx80_mul(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(64);
    res->d = floatx80_mul(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsglmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    int rounding_mode = get_float_rounding_mode(&env->fp_status);
    floatx80 a, b;

    PREC_BEGIN(32);
    set_float_rounding_mode(float_round_to_zero, &env->fp_status);
    a = floatx80_round(val0->d, &env->fp_status);
    b = floatx80_round(val1->d, &env->fp_status);
    set_float_rounding_mode(rounding_mode, &env->fp_status);
    res->d = floatx80_mul(a, b, &env->fp_status);
    PREC_END();
}

void HELPER(fdiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_div(val1->d, val0->d, &env->fp_status);
}

void HELPER(fsdiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(32);
    res->d = floatx80_div(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fddiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(64);
    res->d = floatx80_div(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsgldiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    int rounding_mode = get_float_rounding_mode(&env->fp_status);
    floatx80 a, b;

    PREC_BEGIN(32);
    set_float_rounding_mode(float_round_to_zero, &env->fp_status);
    a = floatx80_round(val1->d, &env->fp_status);
    b = floatx80_round(val0->d, &env->fp_status);
    set_float_rounding_mode(rounding_mode, &env->fp_status);
    res->d = floatx80_div(a, b, &env->fp_status);
    PREC_END();
}

static int float_comp_to_cc(int float_compare)
{
    switch (float_compare) {
    case float_relation_equal:
        return FPSR_CC_Z;
    case float_relation_less:
        return FPSR_CC_N;
    case float_relation_unordered:
        return FPSR_CC_A;
    case float_relation_greater:
        return 0;
    default:
        g_assert_not_reached();
    }
}

void HELPER(fcmp)(CPUM68KState *env, FPReg *val0, FPReg *val1)
{
    int float_compare;

    float_compare = floatx80_compare(val1->d, val0->d, &env->fp_status);
    env->fpsr = (env->fpsr & ~FPSR_CC_MASK) | float_comp_to_cc(float_compare);
}

void HELPER(ftst)(CPUM68KState *env, FPReg *val)
{
    uint32_t cc = 0;

    if (floatx80_is_neg(val->d)) {
        cc |= FPSR_CC_N;
    }

    if (floatx80_is_any_nan(val->d)) {
        cc |= FPSR_CC_A;
    } else if (floatx80_is_infinity(val->d)) {
        cc |= FPSR_CC_I;
    } else if (floatx80_is_zero(val->d)) {
        cc |= FPSR_CC_Z;
    }
    env->fpsr = (env->fpsr & ~FPSR_CC_MASK) | cc;
}

void HELPER(fconst)(CPUM68KState *env, FPReg *val, uint32_t offset)
{
    val->d = fpu_rom[offset];
}
