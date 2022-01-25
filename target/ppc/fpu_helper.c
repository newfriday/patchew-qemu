/*
 *  PowerPC floating point and SPE emulation helpers for QEMU.
 *
 *  Copyright (c) 2003-2007 Jocelyn Mayer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/exec-all.h"
#include "internal.h"
#include "fpu/softfloat.h"

static inline float128 float128_snan_to_qnan(float128 x)
{
    float128 r;

    r.high = x.high | 0x0000800000000000;
    r.low = x.low;
    return r;
}

#define float64_snan_to_qnan(x) ((x) | 0x0008000000000000ULL)
#define float32_snan_to_qnan(x) ((x) | 0x00400000)
#define float16_snan_to_qnan(x) ((x) | 0x0200)

static inline bool fp_exceptions_enabled(CPUPPCState *env)
{
#ifdef CONFIG_USER_ONLY
    return true;
#else
    return (env->msr & ((1U << MSR_FE0) | (1U << MSR_FE1))) != 0;
#endif
}

/*****************************************************************************/
/* Floating point operations helpers */

/*
 * This is the non-arithmatic conversion that happens e.g. on loads.
 * In the Power ISA pseudocode, this is called DOUBLE.
 */
uint64_t helper_todouble(uint32_t arg)
{
    uint32_t abs_arg = arg & 0x7fffffff;
    uint64_t ret;

    if (likely(abs_arg >= 0x00800000)) {
        if (unlikely(extract32(arg, 23, 8) == 0xff)) {
            /* Inf or NAN.  */
            ret  = (uint64_t)extract32(arg, 31, 1) << 63;
            ret |= (uint64_t)0x7ff << 52;
            ret |= (uint64_t)extract32(arg, 0, 23) << 29;
        } else {
            /* Normalized operand.  */
            ret  = (uint64_t)extract32(arg, 30, 2) << 62;
            ret |= ((extract32(arg, 30, 1) ^ 1) * (uint64_t)7) << 59;
            ret |= (uint64_t)extract32(arg, 0, 30) << 29;
        }
    } else {
        /* Zero or Denormalized operand.  */
        ret = (uint64_t)extract32(arg, 31, 1) << 63;
        if (unlikely(abs_arg != 0)) {
            /*
             * Denormalized operand.
             * Shift fraction so that the msb is in the implicit bit position.
             * Thus, shift is in the range [1:23].
             */
            int shift = clz32(abs_arg) - 8;
            /*
             * The first 3 terms compute the float64 exponent.  We then bias
             * this result by -1 so that we can swallow the implicit bit below.
             */
            int exp = -126 - shift + 1023 - 1;

            ret |= (uint64_t)exp << 52;
            ret += (uint64_t)abs_arg << (52 - 23 + shift);
        }
    }
    return ret;
}

/*
 * This is the non-arithmatic conversion that happens e.g. on stores.
 * In the Power ISA pseudocode, this is called SINGLE.
 */
uint32_t helper_tosingle(uint64_t arg)
{
    int exp = extract64(arg, 52, 11);
    uint32_t ret;

    if (likely(exp > 896)) {
        /* No denormalization required (includes Inf, NaN).  */
        ret  = extract64(arg, 62, 2) << 30;
        ret |= extract64(arg, 29, 30);
    } else {
        /*
         * Zero or Denormal result.  If the exponent is in bounds for
         * a single-precision denormal result, extract the proper
         * bits.  If the input is not zero, and the exponent is out of
         * bounds, then the result is undefined; this underflows to
         * zero.
         */
        ret = extract64(arg, 63, 1) << 31;
        if (unlikely(exp >= 874)) {
            /* Denormal result.  */
            ret |= ((1ULL << 52) | extract64(arg, 0, 52)) >> (896 + 30 - exp);
        }
    }
    return ret;
}

static inline int ppc_float32_get_unbiased_exp(float32 f)
{
    return ((f >> 23) & 0xFF) - 127;
}

static inline int ppc_float64_get_unbiased_exp(float64 f)
{
    return ((f >> 52) & 0x7FF) - 1023;
}

/* Classify a floating-point number.  */
enum {
    is_normal   = 1,
    is_zero     = 2,
    is_denormal = 4,
    is_inf      = 8,
    is_qnan     = 16,
    is_snan     = 32,
    is_neg      = 64,
};

