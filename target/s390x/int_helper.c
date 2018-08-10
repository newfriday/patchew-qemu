/*
 *  S/390 integer helper routines
 *
 *  Copyright (c) 2009 Ulrich Hecht
 *  Copyright (c) 2009 Alexander Graf
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
#include "internal.h"
#include "exec/exec-all.h"
#include "qemu/host-utils.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"

/* #define DEBUG_HELPER */
#ifdef DEBUG_HELPER
#define HELPER_LOG(x...) qemu_log(x)
#else
#define HELPER_LOG(x...)
#endif

/* 64/32 -> 32 signed division */
int64_t HELPER(divs32)(CPUS390XState *env, int64_t a, int64_t b64)
{
    int32_t ret, b = b64;
    int64_t q;

    if (b == 0) {
        s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
    }

    ret = q = a / b;
    env->retxl = a % b;

    /* Catch non-representable quotient.  */
    if (ret != q) {
        s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
    }

    return ret;
}

/* 64/32 -> 32 unsigned division */
uint64_t HELPER(divu32)(CPUS390XState *env, uint64_t a, uint64_t b64)
{
    uint32_t ret, b = b64;
    uint64_t q;

    if (b == 0) {
        s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
    }

    ret = q = a / b;
    env->retxl = a % b;

    /* Catch non-representable quotient.  */
    if (ret != q) {
        s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
    }

    return ret;
}

/* 64/64 -> 64 signed division */
int64_t HELPER(divs64)(CPUS390XState *env, int64_t a, int64_t b)
{
    /* Catch divide by zero, and non-representable quotient (MIN / -1).  */
    if (b == 0 || (b == -1 && a == (1ll << 63))) {
        s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
    }
    env->retxl = a % b;
    return a / b;
}

/* 128 -> 64/64 unsigned division */
uint64_t HELPER(divu64)(CPUS390XState *env, uint64_t ah, uint64_t al,
                        uint64_t b)
{
    uint64_t ret;
    /* Signal divide by zero.  */
    if (b == 0) {
        s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
    }
    if (ah == 0) {
        /* 64 -> 64/64 case */
        env->retxl = al % b;
        ret = al / b;
    } else {
        /* ??? Move i386 idivq helper to host-utils.  */
#ifdef CONFIG_INT128
        __uint128_t a = ((__uint128_t)ah << 64) | al;
        __uint128_t q = a / b;
        env->retxl = a % b;
        ret = q;
        if (ret != q) {
            s390_program_interrupt(env, PGM_FIXPT_DIVIDE, ILEN_AUTO, GETPC());
        }
#else
        S390CPU *cpu = s390_env_get_cpu(env);
        /* 32-bit hosts would need special wrapper functionality - just abort if
           we encounter such a case; it's very unlikely anyways. */
        cpu_abort(CPU(cpu), "128 -> 64/64 division not implemented\n");
#endif
    }
    return ret;
}

static void general_operand_exception(CPUS390XState *env, uintptr_t ra)
{
#ifndef CONFIG_USER_ONLY
    LowCore *lowcore;

    lowcore = cpu_map_lowcore(env);
    lowcore->data_exc_code = 0;
    cpu_unmap_lowcore(lowcore);
#endif
    s390_program_interrupt(env, PGM_DATA, ILEN_AUTO, ra);
}

uint64_t HELPER(cvb)(CPUS390XState *env, uint64_t src, uint32_t n)
{
    int i, j;
    uintptr_t ra = GETPC();
    int64_t dec, sign = 0, digit, val = 0, pow10 = 0;

    for (i = 0; i < n; i++) {
        dec = cpu_ldq_data_ra(env, src + (n - i - 1) * 8, ra);
        for (j = 0; j < 16; j++, dec >>= 4) {
            if (i == 0 && j == 0) {
                sign = dec & 0xf;
                if (sign < 0xa) {
                    general_operand_exception(env, ra);
                }
                continue;
            }
            digit = dec & 0xf;
            if (digit > 0x9) {
                general_operand_exception(env, ra);
            }
            if (i == 0 && j == 1) {
                if (sign == 0xb || sign == 0xd) {
                    val = -digit;
                    pow10 = -10;
                } else {
                    val = digit;
                    pow10 = 10;
                }
            } else {
                val += digit * pow10;
                pow10 *= 10;
            }
        }
    }
    return val;
}

uint64_t HELPER(cvd)(int32_t reg)
{
    /* positive 0 */
    uint64_t dec = 0x0c;
    int64_t bin = reg;
    int shift;

    if (bin < 0) {
        bin = -bin;
        dec = 0x0d;
    }

    for (shift = 4; (shift < 64) && bin; shift += 4) {
        dec |= (bin % 10) << shift;
        bin /= 10;
    }

    return dec;
}

uint64_t HELPER(popcnt)(uint64_t val)
{
    /* Note that we don't fold past bytes. */
    val = (val & 0x5555555555555555ULL) + ((val >> 1) & 0x5555555555555555ULL);
    val = (val & 0x3333333333333333ULL) + ((val >> 2) & 0x3333333333333333ULL);
    val = (val + (val >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return val;
}