#define COMPUTE_CLASS(tp)                                      \
static int tp##_classify(tp arg)                               \
{                                                              \
    int ret = tp##_is_neg(arg) * is_neg;                       \
    if (unlikely(tp##_is_any_nan(arg))) {                      \
        float_status dummy = { };  /* snan_bit_is_one = 0 */   \
        ret |= (tp##_is_signaling_nan(arg, &dummy)             \
                ? is_snan : is_qnan);                          \
    } else if (unlikely(tp##_is_infinity(arg))) {              \
        ret |= is_inf;                                         \
    } else if (tp##_is_zero(arg)) {                            \
        ret |= is_zero;                                        \
    } else if (tp##_is_zero_or_denormal(arg)) {                \
        ret |= is_denormal;                                    \
    } else {                                                   \
        ret |= is_normal;                                      \
    }                                                          \
    return ret;                                                \
}

COMPUTE_CLASS(float16)
COMPUTE_CLASS(float32)
COMPUTE_CLASS(float64)
COMPUTE_CLASS(float128)

static void set_fprf_from_class(CPUPPCState *env, int class)
{
    static const uint8_t fprf[6][2] = {
        { 0x04, 0x08 },  /* normalized */
        { 0x02, 0x12 },  /* zero */
        { 0x14, 0x18 },  /* denormalized */
        { 0x05, 0x09 },  /* infinity */
        { 0x11, 0x11 },  /* qnan */
        { 0x00, 0x00 },  /* snan -- flags are undefined */
    };
    bool isneg = class & is_neg;

    env->fpscr &= ~FP_FPRF;
    env->fpscr |= fprf[ctz32(class)][isneg] << FPSCR_FPRF;
}

#define COMPUTE_FPRF(tp)                                \
void helper_compute_fprf_##tp(CPUPPCState *env, tp arg) \
{                                                       \
    set_fprf_from_class(env, tp##_classify(arg));       \
}

COMPUTE_FPRF(float16)
COMPUTE_FPRF(float32)
COMPUTE_FPRF(float64)
COMPUTE_FPRF(float128)

/* Floating-point invalid operations exception */
static void finish_invalid_op_excp(CPUPPCState *env, int op, uintptr_t retaddr)
{
    /* Update the floating-point invalid operation summary */
    env->fpscr |= FP_VX;
    /* Update the floating-point exception summary */
    env->fpscr |= FP_FX;
    if (fpscr_ve != 0) {
        /* Update the floating-point enabled exception summary */
        env->fpscr |= FP_FEX;
        if (fp_exceptions_enabled(env)) {
            raise_exception_err_ra(env, POWERPC_EXCP_PROGRAM,
                                   POWERPC_EXCP_FP | op, retaddr);
        }
    }
}

static void finish_invalid_op_arith(CPUPPCState *env, int op,
                                    bool set_fpcc, uintptr_t retaddr)
{
    env->fpscr &= ~(FP_FR | FP_FI);
    if (fpscr_ve == 0) {
        if (set_fpcc) {
            env->fpscr &= ~FP_FPCC;
            env->fpscr |= (FP_C | FP_FU);
        }
    }
    finish_invalid_op_excp(env, op, retaddr);
}

/* Signalling NaN */
static void float_invalid_op_vxsnan(CPUPPCState *env, uintptr_t retaddr)
{
    env->fpscr |= FP_VXSNAN;
    finish_invalid_op_excp(env, POWERPC_EXCP_FP_VXSNAN, retaddr);
}

/* Magnitude subtraction of infinities */
static void float_invalid_op_vxisi(CPUPPCState *env, bool set_fpcc,
                                   uintptr_t retaddr)
{
    env->fpscr |= FP_VXISI;
    finish_invalid_op_arith(env, POWERPC_EXCP_FP_VXISI, set_fpcc, retaddr);
}

/* Division of infinity by infinity */
static void float_invalid_op_vxidi(CPUPPCState *env, bool set_fpcc,
                                   uintptr_t retaddr)
{
    env->fpscr |= FP_VXIDI;
    finish_invalid_op_arith(env, POWERPC_EXCP_FP_VXIDI, set_fpcc, retaddr);
}

/* Division of zero by zero */
static void float_invalid_op_vxzdz(CPUPPCState *env, bool set_fpcc,
                                   uintptr_t retaddr)
{
    env->fpscr |= FP_VXZDZ;
    finish_invalid_op_arith(env, POWERPC_EXCP_FP_VXZDZ, set_fpcc, retaddr);
}

/* Multiplication of zero by infinity */
static void float_invalid_op_vximz(CPUPPCState *env, bool set_fpcc,
                                   uintptr_t retaddr)
{
    env->fpscr |= FP_VXIMZ;
    finish_invalid_op_arith(env, POWERPC_EXCP_FP_VXIMZ, set_fpcc, retaddr);
}

/* Square root of a negative number */
static void float_invalid_op_vxsqrt(CPUPPCState *env, bool set_fpcc,
                                    uintptr_t retaddr)
{
    env->fpscr |= FP_VXSQRT;
    finish_invalid_op_arith(env, POWERPC_EXCP_FP_VXSQRT, set_fpcc, retaddr);
}

/* Ordered comparison of NaN */
static void float_invalid_op_vxvc(CPUPPCState *env, bool set_fpcc,
                                  uintptr_t retaddr)
{
    env->fpscr |= FP_VXVC;
    if (set_fpcc) {
        env->fpscr &= ~FP_FPCC;
        env->fpscr |= (FP_C | FP_FU);
    }
    /* Update the floating-point invalid operation summary */
    env->fpscr |= FP_VX;
    /* Update the floating-point exception summary */
    env->fpscr |= FP_FX;
    /* We must update the target FPR before raising the exception */
    if (fpscr_ve != 0) {
        CPUState *cs = env_cpu(env);

        cs->exception_index = POWERPC_EXCP_PROGRAM;
        env->error_code = POWERPC_EXCP_FP | POWERPC_EXCP_FP_VXVC;
        /* Update the floating-point enabled exception summary */
        env->fpscr |= FP_FEX;
        /* Exception is deferred */
    }
}

/* Invalid conversion */
static void float_invalid_op_vxcvi(CPUPPCState *env, bool set_fpcc,
                                   uintptr_t retaddr)
{
    env->fpscr |= FP_VXCVI;
    env->fpscr &= ~(FP_FR | FP_FI);
    if (fpscr_ve == 0) {
        if (set_fpcc) {
            env->fpscr &= ~FP_FPCC;
            env->fpscr |= (FP_C | FP_FU);
        }
    }
    finish_invalid_op_excp(env, POWERPC_EXCP_FP_VXCVI, retaddr);
}

static inline void float_zero_divide_excp(CPUPPCState *env, uintptr_t raddr)
{
    env->fpscr |= FP_ZX;
    env->fpscr &= ~(FP_FR | FP_FI);
    /* Update the floating-point exception summary */
    env->fpscr |= FP_FX;
    if (fpscr_ze != 0) {
        /* Update the floating-point enabled exception summary */
        env->fpscr |= FP_FEX;
        if (fp_exceptions_enabled(env)) {
            raise_exception_err_ra(env, POWERPC_EXCP_PROGRAM,
                                   POWERPC_EXCP_FP | POWERPC_EXCP_FP_ZX,
                                   raddr);
        }
    }
}

static inline void float_overflow_excp(CPUPPCState *env)
{
    CPUState *cs = env_cpu(env);

    env->fpscr |= FP_OX;
    /* Update the floating-point exception summary */
    env->fpscr |= FP_FX;
    if (fpscr_oe != 0) {
        /* XXX: should adjust the result */
        /* Update the floating-point enabled exception summary */
        env->fpscr |= FP_FEX;
        /* We must update the target FPR before raising the exception */
        cs->exception_index = POWERPC_EXCP_PROGRAM;
        env->error_code = POWERPC_EXCP_FP | POWERPC_EXCP_FP_OX;
    } else {
        env->fpscr |= FP_XX;
        env->fpscr |= FP_FI;
    }
}

static inline void float_underflow_excp(CPUPPCState *env)
{
    CPUState *cs = env_cpu(env);

    env->fpscr |= FP_UX;
    /* Update the floating-point exception summary */
    env->fpscr |= FP_FX;
    if (fpscr_ue != 0) {
        /* XXX: should adjust the result */
        /* Update the floating-point enabled exception summary */
        env->fpscr |= FP_FEX;
        /* We must update the target FPR before raising the exception */
        cs->exception_index = POWERPC_EXCP_PROGRAM;
        env->error_code = POWERPC_EXCP_FP | POWERPC_EXCP_FP_UX;
    }
}

static inline void float_inexact_excp(CPUPPCState *env)
{
    CPUState *cs = env_cpu(env);

    env->fpscr |= FP_FI;
    env->fpscr |= FP_XX;
    /* Update the floating-point exception summary */
    env->fpscr |= FP_FX;
    if (fpscr_xe != 0) {
        /* Update the floating-point enabled exception summary */
        env->fpscr |= FP_FEX;
        /* We must update the target FPR before raising the exception */
        cs->exception_index = POWERPC_EXCP_PROGRAM;
        env->error_code = POWERPC_EXCP_FP | POWERPC_EXCP_FP_XX;
    }
}

void helper_fpscr_clrbit(CPUPPCState *env, uint32_t bit)
{
    uint32_t mask = 1u << bit;
    if (env->fpscr & mask) {
        ppc_store_fpscr(env, env->fpscr & ~(target_ulong)mask);
    }
}

void helper_fpscr_setbit(CPUPPCState *env, uint32_t bit)
{
    uint32_t mask = 1u << bit;
    if (!(env->fpscr & mask)) {
        ppc_store_fpscr(env, env->fpscr | mask);
    }
}

void helper_store_fpscr(CPUPPCState *env, uint64_t val, uint32_t nibbles)
{
    target_ulong mask = 0;
    int i;

    /* TODO: push this extension back to translation time */
    for (i = 0; i < sizeof(target_ulong) * 2; i++) {
        if (nibbles & (1 << i)) {
            mask |= (target_ulong) 0xf << (4 * i);
        }
    }
    val = (val & mask) | (env->fpscr & ~mask);
    ppc_store_fpscr(env, val);
}

void helper_fpscr_check_status(CPUPPCState *env)
{
    CPUState *cs = env_cpu(env);
    target_ulong fpscr = env->fpscr;
    int error = 0;

    if ((fpscr & FP_OX) && (fpscr & FP_OE)) {
        error = POWERPC_EXCP_FP_OX;
    } else if ((fpscr & FP_UX) && (fpscr & FP_UE)) {
        error = POWERPC_EXCP_FP_UX;
    } else if ((fpscr & FP_XX) && (fpscr & FP_XE)) {
        error = POWERPC_EXCP_FP_XX;
    } else if ((fpscr & FP_ZX) && (fpscr & FP_ZE)) {
        error = POWERPC_EXCP_FP_ZX;
    } else if (fpscr & FP_VE) {
        if (fpscr & FP_VXSOFT) {
            error = POWERPC_EXCP_FP_VXSOFT;
        } else if (fpscr & FP_VXSNAN) {
            error = POWERPC_EXCP_FP_VXSNAN;
        } else if (fpscr & FP_VXISI) {
            error = POWERPC_EXCP_FP_VXISI;
        } else if (fpscr & FP_VXIDI) {
            error = POWERPC_EXCP_FP_VXIDI;
        } else if (fpscr & FP_VXZDZ) {
            error = POWERPC_EXCP_FP_VXZDZ;
        } else if (fpscr & FP_VXIMZ) {
            error = POWERPC_EXCP_FP_VXIMZ;
        } else if (fpscr & FP_VXVC) {
            error = POWERPC_EXCP_FP_VXVC;
        } else if (fpscr & FP_VXSQRT) {
            error = POWERPC_EXCP_FP_VXSQRT;
        } else if (fpscr & FP_VXCVI) {
            error = POWERPC_EXCP_FP_VXCVI;
        } else {
            return;
        }
    } else {
        return;
    }
    cs->exception_index = POWERPC_EXCP_PROGRAM;
    env->error_code = error | POWERPC_EXCP_FP;
    /* Deferred floating-point exception after target FPSCR update */
    if (fp_exceptions_enabled(env)) {
        raise_exception_err_ra(env, cs->exception_index,
                               env->error_code, GETPC());
    }
}

static void do_float_check_status(CPUPPCState *env, uintptr_t raddr)
{
    CPUState *cs = env_cpu(env);
    int status = get_float_exception_flags(&env->fp_status);

    if (status & float_flag_overflow) {
        float_overflow_excp(env);
    } else if (status & float_flag_underflow) {
        float_underflow_excp(env);
    }
    if (status & float_flag_inexact) {
        float_inexact_excp(env);
    } else {
        env->fpscr &= ~FP_FI; /* clear the FPSCR[FI] bit */
    }

    if (cs->exception_index == POWERPC_EXCP_PROGRAM &&
        (env->error_code & POWERPC_EXCP_FP)) {
        /* Deferred floating-point exception after target FPR update */
        if (fp_exceptions_enabled(env)) {
            raise_exception_err_ra(env, cs->exception_index,
                                   env->error_code, raddr);
        }
    }
}

void helper_float_check_status(CPUPPCState *env)
{
    do_float_check_status(env, GETPC());
}

void helper_reset_fpstatus(CPUPPCState *env)
{
    set_float_exception_flags(0, &env->fp_status);
}

static void float_invalid_op_addsub(CPUPPCState *env, int flags,
                                    bool set_fpcc, uintptr_t retaddr)
{
    if (flags & float_flag_invalid_isi) {
        float_invalid_op_vxisi(env, set_fpcc, retaddr);
    } else if (flags & float_flag_invalid_snan) {
        float_invalid_op_vxsnan(env, retaddr);
    }
}

/* fadd - fadd. */
float64 helper_fadd(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64_add(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_addsub(env, flags, 1, GETPC());
    }

    return ret;
}

/* fadds - fadds. */
float64 helper_fadds(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64r32_add(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_addsub(env, flags, 1, GETPC());
    }
    return ret;
}

/* fsub - fsub. */
float64 helper_fsub(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64_sub(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_addsub(env, flags, 1, GETPC());
    }

    return ret;
}

/* fsubs - fsubs. */
float64 helper_fsubs(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64r32_sub(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_addsub(env, flags, 1, GETPC());
    }
    return ret;
}

static void float_invalid_op_mul(CPUPPCState *env, int flags,
                                 bool set_fprc, uintptr_t retaddr)
{
    if (flags & float_flag_invalid_imz) {
        float_invalid_op_vximz(env, set_fprc, retaddr);
    } else if (flags & float_flag_invalid_snan) {
        float_invalid_op_vxsnan(env, retaddr);
    }
}

/* fmul - fmul. */
float64 helper_fmul(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64_mul(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_mul(env, flags, 1, GETPC());
    }

    return ret;
}

/* fmuls - fmuls. */
float64 helper_fmuls(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64r32_mul(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_mul(env, flags, 1, GETPC());
    }
    return ret;
}

static void float_invalid_op_div(CPUPPCState *env, int flags,
                                 bool set_fprc, uintptr_t retaddr)
{
    if (flags & float_flag_invalid_idi) {
        float_invalid_op_vxidi(env, set_fprc, retaddr);
    } else if (flags & float_flag_invalid_zdz) {
        float_invalid_op_vxzdz(env, set_fprc, retaddr);
    } else if (flags & float_flag_invalid_snan) {
        float_invalid_op_vxsnan(env, retaddr);
    }
}

/* fdiv - fdiv. */
float64 helper_fdiv(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64_div(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_div(env, flags, 1, GETPC());
    }
    if (unlikely(flags & float_flag_divbyzero)) {
        float_zero_divide_excp(env, GETPC());
    }

    return ret;
}

/* fdivs - fdivs. */
float64 helper_fdivs(CPUPPCState *env, float64 arg1, float64 arg2)
{
    float64 ret = float64r32_div(arg1, arg2, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_div(env, flags, 1, GETPC());
    }
    if (unlikely(flags & float_flag_divbyzero)) {
        float_zero_divide_excp(env, GETPC());
    }

    return ret;
}

static uint64_t float_invalid_cvt(CPUPPCState *env, int flags,
                                  uint64_t ret, uint64_t ret_nan,
                                  bool set_fprc, uintptr_t retaddr)
{
    /*
     * VXCVI is different from most in that it sets two exception bits,
     * VXCVI and VXSNAN for an SNaN input.
     */
    if (flags & float_flag_invalid_snan) {
        env->fpscr |= FP_VXSNAN;
    }
    float_invalid_op_vxcvi(env, set_fprc, retaddr);

    return flags & float_flag_invalid_cvti ? ret : ret_nan;
}

#define FPU_FCTI(op, cvt, nanval)                                      \
uint64_t helper_##op(CPUPPCState *env, float64 arg)                    \
{                                                                      \
    uint64_t ret = float64_to_##cvt(arg, &env->fp_status);             \
    int flags = get_float_exception_flags(&env->fp_status);            \
    if (unlikely(flags & float_flag_invalid)) {                        \
        ret = float_invalid_cvt(env, flags, ret, nanval, 1, GETPC());  \
    }                                                                  \
    return ret;                                                        \
}

FPU_FCTI(fctiw, int32, 0x80000000U)
FPU_FCTI(fctiwz, int32_round_to_zero, 0x80000000U)
FPU_FCTI(fctiwu, uint32, 0x00000000U)
FPU_FCTI(fctiwuz, uint32_round_to_zero, 0x00000000U)
FPU_FCTI(fctid, int64, 0x8000000000000000ULL)
FPU_FCTI(fctidz, int64_round_to_zero, 0x8000000000000000ULL)
FPU_FCTI(fctidu, uint64, 0x0000000000000000ULL)
FPU_FCTI(fctiduz, uint64_round_to_zero, 0x0000000000000000ULL)

#define FPU_FCFI(op, cvtr, is_single)                      \
uint64_t helper_##op(CPUPPCState *env, uint64_t arg)       \
{                                                          \
    CPU_DoubleU farg;                                      \
                                                           \
    if (is_single) {                                       \
        float32 tmp = cvtr(arg, &env->fp_status);          \
        farg.d = float32_to_float64(tmp, &env->fp_status); \
    } else {                                               \
        farg.d = cvtr(arg, &env->fp_status);               \
    }                                                      \
    do_float_check_status(env, GETPC());                   \
    return farg.ll;                                        \
}

FPU_FCFI(fcfid, int64_to_float64, 0)
FPU_FCFI(fcfids, int64_to_float32, 1)
FPU_FCFI(fcfidu, uint64_to_float64, 0)
FPU_FCFI(fcfidus, uint64_to_float32, 1)

static uint64_t do_fri(CPUPPCState *env, uint64_t arg,
                       FloatRoundMode rounding_mode)
{
    FloatRoundMode old_rounding_mode = get_float_rounding_mode(&env->fp_status);
    int flags;

    set_float_rounding_mode(rounding_mode, &env->fp_status);
    arg = float64_round_to_int(arg, &env->fp_status);
    set_float_rounding_mode(old_rounding_mode, &env->fp_status);

    flags = get_float_exception_flags(&env->fp_status);
    if (flags & float_flag_invalid_snan) {
        float_invalid_op_vxsnan(env, GETPC());
    }

    /* fri* does not set FPSCR[XX] */
    set_float_exception_flags(flags & ~float_flag_inexact, &env->fp_status);
    do_float_check_status(env, GETPC());

    return arg;
}

uint64_t helper_frin(CPUPPCState *env, uint64_t arg)
{
    return do_fri(env, arg, float_round_ties_away);
}

uint64_t helper_friz(CPUPPCState *env, uint64_t arg)
{
    return do_fri(env, arg, float_round_to_zero);
}

uint64_t helper_frip(CPUPPCState *env, uint64_t arg)
{
    return do_fri(env, arg, float_round_up);
}

uint64_t helper_frim(CPUPPCState *env, uint64_t arg)
{
    return do_fri(env, arg, float_round_down);
}

static void float_invalid_op_madd(CPUPPCState *env, int flags,
                                  bool set_fpcc, uintptr_t retaddr)
{
    if (flags & float_flag_invalid_imz) {
        float_invalid_op_vximz(env, set_fpcc, retaddr);
    } else {
        float_invalid_op_addsub(env, flags, set_fpcc, retaddr);
    }
}

static float64 do_fmadd(CPUPPCState *env, float64 a, float64 b,
                         float64 c, int madd_flags, uintptr_t retaddr)
{
    float64 ret = float64_muladd(a, b, c, madd_flags, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_madd(env, flags, 1, retaddr);
    }
    return ret;
}

static uint64_t do_fmadds(CPUPPCState *env, float64 a, float64 b,
                          float64 c, int madd_flags, uintptr_t retaddr)
{
    float64 ret = float64r32_muladd(a, b, c, madd_flags, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_madd(env, flags, 1, retaddr);
    }
    return ret;
}

#define FPU_FMADD(op, madd_flags)                                    \
    uint64_t helper_##op(CPUPPCState *env, uint64_t arg1,            \
                         uint64_t arg2, uint64_t arg3)               \
    { return do_fmadd(env, arg1, arg2, arg3, madd_flags, GETPC()); } \
    uint64_t helper_##op##s(CPUPPCState *env, uint64_t arg1,         \
                         uint64_t arg2, uint64_t arg3)               \
    { return do_fmadds(env, arg1, arg2, arg3, madd_flags, GETPC()); }

#define MADD_FLGS 0
#define MSUB_FLGS float_muladd_negate_c
#define NMADD_FLGS float_muladd_negate_result
#define NMSUB_FLGS (float_muladd_negate_c | float_muladd_negate_result)

FPU_FMADD(fmadd, MADD_FLGS)
FPU_FMADD(fnmadd, NMADD_FLGS)
FPU_FMADD(fmsub, MSUB_FLGS)
FPU_FMADD(fnmsub, NMSUB_FLGS)

/* frsp - frsp. */
static uint64_t do_frsp(CPUPPCState *env, uint64_t arg, uintptr_t retaddr)
{
    float32 f32 = float64_to_float32(arg, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid_snan)) {
        float_invalid_op_vxsnan(env, retaddr);
    }
    return helper_todouble(f32);
}

uint64_t helper_frsp(CPUPPCState *env, uint64_t arg)
{
    return do_frsp(env, arg, GETPC());
}

static void float_invalid_op_sqrt(CPUPPCState *env, int flags,
                                  bool set_fpcc, uintptr_t retaddr)
{
    if (unlikely(flags & float_flag_invalid_sqrt)) {
        float_invalid_op_vxsqrt(env, set_fpcc, retaddr);
    } else if (unlikely(flags & float_flag_invalid_snan)) {
        float_invalid_op_vxsnan(env, retaddr);
    }
}

/* fsqrt - fsqrt. */
float64 helper_fsqrt(CPUPPCState *env, float64 arg)
{
    float64 ret = float64_sqrt(arg, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_sqrt(env, flags, 1, GETPC());
    }

    return ret;
}

/* fsqrts - fsqrts. */
float64 helper_fsqrts(CPUPPCState *env, float64 arg)
{
    float64 ret = float64r32_sqrt(arg, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_sqrt(env, flags, 1, GETPC());
    }
    return ret;
}

/* fre - fre. */
float64 helper_fre(CPUPPCState *env, float64 arg)
{
    /* "Estimate" the reciprocal with actual division.  */
    float64 ret = float64_div(float64_one, arg, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid_snan)) {
        float_invalid_op_vxsnan(env, GETPC());
    }
    if (unlikely(flags & float_flag_divbyzero)) {
        float_zero_divide_excp(env, GETPC());
        /* For FPSCR.ZE == 0, the result is 1/2.  */
        ret = float64_set_sign(float64_half, float64_is_neg(arg));
    }

    return ret;
}

/* fres - fres. */
uint64_t helper_fres(CPUPPCState *env, uint64_t arg)
{
    /* "Estimate" the reciprocal with actual division.  */
    float64 ret = float64r32_div(float64_one, arg, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid_snan)) {
        float_invalid_op_vxsnan(env, GETPC());
    }
    if (unlikely(flags & float_flag_divbyzero)) {
        float_zero_divide_excp(env, GETPC());
        /* For FPSCR.ZE == 0, the result is 1/2.  */
        ret = float64_set_sign(float64_half, float64_is_neg(arg));
    }

    return ret;
}

/* frsqrte  - frsqrte. */
float64 helper_frsqrte(CPUPPCState *env, float64 arg)
{
    /* "Estimate" the reciprocal with actual division.  */
    float64 rets = float64_sqrt(arg, &env->fp_status);
    float64 retd = float64_div(float64_one, rets, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_sqrt(env, flags, 1, GETPC());
    }
    if (unlikely(flags & float_flag_divbyzero)) {
        /* Reciprocal of (square root of) zero.  */
        float_zero_divide_excp(env, GETPC());
    }

    return retd;
}

/* frsqrtes  - frsqrtes. */
float64 helper_frsqrtes(CPUPPCState *env, float64 arg)
{
    /* "Estimate" the reciprocal with actual division.  */
    float64 rets = float64_sqrt(arg, &env->fp_status);
    float64 retd = float64r32_div(float64_one, rets, &env->fp_status);
    int flags = get_float_exception_flags(&env->fp_status);

    if (unlikely(flags & float_flag_invalid)) {
        float_invalid_op_sqrt(env, flags, 1, GETPC());
    }
    if (unlikely(flags & float_flag_divbyzero)) {
        /* Reciprocal of (square root of) zero.  */
        float_zero_divide_excp(env, GETPC());
    }

    return retd;
}

/* fsel - fsel. */
uint64_t helper_fsel(CPUPPCState *env, uint64_t arg1, uint64_t arg2,
                     uint64_t arg3)
{
    CPU_DoubleU farg1;

    farg1.ll = arg1;

    if ((!float64_is_neg(farg1.d) || float64_is_zero(farg1.d)) &&
        !float64_is_any_nan(farg1.d)) {
        return arg2;
    } else {
        return arg3;
    }
}

uint32_t helper_ftdiv(uint64_t fra, uint64_t frb)
{
    int fe_flag = 0;
    int fg_flag = 0;

    if (unlikely(float64_is_infinity(fra) ||
                 float64_is_infinity(frb) ||
                 float64_is_zero(frb))) {
        fe_flag = 1;
        fg_flag = 1;
    } else {
        int e_a = ppc_float64_get_unbiased_exp(fra);
        int e_b = ppc_float64_get_unbiased_exp(frb);

        if (unlikely(float64_is_any_nan(fra) ||
                     float64_is_any_nan(frb))) {
            fe_flag = 1;
        } else if ((e_b <= -1022) || (e_b >= 1021)) {
            fe_flag = 1;
        } else if (!float64_is_zero(fra) &&
                   (((e_a - e_b) >= 1023) ||
                    ((e_a - e_b) <= -1021) ||
                    (e_a <= -970))) {
            fe_flag = 1;
        }

        if (unlikely(float64_is_zero_or_denormal(frb))) {
            /* XB is not zero because of the above check and */
            /* so must be denormalized.                      */
            fg_flag = 1;
        }
    }

    return 0x8 | (fg_flag ? 4 : 0) | (fe_flag ? 2 : 0);
}

uint32_t helper_ftsqrt(uint64_t frb)
{
    int fe_flag = 0;
    int fg_flag = 0;

    if (unlikely(float64_is_infinity(frb) || float64_is_zero(frb))) {
        fe_flag = 1;
        fg_flag = 1;
    } else {
        int e_b = ppc_float64_get_unbiased_exp(frb);

        if (unlikely(float64_is_any_nan(frb))) {
            fe_flag = 1;
        } else if (unlikely(float64_is_zero(frb))) {
            fe_flag = 1;
        } else if (unlikely(float64_is_neg(frb))) {
            fe_flag = 1;
        } else if (!float64_is_zero(frb) && (e_b <= (-1022 + 52))) {
            fe_flag = 1;
        }

        if (unlikely(float64_is_zero_or_denormal(frb))) {
            /* XB is not zero because of the above check and */
            /* therefore must be denormalized.               */
            fg_flag = 1;
        }
    }

    return 0x8 | (fg_flag ? 4 : 0) | (fe_flag ? 2 : 0);
}

void helper_fcmpu(CPUPPCState *env, uint64_t arg1, uint64_t arg2,
                  uint32_t crfD)
{
    CPU_DoubleU farg1, farg2;
    uint32_t ret = 0;

    farg1.ll = arg1;
    farg2.ll = arg2;

    if (unlikely(float64_is_any_nan(farg1.d) ||
                 float64_is_any_nan(farg2.d))) {
        ret = 0x01UL;
    } else if (float64_lt(farg1.d, farg2.d, &env->fp_status)) {
        ret = 0x08UL;
    } else if (!float64_le(farg1.d, farg2.d, &env->fp_status)) {
        ret = 0x04UL;
    } else {
        ret = 0x02UL;
    }

    env->fpscr &= ~FP_FPCC;
    env->fpscr |= ret << FPSCR_FPCC;
    env->crf[crfD] = ret;
    if (unlikely(ret == 0x01UL
                 && (float64_is_signaling_nan(farg1.d, &env->fp_status) ||
                     float64_is_signaling_nan(farg2.d, &env->fp_status)))) {
        /* sNaN comparison */
        float_invalid_op_vxsnan(env, GETPC());
    }
}

void helper_fcmpo(CPUPPCState *env, uint64_t arg1, uint64_t arg2,
                  uint32_t crfD)
{
    CPU_DoubleU farg1, farg2;
    uint32_t ret = 0;

    farg1.ll = arg1;
    farg2.ll = arg2;

    if (unlikely(float64_is_any_nan(farg1.d) ||
                 float64_is_any_nan(farg2.d))) {
        ret = 0x01UL;
    } else if (float64_lt(farg1.d, farg2.d, &env->fp_status)) {
        ret = 0x08UL;
    } else if (!float64_le(farg1.d, farg2.d, &env->fp_status)) {
        ret = 0x04UL;
    } else {
        ret = 0x02UL;
    }

    env->fpscr &= ~FP_FPCC;
    env->fpscr |= ret << FPSCR_FPCC;
    env->crf[crfD] = (uint32_t) ret;
    if (unlikely(ret == 0x01UL)) {
        float_invalid_op_vxvc(env, 1, GETPC());
        if (float64_is_signaling_nan(farg1.d, &env->fp_status) ||
            float64_is_signaling_nan(farg2.d, &env->fp_status)) {
            /* sNaN comparison */
            float_invalid_op_vxsnan(env, GETPC());
        }
    }
}

/* Single-precision floating-point conversions */
static inline uint32_t efscfsi(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;

    u.f = int32_to_float32(val, &env->vec_status);

    return u.l;
}

static inline uint32_t efscfui(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;

    u.f = uint32_to_float32(val, &env->vec_status);

    return u.l;
}

static inline int32_t efsctsi(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;

    u.l = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float32_is_quiet_nan(u.f, &env->vec_status))) {
        return 0;
    }

    return float32_to_int32(u.f, &env->vec_status);
}

static inline uint32_t efsctui(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;

    u.l = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float32_is_quiet_nan(u.f, &env->vec_status))) {
        return 0;
    }

    return float32_to_uint32(u.f, &env->vec_status);
}

static inline uint32_t efsctsiz(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;

    u.l = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float32_is_quiet_nan(u.f, &env->vec_status))) {
        return 0;
    }

    return float32_to_int32_round_to_zero(u.f, &env->vec_status);
}

static inline uint32_t efsctuiz(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;

    u.l = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float32_is_quiet_nan(u.f, &env->vec_status))) {
        return 0;
    }

    return float32_to_uint32_round_to_zero(u.f, &env->vec_status);
}

static inline uint32_t efscfsf(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;
    float32 tmp;

    u.f = int32_to_float32(val, &env->vec_status);
    tmp = int64_to_float32(1ULL << 32, &env->vec_status);
    u.f = float32_div(u.f, tmp, &env->vec_status);

    return u.l;
}

static inline uint32_t efscfuf(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;
    float32 tmp;

    u.f = uint32_to_float32(val, &env->vec_status);
    tmp = uint64_to_float32(1ULL << 32, &env->vec_status);
    u.f = float32_div(u.f, tmp, &env->vec_status);

    return u.l;
}

static inline uint32_t efsctsf(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;
    float32 tmp;

    u.l = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float32_is_quiet_nan(u.f, &env->vec_status))) {
        return 0;
    }
    tmp = uint64_to_float32(1ULL << 32, &env->vec_status);
    u.f = float32_mul(u.f, tmp, &env->vec_status);

    return float32_to_int32(u.f, &env->vec_status);
}

static inline uint32_t efsctuf(CPUPPCState *env, uint32_t val)
{
    CPU_FloatU u;
    float32 tmp;

    u.l = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float32_is_quiet_nan(u.f, &env->vec_status))) {
        return 0;
    }
    tmp = uint64_to_float32(1ULL << 32, &env->vec_status);
    u.f = float32_mul(u.f, tmp, &env->vec_status);

    return float32_to_uint32(u.f, &env->vec_status);
}

#define HELPER_SPE_SINGLE_CONV(name)                              \
    uint32_t helper_e##name(CPUPPCState *env, uint32_t val)       \
    {                                                             \
        return e##name(env, val);                                 \
    }
/* efscfsi */
HELPER_SPE_SINGLE_CONV(fscfsi);
/* efscfui */
HELPER_SPE_SINGLE_CONV(fscfui);
/* efscfuf */
HELPER_SPE_SINGLE_CONV(fscfuf);
/* efscfsf */
HELPER_SPE_SINGLE_CONV(fscfsf);
/* efsctsi */
HELPER_SPE_SINGLE_CONV(fsctsi);
/* efsctui */
HELPER_SPE_SINGLE_CONV(fsctui);
/* efsctsiz */
HELPER_SPE_SINGLE_CONV(fsctsiz);
/* efsctuiz */
HELPER_SPE_SINGLE_CONV(fsctuiz);
/* efsctsf */
HELPER_SPE_SINGLE_CONV(fsctsf);
/* efsctuf */
HELPER_SPE_SINGLE_CONV(fsctuf);

#define HELPER_SPE_VECTOR_CONV(name)                            \
    uint64_t helper_ev##name(CPUPPCState *env, uint64_t val)    \
    {                                                           \
        return ((uint64_t)e##name(env, val >> 32) << 32) |      \
            (uint64_t)e##name(env, val);                        \
    }
/* evfscfsi */
HELPER_SPE_VECTOR_CONV(fscfsi);
/* evfscfui */
HELPER_SPE_VECTOR_CONV(fscfui);
/* evfscfuf */
HELPER_SPE_VECTOR_CONV(fscfuf);
/* evfscfsf */
HELPER_SPE_VECTOR_CONV(fscfsf);
/* evfsctsi */
HELPER_SPE_VECTOR_CONV(fsctsi);
/* evfsctui */
HELPER_SPE_VECTOR_CONV(fsctui);
/* evfsctsiz */
HELPER_SPE_VECTOR_CONV(fsctsiz);
/* evfsctuiz */
HELPER_SPE_VECTOR_CONV(fsctuiz);
/* evfsctsf */
HELPER_SPE_VECTOR_CONV(fsctsf);
/* evfsctuf */
HELPER_SPE_VECTOR_CONV(fsctuf);

/* Single-precision floating-point arithmetic */
static inline uint32_t efsadd(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    u1.f = float32_add(u1.f, u2.f, &env->vec_status);
    return u1.l;
}

static inline uint32_t efssub(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    u1.f = float32_sub(u1.f, u2.f, &env->vec_status);
    return u1.l;
}

static inline uint32_t efsmul(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    u1.f = float32_mul(u1.f, u2.f, &env->vec_status);
    return u1.l;
}

static inline uint32_t efsdiv(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    u1.f = float32_div(u1.f, u2.f, &env->vec_status);
    return u1.l;
}

#define HELPER_SPE_SINGLE_ARITH(name)                                   \
    uint32_t helper_e##name(CPUPPCState *env, uint32_t op1, uint32_t op2) \
    {                                                                   \
        return e##name(env, op1, op2);                                  \
    }
/* efsadd */
HELPER_SPE_SINGLE_ARITH(fsadd);
/* efssub */
HELPER_SPE_SINGLE_ARITH(fssub);
/* efsmul */
HELPER_SPE_SINGLE_ARITH(fsmul);
/* efsdiv */
HELPER_SPE_SINGLE_ARITH(fsdiv);

#define HELPER_SPE_VECTOR_ARITH(name)                                   \
    uint64_t helper_ev##name(CPUPPCState *env, uint64_t op1, uint64_t op2) \
    {                                                                   \
        return ((uint64_t)e##name(env, op1 >> 32, op2 >> 32) << 32) |   \
            (uint64_t)e##name(env, op1, op2);                           \
    }
/* evfsadd */
HELPER_SPE_VECTOR_ARITH(fsadd);
/* evfssub */
HELPER_SPE_VECTOR_ARITH(fssub);
/* evfsmul */
HELPER_SPE_VECTOR_ARITH(fsmul);
/* evfsdiv */
HELPER_SPE_VECTOR_ARITH(fsdiv);

/* Single-precision floating-point comparisons */
static inline uint32_t efscmplt(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    return float32_lt(u1.f, u2.f, &env->vec_status) ? 4 : 0;
}

static inline uint32_t efscmpgt(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    return float32_le(u1.f, u2.f, &env->vec_status) ? 0 : 4;
}

static inline uint32_t efscmpeq(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    CPU_FloatU u1, u2;

    u1.l = op1;
    u2.l = op2;
    return float32_eq(u1.f, u2.f, &env->vec_status) ? 4 : 0;
}

static inline uint32_t efststlt(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    /* XXX: TODO: ignore special values (NaN, infinites, ...) */
    return efscmplt(env, op1, op2);
}

static inline uint32_t efststgt(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    /* XXX: TODO: ignore special values (NaN, infinites, ...) */
    return efscmpgt(env, op1, op2);
}

static inline uint32_t efststeq(CPUPPCState *env, uint32_t op1, uint32_t op2)
{
    /* XXX: TODO: ignore special values (NaN, infinites, ...) */
    return efscmpeq(env, op1, op2);
}

#define HELPER_SINGLE_SPE_CMP(name)                                     \
    uint32_t helper_e##name(CPUPPCState *env, uint32_t op1, uint32_t op2) \
    {                                                                   \
        return e##name(env, op1, op2);                                  \
    }
/* efststlt */
HELPER_SINGLE_SPE_CMP(fststlt);
/* efststgt */
HELPER_SINGLE_SPE_CMP(fststgt);
/* efststeq */
HELPER_SINGLE_SPE_CMP(fststeq);
/* efscmplt */
HELPER_SINGLE_SPE_CMP(fscmplt);
/* efscmpgt */
HELPER_SINGLE_SPE_CMP(fscmpgt);
/* efscmpeq */
HELPER_SINGLE_SPE_CMP(fscmpeq);

static inline uint32_t evcmp_merge(int t0, int t1)
{
    return (t0 << 3) | (t1 << 2) | ((t0 | t1) << 1) | (t0 & t1);
}

#define HELPER_VECTOR_SPE_CMP(name)                                     \
    uint32_t helper_ev##name(CPUPPCState *env, uint64_t op1, uint64_t op2) \
    {                                                                   \
        return evcmp_merge(e##name(env, op1 >> 32, op2 >> 32),          \
                           e##name(env, op1, op2));                     \
    }
/* evfststlt */
HELPER_VECTOR_SPE_CMP(fststlt);
/* evfststgt */
HELPER_VECTOR_SPE_CMP(fststgt);
/* evfststeq */
HELPER_VECTOR_SPE_CMP(fststeq);
/* evfscmplt */
HELPER_VECTOR_SPE_CMP(fscmplt);
/* evfscmpgt */
HELPER_VECTOR_SPE_CMP(fscmpgt);
/* evfscmpeq */
HELPER_VECTOR_SPE_CMP(fscmpeq);

/* Double-precision floating-point conversion */
uint64_t helper_efdcfsi(CPUPPCState *env, uint32_t val)
{
    CPU_DoubleU u;

    u.d = int32_to_float64(val, &env->vec_status);

    return u.ll;
}

uint64_t helper_efdcfsid(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.d = int64_to_float64(val, &env->vec_status);

    return u.ll;
}

uint64_t helper_efdcfui(CPUPPCState *env, uint32_t val)
{
    CPU_DoubleU u;

    u.d = uint32_to_float64(val, &env->vec_status);

    return u.ll;
}

uint64_t helper_efdcfuid(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.d = uint64_to_float64(val, &env->vec_status);

    return u.ll;
}

uint32_t helper_efdctsi(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }

    return float64_to_int32(u.d, &env->vec_status);
}

uint32_t helper_efdctui(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }

    return float64_to_uint32(u.d, &env->vec_status);
}

uint32_t helper_efdctsiz(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }

    return float64_to_int32_round_to_zero(u.d, &env->vec_status);
}

uint64_t helper_efdctsidz(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }

    return float64_to_int64_round_to_zero(u.d, &env->vec_status);
}

uint32_t helper_efdctuiz(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }

    return float64_to_uint32_round_to_zero(u.d, &env->vec_status);
}

uint64_t helper_efdctuidz(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }

    return float64_to_uint64_round_to_zero(u.d, &env->vec_status);
}

uint64_t helper_efdcfsf(CPUPPCState *env, uint32_t val)
{
    CPU_DoubleU u;
    float64 tmp;

    u.d = int32_to_float64(val, &env->vec_status);
    tmp = int64_to_float64(1ULL << 32, &env->vec_status);
    u.d = float64_div(u.d, tmp, &env->vec_status);

    return u.ll;
}

uint64_t helper_efdcfuf(CPUPPCState *env, uint32_t val)
{
    CPU_DoubleU u;
    float64 tmp;

    u.d = uint32_to_float64(val, &env->vec_status);
    tmp = int64_to_float64(1ULL << 32, &env->vec_status);
    u.d = float64_div(u.d, tmp, &env->vec_status);

    return u.ll;
}

uint32_t helper_efdctsf(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;
    float64 tmp;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }
    tmp = uint64_to_float64(1ULL << 32, &env->vec_status);
    u.d = float64_mul(u.d, tmp, &env->vec_status);

    return float64_to_int32(u.d, &env->vec_status);
}

uint32_t helper_efdctuf(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u;
    float64 tmp;

    u.ll = val;
    /* NaN are not treated the same way IEEE 754 does */
    if (unlikely(float64_is_any_nan(u.d))) {
        return 0;
    }
    tmp = uint64_to_float64(1ULL << 32, &env->vec_status);
    u.d = float64_mul(u.d, tmp, &env->vec_status);

    return float64_to_uint32(u.d, &env->vec_status);
}

uint32_t helper_efscfd(CPUPPCState *env, uint64_t val)
{
    CPU_DoubleU u1;
    CPU_FloatU u2;

    u1.ll = val;
    u2.f = float64_to_float32(u1.d, &env->vec_status);

    return u2.l;
}

uint64_t helper_efdcfs(CPUPPCState *env, uint32_t val)
{
    CPU_DoubleU u2;
    CPU_FloatU u1;

    u1.l = val;
    u2.d = float32_to_float64(u1.f, &env->vec_status);

    return u2.ll;
}

/* Double precision fixed-point arithmetic */
uint64_t helper_efdadd(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    u1.d = float64_add(u1.d, u2.d, &env->vec_status);
    return u1.ll;
}

uint64_t helper_efdsub(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    u1.d = float64_sub(u1.d, u2.d, &env->vec_status);
    return u1.ll;
}

uint64_t helper_efdmul(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    u1.d = float64_mul(u1.d, u2.d, &env->vec_status);
    return u1.ll;
}

uint64_t helper_efddiv(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    u1.d = float64_div(u1.d, u2.d, &env->vec_status);
    return u1.ll;
}

/* Double precision floating point helpers */
uint32_t helper_efdtstlt(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    return float64_lt(u1.d, u2.d, &env->vec_status) ? 4 : 0;
}

uint32_t helper_efdtstgt(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    return float64_le(u1.d, u2.d, &env->vec_status) ? 0 : 4;
}

uint32_t helper_efdtsteq(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    CPU_DoubleU u1, u2;

    u1.ll = op1;
    u2.ll = op2;
    return float64_eq_quiet(u1.d, u2.d, &env->vec_status) ? 4 : 0;
}

uint32_t helper_efdcmplt(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    /* XXX: TODO: test special values (NaN, infinites, ...) */
    return helper_efdtstlt(env, op1, op2);
}

uint32_t helper_efdcmpgt(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    /* XXX: TODO: test special values (NaN, infinites, ...) */
    return helper_efdtstgt(env, op1, op2);
}

uint32_t helper_efdcmpeq(CPUPPCState *env, uint64_t op1, uint64_t op2)
{
    /* XXX: TODO: test special values (NaN, infinites, ...) */
    return helper_efdtsteq(env, op1, op2);
}

#define float64_to_float64(x, env) x


/*
 * VSX_ADD_SUB - VSX floating point add/subtract
 *   name  - instruction mnemonic
 *   op    - operation (add or sub)
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   sfprf - set FPRF
 */
#define VSX_ADD_SUB(name, op, nels, tp, fld, sfprf, r2sp)                    \
void helper_##name(CPUPPCState *env, ppc_vsr_t *xt,                          \
                   ppc_vsr_t *xa, ppc_vsr_t *xb)                             \
{                                                                            \
    ppc_vsr_t t = *xt;                                                       \
    int i;                                                                   \
                                                                             \
    helper_reset_fpstatus(env);                                              \
                                                                             \
    for (i = 0; i < nels; i++) {                                             \
        float_status tstat = env->fp_status;                                 \
        set_float_exception_flags(0, &tstat);                                \
        t.fld = tp##_##op(xa->fld, xb->fld, &tstat);                         \
        env->fp_status.float_exception_flags |= tstat.float_exception_flags; \
                                                                             \
        if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {    \
            float_invalid_op_addsub(env, tstat.float_exception_flags,        \
                                    sfprf, GETPC());                         \
        }                                                                    \
                                                                             \
        if (r2sp) {                                                          \
            t.fld = do_frsp(env, t.fld, GETPC());                            \
        }                                                                    \
                                                                             \
        if (sfprf) {                                                         \
            helper_compute_fprf_float64(env, t.fld);                         \
        }                                                                    \
    }                                                                        \
    *xt = t;                                                                 \
    do_float_check_status(env, GETPC());                                     \
}

VSX_ADD_SUB(xsadddp, add, 1, float64, VsrD(0), 1, 0)
VSX_ADD_SUB(xsaddsp, add, 1, float64, VsrD(0), 1, 1)
VSX_ADD_SUB(xvadddp, add, 2, float64, VsrD(i), 0, 0)
VSX_ADD_SUB(xvaddsp, add, 4, float32, VsrW(i), 0, 0)
VSX_ADD_SUB(xssubdp, sub, 1, float64, VsrD(0), 1, 0)
VSX_ADD_SUB(xssubsp, sub, 1, float64, VsrD(0), 1, 1)
VSX_ADD_SUB(xvsubdp, sub, 2, float64, VsrD(i), 0, 0)
VSX_ADD_SUB(xvsubsp, sub, 4, float32, VsrW(i), 0, 0)

void helper_xsaddqp(CPUPPCState *env, uint32_t opcode,
                    ppc_vsr_t *xt, ppc_vsr_t *xa, ppc_vsr_t *xb)
{
    ppc_vsr_t t = *xt;
    float_status tstat;

    helper_reset_fpstatus(env);

    tstat = env->fp_status;
    if (unlikely(Rc(opcode) != 0)) {
        tstat.float_rounding_mode = float_round_to_odd;
    }

    set_float_exception_flags(0, &tstat);
    t.f128 = float128_add(xa->f128, xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {
        float_invalid_op_addsub(env, tstat.float_exception_flags, 1, GETPC());
    }

    helper_compute_fprf_float128(env, t.f128);

    *xt = t;
    do_float_check_status(env, GETPC());
}

/*
 * VSX_MUL - VSX floating point multiply
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   sfprf - set FPRF
 */
#define VSX_MUL(op, nels, tp, fld, sfprf, r2sp)                              \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt,                            \
                 ppc_vsr_t *xa, ppc_vsr_t *xb)                               \
{                                                                            \
    ppc_vsr_t t = *xt;                                                       \
    int i;                                                                   \
                                                                             \
    helper_reset_fpstatus(env);                                              \
                                                                             \
    for (i = 0; i < nels; i++) {                                             \
        float_status tstat = env->fp_status;                                 \
        set_float_exception_flags(0, &tstat);                                \
        t.fld = tp##_mul(xa->fld, xb->fld, &tstat);                          \
        env->fp_status.float_exception_flags |= tstat.float_exception_flags; \
                                                                             \
        if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {    \
            float_invalid_op_mul(env, tstat.float_exception_flags,           \
                                 sfprf, GETPC());                            \
        }                                                                    \
                                                                             \
        if (r2sp) {                                                          \
            t.fld = do_frsp(env, t.fld, GETPC());                            \
        }                                                                    \
                                                                             \
        if (sfprf) {                                                         \
            helper_compute_fprf_float64(env, t.fld);                         \
        }                                                                    \
    }                                                                        \
                                                                             \
    *xt = t;                                                                 \
    do_float_check_status(env, GETPC());                                     \
}

VSX_MUL(xsmuldp, 1, float64, VsrD(0), 1, 0)
VSX_MUL(xsmulsp, 1, float64, VsrD(0), 1, 1)
VSX_MUL(xvmuldp, 2, float64, VsrD(i), 0, 0)
VSX_MUL(xvmulsp, 4, float32, VsrW(i), 0, 0)

void helper_xsmulqp(CPUPPCState *env, uint32_t opcode,
                    ppc_vsr_t *xt, ppc_vsr_t *xa, ppc_vsr_t *xb)
{
    ppc_vsr_t t = *xt;
    float_status tstat;

    helper_reset_fpstatus(env);
    tstat = env->fp_status;
    if (unlikely(Rc(opcode) != 0)) {
        tstat.float_rounding_mode = float_round_to_odd;
    }

    set_float_exception_flags(0, &tstat);
    t.f128 = float128_mul(xa->f128, xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {
        float_invalid_op_mul(env, tstat.float_exception_flags, 1, GETPC());
    }
    helper_compute_fprf_float128(env, t.f128);

    *xt = t;
    do_float_check_status(env, GETPC());
}

/*
 * VSX_DIV - VSX floating point divide
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   sfprf - set FPRF
 */
#define VSX_DIV(op, nels, tp, fld, sfprf, r2sp)                               \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt,                             \
                 ppc_vsr_t *xa, ppc_vsr_t *xb)                                \
{                                                                             \
    ppc_vsr_t t = *xt;                                                        \
    int i;                                                                    \
                                                                              \
    helper_reset_fpstatus(env);                                               \
                                                                              \
    for (i = 0; i < nels; i++) {                                              \
        float_status tstat = env->fp_status;                                  \
        set_float_exception_flags(0, &tstat);                                 \
        t.fld = tp##_div(xa->fld, xb->fld, &tstat);                           \
        env->fp_status.float_exception_flags |= tstat.float_exception_flags;  \
                                                                              \
        if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {     \
            float_invalid_op_div(env, tstat.float_exception_flags,            \
                                 sfprf, GETPC());                             \
        }                                                                     \
        if (unlikely(tstat.float_exception_flags & float_flag_divbyzero)) {   \
            float_zero_divide_excp(env, GETPC());                             \
        }                                                                     \
                                                                              \
        if (r2sp) {                                                           \
            t.fld = do_frsp(env, t.fld, GETPC());                             \
        }                                                                     \
                                                                              \
        if (sfprf) {                                                          \
            helper_compute_fprf_float64(env, t.fld);                          \
        }                                                                     \
    }                                                                         \
                                                                              \
    *xt = t;                                                                  \
    do_float_check_status(env, GETPC());                                      \
}

VSX_DIV(xsdivdp, 1, float64, VsrD(0), 1, 0)
VSX_DIV(xsdivsp, 1, float64, VsrD(0), 1, 1)
VSX_DIV(xvdivdp, 2, float64, VsrD(i), 0, 0)
VSX_DIV(xvdivsp, 4, float32, VsrW(i), 0, 0)

void helper_xsdivqp(CPUPPCState *env, uint32_t opcode,
                    ppc_vsr_t *xt, ppc_vsr_t *xa, ppc_vsr_t *xb)
{
    ppc_vsr_t t = *xt;
    float_status tstat;

    helper_reset_fpstatus(env);
    tstat = env->fp_status;
    if (unlikely(Rc(opcode) != 0)) {
        tstat.float_rounding_mode = float_round_to_odd;
    }

    set_float_exception_flags(0, &tstat);
    t.f128 = float128_div(xa->f128, xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {
        float_invalid_op_div(env, tstat.float_exception_flags, 1, GETPC());
    }
    if (unlikely(tstat.float_exception_flags & float_flag_divbyzero)) {
        float_zero_divide_excp(env, GETPC());
    }

    helper_compute_fprf_float128(env, t.f128);
    *xt = t;
    do_float_check_status(env, GETPC());
}

/*
 * VSX_RE  - VSX floating point reciprocal estimate
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   sfprf - set FPRF
 */
#define VSX_RE(op, nels, tp, fld, sfprf, r2sp)                                \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)              \
{                                                                             \
    ppc_vsr_t t = *xt;                                                        \
    int i;                                                                    \
                                                                              \
    helper_reset_fpstatus(env);                                               \
                                                                              \
    for (i = 0; i < nels; i++) {                                              \
        if (unlikely(tp##_is_signaling_nan(xb->fld, &env->fp_status))) {      \
            float_invalid_op_vxsnan(env, GETPC());                            \
        }                                                                     \
        t.fld = tp##_div(tp##_one, xb->fld, &env->fp_status);                 \
                                                                              \
        if (r2sp) {                                                           \
            t.fld = do_frsp(env, t.fld, GETPC());                             \
        }                                                                     \
                                                                              \
        if (sfprf) {                                                          \
            helper_compute_fprf_float64(env, t.fld);                          \
        }                                                                     \
    }                                                                         \
                                                                              \
    *xt = t;                                                                  \
    do_float_check_status(env, GETPC());                                      \
}

VSX_RE(xsredp, 1, float64, VsrD(0), 1, 0)
VSX_RE(xsresp, 1, float64, VsrD(0), 1, 1)
VSX_RE(xvredp, 2, float64, VsrD(i), 0, 0)
VSX_RE(xvresp, 4, float32, VsrW(i), 0, 0)

/*
 * VSX_SQRT - VSX floating point square root
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   sfprf - set FPRF
 */
#define VSX_SQRT(op, nels, tp, fld, sfprf, r2sp)                             \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)             \
{                                                                            \
    ppc_vsr_t t = *xt;                                                       \
    int i;                                                                   \
                                                                             \
    helper_reset_fpstatus(env);                                              \
                                                                             \
    for (i = 0; i < nels; i++) {                                             \
        float_status tstat = env->fp_status;                                 \
        set_float_exception_flags(0, &tstat);                                \
        t.fld = tp##_sqrt(xb->fld, &tstat);                                  \
        env->fp_status.float_exception_flags |= tstat.float_exception_flags; \
                                                                             \
        if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {    \
            float_invalid_op_sqrt(env, tstat.float_exception_flags,          \
                                  sfprf, GETPC());                           \
        }                                                                    \
                                                                             \
        if (r2sp) {                                                          \
            t.fld = do_frsp(env, t.fld, GETPC());                            \
        }                                                                    \
                                                                             \
        if (sfprf) {                                                         \
            helper_compute_fprf_float64(env, t.fld);                         \
        }                                                                    \
    }                                                                        \
                                                                             \
    *xt = t;                                                                 \
    do_float_check_status(env, GETPC());                                     \
}

VSX_SQRT(xssqrtdp, 1, float64, VsrD(0), 1, 0)
VSX_SQRT(xssqrtsp, 1, float64, VsrD(0), 1, 1)
VSX_SQRT(xvsqrtdp, 2, float64, VsrD(i), 0, 0)
VSX_SQRT(xvsqrtsp, 4, float32, VsrW(i), 0, 0)

/*
 *VSX_RSQRTE - VSX floating point reciprocal square root estimate
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   sfprf - set FPRF
 */
#define VSX_RSQRTE(op, nels, tp, fld, sfprf, r2sp)                           \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)             \
{                                                                            \
    ppc_vsr_t t = *xt;                                                       \
    int i;                                                                   \
                                                                             \
    helper_reset_fpstatus(env);                                              \
                                                                             \
    for (i = 0; i < nels; i++) {                                             \
        float_status tstat = env->fp_status;                                 \
        set_float_exception_flags(0, &tstat);                                \
        t.fld = tp##_sqrt(xb->fld, &tstat);                                  \
        t.fld = tp##_div(tp##_one, t.fld, &tstat);                           \
        env->fp_status.float_exception_flags |= tstat.float_exception_flags; \
        if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {    \
            float_invalid_op_sqrt(env, tstat.float_exception_flags,          \
                                  sfprf, GETPC());                           \
        }                                                                    \
        if (r2sp) {                                                          \
            t.fld = do_frsp(env, t.fld, GETPC());                            \
        }                                                                    \
                                                                             \
        if (sfprf) {                                                         \
            helper_compute_fprf_float64(env, t.fld);                         \
        }                                                                    \
    }                                                                        \
                                                                             \
    *xt = t;                                                                 \
    do_float_check_status(env, GETPC());                                     \
}

VSX_RSQRTE(xsrsqrtedp, 1, float64, VsrD(0), 1, 0)
VSX_RSQRTE(xsrsqrtesp, 1, float64, VsrD(0), 1, 1)
VSX_RSQRTE(xvrsqrtedp, 2, float64, VsrD(i), 0, 0)
VSX_RSQRTE(xvrsqrtesp, 4, float32, VsrW(i), 0, 0)

/*
 * VSX_TDIV - VSX floating point test for divide
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   emin  - minimum unbiased exponent
 *   emax  - maximum unbiased exponent
 *   nbits - number of fraction bits
 */
#define VSX_TDIV(op, nels, tp, fld, emin, emax, nbits)                  \
void helper_##op(CPUPPCState *env, uint32_t opcode,                     \
                 ppc_vsr_t *xa, ppc_vsr_t *xb)                          \
{                                                                       \
    int i;                                                              \
    int fe_flag = 0;                                                    \
    int fg_flag = 0;                                                    \
                                                                        \
    for (i = 0; i < nels; i++) {                                        \
        if (unlikely(tp##_is_infinity(xa->fld) ||                       \
                     tp##_is_infinity(xb->fld) ||                       \
                     tp##_is_zero(xb->fld))) {                          \
            fe_flag = 1;                                                \
            fg_flag = 1;                                                \
        } else {                                                        \
            int e_a = ppc_##tp##_get_unbiased_exp(xa->fld);             \
            int e_b = ppc_##tp##_get_unbiased_exp(xb->fld);             \
                                                                        \
            if (unlikely(tp##_is_any_nan(xa->fld) ||                    \
                         tp##_is_any_nan(xb->fld))) {                   \
                fe_flag = 1;                                            \
            } else if ((e_b <= emin) || (e_b >= (emax - 2))) {          \
                fe_flag = 1;                                            \
            } else if (!tp##_is_zero(xa->fld) &&                        \
                       (((e_a - e_b) >= emax) ||                        \
                        ((e_a - e_b) <= (emin + 1)) ||                  \
                        (e_a <= (emin + nbits)))) {                     \
                fe_flag = 1;                                            \
            }                                                           \
                                                                        \
            if (unlikely(tp##_is_zero_or_denormal(xb->fld))) {          \
                /*                                                      \
                 * XB is not zero because of the above check and so     \
                 * must be denormalized.                                \
                 */                                                     \
                fg_flag = 1;                                            \
            }                                                           \
        }                                                               \
    }                                                                   \
                                                                        \
    env->crf[BF(opcode)] = 0x8 | (fg_flag ? 4 : 0) | (fe_flag ? 2 : 0); \
}

VSX_TDIV(xstdivdp, 1, float64, VsrD(0), -1022, 1023, 52)
VSX_TDIV(xvtdivdp, 2, float64, VsrD(i), -1022, 1023, 52)
VSX_TDIV(xvtdivsp, 4, float32, VsrW(i), -126, 127, 23)

/*
 * VSX_TSQRT - VSX floating point test for square root
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   emin  - minimum unbiased exponent
 *   emax  - maximum unbiased exponent
 *   nbits - number of fraction bits
 */
#define VSX_TSQRT(op, nels, tp, fld, emin, nbits)                       \
void helper_##op(CPUPPCState *env, uint32_t opcode, ppc_vsr_t *xb)      \
{                                                                       \
    int i;                                                              \
    int fe_flag = 0;                                                    \
    int fg_flag = 0;                                                    \
                                                                        \
    for (i = 0; i < nels; i++) {                                        \
        if (unlikely(tp##_is_infinity(xb->fld) ||                       \
                     tp##_is_zero(xb->fld))) {                          \
            fe_flag = 1;                                                \
            fg_flag = 1;                                                \
        } else {                                                        \
            int e_b = ppc_##tp##_get_unbiased_exp(xb->fld);             \
                                                                        \
            if (unlikely(tp##_is_any_nan(xb->fld))) {                   \
                fe_flag = 1;                                            \
            } else if (unlikely(tp##_is_zero(xb->fld))) {               \
                fe_flag = 1;                                            \
            } else if (unlikely(tp##_is_neg(xb->fld))) {                \
                fe_flag = 1;                                            \
            } else if (!tp##_is_zero(xb->fld) &&                        \
                       (e_b <= (emin + nbits))) {                       \
                fe_flag = 1;                                            \
            }                                                           \
                                                                        \
            if (unlikely(tp##_is_zero_or_denormal(xb->fld))) {          \
                /*                                                      \
                 * XB is not zero because of the above check and        \
                 * therefore must be denormalized.                      \
                 */                                                     \
                fg_flag = 1;                                            \
            }                                                           \
        }                                                               \
    }                                                                   \
                                                                        \
    env->crf[BF(opcode)] = 0x8 | (fg_flag ? 4 : 0) | (fe_flag ? 2 : 0); \
}

VSX_TSQRT(xstsqrtdp, 1, float64, VsrD(0), -1022, 52)
VSX_TSQRT(xvtsqrtdp, 2, float64, VsrD(i), -1022, 52)
VSX_TSQRT(xvtsqrtsp, 4, float32, VsrW(i), -126, 23)

/*
 * VSX_MADD - VSX floating point muliply/add variations
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   maddflgs - flags for the float*muladd routine that control the
 *           various forms (madd, msub, nmadd, nmsub)
 *   sfprf - set FPRF
 *   r2sp  - round intermediate double precision result to single precision
 */
#define VSX_MADD(op, nels, tp, fld, maddflgs, sfprf, r2sp)                    \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt,                             \
                 ppc_vsr_t *s1, ppc_vsr_t *s2, ppc_vsr_t *s3)                 \
{                                                                             \
    ppc_vsr_t t = *xt;                                                        \
    int i;                                                                    \
                                                                              \
    helper_reset_fpstatus(env);                                               \
                                                                              \
    for (i = 0; i < nels; i++) {                                              \
        float_status tstat = env->fp_status;                                  \
        set_float_exception_flags(0, &tstat);                                 \
        if (r2sp && (tstat.float_rounding_mode == float_round_nearest_even)) {\
            /*                                                                \
             * Avoid double rounding errors by rounding the intermediate      \
             * result to odd.                                                 \
             */                                                               \
            set_float_rounding_mode(float_round_to_zero, &tstat);             \
            t.fld = tp##_muladd(s1->fld, s3->fld, s2->fld,                    \
                                maddflgs, &tstat);                            \
            t.fld |= (get_float_exception_flags(&tstat) &                     \
                      float_flag_inexact) != 0;                               \
        } else {                                                              \
            t.fld = tp##_muladd(s1->fld, s3->fld, s2->fld,                    \
                                maddflgs, &tstat);                            \
        }                                                                     \
        env->fp_status.float_exception_flags |= tstat.float_exception_flags;  \
                                                                              \
        if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {     \
            float_invalid_op_madd(env, tstat.float_exception_flags,           \
                                  sfprf, GETPC());                            \
        }                                                                     \
                                                                              \
        if (r2sp) {                                                           \
            t.fld = do_frsp(env, t.fld, GETPC());                             \
        }                                                                     \
                                                                              \
        if (sfprf) {                                                          \
            helper_compute_fprf_float64(env, t.fld);                          \
        }                                                                     \
    }                                                                         \
    *xt = t;                                                                  \
    do_float_check_status(env, GETPC());                                      \
}

VSX_MADD(XSMADDDP, 1, float64, VsrD(0), MADD_FLGS, 1, 0)
VSX_MADD(XSMSUBDP, 1, float64, VsrD(0), MSUB_FLGS, 1, 0)
VSX_MADD(XSNMADDDP, 1, float64, VsrD(0), NMADD_FLGS, 1, 0)
VSX_MADD(XSNMSUBDP, 1, float64, VsrD(0), NMSUB_FLGS, 1, 0)
VSX_MADD(XSMADDSP, 1, float64, VsrD(0), MADD_FLGS, 1, 1)
VSX_MADD(XSMSUBSP, 1, float64, VsrD(0), MSUB_FLGS, 1, 1)
VSX_MADD(XSNMADDSP, 1, float64, VsrD(0), NMADD_FLGS, 1, 1)
VSX_MADD(XSNMSUBSP, 1, float64, VsrD(0), NMSUB_FLGS, 1, 1)

VSX_MADD(xvmadddp, 2, float64, VsrD(i), MADD_FLGS, 0, 0)
VSX_MADD(xvmsubdp, 2, float64, VsrD(i), MSUB_FLGS, 0, 0)
VSX_MADD(xvnmadddp, 2, float64, VsrD(i), NMADD_FLGS, 0, 0)
VSX_MADD(xvnmsubdp, 2, float64, VsrD(i), NMSUB_FLGS, 0, 0)

VSX_MADD(xvmaddsp, 4, float32, VsrW(i), MADD_FLGS, 0, 0)
VSX_MADD(xvmsubsp, 4, float32, VsrW(i), MSUB_FLGS, 0, 0)
VSX_MADD(xvnmaddsp, 4, float32, VsrW(i), NMADD_FLGS, 0, 0)
VSX_MADD(xvnmsubsp, 4, float32, VsrW(i), NMSUB_FLGS, 0, 0)

/*
 * VSX_MADDQ - VSX floating point quad-precision muliply/add
 *   op    - instruction mnemonic
 *   maddflgs - flags for the float*muladd routine that control the
 *           various forms (madd, msub, nmadd, nmsub)
 *   ro    - round to odd
 */
#define VSX_MADDQ(op, maddflgs, ro)                                            \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *s1, ppc_vsr_t *s2,\
                 ppc_vsr_t *s3)                                                \
{                                                                              \
    ppc_vsr_t t = *xt;                                                         \
                                                                               \
    helper_reset_fpstatus(env);                                                \
                                                                               \
    float_status tstat = env->fp_status;                                       \
    set_float_exception_flags(0, &tstat);                                      \
    if (ro) {                                                                  \
        tstat.float_rounding_mode = float_round_to_odd;                        \
    }                                                                          \
    t.f128 = float128_muladd(s1->f128, s3->f128, s2->f128, maddflgs, &tstat);  \
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;       \
                                                                               \
    if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {          \
        float_invalid_op_madd(env, tstat.float_exception_flags,                \
                              false, GETPC());                                 \
    }                                                                          \
                                                                               \
    helper_compute_fprf_float128(env, t.f128);                                 \
    *xt = t;                                                                   \
    do_float_check_status(env, GETPC());                                       \
}

VSX_MADDQ(XSMADDQP, MADD_FLGS, 0)
VSX_MADDQ(XSMADDQPO, MADD_FLGS, 1)
VSX_MADDQ(XSMSUBQP, MSUB_FLGS, 0)
VSX_MADDQ(XSMSUBQPO, MSUB_FLGS, 1)
VSX_MADDQ(XSNMADDQP, NMADD_FLGS, 0)
VSX_MADDQ(XSNMADDQPO, NMADD_FLGS, 1)
VSX_MADDQ(XSNMSUBQP, NMSUB_FLGS, 0)
VSX_MADDQ(XSNMSUBQPO, NMSUB_FLGS, 0)

/*
 * VSX_SCALAR_CMP - VSX scalar floating point compare
 *   op    - instruction mnemonic
 *   tp    - type
 *   cmp   - comparison operation
 *   exp   - expected result of comparison
 *   fld   - vsr_t field
 *   svxvc - set VXVC bit
 */
#define VSX_SCALAR_CMP(op, tp, cmp, fld, exp, svxvc)                          \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt,                             \
                 ppc_vsr_t *xa, ppc_vsr_t *xb)                                \
{                                                                             \
    ppc_vsr_t t = { };                                                        \
    bool vxsnan_flag = false, vxvc_flag = false, vex_flag = false;            \
                                                                              \
    if (tp##_is_signaling_nan(xa->fld, &env->fp_status) ||                    \
        tp##_is_signaling_nan(xb->fld, &env->fp_status)) {                    \
        vxsnan_flag = true;                                                   \
        if (fpscr_ve == 0 && svxvc) {                                         \
            vxvc_flag = true;                                                 \
        }                                                                     \
    } else if (svxvc) {                                                       \
        vxvc_flag = tp##_is_quiet_nan(xa->fld, &env->fp_status) ||            \
            tp##_is_quiet_nan(xb->fld, &env->fp_status);                      \
    }                                                                         \
    if (vxsnan_flag) {                                                        \
        float_invalid_op_vxsnan(env, GETPC());                                \
    }                                                                         \
    if (vxvc_flag) {                                                          \
        float_invalid_op_vxvc(env, 0, GETPC());                               \
    }                                                                         \
    vex_flag = fpscr_ve && (vxvc_flag || vxsnan_flag);                        \
                                                                              \
    if (!vex_flag) {                                                          \
        if (tp##_##cmp(xb->fld, xa->fld, &env->fp_status) == exp) {           \
            memset(&t.fld, 0xFF, sizeof(t.fld));                              \
        }                                                                     \
    }                                                                         \
    *xt = t;                                                                  \
    do_float_check_status(env, GETPC());                                      \
}

VSX_SCALAR_CMP(XSCMPEQDP, float64, eq, VsrD(0), 1, 0)
VSX_SCALAR_CMP(XSCMPGEDP, float64, le, VsrD(0), 1, 1)
VSX_SCALAR_CMP(XSCMPGTDP, float64, lt, VsrD(0), 1, 1)
VSX_SCALAR_CMP(XSCMPEQQP, float128, eq, f128, 1, 0)
VSX_SCALAR_CMP(XSCMPGEQP, float128, le, f128, 1, 1)
VSX_SCALAR_CMP(XSCMPGTQP, float128, lt, f128, 1, 1)

void helper_xscmpexpdp(CPUPPCState *env, uint32_t opcode,
                       ppc_vsr_t *xa, ppc_vsr_t *xb)
{
    int64_t exp_a, exp_b;
    uint32_t cc;

    exp_a = extract64(xa->VsrD(0), 52, 11);
    exp_b = extract64(xb->VsrD(0), 52, 11);

    if (unlikely(float64_is_any_nan(xa->VsrD(0)) ||
                 float64_is_any_nan(xb->VsrD(0)))) {
        cc = CRF_SO;
    } else {
        if (exp_a < exp_b) {
            cc = CRF_LT;
        } else if (exp_a > exp_b) {
            cc = CRF_GT;
        } else {
            cc = CRF_EQ;
        }
    }

    env->fpscr &= ~FP_FPCC;
    env->fpscr |= cc << FPSCR_FPCC;
    env->crf[BF(opcode)] = cc;

    do_float_check_status(env, GETPC());
}

void helper_xscmpexpqp(CPUPPCState *env, uint32_t opcode,
                       ppc_vsr_t *xa, ppc_vsr_t *xb)
{
    int64_t exp_a, exp_b;
    uint32_t cc;

    exp_a = extract64(xa->VsrD(0), 48, 15);
    exp_b = extract64(xb->VsrD(0), 48, 15);

    if (unlikely(float128_is_any_nan(xa->f128) ||
                 float128_is_any_nan(xb->f128))) {
        cc = CRF_SO;
    } else {
        if (exp_a < exp_b) {
            cc = CRF_LT;
        } else if (exp_a > exp_b) {
            cc = CRF_GT;
        } else {
            cc = CRF_EQ;
        }
    }

    env->fpscr &= ~FP_FPCC;
    env->fpscr |= cc << FPSCR_FPCC;
    env->crf[BF(opcode)] = cc;

    do_float_check_status(env, GETPC());
}

static inline void do_scalar_cmp(CPUPPCState *env, ppc_vsr_t *xa, ppc_vsr_t *xb,
                                 int crf_idx, bool ordered)
{
    uint32_t cc;
    bool vxsnan_flag = false, vxvc_flag = false;

    helper_reset_fpstatus(env);

    switch (float64_compare(xa->VsrD(0), xb->VsrD(0), &env->fp_status)) {
    case float_relation_less:
        cc = CRF_LT;
        break;
    case float_relation_equal:
        cc = CRF_EQ;
        break;
    case float_relation_greater:
        cc = CRF_GT;
        break;
    case float_relation_unordered:
        cc = CRF_SO;

        if (float64_is_signaling_nan(xa->VsrD(0), &env->fp_status) ||
            float64_is_signaling_nan(xb->VsrD(0), &env->fp_status)) {
            vxsnan_flag = true;
            if (fpscr_ve == 0 && ordered) {
                vxvc_flag = true;
            }
        } else if (float64_is_quiet_nan(xa->VsrD(0), &env->fp_status) ||
                   float64_is_quiet_nan(xb->VsrD(0), &env->fp_status)) {
            if (ordered) {
                vxvc_flag = true;
            }
        }

        break;
    default:
        g_assert_not_reached();
    }

    env->fpscr &= ~FP_FPCC;
    env->fpscr |= cc << FPSCR_FPCC;
    env->crf[crf_idx] = cc;

    if (vxsnan_flag) {
        float_invalid_op_vxsnan(env, GETPC());
    }
    if (vxvc_flag) {
        float_invalid_op_vxvc(env, 0, GETPC());
    }

    do_float_check_status(env, GETPC());
}

void helper_xscmpodp(CPUPPCState *env, uint32_t opcode, ppc_vsr_t *xa,
                     ppc_vsr_t *xb)
{
    do_scalar_cmp(env, xa, xb, BF(opcode), true);
}

void helper_xscmpudp(CPUPPCState *env, uint32_t opcode, ppc_vsr_t *xa,
                     ppc_vsr_t *xb)
{
    do_scalar_cmp(env, xa, xb, BF(opcode), false);
}

static inline void do_scalar_cmpq(CPUPPCState *env, ppc_vsr_t *xa,
                                  ppc_vsr_t *xb, int crf_idx, bool ordered)
{
    uint32_t cc;
    bool vxsnan_flag = false, vxvc_flag = false;

    helper_reset_fpstatus(env);

    switch (float128_compare(xa->f128, xb->f128, &env->fp_status)) {
    case float_relation_less:
        cc = CRF_LT;
        break;
    case float_relation_equal:
        cc = CRF_EQ;
        break;
    case float_relation_greater:
        cc = CRF_GT;
        break;
    case float_relation_unordered:
        cc = CRF_SO;

        if (float128_is_signaling_nan(xa->f128, &env->fp_status) ||
            float128_is_signaling_nan(xb->f128, &env->fp_status)) {
            vxsnan_flag = true;
            if (fpscr_ve == 0 && ordered) {
                vxvc_flag = true;
            }
        } else if (float128_is_quiet_nan(xa->f128, &env->fp_status) ||
                   float128_is_quiet_nan(xb->f128, &env->fp_status)) {
            if (ordered) {
                vxvc_flag = true;
            }
        }

        break;
    default:
        g_assert_not_reached();
    }

    env->fpscr &= ~FP_FPCC;
    env->fpscr |= cc << FPSCR_FPCC;
    env->crf[crf_idx] = cc;

    if (vxsnan_flag) {
        float_invalid_op_vxsnan(env, GETPC());
    }
    if (vxvc_flag) {
        float_invalid_op_vxvc(env, 0, GETPC());
    }

    do_float_check_status(env, GETPC());
}

void helper_xscmpoqp(CPUPPCState *env, uint32_t opcode, ppc_vsr_t *xa,
                     ppc_vsr_t *xb)
{
    do_scalar_cmpq(env, xa, xb, BF(opcode), true);
}

void helper_xscmpuqp(CPUPPCState *env, uint32_t opcode, ppc_vsr_t *xa,
                     ppc_vsr_t *xb)
{
    do_scalar_cmpq(env, xa, xb, BF(opcode), false);
}

/*
 * VSX_MAX_MIN - VSX floating point maximum/minimum
 *   name  - instruction mnemonic
 *   op    - operation (max or min)
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 */
#define VSX_MAX_MIN(name, op, nels, tp, fld)                                  \
void helper_##name(CPUPPCState *env, ppc_vsr_t *xt,                           \
                   ppc_vsr_t *xa, ppc_vsr_t *xb)                              \
{                                                                             \
    ppc_vsr_t t = *xt;                                                        \
    int i;                                                                    \
                                                                              \
    for (i = 0; i < nels; i++) {                                              \
        t.fld = tp##_##op(xa->fld, xb->fld, &env->fp_status);                 \
        if (unlikely(tp##_is_signaling_nan(xa->fld, &env->fp_status) ||       \
                     tp##_is_signaling_nan(xb->fld, &env->fp_status))) {      \
            float_invalid_op_vxsnan(env, GETPC());                            \
        }                                                                     \
    }                                                                         \
                                                                              \
    *xt = t;                                                                  \
    do_float_check_status(env, GETPC());                                      \
}

VSX_MAX_MIN(xsmaxdp, maxnum, 1, float64, VsrD(0))
VSX_MAX_MIN(xvmaxdp, maxnum, 2, float64, VsrD(i))
VSX_MAX_MIN(xvmaxsp, maxnum, 4, float32, VsrW(i))
VSX_MAX_MIN(xsmindp, minnum, 1, float64, VsrD(0))
VSX_MAX_MIN(xvmindp, minnum, 2, float64, VsrD(i))
VSX_MAX_MIN(xvminsp, minnum, 4, float32, VsrW(i))

#define VSX_MAX_MINC(name, op, tp, fld)                                       \
void helper_##name(CPUPPCState *env,                                          \
                   ppc_vsr_t *xt, ppc_vsr_t *xa, ppc_vsr_t *xb)               \
{                                                                             \
    ppc_vsr_t t = *xt;                                                        \
    bool vxsnan_flag = false, vex_flag = false;                               \
                                                                              \
    if (unlikely(tp##_is_any_nan(xa->fld) ||                                  \
                 tp##_is_any_nan(xb->fld))) {                                 \
        if (tp##_is_signaling_nan(xa->fld, &env->fp_status) ||                \
            tp##_is_signaling_nan(xb->fld, &env->fp_status)) {                \
            vxsnan_flag = true;                                               \
        }                                                                     \
        t.fld = xb->fld;                                                      \
    } else {                                                                  \
        t.fld = tp##_##op(xa->fld, xb->fld, &env->fp_status);                 \
    }                                                                         \
                                                                              \
    vex_flag = fpscr_ve & vxsnan_flag;                                        \
    if (vxsnan_flag) {                                                        \
        float_invalid_op_vxsnan(env, GETPC());                                \
    }                                                                         \
    if (!vex_flag) {                                                          \
        *xt = t;                                                              \
    }                                                                         \
}                                                                             \

VSX_MAX_MINC(XSMAXCDP, maxnum, float64, VsrD(0));
VSX_MAX_MINC(XSMINCDP, minnum, float64, VsrD(0));
VSX_MAX_MINC(XSMAXCQP, maxnum, float128, f128);
VSX_MAX_MINC(XSMINCQP, minnum, float128, f128);

#define VSX_MAX_MINJ(name, max)                                               \
void helper_##name(CPUPPCState *env,                                          \
                   ppc_vsr_t *xt, ppc_vsr_t *xa, ppc_vsr_t *xb)               \
{                                                                             \
    ppc_vsr_t t = *xt;                                                        \
    bool vxsnan_flag = false, vex_flag = false;                               \
                                                                              \
    if (unlikely(float64_is_any_nan(xa->VsrD(0)))) {                          \
        if (float64_is_signaling_nan(xa->VsrD(0), &env->fp_status)) {         \
            vxsnan_flag = true;                                               \
        }                                                                     \
        t.VsrD(0) = xa->VsrD(0);                                              \
    } else if (unlikely(float64_is_any_nan(xb->VsrD(0)))) {                   \
        if (float64_is_signaling_nan(xb->VsrD(0), &env->fp_status)) {         \
            vxsnan_flag = true;                                               \
        }                                                                     \
        t.VsrD(0) = xb->VsrD(0);                                              \
    } else if (float64_is_zero(xa->VsrD(0)) &&                                \
               float64_is_zero(xb->VsrD(0))) {                                \
        if (max) {                                                            \
            if (!float64_is_neg(xa->VsrD(0)) ||                               \
                !float64_is_neg(xb->VsrD(0))) {                               \
                t.VsrD(0) = 0ULL;                                             \
            } else {                                                          \
                t.VsrD(0) = 0x8000000000000000ULL;                            \
            }                                                                 \
        } else {                                                              \
            if (float64_is_neg(xa->VsrD(0)) ||                                \
                float64_is_neg(xb->VsrD(0))) {                                \
                t.VsrD(0) = 0x8000000000000000ULL;                            \
            } else {                                                          \
                t.VsrD(0) = 0ULL;                                             \
            }                                                                 \
        }                                                                     \
    } else if ((max &&                                                        \
               !float64_lt(xa->VsrD(0), xb->VsrD(0), &env->fp_status)) ||     \
               (!max &&                                                       \
               float64_lt(xa->VsrD(0), xb->VsrD(0), &env->fp_status))) {      \
        t.VsrD(0) = xa->VsrD(0);                                              \
    } else {                                                                  \
        t.VsrD(0) = xb->VsrD(0);                                              \
    }                                                                         \
                                                                              \
    vex_flag = fpscr_ve & vxsnan_flag;                                        \
    if (vxsnan_flag) {                                                        \
        float_invalid_op_vxsnan(env, GETPC());                                \
    }                                                                         \
    if (!vex_flag) {                                                          \
        *xt = t;                                                              \
    }                                                                         \
}                                                                             \

VSX_MAX_MINJ(XSMAXJDP, 1);
VSX_MAX_MINJ(XSMINJDP, 0);

/*
 * VSX_CMP - VSX floating point compare
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   cmp   - comparison operation
 *   svxvc - set VXVC bit
 *   exp   - expected result of comparison
 */
#define VSX_CMP(op, nels, tp, fld, cmp, svxvc, exp)                       \
uint32_t helper_##op(CPUPPCState *env, ppc_vsr_t *xt,                     \
                     ppc_vsr_t *xa, ppc_vsr_t *xb)                        \
{                                                                         \
    ppc_vsr_t t = *xt;                                                    \
    uint32_t crf6 = 0;                                                    \
    int i;                                                                \
    int all_true = 1;                                                     \
    int all_false = 1;                                                    \
                                                                          \
    for (i = 0; i < nels; i++) {                                          \
        if (unlikely(tp##_is_any_nan(xa->fld) ||                          \
                     tp##_is_any_nan(xb->fld))) {                         \
            if (tp##_is_signaling_nan(xa->fld, &env->fp_status) ||        \
                tp##_is_signaling_nan(xb->fld, &env->fp_status)) {        \
                float_invalid_op_vxsnan(env, GETPC());                    \
            }                                                             \
            if (svxvc) {                                                  \
                float_invalid_op_vxvc(env, 0, GETPC());                   \
            }                                                             \
            t.fld = 0;                                                    \
            all_true = 0;                                                 \
        } else {                                                          \
            if (tp##_##cmp(xb->fld, xa->fld, &env->fp_status) == exp) {   \
                t.fld = -1;                                               \
                all_false = 0;                                            \
            } else {                                                      \
                t.fld = 0;                                                \
                all_true = 0;                                             \
            }                                                             \
        }                                                                 \
    }                                                                     \
                                                                          \
    *xt = t;                                                              \
    crf6 = (all_true ? 0x8 : 0) | (all_false ? 0x2 : 0);                  \
    return crf6;                                                          \
}

VSX_CMP(xvcmpeqdp, 2, float64, VsrD(i), eq, 0, 1)
VSX_CMP(xvcmpgedp, 2, float64, VsrD(i), le, 1, 1)
VSX_CMP(xvcmpgtdp, 2, float64, VsrD(i), lt, 1, 1)
VSX_CMP(xvcmpnedp, 2, float64, VsrD(i), eq, 0, 0)
VSX_CMP(xvcmpeqsp, 4, float32, VsrW(i), eq, 0, 1)
VSX_CMP(xvcmpgesp, 4, float32, VsrW(i), le, 1, 1)
VSX_CMP(xvcmpgtsp, 4, float32, VsrW(i), lt, 1, 1)
VSX_CMP(xvcmpnesp, 4, float32, VsrW(i), eq, 0, 0)

/*
 * VSX_CVT_FP_TO_FP - VSX floating point/floating point conversion
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   stp   - source type (float32 or float64)
 *   ttp   - target type (float32 or float64)
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field (f32 or f64)
 *   sfprf - set FPRF
 */
#define VSX_CVT_FP_TO_FP(op, nels, stp, ttp, sfld, tfld, sfprf)    \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)   \
{                                                                  \
    ppc_vsr_t t = *xt;                                             \
    int i;                                                         \
                                                                   \
    for (i = 0; i < nels; i++) {                                   \
        t.tfld = stp##_to_##ttp(xb->sfld, &env->fp_status);        \
        if (unlikely(stp##_is_signaling_nan(xb->sfld,              \
                                            &env->fp_status))) {   \
            float_invalid_op_vxsnan(env, GETPC());                 \
            t.tfld = ttp##_snan_to_qnan(t.tfld);                   \
        }                                                          \
        if (sfprf) {                                               \
            helper_compute_fprf_##ttp(env, t.tfld);                \
        }                                                          \
    }                                                              \
                                                                   \
    *xt = t;                                                       \
    do_float_check_status(env, GETPC());                           \
}

VSX_CVT_FP_TO_FP(xscvdpsp, 1, float64, float32, VsrD(0), VsrW(0), 1)
VSX_CVT_FP_TO_FP(xscvspdp, 1, float32, float64, VsrW(0), VsrD(0), 1)
VSX_CVT_FP_TO_FP(xvcvdpsp, 2, float64, float32, VsrD(i), VsrW(2 * i), 0)
VSX_CVT_FP_TO_FP(xvcvspdp, 2, float32, float64, VsrW(2 * i), VsrD(i), 0)

/*
 * VSX_CVT_FP_TO_FP_VECTOR - VSX floating point/floating point conversion
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   stp   - source type (float32 or float64)
 *   ttp   - target type (float32 or float64)
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field (f32 or f64)
 *   sfprf - set FPRF
 */
#define VSX_CVT_FP_TO_FP_VECTOR(op, nels, stp, ttp, sfld, tfld, sfprf)    \
void helper_##op(CPUPPCState *env, uint32_t opcode,                       \
                 ppc_vsr_t *xt, ppc_vsr_t *xb)                            \
{                                                                       \
    ppc_vsr_t t = *xt;                                                  \
    int i;                                                              \
                                                                        \
    for (i = 0; i < nels; i++) {                                        \
        t.tfld = stp##_to_##ttp(xb->sfld, &env->fp_status);             \
        if (unlikely(stp##_is_signaling_nan(xb->sfld,                   \
                                            &env->fp_status))) {        \
            float_invalid_op_vxsnan(env, GETPC());                      \
            t.tfld = ttp##_snan_to_qnan(t.tfld);                        \
        }                                                               \
        if (sfprf) {                                                    \
            helper_compute_fprf_##ttp(env, t.tfld);                     \
        }                                                               \
    }                                                                   \
                                                                        \
    *xt = t;                                                            \
    do_float_check_status(env, GETPC());                                \
}

VSX_CVT_FP_TO_FP_VECTOR(xscvdpqp, 1, float64, float128, VsrD(0), f128, 1)

/*
 * VSX_CVT_FP_TO_FP_HP - VSX floating point/floating point conversion
 *                       involving one half precision value
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   stp   - source type
 *   ttp   - target type
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field
 *   sfprf - set FPRF
 */
#define VSX_CVT_FP_TO_FP_HP(op, nels, stp, ttp, sfld, tfld, sfprf) \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)   \
{                                                                  \
    ppc_vsr_t t = { };                                             \
    int i;                                                         \
                                                                   \
    for (i = 0; i < nels; i++) {                                   \
        t.tfld = stp##_to_##ttp(xb->sfld, 1, &env->fp_status);     \
        if (unlikely(stp##_is_signaling_nan(xb->sfld,              \
                                            &env->fp_status))) {   \
            float_invalid_op_vxsnan(env, GETPC());                 \
            t.tfld = ttp##_snan_to_qnan(t.tfld);                   \
        }                                                          \
        if (sfprf) {                                               \
            helper_compute_fprf_##ttp(env, t.tfld);                \
        }                                                          \
    }                                                              \
                                                                   \
    *xt = t;                                                       \
    do_float_check_status(env, GETPC());                           \
}

VSX_CVT_FP_TO_FP_HP(xscvdphp, 1, float64, float16, VsrD(0), VsrH(3), 1)
VSX_CVT_FP_TO_FP_HP(xscvhpdp, 1, float16, float64, VsrH(3), VsrD(0), 1)
VSX_CVT_FP_TO_FP_HP(xvcvsphp, 4, float32, float16, VsrW(i), VsrH(2 * i  + 1), 0)
VSX_CVT_FP_TO_FP_HP(xvcvhpsp, 4, float16, float32, VsrH(2 * i + 1), VsrW(i), 0)

void helper_XSCVQPDP(CPUPPCState *env, uint32_t ro, ppc_vsr_t *xt,
                     ppc_vsr_t *xb)
{
    ppc_vsr_t t = { };
    float_status tstat;

    tstat = env->fp_status;
    if (ro != 0) {
        tstat.float_rounding_mode = float_round_to_odd;
    }

    t.VsrD(0) = float128_to_float64(xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;
    if (unlikely(float128_is_signaling_nan(xb->f128, &tstat))) {
        float_invalid_op_vxsnan(env, GETPC());
        t.VsrD(0) = float64_snan_to_qnan(t.VsrD(0));
    }
    helper_compute_fprf_float64(env, t.VsrD(0));

    *xt = t;
    do_float_check_status(env, GETPC());
}

uint64_t helper_xscvdpspn(CPUPPCState *env, uint64_t xb)
{
    uint64_t result, sign, exp, frac;

    float_status tstat = env->fp_status;
    set_float_exception_flags(0, &tstat);

    sign = extract64(xb, 63,  1);
    exp  = extract64(xb, 52, 11);
    frac = extract64(xb,  0, 52) | 0x10000000000000ULL;

    if (unlikely(exp == 0 && extract64(frac, 0, 52) != 0)) {
        /* DP denormal operand.  */
        /* Exponent override to DP min exp.  */
        exp = 1;
        /* Implicit bit override to 0.  */
        frac = deposit64(frac, 53, 1, 0);
    }

    if (unlikely(exp < 897 && frac != 0)) {
        /* SP tiny operand.  */
        if (897 - exp > 63) {
            frac = 0;
        } else {
            /* Denormalize until exp = SP min exp.  */
            frac >>= (897 - exp);
        }
        /* Exponent override to SP min exp - 1.  */
        exp = 896;
    }

    result = sign << 31;
    result |= extract64(exp, 10, 1) << 30;
    result |= extract64(exp, 0, 7) << 23;
    result |= extract64(frac, 29, 23);

    /* hardware replicates result to both words of the doubleword result.  */
    return (result << 32) | result;
}

uint64_t helper_xscvspdpn(CPUPPCState *env, uint64_t xb)
{
    return helper_todouble(xb >> 32);
}

/*
 * VSX_CVT_FP_TO_INT - VSX floating point to integer conversion
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   stp   - source type (float32 or float64)
 *   ttp   - target type (int32, uint32, int64 or uint64)
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field
 *   rnan  - resulting NaN
 */
#define VSX_CVT_FP_TO_INT(op, nels, stp, ttp, sfld, tfld, rnan)              \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)             \
{                                                                            \
    int all_flags = env->fp_status.float_exception_flags, flags;             \
    ppc_vsr_t t = *xt;                                                       \
    int i;                                                                   \
                                                                             \
    for (i = 0; i < nels; i++) {                                             \
        env->fp_status.float_exception_flags = 0;                            \
        t.tfld = stp##_to_##ttp##_round_to_zero(xb->sfld, &env->fp_status);  \
        flags = env->fp_status.float_exception_flags;                        \
        if (unlikely(flags & float_flag_invalid)) {                          \
            t.tfld = float_invalid_cvt(env, flags, t.tfld, rnan, 0, GETPC());\
        }                                                                    \
        all_flags |= flags;                                                  \
    }                                                                        \
                                                                             \
    *xt = t;                                                                 \
    env->fp_status.float_exception_flags = all_flags;                        \
    do_float_check_status(env, GETPC());                                     \
}

VSX_CVT_FP_TO_INT(xscvdpsxds, 1, float64, int64, VsrD(0), VsrD(0), \
                  0x8000000000000000ULL)
VSX_CVT_FP_TO_INT(xscvdpsxws, 1, float64, int32, VsrD(0), VsrW(1), \
                  0x80000000U)
VSX_CVT_FP_TO_INT(xscvdpuxds, 1, float64, uint64, VsrD(0), VsrD(0), 0ULL)
VSX_CVT_FP_TO_INT(xscvdpuxws, 1, float64, uint32, VsrD(0), VsrW(1), 0U)
VSX_CVT_FP_TO_INT(xvcvdpsxds, 2, float64, int64, VsrD(i), VsrD(i), \
                  0x8000000000000000ULL)
VSX_CVT_FP_TO_INT(xvcvdpsxws, 2, float64, int32, VsrD(i), VsrW(2 * i), \
                  0x80000000U)
VSX_CVT_FP_TO_INT(xvcvdpuxds, 2, float64, uint64, VsrD(i), VsrD(i), 0ULL)
VSX_CVT_FP_TO_INT(xvcvdpuxws, 2, float64, uint32, VsrD(i), VsrW(2 * i), 0U)
VSX_CVT_FP_TO_INT(xvcvspsxds, 2, float32, int64, VsrW(2 * i), VsrD(i), \
                  0x8000000000000000ULL)
VSX_CVT_FP_TO_INT(xvcvspsxws, 4, float32, int32, VsrW(i), VsrW(i), 0x80000000U)
VSX_CVT_FP_TO_INT(xvcvspuxds, 2, float32, uint64, VsrW(2 * i), VsrD(i), 0ULL)
VSX_CVT_FP_TO_INT(xvcvspuxws, 4, float32, uint32, VsrW(i), VsrW(i), 0U)

/*
 * VSX_CVT_FP_TO_INT_VECTOR - VSX floating point to integer conversion
 *   op    - instruction mnemonic
 *   stp   - source type (float32 or float64)
 *   ttp   - target type (int32, uint32, int64 or uint64)
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field
 *   rnan  - resulting NaN
 */
#define VSX_CVT_FP_TO_INT_VECTOR(op, stp, ttp, sfld, tfld, rnan)             \
void helper_##op(CPUPPCState *env, uint32_t opcode,                          \
                 ppc_vsr_t *xt, ppc_vsr_t *xb)                               \
{                                                                            \
    ppc_vsr_t t = { };                                                       \
    int flags;                                                               \
                                                                             \
    t.tfld = stp##_to_##ttp##_round_to_zero(xb->sfld, &env->fp_status);      \
    flags = get_float_exception_flags(&env->fp_status);                      \
    if (flags & float_flag_invalid) {                                        \
        t.tfld = float_invalid_cvt(env, flags, t.tfld, rnan, 0, GETPC());    \
    }                                                                        \
                                                                             \
    *xt = t;                                                                 \
    do_float_check_status(env, GETPC());                                     \
}

VSX_CVT_FP_TO_INT_VECTOR(xscvqpsdz, float128, int64, f128, VsrD(0),          \
                  0x8000000000000000ULL)

VSX_CVT_FP_TO_INT_VECTOR(xscvqpswz, float128, int32, f128, VsrD(0),          \
                  0xffffffff80000000ULL)
VSX_CVT_FP_TO_INT_VECTOR(xscvqpudz, float128, uint64, f128, VsrD(0), 0x0ULL)
VSX_CVT_FP_TO_INT_VECTOR(xscvqpuwz, float128, uint32, f128, VsrD(0), 0x0ULL)

/*
 * VSX_CVT_INT_TO_FP - VSX integer to floating point conversion
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   stp   - source type (int32, uint32, int64 or uint64)
 *   ttp   - target type (float32 or float64)
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field
 *   jdef  - definition of the j index (i or 2*i)
 *   sfprf - set FPRF
 */
#define VSX_CVT_INT_TO_FP(op, nels, stp, ttp, sfld, tfld, sfprf, r2sp)  \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)        \
{                                                                       \
    ppc_vsr_t t = *xt;                                                  \
    int i;                                                              \
                                                                        \
    for (i = 0; i < nels; i++) {                                        \
        t.tfld = stp##_to_##ttp(xb->sfld, &env->fp_status);             \
        if (r2sp) {                                                     \
            t.tfld = do_frsp(env, t.tfld, GETPC());                     \
        }                                                               \
        if (sfprf) {                                                    \
            helper_compute_fprf_float64(env, t.tfld);                   \
        }                                                               \
    }                                                                   \
                                                                        \
    *xt = t;                                                            \
    do_float_check_status(env, GETPC());                                \
}

VSX_CVT_INT_TO_FP(xscvsxddp, 1, int64, float64, VsrD(0), VsrD(0), 1, 0)
VSX_CVT_INT_TO_FP(xscvuxddp, 1, uint64, float64, VsrD(0), VsrD(0), 1, 0)
VSX_CVT_INT_TO_FP(xscvsxdsp, 1, int64, float64, VsrD(0), VsrD(0), 1, 1)
VSX_CVT_INT_TO_FP(xscvuxdsp, 1, uint64, float64, VsrD(0), VsrD(0), 1, 1)
VSX_CVT_INT_TO_FP(xvcvsxddp, 2, int64, float64, VsrD(i), VsrD(i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvuxddp, 2, uint64, float64, VsrD(i), VsrD(i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvsxwdp, 2, int32, float64, VsrW(2 * i), VsrD(i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvuxwdp, 2, uint64, float64, VsrW(2 * i), VsrD(i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvsxdsp, 2, int64, float32, VsrD(i), VsrW(2 * i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvuxdsp, 2, uint64, float32, VsrD(i), VsrW(2 * i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvsxwsp, 4, int32, float32, VsrW(i), VsrW(i), 0, 0)
VSX_CVT_INT_TO_FP(xvcvuxwsp, 4, uint32, float32, VsrW(i), VsrW(i), 0, 0)

/*
 * VSX_CVT_INT_TO_FP_VECTOR - VSX integer to floating point conversion
 *   op    - instruction mnemonic
 *   stp   - source type (int32, uint32, int64 or uint64)
 *   ttp   - target type (float32 or float64)
 *   sfld  - source vsr_t field
 *   tfld  - target vsr_t field
 */
#define VSX_CVT_INT_TO_FP_VECTOR(op, stp, ttp, sfld, tfld)              \
void helper_##op(CPUPPCState *env, uint32_t opcode,                     \
                 ppc_vsr_t *xt, ppc_vsr_t *xb)                          \
{                                                                       \
    ppc_vsr_t t = *xt;                                                  \
                                                                        \
    t.tfld = stp##_to_##ttp(xb->sfld, &env->fp_status);                 \
    helper_compute_fprf_##ttp(env, t.tfld);                             \
                                                                        \
    *xt = t;                                                            \
    do_float_check_status(env, GETPC());                                \
}

VSX_CVT_INT_TO_FP_VECTOR(xscvsdqp, int64, float128, VsrD(0), f128)
VSX_CVT_INT_TO_FP_VECTOR(xscvudqp, uint64, float128, VsrD(0), f128)

/*
 * For "use current rounding mode", define a value that will not be
 * one of the existing rounding model enums.
 */
#define FLOAT_ROUND_CURRENT (float_round_nearest_even + float_round_down + \
  float_round_up + float_round_to_zero)

/*
 * VSX_ROUND - VSX floating point round
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   rmode - rounding mode
 *   sfprf - set FPRF
 */
#define VSX_ROUND(op, nels, tp, fld, rmode, sfprf)                     \
void helper_##op(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)       \
{                                                                      \
    ppc_vsr_t t = *xt;                                                 \
    int i;                                                             \
    FloatRoundMode curr_rounding_mode;                                 \
                                                                       \
    if (rmode != FLOAT_ROUND_CURRENT) {                                \
        curr_rounding_mode = get_float_rounding_mode(&env->fp_status); \
        set_float_rounding_mode(rmode, &env->fp_status);               \
    }                                                                  \
                                                                       \
    for (i = 0; i < nels; i++) {                                       \
        if (unlikely(tp##_is_signaling_nan(xb->fld,                    \
                                           &env->fp_status))) {        \
            float_invalid_op_vxsnan(env, GETPC());                     \
            t.fld = tp##_snan_to_qnan(xb->fld);                        \
        } else {                                                       \
            t.fld = tp##_round_to_int(xb->fld, &env->fp_status);       \
        }                                                              \
        if (sfprf) {                                                   \
            helper_compute_fprf_float64(env, t.fld);                   \
        }                                                              \
    }                                                                  \
                                                                       \
    /*                                                                 \
     * If this is not a "use current rounding mode" instruction,       \
     * then inhibit setting of the XX bit and restore rounding         \
     * mode from FPSCR                                                 \
     */                                                                \
    if (rmode != FLOAT_ROUND_CURRENT) {                                \
        set_float_rounding_mode(curr_rounding_mode, &env->fp_status);  \
        env->fp_status.float_exception_flags &= ~float_flag_inexact;   \
    }                                                                  \
                                                                       \
    *xt = t;                                                           \
    do_float_check_status(env, GETPC());                               \
}

VSX_ROUND(xsrdpi, 1, float64, VsrD(0), float_round_ties_away, 1)
VSX_ROUND(xsrdpic, 1, float64, VsrD(0), FLOAT_ROUND_CURRENT, 1)
VSX_ROUND(xsrdpim, 1, float64, VsrD(0), float_round_down, 1)
VSX_ROUND(xsrdpip, 1, float64, VsrD(0), float_round_up, 1)
VSX_ROUND(xsrdpiz, 1, float64, VsrD(0), float_round_to_zero, 1)

VSX_ROUND(xvrdpi, 2, float64, VsrD(i), float_round_ties_away, 0)
VSX_ROUND(xvrdpic, 2, float64, VsrD(i), FLOAT_ROUND_CURRENT, 0)
VSX_ROUND(xvrdpim, 2, float64, VsrD(i), float_round_down, 0)
VSX_ROUND(xvrdpip, 2, float64, VsrD(i), float_round_up, 0)
VSX_ROUND(xvrdpiz, 2, float64, VsrD(i), float_round_to_zero, 0)

VSX_ROUND(xvrspi, 4, float32, VsrW(i), float_round_ties_away, 0)
VSX_ROUND(xvrspic, 4, float32, VsrW(i), FLOAT_ROUND_CURRENT, 0)
VSX_ROUND(xvrspim, 4, float32, VsrW(i), float_round_down, 0)
VSX_ROUND(xvrspip, 4, float32, VsrW(i), float_round_up, 0)
VSX_ROUND(xvrspiz, 4, float32, VsrW(i), float_round_to_zero, 0)

uint64_t helper_xsrsp(CPUPPCState *env, uint64_t xb)
{
    helper_reset_fpstatus(env);

    uint64_t xt = do_frsp(env, xb, GETPC());

    helper_compute_fprf_float64(env, xt);
    do_float_check_status(env, GETPC());
    return xt;
}

void helper_xvxsigsp(CPUPPCState *env, ppc_vsr_t *xt, ppc_vsr_t *xb)
{
    ppc_vsr_t t = { };
    uint32_t exp, i, fraction;

    for (i = 0; i < 4; i++) {
        exp = (xb->VsrW(i) >> 23) & 0xFF;
        fraction = xb->VsrW(i) & 0x7FFFFF;
        if (exp != 0 && exp != 255) {
            t.VsrW(i) = fraction | 0x00800000;
        } else {
            t.VsrW(i) = fraction;
        }
    }
    *xt = t;
}

/*
 * VSX_TEST_DC - VSX floating point test data class
 *   op    - instruction mnemonic
 *   nels  - number of elements (1, 2 or 4)
 *   xbn   - VSR register number
 *   tp    - type (float32 or float64)
 *   fld   - vsr_t field (VsrD(*) or VsrW(*))
 *   tfld   - target vsr_t field (VsrD(*) or VsrW(*))
 *   fld_max - target field max
 *   scrf - set result in CR and FPCC
 */
#define VSX_TEST_DC(op, nels, xbn, tp, fld, tfld, fld_max, scrf)  \
void helper_##op(CPUPPCState *env, uint32_t opcode)         \
{                                                           \
    ppc_vsr_t *xt = &env->vsr[xT(opcode)];                  \
    ppc_vsr_t *xb = &env->vsr[xbn];                         \
    ppc_vsr_t t = { };                                      \
    uint32_t i, sign, dcmx;                                 \
    uint32_t cc, match = 0;                                 \
                                                            \
    if (!scrf) {                                            \
        dcmx = DCMX_XV(opcode);                             \
    } else {                                                \
        t = *xt;                                            \
        dcmx = DCMX(opcode);                                \
    }                                                       \
                                                            \
    for (i = 0; i < nels; i++) {                            \
        sign = tp##_is_neg(xb->fld);                        \
        if (tp##_is_any_nan(xb->fld)) {                     \
            match = extract32(dcmx, 6, 1);                  \
        } else if (tp##_is_infinity(xb->fld)) {             \
            match = extract32(dcmx, 4 + !sign, 1);          \
        } else if (tp##_is_zero(xb->fld)) {                 \
            match = extract32(dcmx, 2 + !sign, 1);          \
        } else if (tp##_is_zero_or_denormal(xb->fld)) {     \
            match = extract32(dcmx, 0 + !sign, 1);          \
        }                                                   \
                                                            \
        if (scrf) {                                         \
            cc = sign << CRF_LT_BIT | match << CRF_EQ_BIT;  \
            env->fpscr &= ~FP_FPCC;                         \
            env->fpscr |= cc << FPSCR_FPCC;                 \
            env->crf[BF(opcode)] = cc;                      \
        } else {                                            \
            t.tfld = match ? fld_max : 0;                   \
        }                                                   \
        match = 0;                                          \
    }                                                       \
    if (!scrf) {                                            \
        *xt = t;                                            \
    }                                                       \
}

VSX_TEST_DC(xvtstdcdp, 2, xB(opcode), float64, VsrD(i), VsrD(i), UINT64_MAX, 0)
VSX_TEST_DC(xvtstdcsp, 4, xB(opcode), float32, VsrW(i), VsrW(i), UINT32_MAX, 0)
VSX_TEST_DC(xststdcdp, 1, xB(opcode), float64, VsrD(0), VsrD(0), 0, 1)
VSX_TEST_DC(xststdcqp, 1, (rB(opcode) + 32), float128, f128, VsrD(0), 0, 1)

void helper_xststdcsp(CPUPPCState *env, uint32_t opcode, ppc_vsr_t *xb)
{
    uint32_t dcmx, sign, exp;
    uint32_t cc, match = 0, not_sp = 0;
    float64 arg = xb->VsrD(0);
    float64 arg_sp;

    dcmx = DCMX(opcode);
    exp = (arg >> 52) & 0x7FF;
    sign = float64_is_neg(arg);

    if (float64_is_any_nan(arg)) {
        match = extract32(dcmx, 6, 1);
    } else if (float64_is_infinity(arg)) {
        match = extract32(dcmx, 4 + !sign, 1);
    } else if (float64_is_zero(arg)) {
        match = extract32(dcmx, 2 + !sign, 1);
    } else if (float64_is_zero_or_denormal(arg) || (exp > 0 && exp < 0x381)) {
        match = extract32(dcmx, 0 + !sign, 1);
    }

    arg_sp = helper_todouble(helper_tosingle(arg));
    not_sp = arg != arg_sp;

    cc = sign << CRF_LT_BIT | match << CRF_EQ_BIT | not_sp << CRF_SO_BIT;
    env->fpscr &= ~FP_FPCC;
    env->fpscr |= cc << FPSCR_FPCC;
    env->crf[BF(opcode)] = cc;
}

void helper_xsrqpi(CPUPPCState *env, uint32_t opcode,
                   ppc_vsr_t *xt, ppc_vsr_t *xb)
{
    ppc_vsr_t t = { };
    uint8_t r = Rrm(opcode);
    uint8_t ex = Rc(opcode);
    uint8_t rmc = RMC(opcode);
    uint8_t rmode = 0;
    float_status tstat;

    helper_reset_fpstatus(env);

    if (r == 0 && rmc == 0) {
        rmode = float_round_ties_away;
    } else if (r == 0 && rmc == 0x3) {
        rmode = fpscr_rn;
    } else if (r == 1) {
        switch (rmc) {
        case 0:
            rmode = float_round_nearest_even;
            break;
        case 1:
            rmode = float_round_to_zero;
            break;
        case 2:
            rmode = float_round_up;
            break;
        case 3:
            rmode = float_round_down;
            break;
        default:
            abort();
        }
    }

    tstat = env->fp_status;
    set_float_exception_flags(0, &tstat);
    set_float_rounding_mode(rmode, &tstat);
    t.f128 = float128_round_to_int(xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid_snan)) {
        float_invalid_op_vxsnan(env, GETPC());
    }

    if (ex == 0 && (tstat.float_exception_flags & float_flag_inexact)) {
        env->fp_status.float_exception_flags &= ~float_flag_inexact;
    }

    helper_compute_fprf_float128(env, t.f128);
    do_float_check_status(env, GETPC());
    *xt = t;
}

void helper_xsrqpxp(CPUPPCState *env, uint32_t opcode,
                    ppc_vsr_t *xt, ppc_vsr_t *xb)
{
    ppc_vsr_t t = { };
    uint8_t r = Rrm(opcode);
    uint8_t rmc = RMC(opcode);
    uint8_t rmode = 0;
    floatx80 round_res;
    float_status tstat;

    helper_reset_fpstatus(env);

    if (r == 0 && rmc == 0) {
        rmode = float_round_ties_away;
    } else if (r == 0 && rmc == 0x3) {
        rmode = fpscr_rn;
    } else if (r == 1) {
        switch (rmc) {
        case 0:
            rmode = float_round_nearest_even;
            break;
        case 1:
            rmode = float_round_to_zero;
            break;
        case 2:
            rmode = float_round_up;
            break;
        case 3:
            rmode = float_round_down;
            break;
        default:
            abort();
        }
    }

    tstat = env->fp_status;
    set_float_exception_flags(0, &tstat);
    set_float_rounding_mode(rmode, &tstat);
    round_res = float128_to_floatx80(xb->f128, &tstat);
    t.f128 = floatx80_to_float128(round_res, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid_snan)) {
        float_invalid_op_vxsnan(env, GETPC());
        t.f128 = float128_snan_to_qnan(t.f128);
    }

    helper_compute_fprf_float128(env, t.f128);
    *xt = t;
    do_float_check_status(env, GETPC());
}

void helper_xssqrtqp(CPUPPCState *env, uint32_t opcode,
                     ppc_vsr_t *xt, ppc_vsr_t *xb)
{
    ppc_vsr_t t = { };
    float_status tstat;

    helper_reset_fpstatus(env);

    tstat = env->fp_status;
    if (unlikely(Rc(opcode) != 0)) {
        tstat.float_rounding_mode = float_round_to_odd;
    }

    set_float_exception_flags(0, &tstat);
    t.f128 = float128_sqrt(xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {
        float_invalid_op_sqrt(env, tstat.float_exception_flags, 1, GETPC());
    }

    helper_compute_fprf_float128(env, t.f128);
    *xt = t;
    do_float_check_status(env, GETPC());
}

void helper_xssubqp(CPUPPCState *env, uint32_t opcode,
                    ppc_vsr_t *xt, ppc_vsr_t *xa, ppc_vsr_t *xb)
{
    ppc_vsr_t t = *xt;
    float_status tstat;

    helper_reset_fpstatus(env);

    tstat = env->fp_status;
    if (unlikely(Rc(opcode) != 0)) {
        tstat.float_rounding_mode = float_round_to_odd;
    }

    set_float_exception_flags(0, &tstat);
    t.f128 = float128_sub(xa->f128, xb->f128, &tstat);
    env->fp_status.float_exception_flags |= tstat.float_exception_flags;

    if (unlikely(tstat.float_exception_flags & float_flag_invalid)) {
        float_invalid_op_addsub(env, tstat.float_exception_flags, 1, GETPC());
    }

    helper_compute_fprf_float128(env, t.f128);
    *xt = t;
    do_float_check_status(env, GETPC());
}
